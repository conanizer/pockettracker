package com.conanizer.pockettracker.core.audio

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
     * Unload all samples from all instrument slots (0-255).
     *
     * Called when creating a new project so that instruments that previously had
     * samples loaded do not play audio with stale data.
     */
    fun clearAllSamples()

    // Sample editor operations
    fun getSampleLength(id: Int): Int
    fun getSampleWaveform(id: Int, numBins: Int): FloatArray
    fun getSampleWaveformRange(id: Int, startFrame: Int, endFrame: Int, numBins: Int): FloatArray
    fun getSampleData(id: Int): FloatArray
    fun normalizeSample(id: Int, startFrame: Int, endFrame: Int)
    fun fadeInSample(id: Int, startFrame: Int, endFrame: Int)
    fun fadeOutSample(id: Int, startFrame: Int, endFrame: Int)
    fun silenceRegion(id: Int, startFrame: Int, endFrame: Int)
    fun reverseSample(id: Int, startFrame: Int, endFrame: Int)
    fun backupSample(id: Int)
    fun undoSample(id: Int)
    fun getSamplePlaybackPosition(id: Int): Float
    fun cropSample(id: Int, startFrame: Int, endFrame: Int)
    fun deleteSampleRegion(id: Int, startFrame: Int, endFrame: Int)
    fun copyRegion(id: Int, startFrame: Int, endFrame: Int)
    fun pasteRegion(id: Int, insertAt: Int)
    fun getClipboardLength(): Int
    // Returns nearest zero-crossing frame within ±512 frames, or `frame` if none found.
    fun findZeroCrossing(id: Int, frame: Int): Int

    /**
     * Returns an IntArray[8] where each element is (octave * 12 + pitch) for the active voice
     * on that track, or -1 if no voice is currently playing on that track.
     */
    fun getTrackActiveNotes(): IntArray

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
     * Clear only notes/kills scheduled at or after [fromFrame].
     * Notes before that frame (currently playing phrase) are preserved.
     */
    fun clearScheduledNotesFrom(fromFrame: Long)

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

    /**
     * Get per-track peak levels for mixer meters.
     *
     * Returns an array of 16 floats (0.0-1.0) representing stereo peak levels
     * for tracks 0-7, interleaved [L0, R0, L1, R1, ...]. Values decay over time.
     *
     * @param buffer Float array of size 16 to fill with stereo peak levels
     */
    fun getTrackPeaks(buffer: FloatArray)

    /**
     * Get master stereo peak levels for mixer meters.
     *
     * Returns an array of 2 floats (0.0-1.0) representing peak levels
     * for left and right channels. Values decay over time.
     *
     * @param buffer Float array of size 2 to fill with [left, right] peak levels
     */
    fun getMasterPeaks(buffer: FloatArray)

    /** Returns 4 floats [revL, revR, delL, delR] — reverb and delay send return peaks. */
    fun getSendPeaks(buffer: FloatArray)

    // ===================================
    // OFFLINE RENDER (for WAV export)
    // ===================================

    /**
     * Render a specified number of audio frames offline.
     *
     * This processes scheduled notes and voices without using the audio stream,
     * suitable for rendering to a file.
     *
     * @param numFrames Number of frames to render
     * @param sampleRate Sample rate in Hz (typically 44100)
     * @return Interleaved stereo float array [L0, R0, L1, R1, ...]
     */
    fun renderFrames(numFrames: Int, sampleRate: Int): FloatArray

    /**
     * Reset the frame counter to 0.
     * Call this before starting a new render to ensure proper timing.
     */
    fun resetFrameCounter()

    /**
     * Get the current frame counter value.
     * @return Current frame count (frames processed since last reset)
     */
    fun getFrameCounter(): Long

    // ===================================
    // PHASE 1 BUG FIXES: DECAY AND REAL-TIME VOLUME
    // ===================================

    /**
     * Manually decay peak levels.
     * Call this periodically when audio stream is not running (playback stopped)
     * to smoothly fade out the mixer meters.
     */
    fun decayPeaks()

    /**
     * Manually decay waveform buffer.
     * Call this periodically when audio stream is not running (playback stopped)
     * to smoothly fade out the oscilloscope display.
     */
    fun decayWaveform()

    /**
     * Set real-time track volume.
     * This affects playback immediately without needing to reschedule notes.
     *
     * @param trackId Track index (0-7)
     * @param volume Volume level (0.0 to 1.0)
     */
    fun setTrackVolume(trackId: Int, volume: Float)

    /**
     * Set real-time master volume.
     * This affects playback immediately without needing to reschedule notes.
     *
     * @param volume Volume level (0.0 to 1.0)
     */
    fun setMasterVolume(volume: Float)

    // Set OTT depth (0=bypass, 255=full wet). Enables/disables OTT module.
    fun setOttDepth(depth: Int)
    // Reset OTT for offline render: clean state, no warmup fade.
    fun setOttDepthForRender(depth: Int)
    // Select active master bus effect (0=OTT, 1=DUST).
    fun setMasterFx(fx: Int)
    // Set DUST amount (0=bypass, 255=full).
    fun setDustDepth(depth: Int)
    // Reset DUST for offline render: clears delay/envelope state before export.
    fun setDustDepthForRender(depth: Int)

    // ===================================
    // EQ PRESET METHODS
    // ===================================

    // Set one band of a global EQ preset (0-127). freqHex/gainHex/qHex: 00-FF.
    fun setEqBand(slot: Int, band: Int, type: Int, freqHex: Int, gainHex: Int, qHex: Int)
    // Map an instrument to an EQ preset slot (-1 = off). Copies preset at next note trigger.
    fun setInstrumentEqSlot(instrId: Int, slot: Int)

    // ===================================
    // SEND LEVEL METHODS
    // ===================================

    // Set per-instrument reverb/delay send levels (00-FF each).
    fun setInstrumentSendLevels(instrId: Int, reverbSend: Int, delaySend: Int)

    // ===================================
    // REVERB / DELAY METHODS
    // ===================================

    // Set delay→reverb send level. sendHex 00-FF: 00=off, FF=full send.
    fun setDelayReverbSend(sendHex: Int)
    // Set reverb bus params. feedbackHex/dampHex/wetHex: 00-FF. wetHex = return gain.
    fun setReverbParams(feedbackHex: Int, dampHex: Int, wetHex: Int = 0x80)
    // Set delay bus params. syncMode=false: timeOrSubdiv hex 00-FF (0-2s).
    //                        syncMode=true:  timeOrSubdiv is subdivision index 0-11, bpm used.
    //                        wetHex: 00-FF return gain.
    fun setDelayParams(timeOrSubdiv: Int, feedbackHex: Int, syncMode: Boolean, bpm: Float = 120f, wetHex: Int = 0x80)
    // Set reverb/delay input EQ from the global preset bank (-1 = off).
    fun setReverbInputEq(slot: Int)
    fun setDelayInputEq(slot: Int)
    // Set master EQ from the global preset bank (-1 = off).
    fun setMasterEqSlot(slot: Int)

    // ===================================
    // TABLE METHODS (Phase 3.5)
    // ===================================

    /**
     * Load table data into the audio engine.
     *
     * Tables are mini-sequencers that run alongside playing voices,
     * modifying pitch, volume, and effects over time.
     *
     * @param tableId Table slot (0-255)
     * @param rowData 128 bytes: 16 rows × 8 bytes per row
     *                Each row: [transpose, volume, fx1Type, fx1Value, fx2Type, fx2Value, fx3Type, fx3Value]
     */
    fun loadTable(tableId: Int, rowData: ByteArray)

    /**
     * Schedule a note with table processing and pitch modulation.
     *
     * Like scheduleNote(), but the voice will process table data during playback,
     * modifying pitch and volume based on the table rows.
     * Also supports per-note pitch modulation effects (PSL, PBN, PVB, PVX).
     *
     * @param frame Absolute audio frame number
     * @param sampleId Which sample to play (0-255)
     * @param trackId Which track this note belongs to (0-7)
     * @param freq Target playback frequency in Hz
     * @param baseFreq Base frequency of the sample
     * @param vol Volume (0.0 to 1.0)
     * @param pan Stereo pan position (0.0=left, 0.5=center, 1.0=right)
     * @param startPointOverride Optional start point override (-1 = use default)
     * @param tableId Table to use (-1 = no table)
     * @param tableTicRate Ticks per table row advance (default 6)
     * @param noteOctave Octave of the note (0-9) for TICFC mode
     * @param notePitch Pitch of the note (0-11, C=0) for TICFE mode
     * @param pslInitialOffset PSL initial pitch offset in semitones (0 = no PSL)
     * @param pslDuration PSL slide duration in audio frames (0 = no slide)
     * @param pbnRate PBN pitch bend rate in semitones/frame (0 = no bend)
     * @param vibratoSpeed PVB/PVX vibrato speed in Hz (0 = no vibrato)
     * @param vibratoDepth PVB/PVX vibrato depth in semitones (0 = no vibrato)
     */
    fun scheduleNoteWithTable(
        frame: Long,
        sampleId: Int,
        trackId: Int,
        freq: Float,
        baseFreq: Float,
        vol: Float,          // Instrument volume (0.0–1.0) — MOD_SRC_INSTR_VOL
        phraseVol: Float = 1.0f, // Phrase step volume (0.0–1.0) — MOD_SRC_PHRASE_VOL
        pan: Float = 0.5f,
        startPointOverride: Int = -1,
        tableId: Int = -1,
        tableTicRate: Int = 6,
        noteOctave: Int = 4,
        notePitch: Int = 0,
        pslInitialOffset: Float = 0f,
        pslDuration: Float = 0f,
        pbnRate: Float = 0f,
        vibratoSpeed: Float = 0f,
        vibratoDepth: Float = 0f,
        tableStartRow: Int = -1
    )

    /**
     * Get the current table row for a voice on a specific track.
     *
     * Used for UI feedback (highlighting current table row during playback).
     *
     * @param trackId Which track to query (0-7)
     * @return Current table row (0-15), or -1 if no active voice or no table
     */
    fun getVoiceTableRow(trackId: Int): Int

    /**
     * Get the table ID being used by a voice on a specific track.
     *
     * @param trackId Which track to query (0-7)
     * @return Table ID (0-255), or -1 if no active voice or no table
     */
    fun getVoiceTableId(trackId: Int): Int

    /**
     * Set the table row for a voice on a specific track.
     *
     * Used for THO (Table Hop) effect from phrase on empty steps -
     * jumps the currently playing voice's table to a specific row.
     *
     * @param trackId Which track to modify (0-7)
     * @param row Target table row (0-15)
     */
    fun setVoiceTableRow(trackId: Int, row: Int)

    /**
     * Schedule a phraseVol update at exact frame (Vxx effect on empty steps).
     */
    fun scheduleTrackPhraseVol(targetFrame: Long, trackId: Int, phraseVol: Float)

    // ===================================
    // PITCH MODULATION METHODS (Phase 6)
    // ===================================

    /**
     * Set pitch slide for a voice (PSL effect).
     *
     * Slides the pitch from current offset to target over the specified duration.
     * Used for portamento effects between notes.
     *
     * @param trackId Which track to apply pitch slide (0-7)
     * @param targetSemitones Target pitch offset in semitones (can be negative)
     * @param durationTicks Duration of slide in ticks (1 tick = 1/12 of a step at default groove)
     * @param tempo Current tempo in BPM (needed for timing calculations)
     */
    fun setPitchSlide(trackId: Int, targetSemitones: Float, durationTicks: Float, tempo: Int)

    /**
     * Set continuous pitch bend for a voice (PBN effect).
     *
     * Bends the pitch continuously at a specified rate until stopped or a new note.
     * Use semitonesPerTick = 0 to stop bending.
     *
     * @param trackId Which track to apply pitch bend (0-7)
     * @param semitonesPerTick Rate of pitch change per tick (positive = up, negative = down)
     * @param tempo Current tempo in BPM (needed for timing calculations)
     */
    fun setPitchBend(trackId: Int, semitonesPerTick: Float, tempo: Int)

    /**
     * Set vibrato for a voice (PVB/PVX effect).
     *
     * Applies a sine wave LFO modulation to the pitch.
     * Use depth = 0 to stop vibrato.
     *
     * @param trackId Which track to apply vibrato (0-7)
     * @param speed LFO frequency in Hz (typically 2-20 Hz)
     * @param depth Modulation depth in semitones (typically 0.1-2.0, up to 8 for extreme)
     */
    fun setVibrato(trackId: Int, speed: Float, depth: Float)

    /**
     * Clear all pitch modulation for a voice.
     *
     * Resets pitch offset to 0, stops any pitch slide, and disables vibrato.
     * Called automatically when a new note is triggered on the same track.
     *
     * @param trackId Which track to clear pitch modulation (0-7)
     */
    fun clearPitchMod(trackId: Int)

    /**
     * Set initial pitch offset for a voice (used by PSL portamento effect).
     *
     * This sets the starting pitch offset before calling setPitchSlide.
     * Used for portamento: set offset to (previousNote - currentNote) semitones,
     * then call setPitchSlide with target=0 to slide to the current note.
     *
     * @param trackId Which track to set pitch offset (0-7)
     * @param semitones Pitch offset in semitones (can be negative)
     */
    fun setInitialPitchOffset(trackId: Int, semitones: Float)

    // ===================================
    // MODULATION METHODS (Phase 4 — AHD)
    // ===================================

    /**
     * Set a modulation slot for an instrument.
     *
     * Call this before scheduling a note. The engine copies these params to the voice
     * at note-trigger time. attackSamples/holdSamples/decaySamples are already
     * converted from ticks by the caller (AudioEngine.kt).
     *
     * @param sampleId     Instrument's sample slot (0-255)
     * @param slotIndex    Mod slot (0-3)
     * @param type         0=NONE, 1=AHD, 2=ADSR, 3=LFO
     * @param dest         0=NONE, 1=VOL, 3=PITCH
     * @param amount       Modulation depth 0.0-1.0
     * @param attackSamples  Attack duration in audio samples
     * @param holdSamples    Hold duration in audio samples (AHD hold; unused in ADSR)
     * @param decaySamples   Decay duration in audio samples
     * @param sustainLevel   ADSR sustain level 0.0-1.0
     * @param lfoHz          LFO frequency in Hz
     * @param oscShape       LFO shape: 0=TRI,1=SIN,2=RMP+,3=RMP-,6=SQU+,7=SQU-
     */
    fun setInstrumentModulation(
        sampleId: Int,
        slotIndex: Int,
        type: Int,
        dest: Int,
        amount: Float,
        attackSamples: Int,
        holdSamples: Int,
        decaySamples: Int,
        sustainLevel: Float = 0.5f,
        lfoHz: Float = 4.0f,
        oscShape: Int = 0,
        releaseSamples: Int = 0  // ADSR/TRIG: release duration; 0 = instant on note-off
    )

    /**
     * Clear all modulation slots for an instrument.
     * Call this when an instrument has no active modulation.
     *
     * @param sampleId Instrument's sample slot (0-255)
     */
    fun clearInstrumentModulation(sampleId: Int)

    /**
     * Trigger note-off for ADSR/TRIG modulators on a track.
     * Transitions any ADSR/TRIG mod in Sustain (stage 3) → Release (stage 4).
     * Used by KILL effect to allow smooth fade-out via the release envelope.
     *
     * @param trackId Track to trigger note-off on (0-7)
     */
    fun triggerNoteOff(trackId: Int)

    /**
     * Schedule a note-off event at a specific audio frame.
     * Triggers ADSR release at sample-accurate timing (used for automatic step-end release).
     *
     * @param frame Absolute audio frame number when to trigger note-off
     * @param trackId Which track to trigger note-off on (0-7)
     */
    fun scheduleNoteOff(frame: Long, trackId: Int)

    /**
     * Enable or disable offline rendering mode.
     *
     * When true, onAudioReady outputs silence so the live stream cannot consume
     * the note queue while renderFrames() processes it offline (WAV export).
     * Always call setOfflineRendering(false) in a finally block after export.
     *
     * @param rendering true = WAV export in progress, false = normal live playback
     */
    fun setOfflineRendering(rendering: Boolean)

    // ── SoundFont methods ──────────────────────────────────────────────────────

    /**
     * Load an SF2/SF3 file and assign it to an internal slot.
     * @return slot index (0-3), or -1 on failure
     */
    fun loadSoundfont(instrumentId: Int, filePath: String): Int

    /**
     * Set the active bank/preset for a loaded soundfont slot.
     * Preset is also applied per-note in scheduleSoundfontNote.
     */
    fun setSoundfontPreset(sfSlot: Int, bank: Int, preset: Int)

    /**
     * Schedule a soundfont note at a sample-accurate frame.
     */
    fun scheduleSoundfontNote(
        frame: Long, trackId: Int, sfSlot: Int,
        midiNote: Int, velocity: Int, vol: Float, pan: Float, bank: Int, preset: Int,
        pslInitialOffset: Float = 0f, pslDuration: Float = 0f,
        pbnRate: Float = 0f, vibratoSpeed: Float = 0f, vibratoDepth: Float = 0f,
        phraseVol: Float = 1f,   // Phrase step volume (0.0–1.0); multiplied into VOL route
        sampleId: Int = -1,      // Instrument slot index for effect/mod lookup (-1 = none)
        tableId: Int = -1,       // Table ID (-1 = no table)
        tableTicRate: Int = 6,   // Tic rate for table advancement
        noteOctave: Int = 4,     // Note octave (for TICFC/TICFE table modes)
        notePitch: Int = 0,      // Note pitch  (for TICFE table mode)
        tableStartRow: Int = -1  // THO: force starting row (-1 = default)
    )

    /**
     * Apply SF2 envelope overrides by patching TSF preset regions directly.
     * Pass -1 for any field to keep the SF2 preset's built-in value.
     * atk/dec/rel: 0-255 → ~0.001s-10s (exponential). sus: 0-255 → 0.0-1.0 gain.
     */
    fun setSoundfontEnvelopeOverrides(sfSlot: Int, bank: Int, preset: Int,
                                      atk: Int, dec: Int, sus: Int, rel: Int)

    /**
     * Apply SF2 filter override by updating the instrument's instrParams in C++.
     * filterType: 0=off, 1=lp, 2=hp, 3=bp. filterCut/filterRes: 0-255.
     */
    fun setSoundfontFilterOverrides(sampleId: Int, filterType: Int, filterCut: Int, filterRes: Int)

    /**
     * Free the memory used by a soundfont slot.
     */
    fun unloadSoundfont(sfSlot: Int)

    /**
     * Return the preset name string for display on the instrument screen.
     * Returns "---" if slot is invalid or preset not found.
     */
    fun getSoundfontPresetName(sfSlot: Int, bank: Int, preset: Int): String

    /**
     * Returns [bank, preset_number] of the first preset in the SF2, or [-1, -1] if invalid.
     * Called once after loading a soundfont to initialize sfBank/sfPreset.
     */
    fun getSoundfontFirstBankPreset(sfSlot: Int): IntArray

    /** Returns the total number of presets in the SF2 file (0 if not loaded). */
    fun getSoundfontPresetCount(sfSlot: Int): Int

    /** Returns [bank, preset_number] for the preset at the given list index, or [-1,-1] on error. */
    fun getSoundfontPresetAt(sfSlot: Int, index: Int): IntArray
}
