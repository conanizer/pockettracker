package com.example.pockettracker.core.logic

import com.example.pockettracker.core.data.Note
import com.example.pockettracker.core.data.PhraseStep
import com.example.pockettracker.core.data.Project
import com.example.pockettracker.core.data.ScreenType
import com.example.pockettracker.core.data.VolumeUtils
import com.example.pockettracker.core.audio.AudioEngine
import com.example.pockettracker.core.data.Chain
import com.example.pockettracker.core.data.Phrase
import com.example.pockettracker.core.logging.ILogger
import com.example.pockettracker.getEffectTypeName

/**
 * Per-track state for persistent effects and note memory.
 *
 * Used to implement M8/LGPT-style persistent effects where an effect
 * continues until cancelled by specific conditions:
 * - REPEAT (RXY) persists until:
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
    /** Last played pan (0.0=left, 0.5=center, 1.0=right) */
    var lastPan: Float = 0.5f,
    /** Last played start point override (-1 = instrument default) */
    var lastStartPoint: Int = -1,

    // Persistent REPEAT state
    /** Which FX column (1, 2, or 3) the REPEAT was set in, or 0 if not active */
    var repeatActiveColumn: Int = 0,
    /** Repeat tic interval (parsed from RXY) - max 15 ticks */
    var repeatTicInterval: Int = 0,
    /** Repeat volume ramp (0-F): 0,8=none, 1-7=decrease, 9-F=increase */
    var repeatVolRamp: Int = 0,
    /** Audio frame where REPEAT was started (for cross-phrase persistence) */
    var repeatStartFrame: Long = 0,
    /** Cumulative retrig count since repeat started (for cross-step volume ramp) */
    var repeatRetrigCount: Int = 0,
    /** Base volume when repeat started (for additive ramp across steps) */
    var repeatBaseVolume: Float = 1.0f,

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
    var arpeggioStartFrame: Long = 0,

    // HOP state (Phase 5)
    /** Target row for NEXT phrase in chain (-1 = no hop, start at row 0) */
    var hopTargetRow: Int = -1,
    /** Track is stopped by HOPFF (remains stopped until new chain starts) */
    var trackStopped: Boolean = false,

    // Pitch effect state (Phase 7)
    /** Whether pitch bend (PBN) is active */
    var pitchBendActive: Boolean = false,
    /** Whether vibrato (PVB/PVX) is active */
    var vibratoActive: Boolean = false,
    /** Last note MIDI for PSL (pitch slide) calculation */
    var lastNoteMidi: Int = -1,

    // Table override state (TBL/THO effects)
    /** Last table ID override from TBL effect (-1 = use instrument default) */
    var lastTableOverride: Int = -1,
    /** Last table start row from THO effect (-1 = default) */
    var lastTableStartRow: Int = -1,

    // Groove state (GRV effect)
    /** Active groove ID (0 = default uniform timing, 1-255 = groove table) */
    var grooveId: Int = 0,
    /** Current position within groove pattern (0-based) */
    var grooveStep: Int = 0,

    // Per-column FX memory (for RND "previously active" command)
    /** Last non-RND/RNL/CHA FX type per column (1-indexed: [0]=unused, [1]=FX1, [2]=FX2, [3]=FX3) */
    var lastColFxType: IntArray = IntArray(4),
    /** Last non-RND/RNL/CHA FX value per column */
    var lastColFxValue: IntArray = IntArray(4)
) {
    /** Check if persistent REPEAT is active */
    fun hasActiveRepeat(): Boolean = repeatActiveColumn > 0 && repeatTicInterval > 0

    /** Clear persistent REPEAT */
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

    /** Clear HOP state (called when new chain starts) */
    fun clearHopState() {
        hopTargetRow = -1
        trackStopped = false
    }

    /** Check if any pitch modulation is active */
    fun hasPitchMod(): Boolean = pitchBendActive || vibratoActive

    /** Clear pitch modulation state */
    fun clearPitchMod() {
        pitchBendActive = false
        vibratoActive = false
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
        // Clear all track states (persistent effects, note memory, HOP)
        trackStates.forEach {
            it.clearRepeat()
            it.clearAllArpeggioState()
            it.clearHopState()
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
                    // For phrase: reschedule the same phrase (looping)
                    val phrase = project.phrases[currentPhraseId]
                    val trackState = trackStates[0]

                    // Check if HOP target row is set from previous iteration
                    val hopStartRow = trackState.consumeHopTarget()
                    val effectiveStartRow = if (hopStartRow >= 0) hopStartRow else 0

                    val result = schedulePhrase(phrase, nextFrameToSchedule, 0, 0, project, framesPerStep, effectiveStartRow)
                    nextFrameToSchedule += result.framesScheduled
                }

                PlaybackMode.CHAIN -> {
                    // For chain: schedule next non-empty phrase in chain
                    val chain = project.chains[currentChainId]
                    val trackState = trackStates[0]  // Chain mode uses track 0

                    // Check if track is stopped by HOPFF
                    if (trackState.trackStopped) {
                        // Track is stopped, don't schedule more phrases
                        // Keep playback running but silent until chain ends
                        nextChainRowToSchedule = (nextChainRowToSchedule + 1) % 16
                        nextFrameToSchedule += framesPerPhrase
                        return
                    }

                    val nextRow = findNextNonEmptyChainRow(nextChainRowToSchedule, chain)
                    if (nextRow != null) {
                        val phraseId = chain.phraseRefs[nextRow]
                        val transposeSemitones = chain.getTransposeSemitones(nextRow)

                        // Check if HOP target row is set from previous phrase
                        val hopStartRow = trackState.consumeHopTarget()
                        val effectiveStartRow = if (hopStartRow >= 0) hopStartRow else 0

                        // Schedule phrase starting from HOP target row (or 0 if no HOP)
                        val result = schedulePhrase(project.phrases[phraseId], nextFrameToSchedule, 0, transposeSemitones, project, framesPerStep, effectiveStartRow)
                        chainRowStartFrames[nextRow] = nextFrameToSchedule  // Track start frame for cursor

                        // Advance frame by actual frames scheduled (accounts for groove timing + HOP)
                        nextFrameToSchedule += result.framesScheduled

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
                            // Clear trackStopped when new chain starts (new song row)
                            trackStates.forEach { it.trackStopped = false }
                            if (nextSongRowToSchedule >= songLength) {
                                nextSongRowToSchedule = 0  // Loop song
                            }
                        } else if (nextSongChainRowToSchedule < maxChainLength) {
                            // Schedule this chain row for all 8 tracks
                            var scheduledAny = false
                            var maxFramesScheduled = 0L  // Track max frames for frame advancement (groove-accurate)
                            for (trackId in 0..7) {
                                val trackState = trackStates[trackId]

                                // Skip if track is stopped by HOPFF
                                if (trackState.trackStopped) {
                                    continue
                                }

                                if (nextSongRowToSchedule < song[trackId].chainRefs.size) {
                                    val chainId = song[trackId].chainRefs[nextSongRowToSchedule]
                                    if (chainId >= 0 && chainId < 256) {
                                        val chain = project.chains[chainId]
                                        if (!chain.isEmpty(nextSongChainRowToSchedule)) {
                                            val phraseId = chain.phraseRefs[nextSongChainRowToSchedule]
                                            val transposeSemitones = chain.getTransposeSemitones(nextSongChainRowToSchedule)

                                            // Check if HOP target row is set from previous phrase
                                            val hopStartRow = trackState.consumeHopTarget()
                                            val effectiveStartRow = if (hopStartRow >= 0) hopStartRow else 0

                                            // Schedule phrase with HOP start row
                                            val result = schedulePhrase(project.phrases[phraseId], nextFrameToSchedule, trackId, transposeSemitones, project, framesPerStep, effectiveStartRow)
                                            songPositionStartFrames[Pair(nextSongRowToSchedule, nextSongChainRowToSchedule)] = nextFrameToSchedule
                                            scheduledAny = true

                                            // Track max frames scheduled across all tracks (groove-accurate)
                                            if (result.framesScheduled > maxFramesScheduled) {
                                                maxFramesScheduled = result.framesScheduled
                                            }
                                        }
                                    }
                                }
                            }

                            if (scheduledAny) {
                                // Advance by max frames scheduled (so all tracks stay in sync)
                                // Tracks with HOP will have empty time before their next phrase
                                nextFrameToSchedule += maxFramesScheduled
                                nextSongChainRowToSchedule++
                            } else {
                                // No phrases in this chain row, skip to next
                                nextSongChainRowToSchedule++
                            }
                        } else {
                            // Reached end of all chain rows in this song row, move to next song row
                            nextSongRowToSchedule++
                            nextSongChainRowToSchedule = 0
                            // Clear trackStopped when new chain starts (new song row)
                            trackStates.forEach { it.trackStopped = false }
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
        val result = schedulePhrase(phrase, playbackStartFrame, 0, 0, project, framesPerStep)
        nextFrameToSchedule += result.framesScheduled

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
    /**
     * Result of scheduling a phrase, including how many rows were actually played.
     * Used to calculate correct frame advancement when HOP cuts phrase short.
     */
    data class SchedulePhraseResult(
        val rowsScheduled: Int,      // How many rows were actually scheduled (may be < 16 due to HOP)
        val hopTriggered: Boolean,   // Whether HOP effect ended phrase early
        val trackStopped: Boolean,   // Whether HOPFF stopped the track
        val framesScheduled: Long = 0L  // Actual frames used (accounts for groove timing)
    )

    /**
     * Schedule a single phrase for playback.
     *
     * @param phrase The phrase to schedule
     * @param startFrame When this phrase should start (audio frame)
     * @param trackId Which track (0-7)
     * @param transposeSemitones Semitones to transpose
     * @param project Project containing instrument data
     * @param framesPerStep Duration of each step in frames
     * @param startRow Row to start from (0-15), used for HOP effect (default 0)
     * @return SchedulePhraseResult with info about rows scheduled and HOP status
     */
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

        // If track is stopped by HOPFF, don't schedule anything
        if (trackState.trackStopped) {
            logger.d(TAG, "  Track $trackId stopped by HOP FF, skipping phrase")
            return SchedulePhraseResult(0, hopTriggered = false, trackStopped = true, framesScheduled = 0L)
        }

        // framesPerTic = base frame duration per tic (constant at any tempo).
        // Step durations are groove-controlled: each step may have a different tic count.
        val framesPerTic = framesPerStep / TICS_PER_STEP
        var localGrooveStep = trackState.grooveStep
        var anyGrooveActive = false  // Becomes true if any step uses a groove (for post-loop persistence)

        // Schedule steps starting from startRow
        val effectiveStartRow = startRow.coerceIn(0, 15)
        var frameOffset = 0L  // Accumulates actual frame count (may differ from stepIndex * framesPerStep with groove)

        for (stepIndex in effectiveStartRow until 16) {
            val step = phrase.steps[stepIndex]

            // Pre-scan for GRV effect BEFORE computing step duration so the new groove
            // takes effect immediately on the step that triggers it, not on the next phrase.
            // scheduleStepWithEffects will also process GRV (idempotent — same values).
            for (fxSlot in 1..3) {
                val fxType = when (fxSlot) { 1 -> step.fx1Type; 2 -> step.fx2Type; else -> step.fx3Type }
                val fxValue = when (fxSlot) { 1 -> step.fx1Value; 2 -> step.fx2Value; else -> step.fx3Value }
                if (fxType == EffectProcessor.FX_GRV) {
                    trackState.grooveId = fxValue
                    localGrooveStep = 0  // New groove always starts at slot 0
                    break
                }
            }

            // Look up current groove per-step (grooveId may have changed via GRV above).
            // Fall back to exact framesPerStep when groove has no active steps to avoid
            // integer rounding drift in the common case.
            val currentGroove = project.grooves[trackState.grooveId.coerceIn(0, 255)]
            val currentGrooveActive = currentGroove.activeLength() > 0

            val stepDuration = if (currentGrooveActive) {
                anyGrooveActive = true
                val stepTics = currentGroove.getTicksForStep(localGrooveStep)
                framesPerTic * stepTics  // 0 tics = skip step
            } else {
                framesPerStep
            }

            // Groove value 00: skip this phrase step entirely (no note, no effects, no time advance)
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
                    logger.d(TAG, "  HOP FF at row $stepIndex: track $trackId stopped, scheduled $rowsScheduled rows")
                } else {
                    logger.d(TAG, "  HOP at row $stepIndex: jumping to row ${trackState.hopTargetRow}, scheduled $rowsScheduled rows")
                }
                return SchedulePhraseResult(rowsScheduled, hopTriggered = true, trackStopped = trackState.trackStopped, framesScheduled = frameOffset)
            }
        }

        // Persist groove position for the next phrase (if groove was active at any point this phrase)
        if (anyGrooveActive) trackState.grooveStep = localGrooveStep

        if (scheduledNotes > 0) {
            if (effectiveStartRow > 0) {
                logger.d(TAG, "  Scheduled $scheduledNotes notes (starting from row $effectiveStartRow due to HOP)")
            } else {
                logger.d(TAG, "  Scheduled $scheduledNotes notes")
            }
        }

        return SchedulePhraseResult(rowsScheduled, hopTriggered = false, trackStopped = false, framesScheduled = frameOffset)
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
            val result = schedulePhrase(project.phrases[phraseId], playbackStartFrame, 0, transposeSemitones, project, framesPerStep)
            chainRowStartFrames[firstRow] = playbackStartFrame  // Track start frame for cursor
            nextFrameToSchedule += result.framesScheduled
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
                pan = 0.5f,  // Center
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
     * - REPEAT effect (RXY) - M8-style retrigger with volume ramping
     *
     * ## REPEAT (RXY) - M8-style format:
     * - R00 = cancel persistent REPEAT
     * - RX0 (Y=0): retrig every X ticks, no vol ramp
     * - RXY (Y!=0): retrig every Y ticks, vol ramp X
     *   - X=0,8: no volume change
     *   - X=1-7: decrease volume per retrig
     *   - X=9-F: increase volume per retrig
     *
     * REPEAT persists until cancelled by:
     * 1. A new note on this track
     * 2. Any effect in the same FX column where REPEAT was set
     * 3. KILL effect (K00) in any FX column
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
    /**
     * Result of scheduling a single step.
     */
    data class ScheduleStepResult(
        val noteScheduled: Boolean,  // Whether a note was scheduled
        val hopTriggered: Boolean    // Whether HOP effect was triggered (phrase should end)
    )

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
            logger.d(TAG, "🔪 KILL detected → persistent REPEAT and ARPEGGIO cancelled")
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
            // Inline delta lookup to calculate last accumulated volume
            val rampDeltas = floatArrayOf(0f, -0.02f, -0.04f, -0.06f, -0.10f, -0.15f, -0.20f, -0.30f,
                                          0f, 0.02f, 0.04f, 0.06f, 0.10f, 0.15f, 0.20f, 0.30f)
            val oldDelta = rampDeltas[trackState.repeatVolRamp.coerceIn(0, 15)]
            (trackState.repeatBaseVolume + trackState.repeatRetrigCount * oldDelta).coerceIn(0f, 1f)
        } else {
            -1f // No active ramp to preserve
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
        // STEP 2: Handle CHA (Chance), resolve effects, and schedule note
        // ═══════════════════════════════════════════════════════════════════════════

        // CHA xy: probability gate (evaluated BEFORE effect resolution)
        // x = probability (0=never, F=always), y = target (0=note, 1-3=FX slot)
        var skipNote = false
        var effectiveStep = step
        for (slot in 1..3) {
            val (fxType, fxValue) = when (slot) {
                1 -> step.fx1Type to step.fx1Value
                2 -> step.fx2Type to step.fx2Value
                3 -> step.fx3Type to step.fx3Value
                else -> continue
            }
            if (fxType == EffectProcessor.FX_CHA && fxValue > 0) {
                val probability = (fxValue shr 4) and 0x0F
                val target = fxValue and 0x0F
                val roll = kotlin.random.Random.nextInt(15)  // 0-14
                val passed = roll < probability  // probability F (15) always passes, 0 never passes

                if (!passed) {
                    val targetName = if (target == 0) "note" else "FX$target"
                    logger.d(TAG, "🎲 CHA: probability=$probability/15, roll=$roll → BLOCKED $targetName")
                    if (target == 0) {
                        skipNote = true
                    } else if (target in 1..3) {
                        // Zero out the targeted FX slot before resolution
                        effectiveStep = effectiveStep.copy().also { s ->
                            when (target) {
                                1 -> { s.fx1Type = 0x00; s.fx1Value = 0x00 }
                                2 -> { s.fx2Type = 0x00; s.fx2Value = 0x00 }
                                3 -> { s.fx3Type = 0x00; s.fx3Value = 0x00 }
                            }
                        }
                    }
                } else {
                    val targetName = if (target == 0) "note" else "FX$target"
                    logger.d(TAG, "🎲 CHA: probability=$probability/15, roll=$roll → PASSED $targetName")
                }
            }
        }

        // RND/RNL xy: randomize FX values (evaluated BEFORE effect resolution)
        // x = min high nibble, y = max high nibble → random value in range [x0, yF]
        //
        // RND: Randomizes the PREVIOUSLY ACTIVE FX in the same column (temporal).
        //   Replaces itself with the last FX type+randomized value from this column.
        //   If no previous FX exists, does nothing.
        //
        // RNL: Randomizes the FX to the LEFT in the same row (spatial).
        //   FX2 → randomizes FX1 value, FX3 → randomizes FX2 value.
        //   In FX1: randomizes note (X range) and instrument (Y range).
        for (slot in 1..3) {
            val (fxType, fxValue) = when (slot) {
                1 -> effectiveStep.fx1Type to effectiveStep.fx1Value
                2 -> effectiveStep.fx2Type to effectiveStep.fx2Value
                3 -> effectiveStep.fx3Type to effectiveStep.fx3Value
                else -> continue
            }

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
                effectiveStep = effectiveStep.copy().also { s ->
                    when (slot) {
                        1 -> { s.fx1Type = prevType; s.fx1Value = randomValue }
                        2 -> { s.fx2Type = prevType; s.fx2Value = randomValue }
                        3 -> { s.fx3Type = prevType; s.fx3Value = randomValue }
                    }
                }
                logger.d(TAG, "🎲 RND: FX$slot recalled ${getEffectTypeName(prevType)} → " +
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
                                note = Note.fromMidi((noteMidi + noteOffset).coerceIn(0, 119)),
                                instrument = (step.instrument + instOffset).coerceIn(0, 255)
                            )
                            logger.d(TAG, "🎲 RNL FX1: note ${step.note}→${effectiveStep.note} " +
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

                    effectiveStep = effectiveStep.copy().also { s ->
                        when (targetSlot) {
                            1 -> s.fx1Value = randomValue
                            2 -> s.fx2Value = randomValue
                        }
                    }
                    val targetType = when (targetSlot) {
                        1 -> getEffectTypeName(effectiveStep.fx1Type)
                        2 -> getEffectTypeName(effectiveStep.fx2Type)
                        else -> "???"
                    }
                    logger.d(TAG, "🎲 RNL: FX$targetSlot ($targetType) value → " +
                            "0x${randomValue.toString(16).uppercase().padStart(2, '0')} " +
                            "(range ${minNibble.toString(16).uppercase()}0-${maxNibble.toString(16).uppercase()}F)")
                }
            }
        }

        val defaultVolume = effectiveStep.volume / 255.0f
        val params = effectProcessor.resolveStepParams(effectiveStep, targetFrame, defaultVolume)

        // Apply volume chain: instrument × phrase only
        // NOTE: Track × master are applied in C++ in real-time, allowing mixer changes
        // to take effect immediately without rescheduling notes
        val instrument = project.instruments[effectiveStep.instrument]
        val finalVolume = VolumeUtils.calculateNoteVolume(
            instrumentVol = instrument.volume,
            phraseVol = (params.volume * 255).toInt().coerceIn(0, 255)  // Convert back to hex
        )

        // Get instrument pan (hex 0x00-0xFF → float 0.0-1.0)
        val instrumentPan = VolumeUtils.hexToFloat(instrument.pan)

        // ═══════════════════════════════════════════════════════════════════════════
        // STEP 2.1: Apply DEL (Delay) effect - offset the target frame
        // ═══════════════════════════════════════════════════════════════════════════
        val delayTicks = params.delayTicks ?: 0
        val effectiveTargetFrame = if (delayTicks > 0) {
            val framesPerTic = stepDuration / TICS_PER_STEP
            val delayFrames = delayTicks * framesPerTic
            logger.d(TAG, "⏳ DEL: delaying note by $delayTicks ticks ($delayFrames frames)")
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
            logger.d(TAG, "📋 TBL: Override table to ${params.tableOverride.toString(16).uppercase().padStart(2, '0')}")
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
                logger.d(TAG, "📋 THO: Jumped active voice table to row $targetRow (no note)")
            } else {
                logger.d(TAG, "📋 THO: Will start table at row $targetRow (with note)")
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
                logger.d(TAG, "🥁 GRV 00: Disabled groove (default timing)")
            } else {
                logger.d(TAG, "🥁 GRV: Assigned groove ${params.grooveId.toString(16).uppercase().padStart(2, '0')}")
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
                    logger.d(TAG, "🎵 PSL: Portamento from ${Note.fromMidi(previousMidi)} to $note " +
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
                logger.d(TAG, "🎵 PBN ${params.pbnValue.toString(16).uppercase().padStart(2, '0')}: " +
                        "Bend $direction at $pbnRate semitones/tick")
            }

            // PVB: Calculate standard vibrato
            if (params.pvbValue != null && params.pvbValue != 0) {
                val speedNibble = (params.pvbValue shr 4) and 0x0F
                val depthNibble = params.pvbValue and 0x0F
                vibratoSpeed = 2f + speedNibble * 0.5f
                vibratoDepth = depthNibble * 0.125f
                trackState.vibratoActive = true
                logger.d(TAG, "🎵 PVB ${params.pvbValue.toString(16).uppercase().padStart(2, '0')}: " +
                        "Vibrato speed=${vibratoSpeed}Hz, depth=$vibratoDepth semitones")
            }

            // PVX: Calculate extreme vibrato (2x speed, 4x depth)
            if (params.pvxValue != null && params.pvxValue != 0) {
                val speedNibble = (params.pvxValue shr 4) and 0x0F
                val depthNibble = params.pvxValue and 0x0F
                vibratoSpeed = (2f + speedNibble * 0.5f) * 2f  // 2x speed
                vibratoDepth = depthNibble * 0.125f * 4f       // 4x depth
                trackState.vibratoActive = true
                logger.d(TAG, "🎵 PVX ${params.pvxValue.toString(16).uppercase().padStart(2, '0')}: " +
                        "EXTREME vibrato speed=${vibratoSpeed}Hz, depth=$vibratoDepth semitones")
            }

            // Debug: Log the full volume chain so we can verify what reaches the audio engine
            logger.d(TAG, "🔊 Volume chain: instVol=0x${instrument.volume.toString(16).uppercase()} " +
                    "(${instrument.volume}/255=${"%.4f".format(instrument.volume / 255f)}), " +
                    "phraseVol=0x${step.volume.toString(16).uppercase()} " +
                    "(${step.volume}/255=${"%.4f".format(step.volume / 255f)}), " +
                    "finalVolume=${"%.4f".format(finalVolume)}")

            // Schedule the note with all pitch mod params (applied when note triggers in C++)
            audioEngine.scheduleNote(
                targetFrame = effectiveTargetFrame,
                note = note,
                instrumentId = effectiveStep.instrument,
                trackId = trackId,
                volume = finalVolume,
                pan = instrumentPan,
                project = project,
                startPointOverride = params.startPoint,
                pslInitialOffset = pslInitialOffset,
                pslDuration = pslDuration,
                pbnRate = pbnRate,
                vibratoSpeed = vibratoSpeed,
                vibratoDepth = vibratoDepth,
                tableIdOverride = tableIdOverride,
                tableStartRow = tableStartRow
            )
            noteScheduled = true

            // Update track state with this note (for persistent REPEAT retrigger)
            trackState.lastNote = note
            trackState.lastInstrument = effectiveStep.instrument
            trackState.lastVolume = finalVolume
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
            audioEngine.scheduleKill(killFrame, trackId)
            trackState.clearPitchMod()
        }

        // ═══════════════════════════════════════════════════════════════════════════
        // STEP 2.4: Handle Pitch Effects on steps WITHOUT notes (mid-note changes)
        // ═══════════════════════════════════════════════════════════════════════════
        // PBN/PVB/PVX on empty steps modify the currently playing voice
        // This allows changing pitch mod mid-note without retriggering

        if (!hasNote) {
            val tempo = currentProject?.tempo ?: 120

            // Handle PBN (Pitch Bend) - modify currently playing voice
            if (params.pbnValue != null) {
                if (params.pbnValue == 0) {
                    // PBN 00: Stop pitch bend
                    audioEngine.setPitchBend(trackId, 0f, tempo)
                    trackState.pitchBendActive = false
                    logger.d(TAG, "🎵 PBN 00: Pitch bend stopped (mid-note)")
                } else {
                    val semitonesPerTick = if (params.pbnValue < 0x80) {
                        params.pbnValue / 16f
                    } else {
                        -((params.pbnValue and 0x7F) / 16f)
                    }
                    audioEngine.setPitchBend(trackId, semitonesPerTick, tempo)
                    trackState.pitchBendActive = true
                    val direction = if (params.pbnValue < 0x80) "UP" else "DOWN"
                    logger.d(TAG, "🎵 PBN ${params.pbnValue.toString(16).uppercase().padStart(2, '0')}: " +
                            "Bend $direction (mid-note)")
                }
            }

            // Handle PVB (Vibrato) - modify currently playing voice
            if (params.pvbValue != null) {
                if (params.pvbValue == 0) {
                    audioEngine.setVibrato(trackId, 0f, 0f)
                    trackState.vibratoActive = false
                    logger.d(TAG, "🎵 PVB 00: Vibrato stopped (mid-note)")
                } else {
                    val speedNibble = (params.pvbValue shr 4) and 0x0F
                    val depthNibble = params.pvbValue and 0x0F
                    val speed = 2f + speedNibble * 0.5f
                    val depth = depthNibble * 0.125f
                    audioEngine.setVibrato(trackId, speed, depth)
                    trackState.vibratoActive = true
                    logger.d(TAG, "🎵 PVB ${params.pvbValue.toString(16).uppercase().padStart(2, '0')}: " +
                            "Vibrato (mid-note)")
                }
            }

            // Handle PVX (Extreme Vibrato) - modify currently playing voice
            if (params.pvxValue != null) {
                if (params.pvxValue == 0) {
                    audioEngine.setVibrato(trackId, 0f, 0f)
                    trackState.vibratoActive = false
                    logger.d(TAG, "🎵 PVX 00: Extreme vibrato stopped (mid-note)")
                } else {
                    val speedNibble = (params.pvxValue shr 4) and 0x0F
                    val depthNibble = params.pvxValue and 0x0F
                    val speed = (2f + speedNibble * 0.5f) * 2f
                    val depth = depthNibble * 0.125f * 4f
                    audioEngine.setVibrato(trackId, speed, depth)
                    trackState.vibratoActive = true
                    logger.d(TAG, "🎵 PVX ${params.pvxValue.toString(16).uppercase().padStart(2, '0')}: " +
                            "EXTREME vibrato (mid-note)")
                }
            }
        }

        // ═══════════════════════════════════════════════════════════════════════════
        // STEP 2.5: Handle HOP effect (Phase 5)
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
                logger.d(TAG, "🦘 HOP FF: Track $trackId stopped at step $stepIndex")
            } else {
                // HOP XY: Set target row for next phrase (low nibble = Y)
                val targetRow = params.hopValue and 0x0F
                trackState.hopTargetRow = targetRow
                logger.d(TAG, "🦘 HOP: Track $trackId jumping at step $stepIndex, next phrase starts at row $targetRow")
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
                hasNote -> finalVolume  // New note: fresh start at note volume
                savedRampVolume >= 0f -> savedRampVolume  // RPT-to-RPT on empty step: continue from last ramp position
                else -> trackState.lastVolume  // Fallback: use last played note volume
            }

            val rampDesc = when {
                newRepeatVolRamp == 0 || newRepeatVolRamp == 8 -> ""
                newRepeatVolRamp in 1..7 -> ", vol decrease $newRepeatVolRamp"
                else -> ", vol increase ${newRepeatVolRamp - 8}"
            }
            logger.d(TAG, "🔁 REPEAT: retrig every $newRepeatTicInterval ticks$rampDesc " +
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

        // Volume ramp: ADDITIVE delta per retrigger (accumulates across steps)
        // 0,8 = no change; 1-7 = decrease; 9-F = increase
        // Values are subtracted/added to the base volume per retrig
        val volRampDeltas = floatArrayOf(
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
            val rampDelta = volRampDeltas[activeVolRamp.coerceIn(0, 15)]

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
                        tableIdOverride = trackState.lastTableOverride
                    )
                    logger.d(TAG, "🔁 retrig[${trackState.repeatRetrigCount}] vol=${"%.4f".format(retrigVolume)} " +
                            "(base=${"%.4f".format(trackState.repeatBaseVolume)}, delta=$rampDelta)")
                }

                val isPersistent = !hasNote && trackState.hasActiveRepeat()
                val modeLabel = if (isPersistent) "PERSISTENT" else "step"
                logger.d(TAG, "🔁 REPEAT ($modeLabel): ${triggersCount} triggers, interval=$activeRepeatInterval ticks, delta=$rampDelta")
            } else {
                // ═══════════════════════════════════════════════════════════════════
                // MULTI-STEP REPEAT (interval > 12 ticks): Sparse triggers
                // ═══════════════════════════════════════════════════════════════════
                val triggerIntervalFrames = activeRepeatInterval * framesPerTic
                val stepEndFrame = targetFrame + stepDuration

                if (hasNote && newRepeatTicInterval > 0) {
                    logger.d(TAG, "🔁 REPEAT (multi-step): started, interval=$activeRepeatInterval ticks")
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
                                    tableIdOverride = trackState.lastTableOverride
                                )
                                triggersInStep++
                            }
                            triggerFrame += triggerIntervalFrames
                        }

                        if (triggersInStep > 0) {
                            logger.d(TAG, "🔁 REPEAT (PERSISTENT multi-step): $triggersInStep trigger(s)")
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
                step = effectiveStep,
                params = params,
                transposeSemitones = transposeSemitones,
                finalVolume = finalVolume,
                finalPan = instrumentPan
            )
        }

        // Update per-column FX memory for RND (stores "previously active" FX per column)
        // Only store real effects, not meta-effects (RND, RNL, CHA, NONE)
        val metaEffects = setOf(EffectProcessor.FX_NONE, EffectProcessor.FX_RND, EffectProcessor.FX_RNL, EffectProcessor.FX_CHA)
        for (col in 1..3) {
            val (fxType, fxValue) = when (col) {
                1 -> step.fx1Type to step.fx1Value
                2 -> step.fx2Type to step.fx2Value
                3 -> step.fx3Type to step.fx3Value
                else -> continue
            }
            if (fxType !in metaEffects) {
                trackState.lastColFxType[col] = fxType
                trackState.lastColFxValue[col] = fxValue
            }
        }

        return ScheduleStepResult(noteScheduled = noteScheduled, hopTriggered = hopTriggered)
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
     * @param finalVolume Pre-calculated volume (inst × phrase); track × master applied in C++
     * @param finalPan Pre-calculated pan (0.0=left, 0.5=center, 1.0=right)
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
        transposeSemitones: Int,
        finalVolume: Float,
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

        // Get instrument, volume, and pan (finalVolume has inst × phrase; track × master in C++)
        val instrumentId = if (hasNote) step.instrument else trackState.lastInstrument
        val arpVolume = if (hasNote) finalVolume else trackState.lastVolume
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
                    val arpNote = Note.fromMidi(arpMidi.coerceIn(0, 127))

                    audioEngine.scheduleNote(
                        targetFrame = triggerFrame,
                        note = arpNote,
                        instrumentId = instrumentId,
                        trackId = trackId,
                        volume = arpVolume,
                        pan = arpPan,
                        project = project,
                        startPointOverride = startPoint,
                        tableIdOverride = trackState.lastTableOverride
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
