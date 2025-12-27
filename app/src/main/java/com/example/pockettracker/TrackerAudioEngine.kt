package com.example.pockettracker

import android.content.Context
import android.util.Log
import java.io.File
import java.nio.ByteBuffer
import java.nio.ByteOrder

class TrackerAudioEngine(private val context: Context) {

    private val TAG = "TrackerAudioEngine"

    // Waveform buffer for visualization (620 samples for 620px width)
    val waveformBuffer = FloatArray(620) { 0f }
    private var waveformIndex = 0

    // Loaded samples metadata
    private val sampleBaseFrequencies = mutableMapOf<Int, Float>()
    private val sampleRateRatios = mutableMapOf<Int, Float>()  // deviceRate / sampleRate per sample

    init {
        System.loadLibrary("pockettracker")
    }

    fun create(): Boolean {
        val success = native_create()
        if (success) {
            Log.d(TAG, "Tracker audio engine created")
            loadAllSamples()
        } else {
            Log.e(TAG, "Failed to create audio engine")
        }
        return success
    }

    private fun loadAllSamples() {
        try {
            // Load the 12 samples (returns Pair of samples + adjusted base frequency)
            val (kickSamples, kickFreq) = loadWavFile(R.raw.kick)
            val (snareSamples, snareFreq) = loadWavFile(R.raw.snare)
            val (hihatSamples, hihatFreq) = loadWavFile(R.raw.hihat)
            val (bassSamples, bassFreq) = loadWavFile(R.raw.bass)
            val (shimmerSamples, shimmerFreq) = loadWavFile(R.raw.shimmer)
            val (tamboSamples, tamboFreq) = loadWavFile(R.raw.tambo)
            val (lofiSamples, lofiFreq) = loadWavFile(R.raw.lofi)
            val (choirstringSamples, choirstringFreq) = loadWavFile(R.raw.choirstring)
            val (apache162Samples, apache162Freq) = loadWavFile(R.raw.apache162)
            val (copta162Samples, copta162Freq) = loadWavFile(R.raw.copta162)
            val (funky162Samples, funky162Freq) = loadWavFile(R.raw.funky162)
            val (eightoeightSamples, eightoeightFreq) = loadWavFile(R.raw.eightoeight)

            native_loadSample(0, kickSamples)
            native_loadSample(1, snareSamples)
            native_loadSample(2, hihatSamples)
            native_loadSample(3, bassSamples)
            native_loadSample(4, shimmerSamples)
            native_loadSample(5, tamboSamples)
            native_loadSample(6, lofiSamples)
            native_loadSample(7, choirstringSamples)
            native_loadSample(8, apache162Samples)
            native_loadSample(9, copta162Samples)
            native_loadSample(10, funky162Samples)
            native_loadSample(11, eightoeightSamples)

            // Set base frequencies and store ratios (adjusted for sample rate)
            // The ratio is already baked into the frequency, but we also store it separately
            // so it can be reapplied when ROOT/DETUNE changes
            val deviceRate = getDeviceSampleRate().toFloat()
            sampleBaseFrequencies[0] = kickFreq
            sampleBaseFrequencies[1] = snareFreq
            sampleBaseFrequencies[2] = hihatFreq
            sampleBaseFrequencies[3] = bassFreq
            sampleBaseFrequencies[4] = shimmerFreq
            sampleBaseFrequencies[5] = tamboFreq
            sampleBaseFrequencies[6] = lofiFreq
            sampleBaseFrequencies[7] = choirstringFreq
            sampleBaseFrequencies[8] = apache162Freq
            sampleBaseFrequencies[9] = copta162Freq
            sampleBaseFrequencies[10] = funky162Freq
            sampleBaseFrequencies[11] = eightoeightFreq

            // Store ratios for each (kickFreq / 261.63 gives us the ratio)
            for (i in 0..11) {
                sampleRateRatios[i] = sampleBaseFrequencies[i]!! / 261.63f
            }

            Log.d(TAG, "Loaded 12 samples with sample rate compensation")
        } catch (e: Exception) {
            Log.e(TAG, "Error loading samples: ${e.message}")
        }
    }

    /**
     * Get actual device sample rate from audio engine
     * Queries the Oboe audio stream for the real sample rate
     */
    private fun getDeviceSampleRate(): Int {
        return native_getSampleRate()
    }

