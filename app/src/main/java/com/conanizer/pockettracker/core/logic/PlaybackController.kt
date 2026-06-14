package com.conanizer.pockettracker.core.logic

import com.conanizer.pockettracker.core.data.Note
import com.conanizer.pockettracker.core.data.PhraseStep
import com.conanizer.pockettracker.core.data.Project
import com.conanizer.pockettracker.core.data.ScreenType
import com.conanizer.pockettracker.core.data.TICS_PER_STEP
import com.conanizer.pockettracker.core.data.VolumeUtils
import com.conanizer.pockettracker.core.audio.AudioEngine
import com.conanizer.pockettracker.core.data.Chain
import com.conanizer.pockettracker.core.data.Phrase
import com.conanizer.pockettracker.core.logging.ILogger

// Per-track state for persistent effects (REPEAT, ARPEGGIO, HOP, pitch, groove).
// Effects persist until cancelled — e.g. REPEAT until new note, KILL, or same FX column override.
data class TrackState(
    var lastNote: Note = Note.EMPTY,
    var lastInstrument: Int = 0,
    var lastVolume: Float = 1.0f,
    var lastPan: Float = 0.5f,
    var lastStartPoint: Int = -1,

    var repeatActiveColumn: Int = 0,
    var repeatTicInterval: Int = 0,
    // 0,8=none, 1-7=decrease, 9-F=increase
    var repeatVolRamp: Int = 0,
    var repeatStartFrame: Long = 0,
    var repeatRetrigCount: Int = 0,
    var repeatBaseVolume: Float = 1.0f,

    var arpeggioActiveColumn: Int = 0,
    // high nibble = +semitone1, low nibble = +semitone2
    var arpeggioValue: Int = 0,
    // 0=UP, 1=DOWN, 2=PINGPONG, 3=RANDOM
    var arpeggioMode: Int = 0,
    // default 4 = 3 notes/step at 12 tics
    var arpeggioSpeed: Int = 4,
    var arpeggioStartFrame: Long = 0,

    var hopTargetRow: Int = -1,
    var trackStopped: Boolean = false,

    var pitchBendActive: Boolean = false,
    var vibratoActive: Boolean = false,
    var lastNoteMidi: Int = -1,

    var lastTableOverride: Int = -1,
    var lastTableStartRow: Int = -1,

    var grooveId: Int = 0,
    var grooveStep: Int = 0,

    // 1-indexed: [0]=unused, [1]=FX1, [2]=FX2, [3]=FX3
    var lastColFxType: IntArray = IntArray(4),
    var lastColFxValue: IntArray = IntArray(4)
) {
    fun hasActiveRepeat(): Boolean = repeatActiveColumn > 0 && repeatTicInterval > 0

    fun clearRepeat() {
        repeatActiveColumn = 0
        repeatTicInterval = 0
        repeatVolRamp = 0
        repeatStartFrame = 0
        repeatRetrigCount = 0
        repeatBaseVolume = 1.0f
    }

    /** Check if persistent ARPEGGIO is active */
    fun hasActiveArpeggio(): Boolean = arpeggioActiveColumn > 0 && arpeggioValue > 0

    /** Clear persistent ARPEGGIO (keeps ARC config) */
    fun clearArpeggio() {
        arpeggioActiveColumn = 0
        arpeggioValue = 0
        arpeggioStartFrame = 0
    }

    /** Clear all ARPEGGIO state including ARC config */
    fun clearAllArpeggioState() {
        clearArpeggio()
        arpeggioMode = 0
        arpeggioSpeed = 4
    }

    /** Check if HOP is pending for next phrase */
    fun hasHopPending(): Boolean = hopTargetRow >= 0

    /** Get and consume the HOP target row (returns -1 if no HOP pending) */
    fun consumeHopTarget(): Int {
        val target = hopTargetRow
        hopTargetRow = -1
        return target
    }

    fun clearHopState() {
        hopTargetRow = -1
        trackStopped = false
    }

    fun hasPitchMod(): Boolean = pitchBendActive || vibratoActive

    fun clearPitchMod() {
        pitchBendActive = false
        vibratoActive = false
    }
}

