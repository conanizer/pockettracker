package com.example.pockettracker.core.logic

import com.example.pockettracker.Note
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

        // Schedule all 16 steps
        var scheduledNotes = 0
        phrase.steps.forEachIndexed { stepIndex, step ->
            // Calculate when this step should trigger (always, even if empty)
            val targetFrame = audioEngine.calculateTargetFrame(startFrame, stepIndex, tempo)

            // Schedule note if step has one
            if (!step.isEmpty()) {
                // Get instrument and sample info
                val instrument = project.instruments[step.instrument]
                val sampleId = instrument.sampleId

                // Schedule the note
                audioEngine.scheduleNote(
                    targetFrame = targetFrame,
                    note = step.note,
                    instrumentId = step.instrument,
                    trackId = 0,  // Single track for phrase playback
                    volume = step.volume / 255.0f,
                    project = project
                )

                scheduledNotes++
                logger.d(TAG, "  Step $stepIndex: ${step.note} I:${step.instrument.toString(16).uppercase()} V:${step.volume.toString(16).uppercase()} @ frame $targetFrame")
            }

            // Apply effects (ALWAYS - effects affect STEPS not NOTES!)
            // This allows effects like Kill to work even on empty steps
            if (step.fx1Type != 0x00 || step.fx2Type != 0x00 || step.fx3Type != 0x00) {
                val instrument = project.instruments[step.instrument]
                val sampleId = instrument.sampleId

                logger.d(TAG, "  Step $stepIndex EFFECTS: FX1=0x${step.fx1Type.toString(16).uppercase()} FX2=0x${step.fx2Type.toString(16).uppercase()} FX3=0x${step.fx3Type.toString(16).uppercase()}")

                effectProcessor.applyEffects(
                    step = step,
                    baseFrame = targetFrame,
                    stepDuration = framesPerStep,
                    trackId = 0,
                    baseFrequency = step.note.toFrequency(),
                    baseVolume = step.volume / 255.0f,
                    sampleId = sampleId
                )
            }
        }

        logger.d(TAG, "✅ Scheduled $scheduledNotes notes from phrase $phraseId")

        // TODO: Implement looping (reschedule after 16 steps complete)
        // TODO: Update playback cursor in real-time for visual feedback
    }

    /**
     * Play a chain (16 phrases with transpose)
     *
     * TODO: Implement chain playback
     * - Schedule phrase sequences with transpose values
     * - Handle phrase transitions smoothly
     * - Apply per-phrase transpose
     * - 2-phrase lookahead buffering
     *
     * @param project Project containing chain data
     * @param chainId Which chain to play (0-255)
     * @param loop Whether to loop playback
     */
    fun playChain(project: Project, chainId: Int, loop: Boolean = true) {
        logger.w(TAG, "⚠️ playChain() not yet implemented - stub only")
        playbackMode = PlaybackMode.CHAIN

        // TODO: Implement chain playback logic
    }

    /**
     * Play song (8 tracks polyphonic)
     *
     * TODO: Implement song playback
     * - Schedule all 8 tracks simultaneously
     * - Per-track voice allocation
     * - Handle chain sequences per track
     * - Continuous buffering with lookahead
     *
     * @param project Project containing song data
     * @param startRow Which row to start from
     * @param loop Whether to loop playback
     */
    fun playSong(project: Project, startRow: Int = 0, loop: Boolean = true) {
        logger.w(TAG, "⚠️ playSong() not yet implemented - stub only")
        playbackMode = PlaybackMode.SONG

        // TODO: Implement song playback logic
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
