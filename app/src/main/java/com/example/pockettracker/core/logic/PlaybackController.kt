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
 * PlaybackController
 *
 * Manages all playback operations including:
 * - Playback state (playing/stopped)
 * - Phrase/chain/song playback scheduling with continuous lookahead buffering
 * - Sample-accurate note queue management
 * - Playback cursors and position tracking
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
        phrase.steps.forEachIndexed { stepIndex, step ->
            val targetFrame = startFrame + (stepIndex * framesPerStep)

            val noteScheduled = scheduleStepWithEffects(
                step = step,
                targetFrame = targetFrame,
                stepDuration = framesPerStep,
                trackId = trackId,
                transposeSemitones = transposeSemitones,
                project = project
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

        // Handle REPEAT effect (Rxx) - retrigger note every xx tics within step
        // Uses tic-interval approach (LGPT/M8 style):
        // - Rxx = retrig every xx tics (where step = TICS_PER_STEP tics)
        // - R00 = no effect
        // - R01 = retrig every 1 tic = 12 triggers/step (fastest, with 12 tics/step)
        // - R02 = retrig every 2 tics = 6 triggers/step
        // - R03 = retrig every 3 tics = 4 triggers/step (triplets!)
        // - R04 = retrig every 4 tics = 3 triggers/step
        // - R06 = retrig every 6 tics = 2 triggers/step
        // - R0C = retrig every 12 tics = 1 trigger/step (no effect with 12 tics/step)
        // - R0D+ = no effect (interval > step)
        if (params.repeatCount != null && params.repeatCount > 0 && params.repeatCount < TICS_PER_STEP && !step.isEmpty()) {
            val ticInterval = params.repeatCount

            // Calculate timing
            val framesPerTic = stepDuration / TICS_PER_STEP
            val triggersCount = TICS_PER_STEP / ticInterval  // Integer division

            // Main note is already scheduled at tic 0 (targetFrame)
            // Schedule additional retriggers at subsequent tic intervals
            // Retrigger at tics: ticInterval, 2*ticInterval, 3*ticInterval, etc.
            for (i in 1 until triggersCount) {
                val ticPosition = i * ticInterval
                val retrigFrame = targetFrame + (ticPosition * framesPerTic)

                // Apply same transposition as main note
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

                audioEngine.scheduleNote(
                    targetFrame = retrigFrame,
                    note = note,
                    instrumentId = step.instrument,
                    trackId = trackId,
                    volume = params.volume,
                    project = project,
                    startPointOverride = params.startPoint  // Apply OFFSET to retriggers too
                )
            }

            logger.d(TAG, "🔁 REPEAT R${ticInterval.toString(16).uppercase().padStart(2, '0')}: " +
                    "$triggersCount triggers (every $ticInterval tics, ${ticInterval * framesPerTic} frames)")
        }

        // TODO: Handle ARPEGGIO effect (schedule 3 notes)
        // if (params.arpeggioValue != null) { ... }

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