    /**
     * Load WAV file from resources with sample rate detection
     * Returns Pair of (samples, adjustedBaseFrequency)
     */
    private fun loadWavFile(resourceId: Int): Pair<FloatArray, Float> {
        context.resources.openRawResource(resourceId).use { inputStream ->
            val fileSize = inputStream.available()
            val buffer = ByteArray(fileSize)
            inputStream.read(buffer)

            // Read number of channels from WAV header (bytes 22-23)
            val channels = ByteBuffer.wrap(buffer, 22, 2)
                .order(ByteOrder.LITTLE_ENDIAN)
                .short
                .toInt()

            // Read sample rate from WAV header (bytes 24-27)
            val sampleRate = ByteBuffer.wrap(buffer, 24, 4)
                .order(ByteOrder.LITTLE_ENDIAN)
                .int

            // Skip WAV header (44 bytes)
            val dataStart = 44
            val audioDataSize = fileSize - dataStart
            val shortBuffer = ByteBuffer.wrap(buffer, dataStart, audioDataSize)
                .order(ByteOrder.LITTLE_ENDIAN)
                .asShortBuffer()

            // Convert 16-bit samples to float (-1.0 to 1.0)
            val rawSamples = FloatArray(shortBuffer.remaining())
            for (i in rawSamples.indices) {
                rawSamples[i] = shortBuffer.get(i) / 32768f
            }

            // If stereo, mix down to mono by averaging L+R channels
            val samples = if (channels == 2) {
                FloatArray(rawSamples.size / 2) { i ->
                    (rawSamples[i * 2] + rawSamples[i * 2 + 1]) / 2f
                }
            } else {
                rawSamples
            }

            // Calculate adjusted base frequency for sample rate compensation
            // If sample is 44100Hz and device is 48000Hz, samples plays faster (higher pitch)
            // Adjust base frequency: baseFreq * (deviceRate / sampleRate) to compensate
            val deviceSampleRate = getDeviceSampleRate()
            val sampleRateRatio = deviceSampleRate.toFloat() / sampleRate.toFloat()
            val adjustedBaseFreq = 261.63f * sampleRateRatio

            Log.d(TAG, "Loaded resource sample: ${rawSamples.size} samples ($channels ch, ${sampleRate}Hz) -> ${samples.size} mono, baseFreq=$adjustedBaseFreq (ratio=$sampleRateRatio)")
            return Pair(samples, adjustedBaseFreq)
        }
    }

    /**
     * Load WAV file from external path (for user samples)
     * @param instrumentId Which instrument slot (0-255)
     * @param filePath Absolute path to WAV file
     * @return True if successful
     */
    fun loadSampleFromFile(instrumentId: Int, filePath: String): Boolean {
        try {
            val file = File(filePath)
            if (!file.exists() || !file.canRead()) {
                Log.e(TAG, "Cannot read file: $filePath")
                return false
            }

            val (samples, adjustedBaseFreq) = loadWavFileFromPath(filePath)
            native_loadSample(instrumentId, samples)

            // Set adjusted base frequency and store ratio (compensates for sample rate)
            sampleBaseFrequencies[instrumentId] = adjustedBaseFreq
            sampleRateRatios[instrumentId] = adjustedBaseFreq / 261.63f

            Log.d(TAG, "✅ Loaded sample: instrumentId=$instrumentId, sampleLength=${samples.size}, baseFreq=$adjustedBaseFreq, ratio=${sampleRateRatios[instrumentId]}, path=$filePath")
            return true
        } catch (e: Exception) {
            Log.e(TAG, "❌ Error loading sample from file: ${e.message}")
            return false
        }
    }

    /**
     * Load WAV file from external path
     * Private helper function
     * Handles both mono and stereo files (stereo is mixed down to mono)
     * Returns Pair of (samples, adjustedBaseFrequency)
     */
    private fun loadWavFileFromPath(filePath: String): Pair<FloatArray, Float> {
        File(filePath).inputStream().use { inputStream ->
            val fileSize = inputStream.available()
            val buffer = ByteArray(fileSize)
            inputStream.read(buffer)

            // Read number of channels from WAV header (bytes 22-23)
            val channels = ByteBuffer.wrap(buffer, 22, 2)
                .order(ByteOrder.LITTLE_ENDIAN)
                .short
                .toInt()

            // Read sample rate from WAV header (bytes 24-27)
            val sampleRate = ByteBuffer.wrap(buffer, 24, 4)
                .order(ByteOrder.LITTLE_ENDIAN)
                .int

            // Skip WAV header (44 bytes)
            val dataStart = 44
            val audioDataSize = fileSize - dataStart
            val shortBuffer = ByteBuffer.wrap(buffer, dataStart, audioDataSize)
                .order(ByteOrder.LITTLE_ENDIAN)
                .asShortBuffer()

            // Convert 16-bit samples to float (-1.0 to 1.0)
            val rawSamples = FloatArray(shortBuffer.remaining())
            for (i in rawSamples.indices) {
                rawSamples[i] = shortBuffer.get(i) / 32768f
            }

            // If stereo, mix down to mono by averaging L+R channels
            val samples = if (channels == 2) {
                FloatArray(rawSamples.size / 2) { i ->
                    (rawSamples[i * 2] + rawSamples[i * 2 + 1]) / 2f
                }
            } else {
                rawSamples
            }

            // Calculate adjusted base frequency for sample rate compensation
            val deviceSampleRate = getDeviceSampleRate()
            val sampleRateRatio = deviceSampleRate.toFloat() / sampleRate.toFloat()
            val adjustedBaseFreq = 261.63f * sampleRateRatio

            Log.d(TAG, "Loaded WAV: ${rawSamples.size} samples ($channels ch, ${sampleRate}Hz) -> ${samples.size} mono, baseFreq=$adjustedBaseFreq (ratio=$sampleRateRatio)")
            return Pair(samples, adjustedBaseFreq)
        }
    }

