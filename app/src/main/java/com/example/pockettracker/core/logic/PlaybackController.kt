package com.example.pockettracker.core.logic

import com.example.pockettracker.Note
import com.example.pockettracker.PhraseStep
import com.example.pockettracker.Project
import com.example.pockettracker.core.audio.AudioEngine
import com.example.pockettracker.core.logging.ILogger

/**
 * PlaybackController
 *
 * Manages all playback operations including:
 * - Playback state (playing/stopped)
 * - Phrase/chain/song playback scheduling
 * - Sample-accurate note queue management
 * - Playback cursors and position tracking
 *
 * ✅ PLATFORM-AGNOSTIC - No Android dependencies!
 *
 * Updated in Phase 1 refactoring to use the new AudioEngine architecture.
 * Updated in Phase 5 to remove Compose state dependencies.
 */
class PlaybackController(
    private val audioEngine: AudioEngine,
    private val effectProcessor: EffectProcessor,
    private val logger: ILogger,
    private val stateObserver: StateObserver
) {
    private val TAG = "PlaybackController"

    // ═══════════════════════════════════════════════════════════════════════════
    // STATE
    // ═══════════════════════════════════════════════════════════════════════════

    /** Is playback currently active */
    var isPlaying = false
        private set(value) {
            field = value
            stateObserver.onStateChanged()
        }

    /** Current playback mode */
    var playbackMode = PlaybackMode.STOPPED
        private set(value) {
            field = value
            stateObserver.onStateChanged()
        }

    /** Playback cursor position (for visual feedback) */
    var playbackCursor = 0
        private set(value) {
            field = value
            stateObserver.onStateChanged()
        }

    // ═══════════════════════════════════════════════════════════════════════════
    // PLAYBACK CONTROL
    // ═══════════════════════════════════════════════════════════════════════════

    /**
     * Toggle playback on/off
     * Simple toggle for START button
     */
    fun togglePlayback() {
        isPlaying = !isPlaying
        if (!isPlaying) {
            stop()
        }
        logger.d(TAG, if (isPlaying) "▶️ Playback started" else "⏸️ Playback stopped")
    }

    /**
     * Start playback
     */
    fun play() {
        isPlaying = true
        logger.d(TAG, "▶️ Playback started")
    }

    /**
     * Stop playback and clear queue
     */
    fun stop() {
        isPlaying = false
        playbackMode = PlaybackMode.STOPPED
        playbackCursor = 0
        audioEngine.clearScheduledNotes()
        audioEngine.stopAll()
        logger.d(TAG, "⏹️ Playback stopped")
    }

    // ═══════════════════════════════════════════════════════════════════════════
    // PHRASE PLAYBACK
    // ═══════════════════════════════════════════════════════════════════════════

    /**
     * Play a phrase (16 steps)
     *
     * Schedules all 16 steps with sample-accurate timing.
     * Effects are applied through EffectProcessor if available.
     *
     * @param project Project containing phrase data
     * @param phraseId Which phrase to play (0-255)
     * @param loop Whether to loop playback (not implemented yet)
     */
    fun playPhrase(project: Project, phraseId: Int, loop: Boolean = true) {
        logger.d(TAG, "▶️ Playing phrase $phraseId (tempo: ${project.tempo} BPM)")

        // Stop any current playback
        stop()

        // Get phrase
        if (phraseId !in 0..255) {
            logger.e(TAG, "Invalid phraseId: $phraseId")
            return
        }

        val phrase = project.phrases[phraseId]
        playbackMode = PlaybackMode.PHRASE
        isPlaying = true

        // Get timing information
        val startFrame = audioEngine.getCurrentFrame()
        val tempo = project.tempo

        logger.d(TAG, "Start frame: $startFrame, Tempo: $tempo BPM")

        // Calculate step duration for effects
        val sampleRate = audioEngine.getDeviceSampleRate()
        val msPerStep = (60000.0 / tempo / 4.0)  // 4 steps per beat
        val framesPerStep = (msPerStep * sampleRate / 1000.0).toLong()

        // Schedule all 16 steps using the unified helper
        var scheduledNotes = 0
        phrase.steps.forEachIndexed { stepIndex, step ->
            val targetFrame = audioEngine.calculateTargetFrame(startFrame, stepIndex, tempo)

            // Use unified helper for scheduling + effects
            val noteScheduled = scheduleStepWithEffects(
                step = step,
                targetFrame = targetFrame,
                stepDuration = framesPerStep,
                trackId = 0,  // Single track for phrase playback
                transposeSemitones = 0,  // No transpose for phrase playback
                project = project
            )

            if (noteScheduled) {
                scheduledNotes++
                logger.d(TAG, "  Step $stepIndex: ${step.note} I:${step.instrument.toString(16).uppercase()} V:${step.volume.toString(16).uppercase()} @ frame $targetFrame")
            }
        }

        logger.d(TAG, "✅ Scheduled $scheduledNotes notes from phrase $phraseId")

        // TODO: Implement looping (reschedule after 16 steps complete)
        // TODO: Update playback cursor in real-time for visual feedback
    }

    /**
     * Play a chain (16 phrase slots with transpose)
     *
     * Schedules all non-empty phrase slots in the chain sequentially.
     * Each phrase is 16 steps, and transpose is applied per-slot.
     *
     * @param project Project containing chain data
     * @param chainId Which chain to play (0-255)
     * @param loop Whether to loop playback (not implemented yet)
     */
    fun playChain(project: Project, chainId: Int, loop: Boolean = true) {
        logger.d(TAG, "▶️ Playing chain $chainId (tempo: ${project.tempo} BPM)")

        // Stop any current playback
        stop()

        // Validate chain ID
        if (chainId !in 0..255) {
            logger.e(TAG, "Invalid chainId: $chainId")
            return
        }

        val chain = project.chains[chainId]
        playbackMode = PlaybackMode.CHAIN
        isPlaying = true

        // Get timing information
        val startFrame = audioEngine.getCurrentFrame()
        val tempo = project.tempo
        val sampleRate = audioEngine.getDeviceSampleRate()
        val msPerStep = (60000.0 / tempo / 4.0)  // 4 steps per beat
        val framesPerStep = (msPerStep * sampleRate / 1000.0).toLong()
        val framesPerPhrase = framesPerStep * 16  // 16 steps per phrase

        logger.d(TAG, "Start frame: $startFrame, Tempo: $tempo BPM, Frames per phrase: $framesPerPhrase")

        // Schedule all non-empty chain rows
        var scheduledNotes = 0
        var phraseOffset = 0L  // Cumulative frame offset for each phrase

        for (chainRow in 0..15) {
            if (chain.isEmpty(chainRow)) continue

            val phraseId = chain.phraseRefs[chainRow]
            val transposeSemitones = chain.getTransposeSemitones(chainRow)
            val phrase = project.phrases[phraseId]

            logger.d(TAG, "  Chain row $chainRow: Phrase $phraseId, transpose=$transposeSemitones")

            // Schedule all 16 steps of this phrase
            for (stepIndex in 0..15) {
                val step = phrase.steps[stepIndex]
                val targetFrame = startFrame + phraseOffset + (stepIndex * framesPerStep)

                val noteScheduled = scheduleStepWithEffects(
                    step = step,
                    targetFrame = targetFrame,
                    stepDuration = framesPerStep,
                    trackId = 0,  // Single track for chain playback
                    transposeSemitones = transposeSemitones,
                    project = project
                )

                if (noteScheduled) scheduledNotes++
            }

            phraseOffset += framesPerPhrase
        }

        logger.d(TAG, "✅ Scheduled $scheduledNotes notes from chain $chainId")

        // TODO: Implement looping
    }

    /**
     * Play song (8 tracks polyphonic)
     *
     * Schedules all 8 tracks simultaneously. Each track plays its chain sequence,
     * with proper voice allocation (trackId 0-7).
     *
     * @param project Project containing song data
     * @param startRow Which song row to start from (index into each track's chainRefs)
     * @param loop Whether to loop playback (not implemented yet)
     */
    fun playSong(project: Project, startRow: Int = 0, loop: Boolean = true) {
        logger.d(TAG, "▶️ Playing song from row $startRow (tempo: ${project.tempo} BPM)")

        // Stop any current playback
        stop()

        playbackMode = PlaybackMode.SONG
        isPlaying = true

        // Get timing information
        val startFrame = audioEngine.getCurrentFrame()
        val tempo = project.tempo
        val sampleRate = audioEngine.getDeviceSampleRate()
        val msPerStep = (60000.0 / tempo / 4.0)  // 4 steps per beat
        val framesPerStep = (msPerStep * sampleRate / 1000.0).toLong()
        val framesPerPhrase = framesPerStep * 16  // 16 steps per phrase

        logger.d(TAG, "Start frame: $startFrame, Tempo: $tempo BPM")

        var totalScheduledNotes = 0

        // Schedule all 8 tracks
        for (trackId in 0..7) {
            val track = project.tracks[trackId]
            if (track.chainRefs.isEmpty()) continue

            var trackScheduledNotes = 0
            var songRowOffset = 0L  // Frame offset for song rows (each row = full chain playback)

            // Iterate through song rows (chain references in this track)
            for (songRowIndex in startRow until track.chainRefs.size) {
                val chainId = track.chainRefs[songRowIndex]
                if (chainId < 0 || chainId > 255) continue

                val chain = project.chains[chainId]
                var chainOffset = 0L  // Frame offset within the chain

                // Schedule all phrases in this chain
                for (chainRow in 0..15) {
                    if (chain.isEmpty(chainRow)) continue

                    val phraseId = chain.phraseRefs[chainRow]
                    val transposeSemitones = chain.getTransposeSemitones(chainRow)
                    val phrase = project.phrases[phraseId]

                    // Schedule all 16 steps of this phrase
                    for (stepIndex in 0..15) {
                        val step = phrase.steps[stepIndex]
                        val targetFrame = startFrame + songRowOffset + chainOffset + (stepIndex * framesPerStep)

                        val noteScheduled = scheduleStepWithEffects(
                            step = step,
                            targetFrame = targetFrame,
                            stepDuration = framesPerStep,
                            trackId = trackId,  // Each track gets its own trackId
                            transposeSemitones = transposeSemitones,
                            project = project
                        )

                        if (noteScheduled) trackScheduledNotes++
                    }

                    chainOffset += framesPerPhrase
                }

                // Move to next song row position
                // Each song row represents the longest chain in that row
                // For simplicity, we assume all chains have same length (count non-empty rows)
                val chainLength = (0..15).count { !chain.isEmpty(it) }
                songRowOffset += chainLength * framesPerPhrase
            }

            if (trackScheduledNotes > 0) {
                logger.d(TAG, "  Track $trackId: $trackScheduledNotes notes")
            }
            totalScheduledNotes += trackScheduledNotes
        }

        logger.d(TAG, "✅ Scheduled $totalScheduledNotes notes across all tracks")

        // TODO: Implement looping
    }

    // ═══════════════════════════════════════════════════════════════════════════
    // TEST / DEMO PLAYBACK
    // ═══════════════════════════════════════════════════════════════════════════

    /**
     * Test note queue infrastructure
     *
     * Schedules 8 metronome clicks (1 beat apart) to verify sample-accurate timing.
     * This demonstrates the audio engine's queue system works correctly.
     *
     * Used for: Testing, debugging, development verification
     *
     * @param project Project (for tempo and instrument access)
     */
    fun testNoteQueue(project: Project) {
        logger.d(TAG, "═══════════════════════════════════════════")
        logger.d(TAG, "🧪 PHASE 1 TEST: Sample-Accurate Note Queue")
        logger.d(TAG, "═══════════════════════════════════════════")

        val currentFrame = audioEngine.getCurrentFrame()
        val tempo = project.tempo
        val sampleRate = audioEngine.getDeviceSampleRate()

        // Calculate frames per beat (quarter note) at current tempo
        // 60000ms per minute ÷ BPM = ms per beat
        // ms per beat × sampleRate / 1000 = frames per beat
        val msPerBeat = (60000.0 / tempo)
        val framesPerBeat = (msPerBeat * sampleRate / 1000.0).toLong()

        logger.d(TAG, "Tempo: $tempo BPM")
        logger.d(TAG, "Sample Rate: $sampleRate Hz")
        logger.d(TAG, "Frames per beat: $framesPerBeat")
        logger.d(TAG, "Current frame: $currentFrame")
        logger.d(TAG, "-------------------------------------------")

        // Schedule 8 metronome clicks (C-4 note, kick drum, 1 beat apart)
        val metronomeNote = Note.fromString("C-4")
        val kickInstrument = 0  // Instrument 00 = kick drum

        for (beat in 0..7) {
            val targetFrame = currentFrame + (beat * framesPerBeat)
            audioEngine.scheduleNote(
                targetFrame = targetFrame,
                note = metronomeNote,
                instrumentId = kickInstrument,
                trackId = 0,
                volume = 0.8f,
                project = project
            )

            val targetTimeMs = (beat * msPerBeat).toLong()
            logger.d(TAG, "📅 Beat $beat scheduled: frame=$targetFrame (${targetTimeMs}ms from now)")
        }

        logger.d(TAG, "-------------------------------------------")
        logger.d(TAG, "✅ Scheduled 8 beats. Watch for 🎵 trigger logs!")
        logger.d(TAG, "Expected: Notes trigger at exact scheduled frames")
        logger.d(TAG, "Precision: <0.02ms jitter (sample-accurate)")
        logger.d(TAG, "═══════════════════════════════════════════")
    }

    // ═══════════════════════════════════════════════════════════════════════════
    // STEP SCHEDULING HELPER
    // ═══════════════════════════════════════════════════════════════════════════

    /**
     * Schedule a single phrase step with all effects applied.
     *
     * This is the unified helper for scheduling notes across all playback modes
     * (Phrase, Chain, Song). It handles:
     * - Note scheduling with optional transposition
     * - OFFSET effect (Oxx) - modifies sample start point
     * - VOLUME effect (Vxx) - overrides step volume
     * - Other effects via EffectProcessor (KILL, ARPEGGIO, REPEAT)
     *
     * @param step The phrase step to schedule
     * @param targetFrame When this step should trigger (audio frame)
     * @param stepDuration Duration of step in frames (for time-based effects)
     * @param trackId Which track (0-7)
     * @param transposeSemitones Semitones to transpose (0 for Phrase, varies for Chain/Song)
     * @param project Project containing instrument data
     * @return true if a note was scheduled, false if step was empty
     */
    fun scheduleStepWithEffects(
        step: PhraseStep,
        targetFrame: Long,
        stepDuration: Long,
        trackId: Int,
        transposeSemitones: Int,
        project: Project
    ): Boolean {
        // Resolve all effect parameters via EffectProcessor (single source of truth)
        val defaultVolume = step.volume / 255.0f
        val params = effectProcessor.resolveStepParams(step, targetFrame, defaultVolume)

        // Schedule note if step has one
        var noteScheduled = false
        if (!step.isEmpty()) {
            // Apply transposition if needed
            val note = if (transposeSemitones != 0) {
                val originalMidi = step.note.toMidi()
                if (originalMidi >= 0) {
                    val transposedMidi = (originalMidi + transposeSemitones).coerceIn(0, 127)
                    Note.fromMidi(transposedMidi)
                } else {
                    step.note
                }
            } else {
                step.note
            }

            // Schedule the note with resolved effect parameters
            audioEngine.scheduleNote(
                targetFrame = targetFrame,
                note = note,
                instrumentId = step.instrument,
                trackId = trackId,
                volume = params.volume,
                project = project,
                startPointOverride = params.startPoint
            )
            noteScheduled = true
        }

        // Handle KILL effect - schedule kill at the specified frame
        if (params.killAtFrame != null) {
            audioEngine.scheduleKill(params.killAtFrame, trackId)
        }

        // TODO: Handle ARPEGGIO effect (schedule 3 notes)
        // if (params.arpeggioValue != null) { ... }

        // TODO: Handle REPEAT effect (schedule N retriggers)
        // if (params.repeatCount != null) { ... }

        return noteScheduled
    }

    // ═══════════════════════════════════════════════════════════════════════════
    // TIMING HELPERS
    // ═══════════════════════════════════════════════════════════════════════════

    /**
     * Calculate target frame for a step based on tempo
     *
     * @param startFrame Frame when playback started
     * @param stepNumber Which step (0-15 for phrase, 0+ for song)
     * @param tempo Project tempo in BPM
     * @return Target frame number for this step
     */
    fun calculateStepFrame(startFrame: Long, stepNumber: Int, tempo: Int): Long {
        return audioEngine.calculateTargetFrame(startFrame, stepNumber, tempo)
    }

    /**
     * Get current audio frame (for scheduling)
     */
    fun getCurrentFrame(): Long {
        return audioEngine.getCurrentFrame()
    }
}

// ═══════════════════════════════════════════════════════════════════════════
// TYPES
// ═══════════════════════════════════════════════════════════════════════════

/**
 * Playback mode enumeration
 */
enum class PlaybackMode {
    STOPPED,    // No playback
    PHRASE,     // Playing single phrase
    CHAIN,      // Playing chain (sequence of phrases)
    SONG        // Playing full song (8 tracks)
}
