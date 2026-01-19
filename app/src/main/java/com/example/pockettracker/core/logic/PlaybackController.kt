package com.example.pockettracker.core.logic

import com.example.pockettracker.core.data.Note
import com.example.pockettracker.core.data.PhraseStep
import com.example.pockettracker.core.data.Project
import com.example.pockettracker.core.data.ScreenType
import com.example.pockettracker.core.audio.AudioEngine
import com.example.pockettracker.core.data.Chain
import com.example.pockettracker.core.data.Phrase
import com.example.pockettracker.core.logging.ILogger

/**
 * Per-track state for persistent effects and note memory.
 *
 * Used to implement M8/LGPT-style persistent effects where an effect
 * continues until cancelled by specific conditions:
 * - REPEAT (Rxx) persists until:
 *   1. A new note is triggered on this track
 *   2. Any effect in the same FX column where REPEAT was set
 *   3. KILL effect (K00) in any FX column
 */
data class TrackState(
    /** Last played note on this track (for persistent REPEAT/ARPEGGIO retrigger) */
    var lastNote: Note = Note.EMPTY,
    /** Last played instrument ID */
    var lastInstrument: Int = 0,
    /** Last played volume (0.0-1.0) */
    var lastVolume: Float = 1.0f,
    /** Last played start point override (-1 = instrument default) */
    var lastStartPoint: Int = -1,

    // Persistent REPEAT state
    /** Which FX column (1, 2, or 3) the REPEAT was set in, or 0 if not active */
    var repeatActiveColumn: Int = 0,
    /** Repeat tic interval (Rxx value) - can be sub-step (<12) or multi-step (>=12) */
    var repeatTicInterval: Int = 0,
    /** Audio frame where REPEAT was started (for cross-phrase persistence) */
    var repeatStartFrame: Long = 0,

    // Persistent ARPEGGIO state (ARP Axx and ARC Cxx)
    /** Which FX column (1, 2, or 3) the ARPEGGIO (Axx) was set in, or 0 if not active */
    var arpeggioActiveColumn: Int = 0,
    /** Arpeggio value (Axx) - high nibble = +semitone1, low nibble = +semitone2 */
    var arpeggioValue: Int = 0,
    /** Arpeggio mode from ARC (Cxx) - 0=UP, 1=DOWN, 2=PINGPONG, 3=RANDOM */
    var arpeggioMode: Int = 0,
    /** Arpeggio speed in tics from ARC (Cxx) - default 4 (3 notes/step at 12 tics) */
    var arpeggioSpeed: Int = 4,
    /** Audio frame where ARPEGGIO was started (for cross-step phase continuity) */
    var arpeggioStartFrame: Long = 0
) {
    /** Check if persistent REPEAT is active */
    fun hasActiveRepeat(): Boolean = repeatActiveColumn > 0 && repeatTicInterval > 0

    /** Clear persistent REPEAT */
    fun clearRepeat() {
        repeatActiveColumn = 0
        repeatTicInterval = 0
        repeatStartFrame = 0
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
}

/**
 * PlaybackController
 *
 * Manages all playback operations including:
 * - Playback state (playing/stopped)
 * - Phrase/chain/song playback scheduling with continuous lookahead buffering
 * - Sample-accurate note queue management
 * - Playback cursors and position tracking
 * - Per-track persistent effect state (REPEAT, etc.)
 *
 * ✅ PLATFORM-AGNOSTIC - No Android dependencies!
 * ✅ SINGLE SOURCE OF TRUTH - All scheduling logic centralized here
 *
 * IMPORTANT: This controller handles ALL audio scheduling internally.
 * PixelPerfectRenderer ONLY reads playback position for UI updates.
 * This prevents duplicate note scheduling (the "playback doubling" bug).
 *
 * Updated in Phase 1 refactoring to use the new AudioEngine architecture.
 * Updated in Phase 5 to remove Compose state dependencies.
 * Updated Phase 6 to consolidate all scheduling logic (eliminate duplication).
 * Updated Phase 7 to add persistent REPEAT effect (LGPT/M8 style).
 */
class PlaybackController(
    private val audioEngine: AudioEngine,
    private val effectProcessor: EffectProcessor,
    private val logger: ILogger,
    private val stateObserver: StateObserver,
    private var playbackStartFrame: Long = 0,
    private var currentPhraseId: Int = 0,
    private var currentChainId: Int = 0,
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

    /** Store current project for ongoing playback */
    private var currentProject: Project? = null

    /** Track next frame position to schedule (for continuous buffering) */
    private var nextFrameToSchedule: Long = 0

    /** Track next chain/song position to schedule (for continuous looping) */
    private var nextChainRowToSchedule: Int = 0
    private var nextSongRowToSchedule: Int = 0
    private var nextSongChainRowToSchedule: Int = 0  // FIX: Track which chain row within song row

    /** Track current playing position for UI cursor (chain mode) */
    private val chainRowStartFrames = mutableMapOf<Int, Long>()  // Map chain row to start frame

    /** Track current playing position for UI cursor (song mode) */
    private val songPositionStartFrames = mutableMapOf<Pair<Int,Int>, Long>()  // Map (songRow, chainRow) to start frame

    /** Per-track state for persistent effects (REPEAT, note memory) - 8 tracks */
    private val trackStates = Array(8) { TrackState() }

    /** Lookahead configuration */
    companion object {
        const val LOOKAHEAD_MS = 50L           // Minimal latency for responsive start
        const val BUFFER_PHRASES = 2           // Keep 2+ phrases queued ahead

        // ═══════════════════════════════════════════════════════════════════════════
        // TIC SYSTEM - Subdivisions within a step for precise effect timing
        // ═══════════════════════════════════════════════════════════════════════════
        // Tics are the smallest timing unit in tracker music.
        // A step is divided into TICS_PER_STEP tics.
        // Effects like REPEAT and ARPEGGIO operate at tic resolution.
        //
        // 12 tics/step is a good default because it's divisible by:
        // - 2 (half-step subdivisions)
        // - 3 (triplets!)
        // - 4 (quarter-step subdivisions)
        // - 6 (sextuplets)
        //
        // Future: This will be configurable via Groove screen (post-MVP)
        // Common values: 6 (classic), 12 (default), 24 (high resolution)
        const val TICS_PER_STEP = 12
    }

    // ═══════════════════════════════════════════════════════════════════════════
    // PLAYBACK CONTROL
    // ═══════════════════════════════════════════════════════════════════════════
    // ═══════════════════════════════════════════════════════════════════════════
    // PLAYBACK POSITION FOR UI
    // ═══════════════════════════════════════════════════════════════════════════

    /**
     * Get current playback position for UI updates
     * Returns: (row, chainRow, phraseStep, songRow) based on current mode
     *
     * This is READ-ONLY for the UI layer. All scheduling happens internally.
     */
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
                // Find which chain row is currently playing based on audio frame
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
                // Find which song position is currently playing
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

    /**
     * Playback position data class for UI rendering
     */
    data class PlaybackPosition(
        val row: Int,
        val chainRow: Int,
        val phraseStep: Int,
        val songRow: Int
    )
    /**
     * Toggle playback on/off
     * Simple toggle for START button
     */
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
        chainRowStartFrames.clear()
        songPositionStartFrames.clear()
        nextSongChainRowToSchedule = 0
        // Clear all track states (persistent effects, note memory)
        trackStates.forEach {
            it.clearRepeat()
            it.clearAllArpeggioState()
        }
        logger.d(TAG, "⏹️ Playback stopped")
    }

    // ═══════════════════════════════════════════════════════════════════════════
    // LOOKAHEAD BUFFER MAINTENANCE
    // ═══════════════════════════════════════════════════════════════════════════

    /**
     * Update playback buffer by scheduling more notes if needed.
     *
     * CRITICAL: This is called by the UI layer (PixelPerfectRenderer) to maintain
     * the lookahead buffer during playback. This is the ONLY place where continuous
     * scheduling should happen for Phrase/Chain/Song playback.
     *
     * IMPORTANT: This method prevents the "playback doubling" bug by centralizing
     * all scheduling logic in PlaybackController. PixelPerfectRenderer only calls
     * this method and reads playback position via getPlaybackPosition().
     *
     * Call this regularly (e.g., 60 Hz) when playback is active.
     */
    fun updatePlaybackBuffer() {
        if (!isPlaying || currentProject == null) return

        val project = currentProject ?: return
        val sampleRate = audioEngine.getDeviceSampleRate()
        val tempo = project.tempo
        val msPerStep = (60000.0 / tempo / 4.0)
        val framesPerStep = (msPerStep * sampleRate / 1000.0).toLong()
        val framesPerPhrase = framesPerStep * 16
        val currentFrame = audioEngine.getCurrentFrame()

        // Check if we need more notes in the buffer
        val lookaheadFrames = (LOOKAHEAD_MS * sampleRate / 1000.0).toLong()
        val bufferRemaining = nextFrameToSchedule - currentFrame
        val minBuffer = (BUFFER_PHRASES * framesPerPhrase)

        if (bufferRemaining < minBuffer) {
            when (playbackMode) {
                PlaybackMode.PHRASE -> {
                    // For phrase: just reschedule the same phrase
                    val phrase = project.phrases[currentPhraseId]
                    schedulePhrase(phrase, nextFrameToSchedule, 0, 0, project, framesPerStep)
                    nextFrameToSchedule += framesPerPhrase
                }

                PlaybackMode.CHAIN -> {
                    // For chain: schedule next non-empty phrase in chain
                    val chain = project.chains[currentChainId]
                    val nextRow = findNextNonEmptyChainRow(nextChainRowToSchedule, chain)
                    if (nextRow != null) {
                        val phraseId = chain.phraseRefs[nextRow]
                        val transposeSemitones = chain.getTransposeSemitones(nextRow)
                        schedulePhrase(project.phrases[phraseId], nextFrameToSchedule, 0, transposeSemitones, project, framesPerStep)
                        chainRowStartFrames[nextRow] = nextFrameToSchedule  // Track start frame for cursor
                        nextFrameToSchedule += framesPerPhrase
                        nextChainRowToSchedule = (nextRow + 1) % 16
                    } else {
                        // Chain is empty, stop playback
                        stop()
                    }
                }

                PlaybackMode.SONG -> {
                    // For song: schedule next phrases across all tracks
                    val song = project.tracks  // 8 tracks

                    // Compute song length (max number of song rows across tracks)
                    val songLength = (0..7).map { trackId ->
                        song[trackId].chainRefs.size
                    }.maxOrNull() ?: 0

                    // If song has no rows, stop playback
                    if (songLength == 0) {
                        stop()
                    } else {
                        // Find max chain length in current song row (determines how many phrase-rows we need)
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

                        // If no chains in this song row, advance to next song row and wrap at songLength
                        if (maxChainLength == 0) {
                            nextSongRowToSchedule++
                            nextSongChainRowToSchedule = 0
                            if (nextSongRowToSchedule >= songLength) {
                                nextSongRowToSchedule = 0  // Loop song
                            }
                        } else if (nextSongChainRowToSchedule < maxChainLength) {
                            // Schedule this chain row for all 8 tracks
                            var scheduledAny = false
                            for (trackId in 0..7) {
                                if (nextSongRowToSchedule < song[trackId].chainRefs.size) {
                                    val chainId = song[trackId].chainRefs[nextSongRowToSchedule]
                                    if (chainId >= 0 && chainId < 256) {
                                        val chain = project.chains[chainId]
                                        if (!chain.isEmpty(nextSongChainRowToSchedule)) {
                                            val phraseId = chain.phraseRefs[nextSongChainRowToSchedule]
                                            val transposeSemitones = chain.getTransposeSemitones(nextSongChainRowToSchedule)
                                            schedulePhrase(project.phrases[phraseId], nextFrameToSchedule, trackId, transposeSemitones, project, framesPerStep)
                                            songPositionStartFrames[Pair(nextSongRowToSchedule, nextSongChainRowToSchedule)] = nextFrameToSchedule
                                            scheduledAny = true
                                        }
                                    }
                                }
                            }

                            if (scheduledAny) {
                                nextFrameToSchedule += framesPerPhrase
                                nextSongChainRowToSchedule++
                            } else {
                                // No phrases in this chain row, skip to next
                                nextSongChainRowToSchedule++
                            }
                        } else {
                            // Reached end of all chain rows in this song row, move to next song row
                            nextSongRowToSchedule++
                            nextSongChainRowToSchedule = 0
                            if (nextSongRowToSchedule >= songLength) {
                                nextSongRowToSchedule = 0  // Loop song
                            }
                        }
                    }
                }

                else -> {
                    // Not playing, do nothing
                }
            }
        }
    }

    // ═══════════════════════════════════════════════════════════════════════════
    // PHRASE PLAYBACK
    // ═══════════════════════════════════════════════════════════════════════════

    /**
     * Play a phrase (16 steps)
     *
     * Initializes playback with lookahead buffer.
     * Continuous scheduling is NOT done here - PixelPerfectRenderer will call
     * updatePlaybackBuffer() periodically to maintain the buffer.
     *
     * @param project Project containing phrase data
     * @param phraseId Which phrase to play (0-255)
     * @param loop Whether to loop playback
     */
    fun playPhrase(project: Project, phraseId: Int, loop: Boolean = true) {
        // Stop any current playback first
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

        // Initialize scheduling state
        nextFrameToSchedule = playbackStartFrame

        // Schedule first phrase immediately
        schedulePhrase(phrase, playbackStartFrame, 0, 0, project, framesPerStep)
        nextFrameToSchedule += framesPerStep * 16  // Phrase is 16 steps

        logger.d(TAG, "✅ Phrase playback initialized")
    }

    /**
     * Schedule a single phrase for playback
     * Helper used by playPhrase and updatePlaybackBuffer
     *
     * Now handles persistent REPEAT effect:
     * - Passes track state to each step for persistence tracking
     * - REPEAT continues until cancelled by note, same-column FX, or KILL
     * - Supports multi-step intervals (R0C+ = every N steps)
     */
    private fun schedulePhrase(
        phrase: Phrase,
        startFrame: Long,
        trackId: Int,
        transposeSemitones: Int,
        project: Project,
        framesPerStep: Long
    ) {
        var scheduledNotes = 0
        val trackState = trackStates[trackId.coerceIn(0, 7)]

        phrase.steps.forEachIndexed { stepIndex, step ->
            val targetFrame = startFrame + (stepIndex * framesPerStep)

            val noteScheduled = scheduleStepWithEffects(
                step = step,
                targetFrame = targetFrame,
                stepDuration = framesPerStep,
                trackId = trackId,
                transposeSemitones = transposeSemitones,
                project = project,
                trackState = trackState,
                stepIndex = stepIndex  // Pass step index for multi-step REPEAT
            )

            if (noteScheduled) {
                scheduledNotes++
            }
        }
        if (scheduledNotes > 0) {
            logger.d(TAG, "  Scheduled $scheduledNotes notes")
        }
    }

    /**
     * Play a chain (16 phrase slots with transpose)
     *
     * Initializes playback with lookahead buffer.
     * Continuous scheduling is NOT done here - PixelPerfectRenderer will call
     * updatePlaybackBuffer() periodically to maintain the buffer.
     *
     * @param project Project containing chain data
     * @param chainId Which chain to play (0-255)
     * @param loop Whether to loop playback
     */
    fun playChain(project: Project, chainId: Int, loop: Boolean = true) {
        // Stop any current playback first
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
        val framesPerPhrase = framesPerStep * 16

        logger.d(TAG, "▶️ Playing chain $chainId (tempo: $tempo BPM)")

        // Initialize scheduling state
        nextFrameToSchedule = playbackStartFrame
        nextChainRowToSchedule = 0
        chainRowStartFrames.clear()

        // Find and schedule first non-empty phrase
        val firstRow = findNextNonEmptyChainRow(0, chain)
        if (firstRow != null) {
            val phraseId = chain.phraseRefs[firstRow]
            val transposeSemitones = chain.getTransposeSemitones(firstRow)
            schedulePhrase(project.phrases[phraseId], playbackStartFrame, 0, transposeSemitones, project, framesPerStep)
            chainRowStartFrames[firstRow] = playbackStartFrame  // Track start frame for cursor
            nextFrameToSchedule += framesPerPhrase
            nextChainRowToSchedule = firstRow + 1
        }

        logger.d(TAG, "✅ Chain playback initialized")
    }

    /**
     * Find next non-empty chain row (circular, wrapping at 16)
     */
    private fun findNextNonEmptyChainRow(startRow: Int, chain: Chain): Int? {
        var row = startRow
        var attempts = 0
        while (chain.isEmpty(row) && attempts < 16) {
            row = (row + 1) % 16
            attempts++
        }
        return if (attempts >= 16) null else row
    }

    /**
     * Play song (8 tracks polyphonic)
     *
     * Initializes playback with lookahead buffer.
     * Continuous scheduling is NOT done here - PixelPerfectRenderer will call
     * updatePlaybackBuffer() periodically to maintain the buffer.
     *
     * @param project Project containing song data
     * @param startRow Which song row to start from
     * @param loop Whether to loop playback
     */
    fun playSong(project: Project, startRow: Int = 0, loop: Boolean = true) {
        // Stop any current playback first
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

        // Initialize scheduling state
        nextFrameToSchedule = playbackStartFrame
        nextSongRowToSchedule = startRow
        nextSongChainRowToSchedule = 0
        songPositionStartFrames.clear()

        logger.d(TAG, "✅ Song playback initialized")
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
     * - KILL effect (K00) - stops sample and clears persistent REPEAT
     * - REPEAT effect (Rxx) - retrigger with PERSISTENCE (LGPT/M8 style)
     *
     * ## Persistent REPEAT (Rxx)
     * REPEAT persists until cancelled by:
     * 1. A new note on this track
     * 2. Any effect in the same FX column where REPEAT was set
     * 3. KILL effect (K00) in any FX column
     *
     * ## Sub-step vs Multi-step REPEAT
     * - R01-R0B: Sub-step intervals (multiple triggers within one step)
     * - R0C+: Multi-step intervals (one trigger every N steps)
     *   - R0C (12) = every 1 step
     *   - R18 (24) = every 2 steps
     *   - R24 (36) = every 3 steps
     *   - R30 (48) = every 4 steps (4 kicks in 16-step phrase!)
     *   - R12 (18) = every 1.5 steps (dotted notes)
     *
     * When persistent REPEAT is active and step has no note, the last played
     * note/instrument is retriggered at the REPEAT interval.
     *
     * @param step The phrase step to schedule
     * @param targetFrame When this step should trigger (audio frame)
     * @param stepDuration Duration of step in frames (for time-based effects)
     * @param trackId Which track (0-7)
     * @param transposeSemitones Semitones to transpose (0 for Phrase, varies for Chain/Song)
     * @param project Project containing instrument data
     * @param trackState Per-track state for persistent effects (modified in place)
     * @param stepIndex Current step index in phrase (0-15), for multi-step REPEAT
     * @return true if a note was scheduled, false if step was empty
     */
    fun scheduleStepWithEffects(
        step: PhraseStep,
        targetFrame: Long,
        stepDuration: Long,
        trackId: Int,
        transposeSemitones: Int,
        project: Project,
        trackState: TrackState = trackStates[trackId.coerceIn(0, 7)],
        stepIndex: Int = 0
    ): Boolean {
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
            logger.d(TAG, "🔪 KILL detected → persistent REPEAT and ARPEGGIO cancelled")
        }

        // Check if step has a note → clears persistent REPEAT and ARPEGGIO (new note triggers)
        val hasNote = !step.isEmpty()
        if (hasNote) {
            trackState.clearRepeat()
            trackState.clearArpeggio()
            // Note: We don't log here because new effects might be set below
        }

        // Check if persistent REPEAT's column has any effect → clears REPEAT
        // (Any effect in the same column overrides the persistent REPEAT)
        if (trackState.hasActiveRepeat()) {
            val columnHasEffect = when (trackState.repeatActiveColumn) {
                1 -> step.fx1Type != EffectProcessor.FX_NONE
                2 -> step.fx2Type != EffectProcessor.FX_NONE
                3 -> step.fx3Type != EffectProcessor.FX_NONE
                else -> false
            }
            if (columnHasEffect) {
                logger.d(TAG, "🔄 FX in column ${trackState.repeatActiveColumn} → persistent REPEAT cancelled")
                trackState.clearRepeat()
            }
        }

        // Check if persistent ARPEGGIO's column has any effect → clears ARPEGGIO
        // (Any effect in the same column overrides the persistent ARPEGGIO)
        if (trackState.hasActiveArpeggio()) {
            val columnHasEffect = when (trackState.arpeggioActiveColumn) {
                1 -> step.fx1Type != EffectProcessor.FX_NONE
                2 -> step.fx2Type != EffectProcessor.FX_NONE
                3 -> step.fx3Type != EffectProcessor.FX_NONE
                else -> false
            }
            if (columnHasEffect) {
                logger.d(TAG, "🔄 FX in column ${trackState.arpeggioActiveColumn} → persistent ARPEGGIO cancelled")
                trackState.clearArpeggio()
            }
        }

        // ═══════════════════════════════════════════════════════════════════════════
        // STEP 2: Resolve effects and schedule note if present
        // ═══════════════════════════════════════════════════════════════════════════

        val defaultVolume = step.volume / 255.0f
        val params = effectProcessor.resolveStepParams(step, targetFrame, defaultVolume)

        var noteScheduled = false
        if (hasNote) {
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

            // Update track state with this note (for persistent REPEAT retrigger)
            trackState.lastNote = note
            trackState.lastInstrument = step.instrument
            trackState.lastVolume = params.volume
            trackState.lastStartPoint = params.startPoint
        }

        // Handle KILL effect - schedule kill at the specified frame
        if (params.killAtFrame != null) {
            audioEngine.scheduleKill(params.killAtFrame, trackId)
        }

        // ═══════════════════════════════════════════════════════════════════════════
        // STEP 3: Handle REPEAT effect (new or persistent)
        // ═══════════════════════════════════════════════════════════════════════════

        // Find which column has REPEAT effect (if any) and its value
        var newRepeatColumn = 0
        var newRepeatValue = 0
        if (step.fx1Type == EffectProcessor.FX_REPEAT && step.fx1Value > 0) {
            newRepeatColumn = 1
            newRepeatValue = step.fx1Value
        } else if (step.fx2Type == EffectProcessor.FX_REPEAT && step.fx2Value > 0) {
            newRepeatColumn = 2
            newRepeatValue = step.fx2Value
        } else if (step.fx3Type == EffectProcessor.FX_REPEAT && step.fx3Value > 0) {
            newRepeatColumn = 3
            newRepeatValue = step.fx3Value
        }

        // If new REPEAT is set, update persistent state
        if (newRepeatColumn > 0) {
            trackState.repeatActiveColumn = newRepeatColumn
            trackState.repeatTicInterval = newRepeatValue
            trackState.repeatStartFrame = targetFrame  // Remember absolute frame where REPEAT started
            val intervalType = if (newRepeatValue < TICS_PER_STEP) "sub-step" else "multi-step"
            logger.d(TAG, "🔁 REPEAT R${newRepeatValue.toString(16).uppercase().padStart(2, '0')} " +
                    "($intervalType) set in column $newRepeatColumn at frame $targetFrame → now PERSISTENT (cross-phrase)")
        }

        // Determine which REPEAT to apply (new from this step, or persistent)
        val activeRepeatInterval = when {
            // If this step has a new REPEAT, use it
            newRepeatValue > 0 -> newRepeatValue
            // If persistent REPEAT is active, use it
            trackState.hasActiveRepeat() -> trackState.repeatTicInterval
            // No REPEAT active
            else -> 0
        }

        // Apply REPEAT if active
        if (activeRepeatInterval > 0 && trackState.lastNote != Note.EMPTY) {
            val framesPerTic = stepDuration / TICS_PER_STEP

            // Determine note/instrument to retrigger
            val retrigNote = if (hasNote) {
                if (transposeSemitones != 0) {
                    val originalMidi = step.note.toMidi()
                    if (originalMidi >= 0) Note.fromMidi((originalMidi + transposeSemitones).coerceIn(0, 127))
                    else step.note
                } else step.note
            } else {
                trackState.lastNote
            }
            val retrigInstrument = if (hasNote) step.instrument else trackState.lastInstrument
            val retrigVolume = if (hasNote) params.volume else trackState.lastVolume
            val retrigStartPoint = if (hasNote) params.startPoint else trackState.lastStartPoint

            if (activeRepeatInterval < TICS_PER_STEP) {
                // ═══════════════════════════════════════════════════════════════════
                // SUB-STEP REPEAT (R01-R0B): Multiple triggers within this step
                // ═══════════════════════════════════════════════════════════════════
                val triggersCount = TICS_PER_STEP / activeRepeatInterval

                // If step has note, main trigger is at tic 0 (already scheduled above)
                // For persistent REPEAT on empty step, schedule trigger at tic 0 too
                val startTic = if (hasNote) 1 else 0

                for (i in startTic until triggersCount) {
                    val ticPosition = i * activeRepeatInterval
                    val retrigFrame = targetFrame + (ticPosition * framesPerTic)

                    audioEngine.scheduleNote(
                        targetFrame = retrigFrame,
                        note = retrigNote,
                        instrumentId = retrigInstrument,
                        trackId = trackId,
                        volume = retrigVolume,
                        project = project,
                        startPointOverride = retrigStartPoint
                    )
                }

                val isPersistent = !hasNote && trackState.hasActiveRepeat()
                val modeLabel = if (isPersistent) "PERSISTENT" else "step"
                logger.d(TAG, "🔁 REPEAT R${activeRepeatInterval.toString(16).uppercase().padStart(2, '0')} ($modeLabel): " +
                        "$triggersCount triggers within step")
            } else {
                // ═══════════════════════════════════════════════════════════════════
                // MULTI-STEP REPEAT (R0C+): One trigger every N steps
                // ═══════════════════════════════════════════════════════════════════
                // Uses FRAME-BASED calculation for cross-phrase persistence!
                // Triggers occur at frames: repeatStartFrame + k * triggerIntervalFrames
                // where k = 1, 2, 3, ... (k=0 was the original note)

                val triggerIntervalFrames = activeRepeatInterval * framesPerTic
                val stepEndFrame = targetFrame + stepDuration

                // For the step where REPEAT starts (with note), the note is already scheduled
                if (hasNote && newRepeatValue > 0) {
                    // Note already scheduled above, nothing more to do for this step
                    // (First trigger is the note itself at repeatStartFrame)
                    logger.d(TAG, "🔁 REPEAT R${activeRepeatInterval.toString(16).uppercase().padStart(2, '0')} (multi-step): " +
                            "started at frame $targetFrame, interval = ${activeRepeatInterval / TICS_PER_STEP.toFloat()} steps")
                } else {
                    // Calculate trigger points using absolute frames (works across phrases!)
                    // Find the first trigger index k where repeatStartFrame + k*interval >= targetFrame
                    val framesSinceStart = targetFrame - trackState.repeatStartFrame
                    if (framesSinceStart >= 0) {
                        // Find first trigger at or after targetFrame
                        val firstTriggerIndex = ((framesSinceStart + triggerIntervalFrames - 1) / triggerIntervalFrames).toInt()
                        var triggerFrame = trackState.repeatStartFrame + (firstTriggerIndex * triggerIntervalFrames)

                        // Schedule all triggers that fall within this step's frame range
                        var triggersInStep = 0
                        while (triggerFrame < stepEndFrame) {
                            if (triggerFrame >= targetFrame) {
                                audioEngine.scheduleNote(
                                    targetFrame = triggerFrame,
                                    note = retrigNote,
                                    instrumentId = retrigInstrument,
                                    trackId = trackId,
                                    volume = retrigVolume,
                                    project = project,
                                    startPointOverride = retrigStartPoint
                                )
                                triggersInStep++
                            }
                            triggerFrame += triggerIntervalFrames
                        }

                        if (triggersInStep > 0) {
                            val phraseInfo = if (framesSinceStart > stepDuration * 16) " (cross-phrase!)" else ""
                            logger.d(TAG, "🔁 REPEAT R${activeRepeatInterval.toString(16).uppercase().padStart(2, '0')} (PERSISTENT multi-step)$phraseInfo: " +
                                    "$triggersInStep trigger(s)")
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
            logger.d(TAG, "🎼 ARC C${params.arcValue.toString(16).uppercase().padStart(2, '0')}: " +
                    "mode=${modeNames.getOrElse(trackState.arpeggioMode) { "UP" }}, speed=${trackState.arpeggioSpeed} tics")
        }

        // ═══════════════════════════════════════════════════════════════════════════
        // STEP 5: Handle ARPEGGIO effect (ARP Axx) - new or persistent
        // ═══════════════════════════════════════════════════════════════════════════

        // Find which column has ARPEGGIO effect (if any) and its value
        var newArpColumn = 0
        var newArpValue = 0
        if (step.fx1Type == EffectProcessor.FX_ARPEGGIO) {
            newArpColumn = 1
            newArpValue = step.fx1Value
        } else if (step.fx2Type == EffectProcessor.FX_ARPEGGIO) {
            newArpColumn = 2
            newArpValue = step.fx2Value
        } else if (step.fx3Type == EffectProcessor.FX_ARPEGGIO) {
            newArpColumn = 3
            newArpValue = step.fx3Value
        }

        // ARP00 cancels arpeggio (explicit cancellation)
        if (newArpColumn > 0 && newArpValue == 0) {
            trackState.clearArpeggio()
            logger.d(TAG, "🎵 ARP00 → arpeggio cancelled")
        } else if (newArpColumn > 0 && newArpValue > 0) {
            // New ARPEGGIO is set, update persistent state
            trackState.arpeggioActiveColumn = newArpColumn
            trackState.arpeggioValue = newArpValue
            trackState.arpeggioStartFrame = targetFrame  // Track start frame for phase continuity
            val semi1 = (newArpValue shr 4) and 0x0F
            val semi2 = newArpValue and 0x0F
            logger.d(TAG, "🎵 ARP A${newArpValue.toString(16).uppercase().padStart(2, '0')} " +
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
                step = step,
                params = params,
                transposeSemitones = transposeSemitones
            )
        }

        return noteScheduled
    }

    // ═══════════════════════════════════════════════════════════════════════════
    // ARPEGGIO HELPERS
    // ═══════════════════════════════════════════════════════════════════════════

    /**
     * Schedule arpeggio notes for the current step.
     *
     * Generates a sequence of notes based on the arpeggio value (Axx) and config (Cxx),
     * then schedules them at regular tic intervals within the step.
     *
     * @param targetFrame When this step starts (audio frame)
     * @param stepDuration Duration of step in frames
     * @param trackId Which track (0-7)
     * @param trackState Per-track state (contains arpeggio config)
     * @param project Project containing instrument data
     * @param hasNote Whether this step has a note
     * @param step The phrase step
     * @param params Resolved step parameters
     * @param transposeSemitones Semitones to transpose
     */
    private fun scheduleArpeggioNotes(
        targetFrame: Long,
        stepDuration: Long,
        trackId: Int,
        trackState: TrackState,
        project: Project,
        hasNote: Boolean,
        step: PhraseStep,
        params: ResolvedStepParams,
        transposeSemitones: Int
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

        // Get instrument and volume
        val instrumentId = if (hasNote) step.instrument else trackState.lastInstrument
        val volume = if (hasNote) params.volume else trackState.lastVolume
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
                    val arpNote = Note.fromMidi(arpMidi.coerceIn(0, 127))

                    audioEngine.scheduleNote(
                        targetFrame = triggerFrame,
                        note = arpNote,
                        instrumentId = instrumentId,
                        trackId = trackId,
                        volume = volume,
                        project = project,
                        startPointOverride = startPoint
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
            logger.d(TAG, "🎵 ARP A${trackState.arpeggioValue.toString(16).uppercase().padStart(2, '0')} " +
                    "($modeLabel, ${modeNames.getOrElse(trackState.arpeggioMode) { "UP" }})$crossStepInfo: " +
                    "$notesScheduled notes at speed ${trackState.arpeggioSpeed}")
        }
    }

    /**
     * Get the MIDI note for a specific position in the arpeggio pattern.
     *
     * @param baseMidi Base note MIDI number
     * @param semi1 First semitone offset
     * @param semi2 Second semitone offset
     * @param mode Arpeggio mode (0=UP, 1=DOWN, 2=PINGPONG, 3=RANDOM)
     * @param position Position in the pattern (0, 1, 2, or 3 for PINGPONG)
     * @return MIDI note number
     */
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