    /**
     * Update sample base frequency for instrument
     * Used when instrument root note is changed
     */
    fun setSampleBaseFrequency(instrumentId: Int, frequency: Float) {
        sampleBaseFrequencies[instrumentId] = frequency
        Log.d(TAG, "Set base frequency for instrument $instrumentId: $frequency Hz")
    }

    /**
     * Preview a WAV file without permanently loading it
     * Uses temporary slot 255 for preview playback
     * Plays at C-4 (261.63 Hz) as a consistent reference pitch
     * @param filePath Absolute path to WAV file
     * @return True if successful
     */
    fun previewSampleFile(filePath: String): Boolean {
        try {
            val file = File(filePath)
            if (!file.exists() || !file.canRead()) {
                Log.e(TAG, "Cannot read file: $filePath")
                return false
            }

            // CRITICAL: Stop all audio before loading new sample to avoid race condition
            // (audio thread might be playing old sample 255 while we delete/reallocate it)
            native_stopAll()

            val (samples, adjustedBaseFreq) = loadWavFileFromPath(filePath)
            // Load to temporary preview slot (255)
            native_loadSample(255, samples)

            // Play at C-4 as reference pitch with sample rate compensation
            // targetFreq = what we want to hear (C-4 = 261.63 Hz)
            // baseFreq = adjustedBaseFreq (compensated for device sample rate)
            // rate = targetFreq / baseFreq = 261.63 / 284.76 = 0.919 (plays slower to compensate)
            val c4Freq = 261.63f
            native_triggerNote(255, 0, c4Freq, adjustedBaseFreq, 1.0f)

            val rate = c4Freq / adjustedBaseFreq
            Log.d(TAG, "🔊 Preview sample at C-4: $filePath (baseFreq=$adjustedBaseFreq, rate=$rate)")
            return true
        } catch (e: Exception) {
            Log.e(TAG, "❌ Error previewing sample: ${e.message}")
            return false
        }
    }

    /**
     * Preview current instrument with all parameters applied
     * Plays the root note with detune adjustment
     * Transposes from standard C-4 reference to ROOT+DETUNE pitch
     * @param instrument The instrument to preview
     */
    fun previewInstrument(instrument: Instrument) {
        val sampleId = instrument.sampleId

        // Calculate target frequency from ROOT + DETUNE (WITHOUT sample rate compensation)
        val rootFreq = instrument.root.toFrequency()

        // Apply detune
        val detuneSemitones = (instrument.detune shr 4).toFloat()
        val detuneFraction = (instrument.detune and 0x0F) / 16.0f
        val totalDetuneSemitones = detuneSemitones + detuneFraction - 8.0f
        val detuneMultiplier = Math.pow(2.0, (totalDetuneSemitones / 12.0).toDouble()).toFloat()

        val targetFreq = rootFreq * detuneMultiplier

        // Get the compensated base frequency for this sample
        val sampleRateRatio = sampleRateRatios[sampleId] ?: 1.0f
        val compensatedBaseFreq = 261.63f * sampleRateRatio // C-4 compensated for sample rate

        // Calculate playback rate
        // If sample is 44100Hz on 48000Hz device: compensatedBaseFreq = 284.76
        // To play at C-4: rate = 261.63 / 284.76 = 0.919 (slower to compensate)
        native_triggerNote(sampleId, 0, targetFreq, compensatedBaseFreq, 1.0f)

        val rate = targetFreq / compensatedBaseFreq
        Log.d(TAG, "🔊 Preview instrument ${instrument.id.toString(16).padStart(2,'0').uppercase()}: root=${instrument.root}, detune=0x${instrument.detune.toString(16).padStart(2,'0').uppercase()}, targetFreq=$targetFreq Hz, baseFreq=$compensatedBaseFreq Hz, rate=$rate")
    }

