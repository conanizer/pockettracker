package com.example.pockettracker.core.audio

/**
 * Platform-agnostic audio backend interface.
 *
 * This interface abstracts audio operations so they work on any platform.
 * The actual implementation is platform-specific (Oboe on Android, ALSA on Linux).
 *
 * Implementations:
 * - Android: OboeAudioBackend (wraps native Oboe C++ code via JNI)
 * - Linux: ALSAAudioBackend (future - will use ALSA/PulseAudio)
 *
 * Design Philosophy:
 * - Keep interface minimal (only what's needed for playback)
 * - No Android-specific types (no Context, no Resources)
 * - All methods are synchronous (async handled by implementation)
 * - Thread-safe by contract (implementations must handle thread safety)
 */
interface IAudioBackend {
    /**
     * Initialize the audio stream.
     *
     * This must be called before any other operations.
     * On Android: Initializes Oboe stream (LowLatency + Exclusive mode)
     * On Linux: Will initialize ALSA/PulseAudio stream
     *
     * @return true if successful, false if audio initialization failed
     */
    fun create(): Boolean

    /**
     * Load a sample into the specified slot.
     *
     * The sample is stored in memory and can be played via scheduleNote().
     * Samples are resampled automatically to match the audio stream rate.
     *
     * @param id Sample slot (0-255)
     * @param samples Float array of audio samples (mono, -1.0 to 1.0)
     */
    fun loadSample(id: Int, samples: FloatArray)

    /**
     * Schedule a note to play at a specific audio frame.
     *
     * This is the core of sample-accurate playback. Notes are scheduled ahead of time
     * and triggered exactly at the specified frame number.
     *
     * @param frame Absolute audio frame number (from getCurrentFrame())
     * @param sampleId Which sample to play (0-255)
     * @param trackId Which track this note belongs to (0-7, for voice stealing)
     * @param freq Target playback frequency in Hz
     * @param baseFreq Base frequency of the sample (for pitch calculation)
     * @param vol Volume (0.0 to 1.0)
     * @param pan Stereo pan position (0.0=left, 0.5=center, 1.0=right)
     * @param startPointOverride Optional start point override (0-65535, overrides instrument start point, -1 = use default)
     */
    fun scheduleNote(
        frame: Long,
        sampleId: Int,
        trackId: Int,
        freq: Float,
        baseFreq: Float,
        vol: Float,
        pan: Float = 0.5f,
        startPointOverride: Int = -1
    )

    /**
     * Get current audio frame counter.
     *
     * This is a global frame counter that increments with every audio sample rendered.
     * Used for sample-accurate scheduling (see scheduleNote).
     *
     * @return Current frame number (starts at 0 when stream is created)
     */
    fun getCurrentFrame(): Long

    /**
     * Clear all scheduled notes from the queue.
     *
     * Used when stopping playback or resetting the sequencer.
     * Notes currently playing are NOT stopped (use stopAll() for that).
     */
    fun clearScheduledNotes()

    /**
     * Resume the audio stream after it was paused.
     *
     * This is needed because some platforms pause the audio stream when inactive.
     * Call this before scheduling notes to ensure the audio callback is running.
     */
    fun resumeStream()

    /**
     * Stop all currently playing voices immediately.
     *
     * This silences all audio output and resets all voices to inactive state.
     * Does NOT clear the schedule queue (use clearScheduledNotes for that).
     */
    fun stopAll()

    /**
     * Stop a specific track's voice immediately.
     *
     * Used for Kill effect (K00) - stops the voice on the specified track.
     *
     * @param trackId Which track to kill (0-7)
     */
    fun killTrack(trackId: Int)

    /**
     * Schedule a kill event at a specific audio frame.
     *
     * This schedules a track kill to happen at a specific frame time,
     * allowing sample-accurate kill effects.
     *
     * @param frame Absolute audio frame number when to kill
     * @param trackId Which track to kill (0-7)
     */
    fun scheduleKill(frame: Long, trackId: Int)

    /**
     * Get the actual sample rate of the audio stream.
     *
     * This may differ from the requested rate (e.g., requested 44100Hz but got 48000Hz).
     * Used for timing calculations and sample rate compensation.
     *
     * @return Sample rate in Hz (typically 44100 or 48000)
     */
    fun getSampleRate(): Int

    /**
     * Update the waveform visualization buffer.
     *
     * This captures the current mixed audio output for visualization (oscilloscope).
     * The buffer is filled with recent audio samples.
     *
     * @param buffer Float array to fill with waveform data (caller allocates)
     */
    fun updateWaveform(buffer: FloatArray)

    /**
     * Set playback parameters for an instrument.
     *
     * This configures how a sample is played back (start/end points, looping, effects).
     *
     * @param instrumentId Instrument slot (0-255)
     * @param startPoint Sample start position (0-255, mapped to sample length)
     * @param endPoint Sample end position (0-255, mapped to sample length)
     * @param reverse Play backwards if true
     * @param loopMode 0=off, 1=forward loop, 2=ping-pong loop
     * @param loopStart Loop restart position (0-255)
     * @param drive Distortion amount (0-255)
     * @param crush Bit crushing amount (0-15, 0=16-bit, 15=1-bit)
     * @param downsample Downsampling factor (0-15, 0=no downsampling)
     * @param filterType 0=off, 1=lowpass, 2=highpass, 3=bandpass
     * @param filterCut Filter cutoff frequency (0-255)
     * @param filterRes Filter resonance (0-255)
     */
    fun setInstrumentParams(
        instrumentId: Int,
        startPoint: Int,
        endPoint: Int,
        reverse: Boolean,
        loopMode: Int,
        loopStart: Int,
        drive: Int,
        crush: Int,
        downsample: Int,
        filterType: Int,
        filterCut: Int,
        filterRes: Int
    )

    /**
     * Release audio resources and close stream.
     *
     * This should be called when the app is closing or audio is no longer needed.
     * After calling this, create() must be called again to use audio.
     */
    fun close()
}