class PlaybackController(
    private val audioEngine: AudioEngine,
    private val effectProcessor: EffectProcessor,
    private val logger: ILogger,
    private val stateObserver: StateObserver,
    private var playbackStartFrame: Long = 0,
    private var currentPhraseId: Int = 0,
    private var currentChainId: Int = 0,
    var instrumentController: InstrumentController? = null,
) {
    private val TAG = "PlaybackController"

    init {
        // Wire SF slot lookup into AudioEngine so scheduleNote() can route SF instruments.
        // The lambda captures instrumentController by reference (var), so it always sees the
        // current value even if instrumentController is set after PlaybackController is created.
        audioEngine.sfSlotProvider = { path -> instrumentController?.sfSlotMap?.get(path) }
    }

    var isPlaying = false
        private set(value) {
            field = value
            stateObserver.onStateChanged()
        }

    var playbackMode = PlaybackMode.STOPPED
        private set(value) {
            field = value
            stateObserver.onStateChanged()
        }

    var playbackCursor = 0
        private set(value) {
            field = value
            stateObserver.onStateChanged()
        }

    private var currentProject: Project? = null
    private var nextFrameToSchedule: Long = 0
    private var nextChainRowToSchedule: Int = 0
    private var nextSongRowToSchedule: Int = 0
    private var nextSongChainRowToSchedule: Int = 0
    // List (not map) so we keep per-iteration start frames. A map would overwrite row 0's entry
    // when it loops around before the cursor has had a chance to read the first occurrence.
    private val chainRowStartFrames = ArrayDeque<Pair<Int, Long>>()
    private val songPositionStartFrames = mutableMapOf<Pair<Int,Int>, Long>()

    // Snapshot taken just BEFORE scheduling a phrase, so notifyDataChanged() can roll back
    // the buffer to the earliest future phrase boundary without disrupting the current phrase.
    private data class SchedulingCheckpoint(
        val frame: Long,
        val chainRow: Int = 0,
        val songRow: Int = 0,
        val songChainRow: Int = 0
    )

    // Ring capped at 4 entries; oldest = earliest unplayed buffered phrase.
    private val schedulingCheckpoints = ArrayDeque<SchedulingCheckpoint>()

    private val trackStates = Array(8) { TrackState() }

    companion object {
        const val LOOKAHEAD_MS = 50L
        const val BUFFER_PHRASES = 2

        // Per-retrigger ADDITIVE volume delta for the REPEAT (Rxy) effect, indexed by ramp nibble:
        // 0,8 = no change · 1-7 = decrease (1 subtle … 7 rapid) · 9-F = increase (9 subtle … F rapid).
        // Single source of truth — used both to apply the live ramp and to preserve ramp volume
        // across RPT→RPT transitions. Keep these two uses in sync by referencing this constant only.
        val REPEAT_RAMP_DELTAS = floatArrayOf(
            0.00f,   // 0: no change
            -0.02f,  // 1: very subtle decrease (~50 retrigs to silence)
            -0.04f,  // 2: subtle decrease (~25 retrigs)
            -0.06f,  // 3: gentle decrease (~17 retrigs)
            -0.10f,  // 4: moderate decrease (~10 retrigs)
            -0.15f,  // 5: noticeable decrease (~7 retrigs)
            -0.20f,  // 6: heavy decrease (~5 retrigs)
            -0.30f,  // 7: rapid decrease (~3 retrigs)
            0.00f,   // 8: no change
            0.02f,   // 9: very subtle increase
            0.04f,   // A: subtle increase
            0.06f,   // B: gentle increase
            0.10f,   // C: moderate increase
            0.15f,   // D: noticeable increase
            0.20f,   // E: heavy increase
            0.30f    // F: rapid increase
        )

        // Per-step scheduling trace. Off in shipped builds: the verbose logger.d() calls in the
        // scheduling path build strings every pass even when logcat hides them (wasted work on the
        // Miyoo Flip during playback). Flip to true for note-by-note debugging. Mirrors C++ AUDIO_TRACE.
        const val TRACE = false
    }

    fun getPlaybackPosition(): PlaybackPosition {
        if (!isPlaying) return PlaybackPosition(0, 0, 0, 0)

        val currentFrame = audioEngine.getCurrentFrame()
        val elapsedFrames = currentFrame - playbackStartFrame
        val sampleRate = audioEngine.getDeviceSampleRate()
        val tempo = currentProject?.tempo ?: 120
        val msPerStep = (60000.0 / tempo / 4.0)
        val framesPerStep = (msPerStep * sampleRate / 1000.0).toLong()
        val framesPerPhrase = framesPerStep * 16

        return when (playbackMode) {
            PlaybackMode.PHRASE -> {
                val step = ((elapsedFrames % framesPerPhrase) / framesPerStep).toInt().coerceIn(0, 15)
                PlaybackPosition(step, 0, 0, 0)
            }
            PlaybackMode.CHAIN -> {
                // Prune entries that are definitely in the past (> 1 phrase ago).
                chainRowStartFrames.removeAll { (_, startFrame) -> currentFrame > startFrame + framesPerPhrase }
                var chainRow = 0
                var phraseStep = 0
                for ((row, startFrame) in chainRowStartFrames) {
                    val framesIntoPhrasePlayback = currentFrame - startFrame
                    if (framesIntoPhrasePlayback >= 0 && framesIntoPhrasePlayback < framesPerPhrase) {
                        chainRow = row
                        phraseStep = (framesIntoPhrasePlayback / framesPerStep).toInt().coerceIn(0, 15)
                        break
                    }
                }
                PlaybackPosition(phraseStep, chainRow, phraseStep, 0)
            }
            PlaybackMode.SONG -> {
                var songRow = 0
                var chainRow = 0
                var phraseStep = 0
                for ((pos, startFrame) in songPositionStartFrames) {
                    val framesIntoPhrasePlayback = currentFrame - startFrame
                    if (framesIntoPhrasePlayback >= 0 && framesIntoPhrasePlayback < framesPerPhrase) {
                        songRow = pos.first
                        chainRow = pos.second
                        phraseStep = (framesIntoPhrasePlayback / framesPerStep).toInt().coerceIn(0, 15)
                        break
                    }
                }
                PlaybackPosition(phraseStep, chainRow, phraseStep, songRow)
            }
            else -> PlaybackPosition(0, 0, 0, 0)
        }
    }

    fun getTrackNotes(): List<Note> = trackStates.map { it.lastNote }

    fun getCurrentPlayingNotes(): List<Note> {
        if (!isPlaying) return List(8) { Note.EMPTY }
        // Read directly from the C++ voice pool — reflects actual playing note
        // including long samples that sustain past the end of their chain.
        return audioEngine.getActiveTrackNotes()
    }

    data class PlaybackPosition(
        val row: Int,
        val chainRow: Int,
        val phraseStep: Int,
        val songRow: Int
    )

    fun togglePlayback(project: Project, currentScreen: ScreenType, currentPhrase: Int = 0, currentChain: Int = 0) {
        if (isPlaying) {
            stop()
        } else {
            when (currentScreen) {
                ScreenType.PHRASE -> playPhrase(project, currentPhrase)
                ScreenType.CHAIN -> playChain(project, currentChain)
                ScreenType.SONG -> playSong(project)
                else -> playPhrase(project, currentPhrase) // Default
            }
        }
        logger.d(TAG, if (isPlaying) "▶️ Playback started" else "⏸️ Playback stopped")
    }

    fun play() {
        isPlaying = true
        logger.d(TAG, "▶️ Playback started")
    }

    fun notifyDataChanged() {
        if (!isPlaying) return
        val currentFrame = audioEngine.getCurrentFrame()

        // Find the earliest unplayed buffered phrase boundary; clear only from there
        // so the currently-playing phrase is untouched.
        val checkpoint = schedulingCheckpoints.firstOrNull { it.frame > currentFrame }
            ?: return

        audioEngine.clearScheduledNotesFrom(checkpoint.frame)
        nextFrameToSchedule = checkpoint.frame

        when (playbackMode) {
            PlaybackMode.CHAIN -> {
                nextChainRowToSchedule = checkpoint.chainRow
            }
            PlaybackMode.SONG -> {
                nextSongRowToSchedule = checkpoint.songRow
                nextSongChainRowToSchedule = checkpoint.songChainRow
            }
            else -> { /* PHRASE: nextFrameToSchedule reset is enough */ }
        }

        while (schedulingCheckpoints.isNotEmpty() && schedulingCheckpoints.last().frame >= checkpoint.frame) {
            schedulingCheckpoints.removeLast()
        }

        logger.d(TAG, "🔄 Data changed — rolled back buffer to frame ${checkpoint.frame} (was at $currentFrame)")
    }

    fun stop() {
        isPlaying = false
        playbackMode = PlaybackMode.STOPPED
        playbackCursor = 0
        audioEngine.clearScheduledNotes()
        audioEngine.stopAll()
        chainRowStartFrames.clear()
        songPositionStartFrames.clear()
        nextSongChainRowToSchedule = 0
        schedulingCheckpoints.clear()
        trackStates.forEach {
            it.clearRepeat()
            it.clearAllArpeggioState()
            it.clearHopState()
            it.grooveId = 0
            it.grooveStep = 0
        }
        logger.d(TAG, "⏹️ Playback stopped")
    }

    private fun saveCheckpoint(cp: SchedulingCheckpoint) {
        schedulingCheckpoints.addLast(cp)
        if (schedulingCheckpoints.size > 4) schedulingCheckpoints.removeFirst()
    }

    fun updatePlaybackBuffer() {
        if (!isPlaying || currentProject == null) return

        val project = currentProject ?: return
        val sampleRate = audioEngine.getDeviceSampleRate()
        val tempo = project.tempo
        val msPerStep = (60000.0 / tempo / 4.0)
        val framesPerStep = (msPerStep * sampleRate / 1000.0).toLong()
        val framesPerPhrase = framesPerStep * 16
        val currentFrame = audioEngine.getCurrentFrame()

        val bufferRemaining = nextFrameToSchedule - currentFrame
        val minBuffer = (BUFFER_PHRASES * framesPerPhrase)

        if (bufferRemaining < minBuffer) {
            when (playbackMode) {
                PlaybackMode.PHRASE -> {
                    val phrase = project.phrases[currentPhraseId]
                    val trackState = trackStates[0]

                    saveCheckpoint(SchedulingCheckpoint(frame = nextFrameToSchedule))

                    val hopStartRow = trackState.consumeHopTarget()
                    val effectiveStartRow = if (hopStartRow >= 0) hopStartRow else 0

                    val result = schedulePhrase(phrase, nextFrameToSchedule, 0, project.getTransposeSemitones(), project, framesPerStep, effectiveStartRow)
                    nextFrameToSchedule += result.framesScheduled
                }

                PlaybackMode.CHAIN -> {
                    val chain = project.chains[currentChainId]
                    val trackState = trackStates[0]

                    if (trackState.trackStopped) {
                        nextChainRowToSchedule = (nextChainRowToSchedule + 1) % 16
                        nextFrameToSchedule += framesPerPhrase
                        return
                    }

                    val nextRow = findNextNonEmptyChainRow(nextChainRowToSchedule, chain)
                    if (nextRow != null) {
                        val phraseId = chain.phraseRefs[nextRow]
                        val transposeSemitones = chain.getTransposeSemitones(nextRow) + project.getTransposeSemitones()

                        saveCheckpoint(SchedulingCheckpoint(
                            frame = nextFrameToSchedule,
                            chainRow = nextRow
                        ))

                        val hopStartRow = trackState.consumeHopTarget()
                        val effectiveStartRow = if (hopStartRow >= 0) hopStartRow else 0

                        val result = schedulePhrase(project.phrases[phraseId], nextFrameToSchedule, 0, transposeSemitones, project, framesPerStep, effectiveStartRow)
                        chainRowStartFrames.addLast(nextRow to nextFrameToSchedule)
                        nextFrameToSchedule += result.framesScheduled

                        nextChainRowToSchedule = (nextRow + 1) % 16
                    } else {
                        stop()
                    }
                }

                PlaybackMode.SONG -> {
                    val song = project.tracks
                    val songLength = (0..7).map { trackId ->
                        song[trackId].chainRefs.size
                    }.maxOrNull() ?: 0

                    if (songLength == 0) {
                        stop()
                    } else {
                        var maxChainLength = 0
                        for (trackId in 0..7) {
                            if (nextSongRowToSchedule < song[trackId].chainRefs.size) {
                                val chainId = song[trackId].chainRefs[nextSongRowToSchedule]
                                if (chainId >= 0 && chainId < 256) {
                                    val chain = project.chains[chainId]
                                    val chainLength = (0..15).count { !chain.isEmpty(it) }
                                    maxChainLength = maxOf(maxChainLength, chainLength)
                                }
                            }
                        }

                        if (maxChainLength == 0) {
                            nextSongRowToSchedule++
                            nextSongChainRowToSchedule = 0
                            trackStates.forEach { it.trackStopped = false }
                            if (nextSongRowToSchedule >= songLength) {
                                nextSongRowToSchedule = 0
                            }
                        } else if (nextSongChainRowToSchedule < maxChainLength) {
                            saveCheckpoint(SchedulingCheckpoint(
                                frame = nextFrameToSchedule,
                                songRow = nextSongRowToSchedule,
                                songChainRow = nextSongChainRowToSchedule
                            ))

                            var scheduledAny = false
                            var maxFramesScheduled = 0L
                            for (trackId in 0..7) {
                                val trackState = trackStates[trackId]

                                if (trackState.trackStopped) {
                                    continue
                                }

                                if (nextSongRowToSchedule < song[trackId].chainRefs.size) {
                                    val chainId = song[trackId].chainRefs[nextSongRowToSchedule]
                                    if (chainId >= 0 && chainId < 256) {
                                        val chain = project.chains[chainId]
                                        if (!chain.isEmpty(nextSongChainRowToSchedule)) {
                                            val phraseId = chain.phraseRefs[nextSongChainRowToSchedule]
                                            val transposeSemitones = chain.getTransposeSemitones(nextSongChainRowToSchedule) + project.getTransposeSemitones()

                                            val hopStartRow = trackState.consumeHopTarget()
                                            val effectiveStartRow = if (hopStartRow >= 0) hopStartRow else 0

                                            val result = schedulePhrase(project.phrases[phraseId], nextFrameToSchedule, trackId, transposeSemitones, project, framesPerStep, effectiveStartRow)
                                            songPositionStartFrames[Pair(nextSongRowToSchedule, nextSongChainRowToSchedule)] = nextFrameToSchedule
                                            scheduledAny = true

                                            if (result.framesScheduled > maxFramesScheduled) {
                                                maxFramesScheduled = result.framesScheduled
                                            }
                                        }
                                    }
                                }
                            }

                            if (scheduledAny) {
                                // Advance by the longest track's scheduled duration to keep all tracks in sync.
                                // Tracks stopped by HOP will just have silence before their next phrase.
                                nextFrameToSchedule += maxFramesScheduled
                                nextSongChainRowToSchedule++
                            } else {
                                // No phrases in this chain row, skip to next
                                nextSongChainRowToSchedule++
                            }
                        } else {
                            nextSongRowToSchedule++
                            nextSongChainRowToSchedule = 0
                            trackStates.forEach { it.trackStopped = false }
                            if (nextSongRowToSchedule >= songLength) {
                                nextSongRowToSchedule = 0
                            }
                        }
                    }
                }

                else -> {}
            }
        }
    }

    fun playPhrase(project: Project, phraseId: Int, loop: Boolean = true) {
        stop()

        currentProject = project
        currentPhraseId = phraseId
        playbackStartFrame = audioEngine.getCurrentFrame()

        if (phraseId !in 0..255) {
            logger.e(TAG, "Invalid phraseId: $phraseId")
            return
        }

        val phrase = project.phrases[phraseId]
        playbackMode = PlaybackMode.PHRASE
        isPlaying = true

        val tempo = project.tempo
        val sampleRate = audioEngine.getDeviceSampleRate()
        val msPerStep = (60000.0 / tempo / 4.0)
        val framesPerStep = (msPerStep * sampleRate / 1000.0).toLong()

        logger.d(TAG, "▶️ Playing phrase $phraseId (tempo: $tempo BPM)")

        nextFrameToSchedule = playbackStartFrame
        val result = schedulePhrase(phrase, playbackStartFrame, 0, project.getTransposeSemitones(), project, framesPerStep)
        nextFrameToSchedule += result.framesScheduled

        logger.d(TAG, "✅ Phrase playback initialized")
    }

    data class SchedulePhraseResult(
        val rowsScheduled: Int,
        val hopTriggered: Boolean,
        val trackStopped: Boolean,
        val framesScheduled: Long = 0L
    )

    private fun schedulePhrase(
        phrase: Phrase,
        startFrame: Long,
        trackId: Int,
        transposeSemitones: Int,
        project: Project,
        framesPerStep: Long,
        startRow: Int = 0
    ): SchedulePhraseResult {
        var scheduledNotes = 0
        var rowsScheduled = 0
        val trackState = trackStates[trackId.coerceIn(0, 7)]

        if (trackState.trackStopped) {
            if (TRACE) logger.d(TAG, "  Track $trackId stopped by HOP FF, skipping phrase")
            return SchedulePhraseResult(0, hopTriggered = false, trackStopped = true, framesScheduled = 0L)
        }

        val framesPerTic = framesPerStep / TICS_PER_STEP
        var localGrooveStep = trackState.grooveStep
        var anyGrooveActive = false

        val effectiveStartRow = startRow.coerceIn(0, 15)
        var frameOffset = 0L

        for (stepIndex in effectiveStartRow until 16) {
            val step = phrase.steps[stepIndex]

            // Pre-scan GRV before computing step duration so the new groove takes effect
            // immediately on the triggering step, not deferred to the next phrase.
            // scheduleStepWithEffects also processes GRV (idempotent — same result).
            for (fxSlot in 1..3) {
                val (fxType, fxValue) = step.fx(fxSlot)
                if (fxType == EffectProcessor.FX_GRV) {
                    trackState.grooveId = fxValue
                    localGrooveStep = 0  // New groove always starts at slot 0
                    break
                }
            }

            // Fall back to exact framesPerStep (no groove) to avoid rounding drift.
            val currentGroove = project.grooves[trackState.grooveId.coerceIn(0, 255)]
            val currentGrooveActive = currentGroove.activeLength() > 0

            val stepDuration = if (currentGrooveActive) {
                anyGrooveActive = true
                val stepTics = currentGroove.getTicksForStep(localGrooveStep)
                framesPerTic * stepTics  // 0 tics = skip step
            } else {
                framesPerStep
            }

            if (stepDuration == 0L) {
                rowsScheduled++
                localGrooveStep++
                continue
            }

            val targetFrame = startFrame + frameOffset

            val stepResult = scheduleStepWithEffects(
                step = step,
                targetFrame = targetFrame,
                stepDuration = stepDuration,
                trackId = trackId,
                transposeSemitones = transposeSemitones,
                project = project,
                trackState = trackState,
                stepIndex = stepIndex
            )

            rowsScheduled++
            frameOffset += stepDuration
            if (currentGrooveActive) localGrooveStep++

            if (stepResult.noteScheduled) {
                scheduledNotes++
            }

            // Check if HOP was triggered - stop scheduling remaining rows
            if (stepResult.hopTriggered) {
                // Persist groove position for the next phrase (if groove was active at any point)
                if (anyGrooveActive) trackState.grooveStep = localGrooveStep
                if (trackState.trackStopped) {
                    if (TRACE) logger.d(TAG, "  HOP FF at row $stepIndex: track $trackId stopped, scheduled $rowsScheduled rows")
                } else {
                    if (TRACE) logger.d(TAG, "  HOP at row $stepIndex: jumping to row ${trackState.hopTargetRow}, scheduled $rowsScheduled rows")
                }
                return SchedulePhraseResult(rowsScheduled, hopTriggered = true, trackStopped = trackState.trackStopped, framesScheduled = frameOffset)
            }
        }

        if (anyGrooveActive) trackState.grooveStep = localGrooveStep

        if (scheduledNotes > 0) {
            if (effectiveStartRow > 0) {
                if (TRACE) logger.d(TAG, "  Scheduled $scheduledNotes notes (starting from row $effectiveStartRow due to HOP)")
            } else {
                if (TRACE) logger.d(TAG, "  Scheduled $scheduledNotes notes")
            }
        }

        return SchedulePhraseResult(rowsScheduled, hopTriggered = false, trackStopped = false, framesScheduled = frameOffset)
    }

    fun playChain(project: Project, chainId: Int, loop: Boolean = true) {
        stop()

        currentProject = project
        currentChainId = chainId
        playbackStartFrame = audioEngine.getCurrentFrame()

        if (chainId !in 0..255) {
            logger.e(TAG, "Invalid chainId: $chainId")
            return
        }

        val chain = project.chains[chainId]
        playbackMode = PlaybackMode.CHAIN
        isPlaying = true

        val tempo = project.tempo
        val sampleRate = audioEngine.getDeviceSampleRate()
        val msPerStep = (60000.0 / tempo / 4.0)
        val framesPerStep = (msPerStep * sampleRate / 1000.0).toLong()

        logger.d(TAG, "▶️ Playing chain $chainId (tempo: $tempo BPM)")

        nextFrameToSchedule = playbackStartFrame
        nextChainRowToSchedule = 0
        chainRowStartFrames.clear()

        val firstRow = findNextNonEmptyChainRow(0, chain)
        if (firstRow != null) {
            val phraseId = chain.phraseRefs[firstRow]
            val transposeSemitones = chain.getTransposeSemitones(firstRow)
            val result = schedulePhrase(project.phrases[phraseId], playbackStartFrame, 0, transposeSemitones + project.getTransposeSemitones(), project, framesPerStep)
            chainRowStartFrames.addLast(firstRow to playbackStartFrame)
            nextFrameToSchedule += result.framesScheduled
            nextChainRowToSchedule = firstRow + 1
        }

        logger.d(TAG, "✅ Chain playback initialized")
    }

    private fun findNextNonEmptyChainRow(startRow: Int, chain: Chain): Int? {
        var row = startRow
        var attempts = 0
        while (chain.isEmpty(row) && attempts < 16) {
            row = (row + 1) % 16
            attempts++
        }
        return if (attempts >= 16) null else row
    }

    fun playSong(project: Project, startRow: Int = 0, loop: Boolean = true) {
        stop()

        currentProject = project
        playbackStartFrame = audioEngine.getCurrentFrame()
        playbackMode = PlaybackMode.SONG
        isPlaying = true

        val tempo = project.tempo
        val sampleRate = audioEngine.getDeviceSampleRate()
        val msPerStep = (60000.0 / tempo / 4.0)
        val framesPerStep = (msPerStep * sampleRate / 1000.0).toLong()

        logger.d(TAG, "▶️ Playing song from row $startRow (tempo: $tempo BPM)")

        nextFrameToSchedule = playbackStartFrame
        nextSongRowToSchedule = startRow
        nextSongChainRowToSchedule = 0
        songPositionStartFrames.clear()

        logger.d(TAG, "✅ Song playback initialized")
    }

    // Uses the same schedulePhrase logic as live playback — groove, HOP, pitch, etc. all identical.
    fun scheduleSongForRender(project: Project, startRow: Int, endRow: Int): Long =
        scheduleSongRowRange(project, startRow, endRow)

    fun scheduleSelectionForRender(
        project: Project,
        startRow: Int,
        endRow: Int,
        selectedTrackIds: Set<Int>
    ): Long = scheduleSongRowRange(project, startRow, endRow, trackFilter = selectedTrackIds)

    // Shared render-path scheduler. trackFilter = null schedules all tracks; non-null restricts to
    // the given set. Muted tracks are always skipped. Does not update live-playback cursor state.
    private fun scheduleSongRowRange(
        project: Project,
        startRow: Int,
        endRow: Int,
        trackFilter: Set<Int>? = null
    ): Long {
        for (i in trackStates.indices) trackStates[i] = TrackState()

        val sampleRate = audioEngine.getDeviceSampleRate()
        val framesPerStep = (60000.0 / project.tempo / 4.0 * sampleRate / 1000.0).toLong()

        var currentFrame = 0L

        for (songRow in startRow..endRow) {
            trackStates.forEach { it.trackStopped = false }

            var maxChainLength = 0
            for (trackId in 0..7) {
                if (trackFilter != null && trackId !in trackFilter) continue
                val track = project.tracks[trackId]
                if (track.mute) continue
                if (songRow >= track.chainRefs.size) continue
                val chainId = track.chainRefs[songRow]
                if (chainId < 0 || chainId >= 256) continue
                val length = (0..15).count { !project.chains[chainId].isEmpty(it) }
                if (length > maxChainLength) maxChainLength = length
            }

            for (chainRow in 0 until maxChainLength) {
                var maxFramesScheduled = 0L
                var scheduledAny = false

                for (trackId in 0..7) {
                    if (trackFilter != null && trackId !in trackFilter) continue
                    val trackState = trackStates[trackId]
                    if (trackState.trackStopped) continue
                    val track = project.tracks[trackId]
                    if (track.mute) continue
                    if (songRow >= track.chainRefs.size) continue
                    val chainId = track.chainRefs[songRow]
                    if (chainId < 0 || chainId >= 256) continue
                    val chain = project.chains[chainId]
                    if (chain.isEmpty(chainRow)) continue
                    val phraseId = chain.phraseRefs[chainRow]
                    if (phraseId < 0 || phraseId >= 256) continue

                    val transposeSemitones = chain.getTransposeSemitones(chainRow) + project.getTransposeSemitones()
                    val hopStartRow = trackState.consumeHopTarget()
                    val effectiveStartRow = if (hopStartRow >= 0) hopStartRow else 0

                    val result = schedulePhrase(
                        project.phrases[phraseId], currentFrame, trackId,
                        transposeSemitones, project, framesPerStep, effectiveStartRow
                    )
                    scheduledAny = true
                    if (result.framesScheduled > maxFramesScheduled)
                        maxFramesScheduled = result.framesScheduled
                }

                if (scheduledAny) currentFrame += maxFramesScheduled
            }
        }

        return currentFrame
    }

    data class ScheduleStepResult(
        val noteScheduled: Boolean,
        val hopTriggered: Boolean
    )

    /**
     * CHA (chance gate) + RND/RNL (randomize) preprocessing, evaluated BEFORE effect resolution.
     * Returns the possibly-modified step and whether the note trigger was gated out by CHA.
     * Extracted from scheduleStepWithEffects so that method stays focused on scheduling.
     *
     * CHA xy: probability gate — x=probability (0=never, F=always), y=target (0=note, 1-3=FX slot).
     * RND xy: recall the previously-active FX in the same column with a randomized value [x0..yF].
     * RNL xy: randomize the column to the LEFT (FX2→FX1, FX3→FX2); on FX1, randomize note+instrument.
     */
    private fun applyChanceAndRandomize(step: PhraseStep, trackState: TrackState): Pair<PhraseStep, Boolean> {
        val hasNote = !step.isEmpty()
        var skipNote = false
        var effectiveStep = step
        for (slot in 1..3) {
            val (fxType, fxValue) = step.fx(slot)
            // CHA 00 is a valid value (probability 0 = never), so gate on the type alone —
            // not `fxValue > 0`, which silently skipped CHA 00 and let the note always play.
            if (fxType == EffectProcessor.FX_CHA) {
                val probability = (fxValue shr 4) and 0x0F
                val target = fxValue and 0x0F
                val roll = kotlin.random.Random.nextInt(15)  // 0-14
                val passed = roll < probability  // probability F (15) always passes, 0 never passes

                if (!passed) {
                    val targetName = if (target == 0) "note" else "FX$target"
                    if (TRACE) logger.d(TAG, "🎲 CHA: probability=$probability/15, roll=$roll → BLOCKED $targetName")
                    if (target == 0) {
                        skipNote = true
                    } else if (target in 1..3) {
                        // Zero out the targeted FX slot before resolution
                        effectiveStep = effectiveStep.copy().also { it.setFx(target, 0x00, 0x00) }
                    }
                } else {
                    val targetName = if (target == 0) "note" else "FX$target"
                    if (TRACE) logger.d(TAG, "🎲 CHA: probability=$probability/15, roll=$roll → PASSED $targetName")
                }
            }
        }

        for (slot in 1..3) {
            val (fxType, fxValue) = effectiveStep.fx(slot)

            val minNibble = (fxValue shr 4) and 0x0F
            val maxNibble = fxValue and 0x0F

            if (fxType == EffectProcessor.FX_RND) {
                // RND: temporal — recall previously active FX in this column
                val prevType = trackState.lastColFxType[slot]
                val prevValue = trackState.lastColFxValue[slot]
                if (prevType == 0x00) continue  // No previous FX to randomize

                val minVal = minNibble shl 4
                val maxVal = (maxNibble shl 4) or 0x0F
                val randomValue = if (minVal <= maxVal) {
                    kotlin.random.Random.nextInt(minVal, maxVal + 1)
                } else {
                    kotlin.random.Random.nextInt(maxVal, minVal + 1)
                }

                // Replace this column with the previous FX type + random value
                effectiveStep = effectiveStep.copy().also { it.setFx(slot, prevType, randomValue) }
                if (TRACE) logger.d(TAG, "🎲 RND: FX$slot recalled ${EffectProcessor.effectName(prevType)} → " +
                        "0x${randomValue.toString(16).uppercase().padStart(2, '0')} " +
                        "(was 0x${prevValue.toString(16).uppercase().padStart(2, '0')}, " +
                        "range ${minNibble.toString(16).uppercase()}0-${maxNibble.toString(16).uppercase()}F)")

            } else if (fxType == EffectProcessor.FX_RNL) {
                // RNL: spatial — randomize the column to the left
                if (slot == 1) {
                    // Special case: FX1 → randomize note and instrument
                    if (hasNote) {
                        val noteMidi = step.note.toMidi()
                        if (noteMidi >= 0) {
                            // X = note range (semitones ±), Y = instrument range (±)
                            val noteRange = minNibble  // 0=no change, F=±15 semitones
                            val instRange = maxNibble  // 0=no change, F=±15 instruments
                            val noteOffset = if (noteRange > 0) {
                                kotlin.random.Random.nextInt(-noteRange, noteRange + 1)
                            } else 0
                            val instOffset = if (instRange > 0) {
                                kotlin.random.Random.nextInt(-instRange, instRange + 1)
                            } else 0

                            effectiveStep = effectiveStep.copy(
                                note = Note.fromMidi((noteMidi + noteOffset).coerceIn(0, 127)),
                                instrument = (step.instrument + instOffset).coerceIn(0, 255)
                            )
                            if (TRACE) logger.d(TAG, "🎲 RNL FX1: note ${step.note}→${effectiveStep.note} " +
                                    "(±$noteRange), inst ${step.instrument.toString(16).uppercase().padStart(2, '0')}" +
                                    "→${effectiveStep.instrument.toString(16).uppercase().padStart(2, '0')} (±$instRange)")
                        }
                    }
                } else {
                    // FX2→FX1, FX3→FX2: randomize the left column's value
                    val targetSlot = slot - 1
                    val minVal = minNibble shl 4
                    val maxVal = (maxNibble shl 4) or 0x0F
                    val randomValue = if (minVal <= maxVal) {
                        kotlin.random.Random.nextInt(minVal, maxVal + 1)
                    } else {
                        kotlin.random.Random.nextInt(maxVal, minVal + 1)
                    }

                    effectiveStep = effectiveStep.copy().also { it.setFxValue(targetSlot, randomValue) }
                    val targetType = EffectProcessor.effectName(effectiveStep.fxType(targetSlot))
                    if (TRACE) logger.d(TAG, "🎲 RNL: FX$targetSlot ($targetType) value → " +
                            "0x${randomValue.toString(16).uppercase().padStart(2, '0')} " +
                            "(range ${minNibble.toString(16).uppercase()}0-${maxNibble.toString(16).uppercase()}F)")
                }
            }
        }
        return effectiveStep to skipNote
    }

    fun scheduleStepWithEffects(
        step: PhraseStep,
        targetFrame: Long,
        stepDuration: Long,
        trackId: Int,
        transposeSemitones: Int,
        project: Project,
        trackState: TrackState = trackStates[trackId.coerceIn(0, 7)],
        stepIndex: Int = 0
    ): ScheduleStepResult {
        // ═══════════════════════════════════════════════════════════════════════════
        // STEP 1: Check cancellation conditions for persistent REPEAT and ARPEGGIO
        // ═══════════════════════════════════════════════════════════════════════════

        // Check if KILL effect is present in ANY column → clears persistent REPEAT and ARPEGGIO
        val hasKill = step.fx1Type == EffectProcessor.FX_KILL ||
                step.fx2Type == EffectProcessor.FX_KILL ||
                step.fx3Type == EffectProcessor.FX_KILL

        if (hasKill) {
            trackState.clearRepeat()
            trackState.clearArpeggio()
            if (TRACE) logger.d(TAG, "🔪 KILL detected → persistent REPEAT and ARPEGGIO cancelled")
        }

        // Check if step has a note → clears persistent REPEAT and ARPEGGIO (new note triggers)
        val hasNote = !step.isEmpty()
        if (hasNote) {
            trackState.clearRepeat()
            trackState.clearArpeggio()
            // Note: We don't log here because new effects might be set below
        }

        // Save current ramp volume before potential clearRepeat() (for RPT-to-RPT transitions)
        val savedRampVolume = if (trackState.hasActiveRepeat() && trackState.repeatRetrigCount > 0) {
            // Last accumulated ramp volume (preserved across RPT→RPT transitions)
            val oldDelta = REPEAT_RAMP_DELTAS[trackState.repeatVolRamp.coerceIn(0, 15)]
            (trackState.repeatBaseVolume + trackState.repeatRetrigCount * oldDelta).coerceIn(0f, 1f)
        } else {
            -1f // No active ramp to preserve
        }

        // Check if persistent REPEAT's column has any effect → clears REPEAT
        // (Any effect in the same column overrides the persistent REPEAT)
        if (trackState.hasActiveRepeat()) {
            if (step.fxType(trackState.repeatActiveColumn) != EffectProcessor.FX_NONE) {
                if (TRACE) logger.d(TAG, "🔄 FX in column ${trackState.repeatActiveColumn} → persistent REPEAT cancelled")
                trackState.clearRepeat()
            }
        }

        // Check if persistent ARPEGGIO's column has any effect → clears ARPEGGIO
        // (Any effect in the same column overrides the persistent ARPEGGIO)
        if (trackState.hasActiveArpeggio()) {
            if (step.fxType(trackState.arpeggioActiveColumn) != EffectProcessor.FX_NONE) {
                if (TRACE) logger.d(TAG, "🔄 FX in column ${trackState.arpeggioActiveColumn} → persistent ARPEGGIO cancelled")
                trackState.clearArpeggio()
            }
        }

        // ═══════════════════════════════════════════════════════════════════════════
        // STEP 2: Handle CHA (Chance), resolve effects, and schedule note
        // ═══════════════════════════════════════════════════════════════════════════

        // CHA gate + RND/RNL randomize, evaluated before effect resolution (may gate the note or
        // overwrite an FX slot / the note+instrument). See applyChanceAndRandomize().
        val (effectiveStep, skipNote) = applyChanceAndRandomize(step, trackState)

        val defaultVolume = effectiveStep.volume / 255.0f
        val params = effectProcessor.resolveStepParams(effectiveStep, targetFrame, defaultVolume)

        // Instrument and phrase volumes are passed separately to C++ as mod sources.
        // C++ multiplies them via the fixed VOL route: TABLE_VOL × phraseVol × instrVol.
        // Track × master are applied in the C++ mix loop in real-time.
        val instrument = project.instruments[effectiveStep.instrument]
        val instrVol  = VolumeUtils.hexToFloat(instrument.volume)
        val phraseVol = params.volume  // already 0.0–1.0 from resolveStepParams

        // Get instrument pan (hex 0x00-0xFF → float 0.0-1.0)
        val instrumentPan = VolumeUtils.hexToFloat(instrument.pan)

        // ═══════════════════════════════════════════════════════════════════════════
        // STEP 2.1: Apply DEL (Delay) effect - offset the target frame
        // ═══════════════════════════════════════════════════════════════════════════
        val delayTicks = params.delayTicks ?: 0
        val effectiveTargetFrame = if (delayTicks > 0) {
            val framesPerTic = stepDuration / TICS_PER_STEP
            val delayFrames = delayTicks * framesPerTic
            if (TRACE) logger.d(TAG, "⏳ DEL: delaying note by $delayTicks ticks ($delayFrames frames)")
            targetFrame + delayFrames
        } else {
            targetFrame
        }

        // ═══════════════════════════════════════════════════════════════════════════
        // STEP 2.2: Handle TBL (Table Override) and THO (Table Hop) effects
        // ═══════════════════════════════════════════════════════════════════════════

        // TBL XX: Override table ID for this note (and subsequent retrigs)
        val tableIdOverride = if (params.tableOverride != null && params.tableOverride >= 0) {
            trackState.lastTableOverride = params.tableOverride
            if (TRACE) logger.d(TAG, "📋 TBL: Override table to ${params.tableOverride.toString(16).uppercase().padStart(2, '0')}")
            params.tableOverride
        } else if (hasNote) {
            // New note without TBL: reset to instrument default
            trackState.lastTableOverride = -1
            -1
        } else {
            // Empty step: use last override for retrig/arp continuity
            trackState.lastTableOverride
        }

        // THO XX: Table hop (jump table to row Y)
        // With note: sets table start row for the new voice
        // Without note: jumps the active voice's table to target row
        val tableStartRow = if (params.tableHopTarget != null) {
            val targetRow = params.tableHopTarget % 16  // 0-15
            trackState.lastTableStartRow = targetRow
            if (!hasNote) {
                // No note: jump the active voice's table row directly
                audioEngine.setVoiceTableRow(trackId, targetRow)
                if (TRACE) logger.d(TAG, "📋 THO: Jumped active voice table to row $targetRow (no note)")
            } else {
                if (TRACE) logger.d(TAG, "📋 THO: Will start table at row $targetRow (with note)")
            }
            targetRow
        } else {
            -1  // No THO, use default
        }

        // GRV XX: Groove assignment (stored for timing in phrase scheduler)
        if (params.grooveId != null) {
            trackState.grooveId = params.grooveId
            trackState.grooveStep = 0  // Reset groove position
            if (params.grooveId == 0) {
                if (TRACE) logger.d(TAG, "🥁 GRV 00: Disabled groove (default timing)")
            } else {
                if (TRACE) logger.d(TAG, "🥁 GRV: Assigned groove ${params.grooveId.toString(16).uppercase().padStart(2, '0')}")
            }
        }

        var noteScheduled = false
        if (hasNote && !skipNote) {
            // Apply transposition if needed (use effectiveStep for RNL note randomization)
            val note = if (transposeSemitones != 0) {
                val originalMidi = effectiveStep.note.toMidi()
                if (originalMidi >= 0) {
                    val transposedMidi = (originalMidi + transposeSemitones).coerceIn(0, 127)
                    Note.fromMidi(transposedMidi)
                } else {
                    effectiveStep.note
                }
            } else {
                effectiveStep.note
            }

            // Save previous MIDI for PSL calculation BEFORE updating track state
            val previousMidi = trackState.lastNoteMidi

            // ═══════════════════════════════════════════════════════════════════════════
            // Calculate pitch mod params BEFORE scheduling (they travel with the note)
            // ═══════════════════════════════════════════════════════════════════════════
            var pslInitialOffset = 0f
            var pslDuration = 0f
            var pbnRate = 0f
            var vibratoSpeed = 0f
            var vibratoDepth = 0f

            // PSL: Calculate portamento from previous note
            if (params.pslDuration != null && params.pslDuration > 0 && previousMidi >= 0) {
                val currentMidi = note.toMidi()
                if (currentMidi >= 0 && previousMidi != currentMidi) {
                    pslInitialOffset = (previousMidi - currentMidi).toFloat()
                    pslDuration = params.pslDuration.toFloat()
                    if (TRACE) logger.d(TAG, "🎵 PSL: Portamento from ${Note.fromMidi(previousMidi)} to $note " +
                            "(offset=$pslInitialOffset) over ${params.pslDuration} ticks")
                }
            }

            // PBN: Calculate pitch bend rate (non-zero = start bend)
            if (params.pbnValue != null && params.pbnValue != 0) {
                pbnRate = if (params.pbnValue < 0x80) {
                    params.pbnValue / 16f  // UP
                } else {
                    -((params.pbnValue and 0x7F) / 16f)  // DOWN
                }
                trackState.pitchBendActive = true
                val direction = if (params.pbnValue < 0x80) "UP" else "DOWN"
                if (TRACE) logger.d(TAG, "🎵 PBN ${params.pbnValue.toString(16).uppercase().padStart(2, '0')}: " +
                        "Bend $direction at $pbnRate semitones/step")
            }

            // PVB: Calculate standard vibrato
            if (params.pvbValue != null && params.pvbValue != 0) {
                val speedNibble = (params.pvbValue shr 4) and 0x0F
                val depthNibble = params.pvbValue and 0x0F
                vibratoSpeed = 2f + speedNibble * 0.5f
                vibratoDepth = depthNibble * 0.125f
                trackState.vibratoActive = true
                if (TRACE) logger.d(TAG, "🎵 PVB ${params.pvbValue.toString(16).uppercase().padStart(2, '0')}: " +
                        "Vibrato speed=${vibratoSpeed}Hz, depth=$vibratoDepth semitones")
            }

            // PVX: Calculate extreme vibrato (2x speed, 4x depth)
            if (params.pvxValue != null && params.pvxValue != 0) {
                val speedNibble = (params.pvxValue shr 4) and 0x0F
                val depthNibble = params.pvxValue and 0x0F
                vibratoSpeed = (2f + speedNibble * 0.5f) * 2f  // 2x speed
                vibratoDepth = depthNibble * 0.125f * 4f       // 4x depth
                trackState.vibratoActive = true
                if (TRACE) logger.d(TAG, "🎵 PVX ${params.pvxValue.toString(16).uppercase().padStart(2, '0')}: " +
                        "EXTREME vibrato speed=${vibratoSpeed}Hz, depth=$vibratoDepth semitones")
            }

            // Debug: Log the full volume chain so we can verify what reaches the audio engine
            if (TRACE) logger.d(TAG, "🔊 Volume chain: instrVol=${"%.4f".format(instrVol)}" +
                    " phraseVol=${"%.4f".format(phraseVol)}" +
                    " (C++ multiplies: TABLE_VOL × phraseVol × instrVol)")

            // Schedule the note — unified path handles both SAMPLER and SOUNDFONT.
            // AudioEngine.scheduleNote() routes to backend.scheduleSoundfontNote() for SF
            // instruments (via sfSlotProvider), so arpeggio/repeat retriggers work for both.
            audioEngine.scheduleNote(
                targetFrame = effectiveTargetFrame,
                note = note,
                instrumentId = effectiveStep.instrument,
                trackId = trackId,
                volume = instrVol,
                phraseVol = phraseVol,
                pan = instrumentPan,
                project = project,
                startPointOverride = params.startPoint,
                pslInitialOffset = pslInitialOffset,
                pslDuration = pslDuration,
                pbnRate = pbnRate,
                vibratoSpeed = vibratoSpeed,
                vibratoDepth = vibratoDepth,
                tableIdOverride = tableIdOverride,
                tableStartRow = tableStartRow,
                transposeSemitones = transposeSemitones,
                pitSemitones = params.pitSemitones ?: 0,
                sliIndex = params.sliIndex ?: -1
            )
            noteScheduled = true

            // Update track state with this note (for persistent REPEAT retrigger)
            trackState.lastNote = note
            trackState.lastInstrument = effectiveStep.instrument
            trackState.lastVolume = instrVol * phraseVol  // Combined for REPEAT retrigger
            trackState.lastStartPoint = params.startPoint
            trackState.lastPan = instrumentPan
            trackState.lastNoteMidi = note.toMidi()

            // Clear old pitch mod tracking state (new note resets)
            if (trackState.hasPitchMod() && pbnRate == 0f && vibratoDepth == 0f) {
                trackState.clearPitchMod()
            }
        }


        // Handle KILL effect - schedule kill at the specified frame (offset by DEL if present)
        if (params.killAtFrame != null) {
            val killFrame = if (delayTicks > 0) {
                val framesPerTic = stepDuration / TICS_PER_STEP
                params.killAtFrame + delayTicks * framesPerTic
            } else {
                params.killAtFrame
            }
            // scheduleNoteOff (soft kill) at the sample-accurate kill frame.
            // triggerNoteOff transitions ADSR to Release so the release tail plays after K00.
            // For AHD / DRUM / no-mod voices it hard-stops the voice (no release to play).
            audioEngine.scheduleNoteOff(killFrame, trackId)
            trackState.clearPitchMod()
        }

        // ═══════════════════════════════════════════════════════════════════════════
        // STEP 2.4: Handle Pitch Effects on steps WITHOUT notes (mid-note changes)
        // ═══════════════════════════════════════════════════════════════════════════
        // PBN/PVB/PVX on empty steps modify the currently playing voice
        // This allows changing pitch mod mid-note without retriggering

        if (!hasNote) {
            val tempo = currentProject?.tempo ?: 120

            // Vxx on empty step: schedule phraseVol update at the step's target frame so the
            // change fires sample-accurately (PlaybackController runs ahead of the audio clock).
            if (params.volumeFromVxx) {
                audioEngine.scheduleTrackPhraseVol(effectiveTargetFrame, trackId, phraseVol)
                if (TRACE) logger.d(TAG, "🔊 Vxx on empty step: track=$trackId phraseVol=$phraseVol at frame=$effectiveTargetFrame")
            }

            // Handle PBN (Pitch Bend) - modify currently playing voice
            if (params.pbnValue != null) {
                if (params.pbnValue == 0) {
                    // PBN 00: Stop pitch bend
                    audioEngine.setPitchBend(trackId, 0f, tempo)
                    trackState.pitchBendActive = false
                    if (TRACE) logger.d(TAG, "🎵 PBN 00: Pitch bend stopped (mid-note)")
                } else {
                    val semitonesPerTick = if (params.pbnValue < 0x80) {
                        params.pbnValue / 16f
                    } else {
                        -((params.pbnValue and 0x7F) / 16f)
                    }
                    audioEngine.setPitchBend(trackId, semitonesPerTick, tempo)
                    trackState.pitchBendActive = true
                    val direction = if (params.pbnValue < 0x80) "UP" else "DOWN"
                    if (TRACE) logger.d(TAG, "🎵 PBN ${params.pbnValue.toString(16).uppercase().padStart(2, '0')}: " +
                            "Bend $direction (mid-note)")
                }
            }

            // Handle PVB (Vibrato) - modify currently playing voice
            if (params.pvbValue != null) {
                if (params.pvbValue == 0) {
                    audioEngine.setVibrato(trackId, 0f, 0f)
                    trackState.vibratoActive = false
                    if (TRACE) logger.d(TAG, "🎵 PVB 00: Vibrato stopped (mid-note)")
                } else {
                    val speedNibble = (params.pvbValue shr 4) and 0x0F
                    val depthNibble = params.pvbValue and 0x0F
                    val speed = 2f + speedNibble * 0.5f
                    val depth = depthNibble * 0.125f
                    audioEngine.setVibrato(trackId, speed, depth)
                    trackState.vibratoActive = true
                    if (TRACE) logger.d(TAG, "🎵 PVB ${params.pvbValue.toString(16).uppercase().padStart(2, '0')}: " +
                            "Vibrato (mid-note)")
                }
            }

            // Handle PVX (Extreme Vibrato) - modify currently playing voice
            if (params.pvxValue != null) {
                if (params.pvxValue == 0) {
                    audioEngine.setVibrato(trackId, 0f, 0f)
                    trackState.vibratoActive = false
                    if (TRACE) logger.d(TAG, "🎵 PVX 00: Extreme vibrato stopped (mid-note)")
                } else {
                    val speedNibble = (params.pvxValue shr 4) and 0x0F
                    val depthNibble = params.pvxValue and 0x0F
                    val speed = (2f + speedNibble * 0.5f) * 2f
                    val depth = depthNibble * 0.125f * 4f
                    audioEngine.setVibrato(trackId, speed, depth)
                    trackState.vibratoActive = true
                    if (TRACE) logger.d(TAG, "🎵 PVX ${params.pvxValue.toString(16).uppercase().padStart(2, '0')}: " +
                            "EXTREME vibrato (mid-note)")
                }
            }
        }

        // ═══════════════════════════════════════════════════════════════════════════
        // STEP 2.5: Handle HOP effect
        // ═══════════════════════════════════════════════════════════════════════════
        // HOP in phrase context:
        // - HOPFF (0xFF) = Stop track playback (until new chain starts)
        // - HOP XY = Jump to row Y, ending current phrase immediately
        //
        // This enables odd time signatures:
        // - 3/4 time: HOP 00 at row 11 = play 12 rows (0-11)
        // - 5/4 time: Phrase 0 full (16) + Phrase 1 with HOP 00 at row 3 = 20 rows
        var hopTriggered = false
        if (params.hopValue != null) {
            hopTriggered = true  // HOP always ends the current phrase
            if (params.hopValue == 0xFF) {
                // HOPFF: Stop this track
                trackState.trackStopped = true
                if (TRACE) logger.d(TAG, "🦘 HOP FF: Track $trackId stopped at step $stepIndex")
            } else {
                // HOP XY: Set target row for next phrase (low nibble = Y)
                val targetRow = params.hopValue and 0x0F
                trackState.hopTargetRow = targetRow
                if (TRACE) logger.d(TAG, "🦘 HOP: Track $trackId jumping at step $stepIndex, next phrase starts at row $targetRow")
            }
        }

        // ═══════════════════════════════════════════════════════════════════════════
        // STEP 3: Handle REPEAT effect (new or persistent)
        // M8-style RXY format:
        //   R00 = cancel repeat
        //   RX0 (Y=0): retrig every X ticks, no vol ramp
        //   RXY (Y!=0): retrig every Y ticks, vol ramp X
        // ═══════════════════════════════════════════════════════════════════════════

        // Find which column has REPEAT effect (if any)
        // XY parsing is done centrally by EffectProcessor.resolveStepParams()
        var newRepeatColumn = 0
        if (effectiveStep.fx1Type == EffectProcessor.FX_REPEAT && effectiveStep.fx1Value > 0) {
            newRepeatColumn = 1
        } else if (effectiveStep.fx2Type == EffectProcessor.FX_REPEAT && effectiveStep.fx2Value > 0) {
            newRepeatColumn = 2
        } else if (effectiveStep.fx3Type == EffectProcessor.FX_REPEAT && effectiveStep.fx3Value > 0) {
            newRepeatColumn = 3
        }
        // Use centralized XY parsing from EffectProcessor
        val newRepeatTicInterval = params.repeatCount ?: 0
        val newRepeatVolRamp = params.repeatVolRamp ?: 0

        // If new REPEAT is set, update persistent state
        if (newRepeatColumn > 0) {
            trackState.repeatActiveColumn = newRepeatColumn
            trackState.repeatTicInterval = newRepeatTicInterval
            trackState.repeatVolRamp = newRepeatVolRamp
            trackState.repeatStartFrame = targetFrame
            trackState.repeatRetrigCount = 0  // Reset counter for new ramp parameters

            // Set base volume for the new ramp
            trackState.repeatBaseVolume = when {
                hasNote -> instrVol * phraseVol  // New note: fresh start at combined note volume
                savedRampVolume >= 0f -> savedRampVolume  // RPT-to-RPT on empty step: continue from last ramp position
                else -> trackState.lastVolume  // Fallback: use last played note volume
            }

            val rampDesc = when {
                newRepeatVolRamp == 0 || newRepeatVolRamp == 8 -> ""
                newRepeatVolRamp in 1..7 -> ", vol decrease $newRepeatVolRamp"
                else -> ", vol increase ${newRepeatVolRamp - 8}"
            }
            if (TRACE) logger.d(TAG, "🔁 REPEAT: retrig every $newRepeatTicInterval ticks$rampDesc " +
                    "set in column $newRepeatColumn, baseVol=${"%.4f".format(trackState.repeatBaseVolume)} → PERSISTENT")
        }

        // Determine which REPEAT to apply (new from this step, or persistent)
        val activeRepeatInterval = when {
            newRepeatTicInterval > 0 -> newRepeatTicInterval
            trackState.hasActiveRepeat() -> trackState.repeatTicInterval
            else -> 0
        }
        val activeVolRamp = when {
            newRepeatTicInterval > 0 -> newRepeatVolRamp
            trackState.hasActiveRepeat() -> trackState.repeatVolRamp
            else -> 0
        }

        // Apply REPEAT if active
        if (activeRepeatInterval > 0 && trackState.lastNote != Note.EMPTY) {
            val framesPerTic = stepDuration / TICS_PER_STEP

            // Determine note/instrument to retrigger
            val retrigNote = if (hasNote) {
                if (transposeSemitones != 0) {
                    val originalMidi = effectiveStep.note.toMidi()
                    if (originalMidi >= 0) Note.fromMidi((originalMidi + transposeSemitones).coerceIn(0, 127))
                    else effectiveStep.note
                } else effectiveStep.note
            } else {
                trackState.lastNote
            }
            val retrigInstrument = if (hasNote) effectiveStep.instrument else trackState.lastInstrument
            val retrigPan = if (hasNote) instrumentPan else trackState.lastPan
            val retrigStartPoint = if (hasNote) params.startPoint else trackState.lastStartPoint
            val rampDelta = REPEAT_RAMP_DELTAS[activeVolRamp.coerceIn(0, 15)]

            if (activeRepeatInterval < TICS_PER_STEP) {
                // ═══════════════════════════════════════════════════════════════════
                // SUB-STEP REPEAT: Multiple triggers within this step
                // ═══════════════════════════════════════════════════════════════════
                val triggersCount = TICS_PER_STEP / activeRepeatInterval

                // If step has note, main trigger is at tic 0 (already scheduled above)
                // For persistent REPEAT on empty step, schedule trigger at tic 0 too
                val startTic = if (hasNote) 1 else 0

                for (i in startTic until triggersCount) {
                    val ticPosition = i * activeRepeatInterval
                    val retrigFrame = targetFrame + (ticPosition * framesPerTic)

                    // Increment global retrig counter and calculate additive volume ramp
                    trackState.repeatRetrigCount++
                    val retrigVolume = (trackState.repeatBaseVolume + trackState.repeatRetrigCount * rampDelta)
                        .coerceIn(0.0f, 1.0f)

                    audioEngine.scheduleNote(
                        targetFrame = retrigFrame,
                        note = retrigNote,
                        instrumentId = retrigInstrument,
                        trackId = trackId,
                        volume = retrigVolume,
                        pan = retrigPan,
                        project = project,
                        startPointOverride = retrigStartPoint,
                        tableIdOverride = trackState.lastTableOverride,
                        transposeSemitones = transposeSemitones,
                        pitSemitones = params.pitSemitones ?: 0,
                        sliIndex = params.sliIndex ?: -1
                    )
                    if (TRACE) logger.d(TAG, "🔁 retrig[${trackState.repeatRetrigCount}] vol=${"%.4f".format(retrigVolume)} " +
                            "(base=${"%.4f".format(trackState.repeatBaseVolume)}, delta=$rampDelta)")
                }

                val isPersistent = !hasNote && trackState.hasActiveRepeat()
                val modeLabel = if (isPersistent) "PERSISTENT" else "step"
                if (TRACE) logger.d(TAG, "🔁 REPEAT ($modeLabel): ${triggersCount} triggers, interval=$activeRepeatInterval ticks, delta=$rampDelta")
            } else {
                // ═══════════════════════════════════════════════════════════════════
                // MULTI-STEP REPEAT (interval > 12 ticks): Sparse triggers
                // ═══════════════════════════════════════════════════════════════════
                val triggerIntervalFrames = activeRepeatInterval * framesPerTic
                val stepEndFrame = targetFrame + stepDuration

                if (hasNote && newRepeatTicInterval > 0) {
                    if (TRACE) logger.d(TAG, "🔁 REPEAT (multi-step): started, interval=$activeRepeatInterval ticks")
                } else {
                    val framesSinceStart = targetFrame - trackState.repeatStartFrame
                    if (framesSinceStart >= 0) {
                        val firstTriggerIndex = ((framesSinceStart + triggerIntervalFrames - 1) / triggerIntervalFrames).toInt()
                        var triggerFrame = trackState.repeatStartFrame + (firstTriggerIndex * triggerIntervalFrames)

                        var triggersInStep = 0
                        while (triggerFrame < stepEndFrame) {
                            if (triggerFrame >= targetFrame) {
                                // Increment global retrig counter and calculate additive volume ramp
                                trackState.repeatRetrigCount++
                                val retrigVolume = (trackState.repeatBaseVolume + trackState.repeatRetrigCount * rampDelta)
                                    .coerceIn(0.0f, 1.0f)

                                audioEngine.scheduleNote(
                                    targetFrame = triggerFrame,
                                    note = retrigNote,
                                    instrumentId = retrigInstrument,
                                    trackId = trackId,
                                    volume = retrigVolume,
                                    pan = retrigPan,
                                    project = project,
                                    startPointOverride = retrigStartPoint,
                                    tableIdOverride = trackState.lastTableOverride,
                                    transposeSemitones = transposeSemitones,
                                    pitSemitones = params.pitSemitones ?: 0,
                                    sliIndex = params.sliIndex ?: -1
                                )
                                triggersInStep++
                            }
                            triggerFrame += triggerIntervalFrames
                        }

                        if (triggersInStep > 0) {
                            if (TRACE) logger.d(TAG, "🔁 REPEAT (PERSISTENT multi-step): $triggersInStep trigger(s)")
                        }
                    }
                }
            }
        }

        // ═══════════════════════════════════════════════════════════════════════════
        // STEP 4: Handle ARC (Arpeggio Config) effect - mode and speed
        // ═══════════════════════════════════════════════════════════════════════════

        // ARC (Cxx) only updates config, doesn't start arpeggio on its own
        if (params.arcValue != null) {
            val mode = (params.arcValue shr 4) and 0x0F
            val speed = params.arcValue and 0x0F
            trackState.arpeggioMode = mode.coerceIn(0, 3)
            trackState.arpeggioSpeed = if (speed > 0) speed else 4  // Default speed is 4 tics
            val modeNames = listOf("UP", "DOWN", "PINGPONG", "RANDOM")
            if (TRACE) logger.d(TAG, "🎼 ARC C${params.arcValue.toString(16).uppercase().padStart(2, '0')}: " +
                    "mode=${modeNames.getOrElse(trackState.arpeggioMode) { "UP" }}, speed=${trackState.arpeggioSpeed} tics")
        }

        // ═══════════════════════════════════════════════════════════════════════════
        // STEP 5: Handle ARPEGGIO effect (ARP Axx) - new or persistent
        // ═══════════════════════════════════════════════════════════════════════════

        // Find which column has ARPEGGIO effect (if any) and its value
        var newArpColumn = 0
        var newArpValue = 0
        if (effectiveStep.fx1Type == EffectProcessor.FX_ARPEGGIO) {
            newArpColumn = 1
            newArpValue = effectiveStep.fx1Value
        } else if (effectiveStep.fx2Type == EffectProcessor.FX_ARPEGGIO) {
            newArpColumn = 2
            newArpValue = effectiveStep.fx2Value
        } else if (effectiveStep.fx3Type == EffectProcessor.FX_ARPEGGIO) {
            newArpColumn = 3
            newArpValue = effectiveStep.fx3Value
        }

        // ARP00 cancels arpeggio (explicit cancellation)
        if (newArpColumn > 0 && newArpValue == 0) {
            trackState.clearArpeggio()
            if (TRACE) logger.d(TAG, "🎵 ARP00 → arpeggio cancelled")
        } else if (newArpColumn > 0 && newArpValue > 0) {
            // New ARPEGGIO is set, update persistent state
            trackState.arpeggioActiveColumn = newArpColumn
            trackState.arpeggioValue = newArpValue
            trackState.arpeggioStartFrame = targetFrame  // Track start frame for phase continuity
            val semi1 = (newArpValue shr 4) and 0x0F
            val semi2 = newArpValue and 0x0F
            if (TRACE) logger.d(TAG, "🎵 ARP A${newArpValue.toString(16).uppercase().padStart(2, '0')} " +
                    "(+$semi1, +$semi2) set in column $newArpColumn at frame $targetFrame → now PERSISTENT")
        }

        // Determine which ARPEGGIO to apply (new from this step, or persistent)
        val activeArpValue = when {
            // If this step has a new ARP (and not ARP00), use it
            newArpValue > 0 -> newArpValue
            // If persistent ARPEGGIO is active, use it
            trackState.hasActiveArpeggio() -> trackState.arpeggioValue
            // No ARPEGGIO active
            else -> 0
        }

        // Apply ARPEGGIO if active
        if (activeArpValue > 0 && trackState.lastNote != Note.EMPTY) {
            scheduleArpeggioNotes(
                targetFrame = targetFrame,
                stepDuration = stepDuration,
                trackId = trackId,
                trackState = trackState,
                project = project,
                hasNote = hasNote,
                step = effectiveStep,
                params = params,
                transposeSemitones = transposeSemitones,
                instrVol = instrVol,
                phraseVol = phraseVol,
                finalPan = instrumentPan
            )
        }

        // Update per-column FX memory for RND (stores "previously active" FX per column)
        // Only store real effects, not meta-effects (RND, RNL, CHA, NONE)
        val metaEffects = setOf(EffectProcessor.FX_NONE, EffectProcessor.FX_RND, EffectProcessor.FX_RNL, EffectProcessor.FX_CHA)
        for (col in 1..3) {
            val (fxType, fxValue) = step.fx(col)
            if (fxType !in metaEffects) {
                trackState.lastColFxType[col] = fxType
                trackState.lastColFxValue[col] = fxValue
            }
        }

        return ScheduleStepResult(noteScheduled = noteScheduled, hopTriggered = hopTriggered)
    }

    private fun scheduleArpeggioNotes(
        targetFrame: Long,
        stepDuration: Long,
        trackId: Int,
        trackState: TrackState,
        project: Project,
        hasNote: Boolean,
        step: PhraseStep,
        params: ResolvedStepParams,
        transposeSemitones: Int,
        instrVol: Float,
        phraseVol: Float,
        finalPan: Float
    ) {
        val semi1 = (trackState.arpeggioValue shr 4) and 0x0F
        val semi2 = trackState.arpeggioValue and 0x0F

        // Get base note (from step or last played)
        val baseNote = if (hasNote) {
            if (transposeSemitones != 0) {
                val originalMidi = step.note.toMidi()
                if (originalMidi >= 0) {
                    Note.fromMidi((originalMidi + transposeSemitones).coerceIn(0, 127))
                } else step.note
            } else step.note
        } else {
            trackState.lastNote
        }

        val baseMidi = baseNote.toMidi()
        if (baseMidi < 0) return  // Invalid note, can't arpeggio

        // Calculate timing - FRAME-BASED for cross-step continuity
        val framesPerTic = stepDuration / TICS_PER_STEP
        val ticInterval = trackState.arpeggioSpeed
        val framesPerArpNote = ticInterval * framesPerTic  // How many frames between each arp note

        // Get the arpeggio pattern length based on mode
        val patternLength = if (trackState.arpeggioMode == 2) 4 else 3  // PINGPONG uses 4, others use 3

        // Get instrument, volume, and pan; track × master are applied in C++
        val instrumentId = if (hasNote) step.instrument else trackState.lastInstrument
        val arpInstrVol  = if (hasNote) instrVol  else trackState.lastVolume  // Combined as instrVol on retrig
        val arpPhraseVol = if (hasNote) phraseVol else 1.0f                   // Neutral on retrig (combined in lastVolume)
        val arpPan = if (hasNote) finalPan else trackState.lastPan
        val startPoint = if (hasNote) params.startPoint else trackState.lastStartPoint

        // Calculate step boundaries
        val stepEndFrame = targetFrame + stepDuration

        // For step where ARP starts (with note), the first note was already scheduled
        // We need to find subsequent notes within this step
        val isNewArp = hasNote && trackState.arpeggioStartFrame == targetFrame

        // Calculate trigger points using absolute frames (works across steps!)
        val framesSinceStart = targetFrame - trackState.arpeggioStartFrame
        var notesScheduled = 0

        if (framesSinceStart >= 0) {
            // Find the first trigger index at or after targetFrame
            // triggerIndex = how many arp notes have played since start
            val firstTriggerIndex = if (isNewArp) {
                1L  // Skip index 0, it was the original note
            } else {
                // Find first trigger at or after targetFrame
                ((framesSinceStart + framesPerArpNote - 1) / framesPerArpNote)
            }

            var triggerIndex = firstTriggerIndex
            var triggerFrame = trackState.arpeggioStartFrame + (triggerIndex * framesPerArpNote)

            // Schedule all notes that fall within this step's frame range
            while (triggerFrame < stepEndFrame) {
                if (triggerFrame >= targetFrame) {
                    // Calculate which note in the pattern this is
                    val patternPosition = (triggerIndex % patternLength).toInt()
                    val arpMidi = getArpeggioNote(baseMidi, semi1, semi2, trackState.arpeggioMode, patternPosition)

                    audioEngine.scheduleNote(
                        targetFrame = triggerFrame,
                        note = baseNote,
                        instrumentId = instrumentId,
                        trackId = trackId,
                        volume = arpInstrVol,
                        phraseVol = arpPhraseVol,
                        pan = arpPan,
                        project = project,
                        startPointOverride = startPoint,
                        tableIdOverride = trackState.lastTableOverride,
                        transposeSemitones = transposeSemitones,
                        pitSemitones = params.pitSemitones ?: 0,
                        sliIndex = params.sliIndex ?: -1,
                        arpSemitoneOffset = arpMidi - baseMidi
                    )
                    notesScheduled++
                }
                triggerIndex++
                triggerFrame += framesPerArpNote
            }
        }

        if (notesScheduled > 0) {
            val isPersistent = !hasNote && trackState.hasActiveArpeggio()
            val modeLabel = if (isPersistent) "PERSISTENT" else "step"
            val modeNames = listOf("UP", "DOWN", "PINGPONG", "RANDOM")
            val crossStepInfo = if (framesSinceStart > 0) " (cross-step)" else ""
            if (TRACE) logger.d(TAG, "🎵 ARP A${trackState.arpeggioValue.toString(16).uppercase().padStart(2, '0')} " +
                    "($modeLabel, ${modeNames.getOrElse(trackState.arpeggioMode) { "UP" }})$crossStepInfo: " +
                    "$notesScheduled notes at speed ${trackState.arpeggioSpeed}")
        }
    }

    private fun getArpeggioNote(baseMidi: Int, semi1: Int, semi2: Int, mode: Int, position: Int): Int {
        val note0 = baseMidi            // Root
        val note1 = baseMidi + semi1    // +semi1
        val note2 = baseMidi + semi2    // +semi2

        return when (mode) {
            0 -> {
                // UP: root → +semi1 → +semi2 → ...
                when (position % 3) {
                    0 -> note0
                    1 -> note1
                    else -> note2
                }
            }
            1 -> {
                // DOWN: +semi2 → +semi1 → root → ...
                when (position % 3) {
                    0 -> note2
                    1 -> note1
                    else -> note0
                }
            }
            2 -> {
                // PINGPONG: root → +semi1 → +semi2 → +semi1 → ...
                when (position % 4) {
                    0 -> note0
                    1 -> note1
                    2 -> note2
                    else -> note1
                }
            }
            3 -> {
                // RANDOM: random selection
                listOf(note0, note1, note2).random()
            }
            else -> {
                // Default to UP
                when (position % 3) {
                    0 -> note0
                    1 -> note1
                    else -> note2
                }
            }
        }
    }

    fun calculateStepFrame(startFrame: Long, stepNumber: Int, tempo: Int): Long {
        return audioEngine.calculateTargetFrame(startFrame, stepNumber, tempo)
    }

    fun getCurrentFrame(): Long {
        return audioEngine.getCurrentFrame()
    }
}

enum class PlaybackMode {
    STOPPED, PHRASE, CHAIN, SONG
}