    /**
     * Calculate the effective base frequency for an instrument
     * Combines ROOT note, DETUNE parameters, and sample rate compensation
     * @param instrument The instrument
     * @return Calculated base frequency in Hz
     */
    fun calculateInstrumentBaseFrequency(instrument: Instrument): Float {
        // Get frequency from root note
        val rootFreq = instrument.root.toFrequency()

        // Apply detune: high nibble = semitones, low nibble = sixteenths of semitone
        // Example: 0x35 = 3 semitones + 5/16 semitone = 3.3125 semitones
        // Center at 0x80 (128): 0x00 = -8 semitones, 0x80 = 0, 0xFF = +7.9375
        val detuneSemitones = (instrument.detune shr 4).toFloat() // High nibble (0-15)
        val detuneFraction = (instrument.detune and 0x0F) / 16.0f // Low nibble / 16
        val totalDetuneSemitones = detuneSemitones + detuneFraction - 8.0f // Center at 0x80

        // Convert semitone shift to frequency multiplier: 2^(semitones/12)
        val detuneMultiplier = Math.pow(2.0, (totalDetuneSemitones / 12.0).toDouble()).toFloat()

        // Apply sample rate compensation ratio
        val sampleRateRatio = sampleRateRatios[instrument.sampleId] ?: 1.0f

        val result = rootFreq * detuneMultiplier * sampleRateRatio

        Log.d(TAG, "📐 calculateBaseFreq: root=${instrument.root}, rootFreq=$rootFreq Hz, detune=0x${instrument.detune.toString(16)}, detuneMulti=$detuneMultiplier, sampleRateRatio=$sampleRateRatio, result=$result Hz")

        return result
    }

    /**
     * Update the base frequency for an instrument based on its ROOT and DETUNE
     * Should be called whenever ROOT or DETUNE parameters change
     * @param instrument The instrument to update
     */
    fun updateInstrumentBaseFrequency(instrument: Instrument) {
        val baseFreq = calculateInstrumentBaseFrequency(instrument)
        sampleBaseFrequencies[instrument.sampleId] = baseFreq
        Log.d(TAG, "📝 Updated base frequency for instrument ${instrument.id.toString(16).padStart(2,'0').uppercase()}: $baseFreq Hz (root=${instrument.root}, detune=0x${instrument.detune.toString(16).padStart(2,'0').uppercase()})")
    }

    // Play a note on a specific track
    fun playNote(note: Note, instrumentId: Int, trackId: Int, volume: Float = 1.0f, project: Project? = null) {
        if (note == Note.EMPTY) return

        // Get sample for this instrument
        // Use the instrument's actual sampleId instead of wrapping to 0-11
        val sampleId = if (project != null && instrumentId in 0..255) {
            project.instruments[instrumentId].sampleId
        } else {
            instrumentId % 12  // Fallback for old behavior
        }

        val baseFreq = sampleBaseFrequencies[sampleId] ?: 261.63f

        // Calculate frequency for this note
        val frequency = note.toFrequency()

        // Log playback details (only for instruments 0C+ to reduce log spam)
        if (instrumentId >= 12) {
            Log.d(TAG, "🎵 playNote: instrumentId=$instrumentId -> sampleId=$sampleId, note=$note, freq=$frequency, baseFreq=$baseFreq, vol=$volume")
        }

        // Trigger the note in C++
        native_triggerNote(sampleId, trackId, frequency, baseFreq, volume)

        // Update waveform buffer (simple visualization - just mark activity)
        updateWaveform(volume * 0.5f)
    }

    // Update waveform buffer for visualization
    private fun updateWaveform(sample: Float) {
        waveformBuffer[waveformIndex] = sample
        waveformIndex = (waveformIndex + 1) % waveformBuffer.size
    }

    // Stop all notes on a track
    fun stopTrack(trackId: Int) {
        native_stopTrack(trackId)
    }

    // Stop all playback
    fun stopAll() {
        native_stopAll()
        // Clear waveform
        waveformBuffer.fill(0f)
    }

    fun destroy() {
        stopAll()
        native_delete()
    }

    fun getActiveVoiceCount(): Int = native_getActiveVoiceCount()

    // ===================================
    // PHASE 1: NOTE QUEUE INTERFACE
    // ===================================

