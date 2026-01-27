package com.example.pockettracker.core.logic

import android.util.Log
import com.example.pockettracker.core.audio.IAudioBackend
import com.example.pockettracker.core.data.Note
import com.example.pockettracker.core.data.PhraseStep
import com.example.pockettracker.core.data.Project
import com.example.pockettracker.core.data.VolumeUtils
import com.example.pockettracker.core.storage.IFileSystem
import com.example.pockettracker.core.storage.WavWriter

/**
 * RENDER CONTROLLER
 *
 * Handles offline rendering of songs to WAV files.
 * Platform-agnostic - uses interfaces for audio and file system.
 *
 * Rendering process:
 * 1. Find song bounds (first to last used row)
 * 2. Calculate total frames needed
 * 3. Schedule all notes for each step
 * 4. Render audio in chunks
 * 5. Write WAV file
 *
 * Uses a chunked approach to avoid memory issues with long songs.
 */
class RenderController(
    private val audioBackend: IAudioBackend,
    private val fileSystem: IFileSystem
) {
    companion object {
        private const val TAG = "RenderController"
        private const val SAMPLE_RATE = 44100
        private const val CHUNK_SIZE = 4096  // Frames per render chunk
        private const val TICKS_PER_STEP = 12  // Standard tracker tics per step

        // Effect type constants (must match EffectProcessor)
        private const val FX_NONE = 0x00
        private const val FX_ARC = 0x03       // Cxx - Arpeggio Config
        private const val FX_ARPEGGIO = 0x0A  // Axx - Note pattern automation
        private const val FX_KILL = 0x0B      // K00 - Kill sample
        private const val FX_OFFSET = 0x0F    // Oxx - Sample start point
        private const val FX_REPEAT = 0x12    // Rxx - Retrigger sample
        private const val FX_VOLUME = 0x16    // Vxx - Volume automation
    }

    /**
     * Resolved effect parameters for a step during rendering.
     */
    private data class RenderEffects(
        val startPointOverride: Int = -1,  // -1 = use instrument default
        val volumeOverride: Float? = null, // null = use calculated volume
        val killAtFrame: Long? = null,     // null = no kill
        val arpeggioValue: Int? = null,    // null = no arpeggio, otherwise Axx value
        val arcMode: Int = 0,              // ARC mode: 0=UP, 1=DOWN, 2=PINGPONG, 3=RANDOM
        val arcSpeed: Int = 4,             // ARC speed in tics (default 4 = 3 notes/step)
        val repeatInterval: Int? = null    // null = no repeat, otherwise Rxx value in tics
    )

    /**
     * Per-track state for effect persistence during rendering.
     * Effects like ARPEGGIO and REPEAT persist across empty steps.
     */
    private data class RenderTrackState(
        // Last triggered note/instrument for persistence
        var lastNote: Note = Note.EMPTY,
        var lastInstrumentId: Int = -1,
        var lastMidi: Int = 0,
        var lastFrequency: Float = 0f,
        var lastBaseFrequency: Float = 0f,
        var lastVolume: Float = 1f,
        var lastPan: Float = 0.5f,
        var lastStartPointOverride: Int = -1,

        // ARPEGGIO persistence
        var arpeggioActive: Boolean = false,
        var arpeggioValue: Int = 0,      // Axx value
        var arcMode: Int = 0,            // ARC mode
        var arcSpeed: Int = 4,           // ARC speed in tics
        var arpeggioPosition: Int = 0,   // Cumulative position in arp pattern (across steps)

        // REPEAT persistence
        var repeatActive: Boolean = false,
        var repeatInterval: Int = 0,     // Rxx value
        var repeatTicCounter: Int = 0    // Counter for multi-step repeat
    )

    /**
     * Parse effects from a step for rendering.
     */
    private fun parseStepEffects(step: PhraseStep, baseFrame: Long): RenderEffects {
        var startPoint = -1
        var volumeOverride: Float? = null
        var killAtFrame: Long? = null
        var arpeggioValue: Int? = null
        var arcMode = 0
        var arcSpeed = 4
        var repeatInterval: Int? = null

        // Check all 3 FX columns
        val effects = listOf(
            step.fx1Type to step.fx1Value,
            step.fx2Type to step.fx2Value,
            step.fx3Type to step.fx3Value
        )

        for ((type, value) in effects) {
            when (type) {
                FX_OFFSET -> startPoint = value
                FX_VOLUME -> volumeOverride = value / 255.0f
                FX_KILL -> killAtFrame = baseFrame
                FX_ARPEGGIO -> arpeggioValue = value
                FX_ARC -> {
                    arcMode = (value shr 4) and 0x0F
                    arcSpeed = value and 0x0F
                    if (arcSpeed == 0) arcSpeed = 4  // Default speed
                }
                FX_REPEAT -> repeatInterval = value
            }
        }

        return RenderEffects(startPoint, volumeOverride, killAtFrame, arpeggioValue, arcMode, arcSpeed, repeatInterval)
    }

    /**
     * Get arpeggio note for a given position in the pattern.
     */
    private fun getArpeggioNote(baseMidi: Int, semi1: Int, semi2: Int, mode: Int, position: Int): Int {
        val note0 = baseMidi            // Root
        val note1 = baseMidi + semi1    // +semi1
        val note2 = baseMidi + semi2    // +semi2

        return when (mode) {
            0 -> {  // UP: root → +semi1 → +semi2 → ...
                when (position % 3) {
                    0 -> note0
                    1 -> note1
                    else -> note2
                }
            }
            1 -> {  // DOWN: +semi2 → +semi1 → root → ...
                when (position % 3) {
                    0 -> note2
                    1 -> note1
                    else -> note0
                }
            }
            2 -> {  // PINGPONG: root → +semi1 → +semi2 → +semi1 → ...
                when (position % 4) {
                    0 -> note0
                    1 -> note1
                    2 -> note2
                    else -> note1
                }
            }
            3 -> {  // RANDOM: pick randomly (use position as seed for determinism)
                when ((position * 7 + 3) % 3) {  // Simple pseudo-random
                    0 -> note0
                    1 -> note1
                    else -> note2
                }
            }
            else -> note0
        }
    }

    /**
     * Schedule arpeggio notes for a single step (used for persistence).
     * Updates trackState.arpeggioPosition for continuous arpeggio across steps.
     */
    private fun scheduleArpeggioStep(
        trackState: RenderTrackState,
        sampleId: Int,
        trackId: Int,
        triggerFrame: Long,
        framesPerStep: Long,
        framesPerTic: Long,
        phraseStep: Int
    ) {
        val semi1 = (trackState.arpeggioValue shr 4) and 0x0F
        val semi2 = trackState.arpeggioValue and 0x0F
        val framesPerArpNote = trackState.arcSpeed * framesPerTic
        val patternLength = if (trackState.arcMode == 2) 4 else 3

        // Schedule arpeggio notes within this step
        var arpFrame = triggerFrame
        val stepEndFrame = triggerFrame + framesPerStep
        var notesScheduled = 0

        while (arpFrame < stepEndFrame) {
            val patternPos = trackState.arpeggioPosition % patternLength
            val arpMidi = getArpeggioNote(trackState.lastMidi, semi1, semi2, trackState.arcMode, patternPos)
            val arpNote = Note.fromMidi(arpMidi.coerceIn(0, 127))
            val arpFreq = arpNote.toFrequency()

            if (arpFreq > 0f) {
                audioBackend.scheduleNote(
                    frame = arpFrame,
                    sampleId = sampleId,
                    trackId = trackId,
                    freq = arpFreq,
                    baseFreq = trackState.lastBaseFrequency,
                    vol = trackState.lastVolume,
                    pan = trackState.lastPan,
                    startPointOverride = trackState.lastStartPointOverride
                )
                notesScheduled++
            }
            trackState.arpeggioPosition++  // Increment CONTINUOUSLY across steps
            arpFrame += framesPerArpNote
        }
        Log.d(TAG, "🎵 ARP: track=$trackId, step=$phraseStep, notes=$notesScheduled, mode=${trackState.arcMode}, speed=${trackState.arcSpeed}, pos=${trackState.arpeggioPosition}")
    }

    /**
     * Schedule repeat triggers for a single step (used for persistence).
     * Handles both sub-step repeat and multi-step repeat with tic counting.
     */
    private fun scheduleRepeatStep(
        trackState: RenderTrackState,
        sampleId: Int,
        trackId: Int,
        triggerFrame: Long,
        framesPerStep: Long,
        framesPerTic: Long,
        phraseStep: Int
    ) {
        val interval = trackState.repeatInterval

        if (interval < TICKS_PER_STEP) {
            // Sub-step repeat: multiple triggers within this step
            val triggersCount = TICKS_PER_STEP / interval
            val framesPerTrigger = interval * framesPerTic

            for (i in 0 until triggersCount) {
                val retrigFrame = triggerFrame + (i * framesPerTrigger)
                audioBackend.scheduleNote(
                    frame = retrigFrame,
                    sampleId = sampleId,
                    trackId = trackId,
                    freq = trackState.lastFrequency,
                    baseFreq = trackState.lastBaseFrequency,
                    vol = trackState.lastVolume,
                    pan = trackState.lastPan,
                    startPointOverride = trackState.lastStartPointOverride
                )
            }
            Log.d(TAG, "🔁 REPEAT: track=$trackId, step=$phraseStep, triggers=$triggersCount, interval=$interval tics")
        } else {
            // Multi-step repeat: trigger when tic counter reaches interval
            // Each step = 12 tics
            trackState.repeatTicCounter += TICKS_PER_STEP

            if (trackState.repeatTicCounter >= interval) {
                audioBackend.scheduleNote(
                    frame = triggerFrame,
                    sampleId = sampleId,
                    trackId = trackId,
                    freq = trackState.lastFrequency,
                    baseFreq = trackState.lastBaseFrequency,
                    vol = trackState.lastVolume,
                    pan = trackState.lastPan,
                    startPointOverride = trackState.lastStartPointOverride
                )
                trackState.repeatTicCounter = 0  // Reset counter after trigger
                Log.d(TAG, "🔁 REPEAT (multi-step trigger): track=$trackId, step=$phraseStep, interval=$interval tics")
            } else {
                Log.d(TAG, "🔁 REPEAT (counting): track=$trackId, step=$phraseStep, counter=${trackState.repeatTicCounter}/$interval tics")
            }
        }
    }

    /**
     * Render result sealed class
     */
    sealed class RenderResult {
        data class Success(val filename: String, val durationMs: Long) : RenderResult()
        data class Error(val message: String) : RenderResult()
    }

    /**
     * Progress callback for UI updates
     */
    interface ProgressCallback {
        fun onProgress(progress: Float, message: String)
    }

    /**
     * Render song to WAV file.
     *
     * @param project Project to render
     * @param progressCallback Optional callback for progress updates
     * @return RenderResult with success/error info
     */
    fun renderSongToWav(
        project: Project,
        progressCallback: ProgressCallback? = null
    ): RenderResult {
        try {
            progressCallback?.onProgress(0f, "Analyzing song...")

            // 1. Find song bounds
            val (startRow, endRow) = findSongBounds(project)
            if (startRow < 0) {
                return RenderResult.Error("Song is empty")
            }

            // 2. Calculate timing
            val framesPerStep = calculateFramesPerStep(project.tempo)
            val stepsPerPhrase = 16
            val framesPerPhrase = stepsPerPhrase * framesPerStep

            // 3. Prepare for rendering
            audioBackend.stopAll()
            audioBackend.clearScheduledNotes()
            audioBackend.resetFrameCounter()

            // 3a. Set up all instrument parameters that will be used
            setupInstrumentParams(project, startRow, endRow)

            progressCallback?.onProgress(0.1f, "Rendering...")

            // 4. Render row by row, collecting audio data
            val allRowAudio = mutableListOf<FloatArray>()
            var currentFrame = 0L

            Log.d(TAG, "🎵 Starting render: rows $startRow to $endRow, framesPerStep=$framesPerStep, framesPerPhrase=$framesPerPhrase")

            for (row in startRow..endRow) {
                // Update progress
                val rowProgress = (row - startRow).toFloat() / (endRow - startRow + 1)
                progressCallback?.onProgress(0.1f + rowProgress * 0.8f, "Rendering row ${row + 1}...")

                Log.d(TAG, "▶️ Row $row: startFrame=$currentFrame")

                // Render this row (all 8 tracks)
                val (rowFrames, phrasesRendered) = renderSongRow(project, row, currentFrame, framesPerStep)

                if (rowFrames.isNotEmpty()) {
                    allRowAudio.add(rowFrames)
                    val framesBefore = currentFrame
                    currentFrame += phrasesRendered * framesPerPhrase
                    Log.d(TAG, "✅ Row $row: rendered $phrasesRendered phrases, ${rowFrames.size/2} frames, nextFrame=$currentFrame")
                } else {
                    Log.d(TAG, "⏭️ Row $row: empty, skipped")
                }
            }

            // 5. Combine all row audio into final buffers
            val totalSamples = allRowAudio.sumOf { it.size / 2 }
            val leftChannel = FloatArray(totalSamples)
            val rightChannel = FloatArray(totalSamples)

            var outputOffset = 0
            for (rowFrames in allRowAudio) {
                for (i in 0 until rowFrames.size / 2) {
                    leftChannel[outputOffset + i] = rowFrames[i * 2]
                    rightChannel[outputOffset + i] = rowFrames[i * 2 + 1]
                }
                outputOffset += rowFrames.size / 2
            }

            val leftTrimmed = leftChannel
            val rightTrimmed = rightChannel

            progressCallback?.onProgress(0.9f, "Writing WAV file...")

            // 6. Generate filename and write
            val outputDir = fileSystem.getRendersDirectory()
            val filename = generateFilename(project.name, outputDir)

            val success = WavWriter.writeWav(
                fileSystem = fileSystem,
                path = filename,
                leftChannel = leftTrimmed,
                rightChannel = rightTrimmed,
                sampleRate = SAMPLE_RATE
            )

            return if (success) {
                val durationMs = (leftTrimmed.size.toLong() * 1000L) / SAMPLE_RATE
                progressCallback?.onProgress(1f, "Done!")
                RenderResult.Success(filename, durationMs)
            } else {
                RenderResult.Error("Failed to write WAV file")
            }

        } catch (e: Exception) {
            return RenderResult.Error(e.message ?: "Unknown error")
        } finally {
            // Cleanup
            audioBackend.stopAll()
            audioBackend.clearScheduledNotes()
        }
    }

    /**
     * Find first and last used row in song.
     */
    private fun findSongBounds(project: Project): Pair<Int, Int> {
        var firstUsed = -1
        var lastUsed = -1

        for (row in 0 until 256) {
            val hasContent = project.tracks.any { track ->
                row < track.chainRefs.size && track.chainRefs[row] >= 0 && track.chainRefs[row] < 256
            }
            if (hasContent) {
                if (firstUsed < 0) firstUsed = row
                lastUsed = row
            }
        }

        return Pair(firstUsed, lastUsed)
    }

    /**
     * Calculate frames per step based on tempo.
     * Formula: (sampleRate * 60) / (tempo * 4) / 4
     * At 120 BPM: 44100 * 60 / 120 / 4 / 4 = 1378.125 frames/step
     */
    private fun calculateFramesPerStep(tempo: Int): Long {
        // BPM to frames per step
        // At 120 BPM, 4 beats per bar, 16 steps per bar = 16 steps per beat cycle
        // frames per minute = sampleRate * 60
        // frames per beat = frames per minute / tempo
        // frames per step = frames per beat / 4 (4 steps per beat in 16-step phrase)
        val framesPerMinute = SAMPLE_RATE * 60L
        val framesPerBeat = framesPerMinute / tempo
        return framesPerBeat / 4
    }

    /**
     * Render a single song row (all 8 tracks, synchronized).
     * Returns the rendered audio and the number of phrase slots processed.
     *
     * IMPORTANT: Matches PlaybackController timing behavior:
     * - Empty slots (no phrase in ANY track) are SKIPPED, not played as silence
     * - Only slots with at least one phrase across all tracks advance time
     */
    private fun renderSongRow(
        project: Project,
        row: Int,
        startFrame: Long,
        framesPerStep: Long
    ): Pair<FloatArray, Int> {
        val stepsPerPhrase = 16
        val framesPerPhrase = stepsPerPhrase * framesPerStep

        // Find which phrase slots have content in ANY track (matching PlaybackController logic)
        val slotsWithContent = mutableListOf<Int>()
        for (phraseSlot in 0 until 16) {
            var slotHasContent = false
            for (trackId in 0 until 8) {
                val track = project.tracks[trackId]
                if (track.mute) continue
                if (row >= track.chainRefs.size) continue

                val chainRef = track.chainRefs[row]
                if (chainRef < 0 || chainRef >= 256) continue  // -1 = empty

                val chain = project.chains[chainRef]
                val phraseRef = chain.phraseRefs[phraseSlot]
                if (phraseRef >= 0 && phraseRef < 256) {  // -1 = empty
                    slotHasContent = true
                    break
                }
            }
            if (slotHasContent) {
                slotsWithContent.add(phraseSlot)
            }
        }

        // If no slots with content, return empty
        if (slotsWithContent.isEmpty()) {
            return Pair(FloatArray(0), 0)
        }

        val numPhraseSlots = slotsWithContent.size
        val totalFrames = (numPhraseSlots * framesPerPhrase).toInt()

        Log.d(TAG, "🎬 Rendering row $row: slots=$slotsWithContent, totalFrames=$totalFrames, startFrame=$startFrame")

        // Log which tracks have content at this row
        for (t in 0 until 8) {
            val trk = project.tracks[t]
            if (row < trk.chainRefs.size) {
                val ref = trk.chainRefs[row]
                if (ref >= 0 && ref < 256 && !trk.mute) {  // -1 = empty
                    Log.d(TAG, "  Track $t: chain=${ref.toString(16).uppercase()}, mute=${trk.mute}")
                }
            }
        }

        // Initialize per-track state for effect persistence
        val trackStates = Array(8) { RenderTrackState() }

        // Schedule notes for each NON-EMPTY phrase slot (matching PlaybackController)
        var slotIndex = 0
        for (phraseSlot in slotsWithContent) {
            // slotIndex is the TIME position (0, 1, 2...), phraseSlot is the CHAIN position
            val slotStartFrame = startFrame + (slotIndex * framesPerPhrase)

            for (trackId in 0 until 8) {
                val track = project.tracks[trackId]
                val trackState = trackStates[trackId]

                // Skip if track is muted or no chain at this row
                if (track.mute) continue
                if (row >= track.chainRefs.size) continue

                val chainRef = track.chainRefs[row]
                if (chainRef < 0 || chainRef >= 256) continue  // -1 = empty

                val chain = project.chains[chainRef]

                // Check if this track has a phrase at this slot
                val phraseRef = chain.phraseRefs[phraseSlot]
                if (phraseRef < 0 || phraseRef >= 256) continue  // -1 = empty

                val phrase = project.phrases[phraseRef]
                val transpose = chain.getTransposeSemitones(phraseSlot)

                // Calculate tic timing for this phrase
                val framesPerTic = framesPerStep / TICKS_PER_STEP

                // Schedule each step in the phrase
                for (phraseStep in 0 until stepsPerPhrase) {
                    val step = phrase.steps[phraseStep]
                    val triggerFrame = slotStartFrame + (phraseStep * framesPerStep)

                    // Check if this step has a new note (non-empty)
                    val hasNewNote = !step.isEmpty() && step.note != Note.EMPTY

                    if (hasNewNote) {
                        // NEW NOTE: Update state and handle effects

                        val instrument = project.instruments.getOrNull(step.instrument) ?: continue
                        if (instrument.sampleId < 0) continue

                        // Debug: log note details when frequency would be 0
                        if (step.note.toFrequency() == 0f) {
                            Log.w(TAG, "⚠️ Zero freq note: track=$trackId, slot=$phraseSlot, step=$phraseStep, note=${step.note}, inst=${step.instrument}")
                            continue
                        }

                        // Parse effects from this step
                        val effects = parseStepEffects(step, triggerFrame)

                        // Calculate volume (instrument × phrase × track × master)
                        val phraseVol = if (step.volume >= 0) step.volume else 0xFF
                        var volume = VolumeUtils.calculateFinalVolume(
                            instrumentVol = instrument.volume,
                            phraseVol = phraseVol,
                            trackVol = track.volume,
                            masterVol = project.masterVolume
                        )

                        // Apply VOLUME effect (Vxx) if present
                        effects.volumeOverride?.let { vol ->
                            volume = VolumeUtils.calculateFinalVolume(
                                instrumentVol = instrument.volume,
                                phraseVol = (vol * 255).toInt(),
                                trackVol = track.volume,
                                masterVol = project.masterVolume
                            )
                        }

                        // Calculate frequency with transpose
                        val transposedMidi = step.note.toMidi() + transpose + project.transpose
                        val noteWithTranspose = Note.fromMidi(transposedMidi)
                        val frequency = noteWithTranspose.toFrequency()
                        val rootFreq = instrument.root.toFrequency()
                        val baseRootFreq = if (rootFreq > 0f) rootFreq else 261.63f

                        // Apply detune to base frequency
                        val detuneSemitones = (instrument.detune shr 4).toFloat()
                        val detuneFraction = (instrument.detune and 0x0F) / 16.0f
                        val totalDetuneSemitones = detuneSemitones + detuneFraction - 8.0f
                        val detuneMultiplier = Math.pow(2.0, (totalDetuneSemitones / 12.0)).toFloat()
                        val baseFrequency = baseRootFreq * detuneMultiplier

                        // Calculate pan
                        val pan = VolumeUtils.hexToFloat(instrument.pan)

                        // Update track state with new note info
                        trackState.lastNote = step.note
                        trackState.lastInstrumentId = step.instrument
                        trackState.lastMidi = transposedMidi
                        trackState.lastFrequency = frequency
                        trackState.lastBaseFrequency = baseFrequency
                        trackState.lastVolume = volume
                        trackState.lastPan = pan
                        trackState.lastStartPointOverride = effects.startPointOverride

                        // Handle ARPEGGIO effect (Axx) - starts new arp
                        if (effects.arpeggioValue != null && effects.arpeggioValue > 0) {
                            // Clear any previous repeat, activate arpeggio
                            trackState.repeatActive = false
                            trackState.arpeggioActive = true
                            trackState.arpeggioValue = effects.arpeggioValue
                            trackState.arcMode = effects.arcMode
                            trackState.arcSpeed = effects.arcSpeed
                            trackState.arpeggioPosition = 0  // Reset position for new arp

                            scheduleArpeggioStep(
                                trackState, instrument.sampleId, trackId, triggerFrame,
                                framesPerStep, framesPerTic, phraseStep
                            )
                        }
                        // Handle REPEAT effect (Rxx) - starts new repeat
                        else if (effects.repeatInterval != null && effects.repeatInterval > 0) {
                            // Clear any previous arpeggio, activate repeat
                            trackState.arpeggioActive = false
                            trackState.repeatActive = true
                            trackState.repeatInterval = effects.repeatInterval
                            trackState.repeatTicCounter = 0

                            val interval = effects.repeatInterval
                            if (interval < TICKS_PER_STEP) {
                                // Sub-step repeat: scheduleRepeatStep handles all triggers including initial
                                scheduleRepeatStep(
                                    trackState, instrument.sampleId, trackId, triggerFrame,
                                    framesPerStep, framesPerTic, phraseStep
                                )
                            } else {
                                // Multi-step repeat: schedule initial note, then persistence handles rest
                                audioBackend.scheduleNote(
                                    frame = triggerFrame,
                                    sampleId = instrument.sampleId,
                                    trackId = trackId,
                                    freq = frequency,
                                    baseFreq = baseFrequency,
                                    vol = volume,
                                    pan = pan,
                                    startPointOverride = effects.startPointOverride
                                )
                                Log.d(TAG, "🔁 REPEAT (multi-step start): track=$trackId, step=$phraseStep, interval=$interval tics")
                            }
                        }
                        // Normal note - clear persistent effects
                        else {
                            trackState.arpeggioActive = false
                            trackState.repeatActive = false

                            Log.d(TAG, "📝 Schedule: track=$trackId, sample=${instrument.sampleId}, frame=$triggerFrame, freq=${"%.1f".format(frequency)}, vol=${"%.2f".format(volume)}, pan=${"%.2f".format(pan)}, slot=$phraseSlot, step=$phraseStep")
                            audioBackend.scheduleNote(
                                frame = triggerFrame,
                                sampleId = instrument.sampleId,
                                trackId = trackId,
                                freq = frequency,
                                baseFreq = baseFrequency,
                                vol = volume,
                                pan = pan,
                                startPointOverride = effects.startPointOverride
                            )
                        }

                        // Schedule KILL effect if present
                        effects.killAtFrame?.let { killFrame ->
                            audioBackend.scheduleKill(killFrame + 1, trackId)
                            Log.d(TAG, "🔪 Schedule kill: track=$trackId, frame=${killFrame + 1}")
                        }
                    }
                    // EMPTY STEP: Check for persistent effects
                    else if (trackState.arpeggioActive || trackState.repeatActive) {
                        val instrument = project.instruments.getOrNull(trackState.lastInstrumentId)
                        if (instrument != null && instrument.sampleId >= 0) {
                            if (trackState.arpeggioActive) {
                                scheduleArpeggioStep(
                                    trackState, instrument.sampleId, trackId, triggerFrame,
                                    framesPerStep, framesPerTic, phraseStep
                                )
                            } else if (trackState.repeatActive) {
                                scheduleRepeatStep(
                                    trackState, instrument.sampleId, trackId, triggerFrame,
                                    framesPerStep, framesPerTic, phraseStep
                                )
                            }
                        }
                    }
                }
            }
            slotIndex++
        }

        // Render the frames
        return Pair(audioBackend.renderFrames(totalFrames, SAMPLE_RATE), numPhraseSlots)
    }

    /**
     * Set up all instrument parameters that will be used in the render.
     * This must be called before scheduling notes.
     */
    private fun setupInstrumentParams(project: Project, startRow: Int, endRow: Int) {
        // Collect all used instrument IDs
        val usedInstruments = mutableSetOf<Int>()

        for (row in startRow..endRow) {
            for (track in project.tracks) {
                if (track.mute) continue
                if (row >= track.chainRefs.size) continue

                val chainRef = track.chainRefs[row]
                if (chainRef < 0 || chainRef >= 256) continue  // -1 = empty

                val chain = project.chains[chainRef]

                for (phraseSlot in 0 until 16) {
                    val phraseRef = chain.phraseRefs[phraseSlot]
                    if (phraseRef < 0 || phraseRef >= 256) continue  // -1 = empty

                    val phrase = project.phrases[phraseRef]

                    for (step in phrase.steps) {
                        if (!step.isEmpty() && step.instrument >= 0 && step.instrument < 256) {
                            usedInstruments.add(step.instrument)
                        }
                    }
                }
            }
        }

        // Configure each used instrument
        for (instId in usedInstruments) {
            val instrument = project.instruments.getOrNull(instId) ?: continue
            if (instrument.sampleId < 0) continue

            // Convert loop mode string to int
            val loopModeInt = when (instrument.loopMode) {
                "fwd" -> 1
                "png" -> 2
                else -> 0
            }

            // Convert filter type string to int
            val filterTypeInt = when (instrument.filterType) {
                "lp" -> 1
                "hp" -> 2
                "bp" -> 3
                else -> 0
            }

            audioBackend.setInstrumentParams(
                instrumentId = instrument.sampleId,
                startPoint = instrument.sampleStart,
                endPoint = instrument.sampleEnd,
                reverse = instrument.reverse,
                loopMode = loopModeInt,
                loopStart = instrument.loopStart,
                drive = instrument.drive,
                crush = instrument.crush,
                downsample = instrument.downsample,
                filterType = filterTypeInt,
                filterCut = instrument.filterCut,
                filterRes = instrument.filterRes
            )
        }
    }

    /**
     * Generate unique filename with auto-increment.
     */
    private fun generateFilename(projectName: String, outputDir: String): String {
        val baseName = projectName
            .replace(Regex("[^a-zA-Z0-9_\\-]"), "_")
            .take(32)

        var index = 1
        var filename: String

        do {
            val indexStr = index.toString().padStart(4, '0')
            filename = "$outputDir/${baseName}_$indexStr.wav"
            index++
        } while (fileSystem.fileExists(filename) && index < 10000)

        return filename
    }
}