    /**
     * Get current audio frame number (for sample-accurate scheduling)
     * @return Global frame counter from audio engine
     */
    fun getCurrentFrame(): Long = native_getCurrentFrame()

    /**
     * Schedule a note to be played at exact audio frame
     * @param targetFrame Exact frame number to trigger note
     * @param note Note to play
     * @param instrumentId Instrument slot (0-255)
     * @param trackId Track/voice assignment (0-7)
     * @param volume Playback volume (0.0-1.0)
     * @param project Project containing instrument data
     */
    fun scheduleNote(targetFrame: Long, note: Note, instrumentId: Int, trackId: Int,
                    volume: Float = 1.0f, project: Project) {
        if (note == Note.EMPTY) return

        // Get sample for this instrument
        val sampleId = if (instrumentId in 0..255) {
            project.instruments[instrumentId].sampleId
        } else {
            return  // Invalid instrument
        }

        val baseFreq = sampleBaseFrequencies[sampleId] ?: 261.63f
        val frequency = note.toFrequency()

        native_scheduleNote(targetFrame, sampleId, trackId, frequency, baseFreq, volume)

        Log.d(TAG, "📅 Scheduled: frame=$targetFrame, note=$note, inst=$instrumentId, track=$trackId")
    }

    /**
     * Clear all scheduled notes (call on stop/reset)
     */
    fun clearScheduledNotes() {
        native_clearScheduledNotes()
        Log.d(TAG, "🗑️ Cleared all scheduled notes")
    }

    /**
     * Calculate target frame for a note based on tempo and step number
     * @param startFrame Frame number when playback started
     * @param stepNumber Which step (0, 1, 2, ...)
     * @param tempo BPM
     * @return Target frame number for this step
     */
    fun calculateTargetFrame(startFrame: Long, stepNumber: Int, tempo: Int): Long {
        val sampleRate = getDeviceSampleRate()
        // 60000ms per minute ÷ BPM ÷ 4 (16th notes) = ms per step
        val msPerStep = (60000.0 / tempo / 4.0)
        // Convert to frames: ms * sampleRate / 1000
        val framesPerStep = (msPerStep * sampleRate / 1000.0).toLong()
        return startFrame + (stepNumber * framesPerStep)
    }

    /**
     * Set playback parameters for an instrument
     * @param instrumentId Instrument slot (0-255)
     * @param startPoint Sample start position (0-255)
     * @param endPoint Sample end position (0-255)
     * @param reverse Play backwards
     * @param loopMode 0=off, 1=forward loop, 2=ping-pong loop
     * @param loopStart Loop restart position (0-255)
     */
    fun setInstrumentParams(instrumentId: Int, startPoint: Int, endPoint: Int,
                           reverse: Boolean, loopMode: Int, loopStart: Int) {
        native_setInstrumentParams(instrumentId, startPoint, endPoint, reverse, loopMode, loopStart)
        Log.d(TAG, "🎛️ Set params for instrument $instrumentId: start=$startPoint, end=$endPoint, rev=$reverse, loop=$loopMode, loopSt=$loopStart")
    }

    /**
     * Update instrument parameters from Instrument data class
     * Convenience wrapper that extracts all playback parameters
     */
    fun updateInstrumentPlaybackParams(instrument: Instrument) {
        // Convert loop mode string to int
        val loopModeInt = when (instrument.loopMode) {
            "fwd" -> 1
            "png" -> 2
            else -> 0  // "off"
        }

        setInstrumentParams(
            instrumentId = instrument.sampleId,
            startPoint = instrument.sampleStart,
            endPoint = instrument.sampleEnd,
            reverse = instrument.reverse,
            loopMode = loopModeInt,
            loopStart = instrument.loopStart
        )
    }

    // Native methods
    private external fun native_create(): Boolean
    private external fun native_delete()
    private external fun native_loadSample(sampleId: Int, sampleData: FloatArray)
    private external fun native_triggerNote(sampleId: Int, trackId: Int, frequency: Float, baseFrequency: Float, volume: Float)
    private external fun native_stopTrack(trackId: Int)
    private external fun native_stopAll()
    private external fun native_getActiveVoiceCount(): Int
    private external fun native_getSampleRate(): Int
    private external fun native_setInstrumentParams(instrumentId: Int, startPoint: Int, endPoint: Int,
                                                    reverse: Boolean, loopMode: Int, loopStart: Int)

    // PHASE 1: Note queue native methods
    private external fun native_getCurrentFrame(): Long
    private external fun native_scheduleNote(targetFrame: Long, sampleId: Int, trackId: Int,
                                           frequency: Float, baseFrequency: Float, volume: Float)
    private external fun native_clearScheduledNotes()
}