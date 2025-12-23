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
            // Load the 4 samples
            val kick = loadWavFile(R.raw.kick)
            val snare = loadWavFile(R.raw.snare)
            val hihat = loadWavFile(R.raw.hihat)
            val bass = loadWavFile(R.raw.bass)
            val shimmer = loadWavFile(R.raw.shimmer)
            val tambo = loadWavFile(R.raw.tambo)
            val lofi = loadWavFile(R.raw.lofi)
            val choirstring = loadWavFile(R.raw.choirstring)
            val apache162 = loadWavFile(R.raw.apache162)
            val copta162 = loadWavFile(R.raw.copta162)
            val funky162 = loadWavFile(R.raw.funky162)
            val eightoeight = loadWavFile(R.raw.eightoeight)

            native_loadSample(0, kick)
            native_loadSample(1, snare)
            native_loadSample(2, hihat)
            native_loadSample(3, bass)
            native_loadSample(4, shimmer)
            native_loadSample(5, tambo)
            native_loadSample(6, lofi)
            native_loadSample(7, choirstring)
            native_loadSample(8, apache162)
            native_loadSample(9, copta162)
            native_loadSample(10, funky162)
            native_loadSample(11, eightoeight)



            // Set base frequencies (assume samples are at C-4 = 261.63 Hz)
            sampleBaseFrequencies[0] = 261.63f
            sampleBaseFrequencies[1] = 261.63f
            sampleBaseFrequencies[2] = 261.63f
            sampleBaseFrequencies[3] = 261.63f
            sampleBaseFrequencies[4] = 261.63f
            sampleBaseFrequencies[5] = 261.63f
            sampleBaseFrequencies[6] = 261.63f
            sampleBaseFrequencies[7] = 261.63f
            sampleBaseFrequencies[8] = 261.63f
            sampleBaseFrequencies[9] = 261.63f
            sampleBaseFrequencies[10] = 261.63f
            sampleBaseFrequencies[11] = 261.63f

            Log.d(TAG, "Loaded 12 samples")
        } catch (e: Exception) {
            Log.e(TAG, "Error loading samples: ${e.message}")
        }
    }

    private fun loadWavFile(resourceId: Int): FloatArray {
        context.resources.openRawResource(resourceId).use { inputStream ->
            val fileSize = inputStream.available()
            val buffer = ByteArray(fileSize)
            inputStream.read(buffer)

            // Read number of channels from WAV header (bytes 22-23)
            val channels = ByteBuffer.wrap(buffer, 22, 2)
                .order(ByteOrder.LITTLE_ENDIAN)
                .short
                .toInt()

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

            Log.d(TAG, "Loaded resource sample: ${rawSamples.size} samples ($channels ch) -> ${samples.size} mono")
            return samples
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

            val samples = loadWavFileFromPath(filePath)
            native_loadSample(instrumentId, samples)

            // Set default base frequency for this sample (assume C-4 = 261.63 Hz)
            // This will be updated if the user changes the ROOT parameter
            sampleBaseFrequencies[instrumentId] = 261.63f

            Log.d(TAG, "✅ Loaded sample: instrumentId=$instrumentId, sampleLength=${samples.size}, baseFreq=261.63, path=$filePath")
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
     */
    private fun loadWavFileFromPath(filePath: String): FloatArray {
        File(filePath).inputStream().use { inputStream ->
            val fileSize = inputStream.available()
            val buffer = ByteArray(fileSize)
            inputStream.read(buffer)

            // Read number of channels from WAV header (bytes 22-23)
            val channels = ByteBuffer.wrap(buffer, 22, 2)
                .order(ByteOrder.LITTLE_ENDIAN)
                .short
                .toInt()

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

            Log.d(TAG, "Loaded WAV: ${rawSamples.size} samples ($channels ch) -> ${samples.size} mono")
            return samples
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

            val samples = loadWavFileFromPath(filePath)
            // Load to temporary preview slot (255)
            native_loadSample(255, samples)

            // Play at C-4 as reference pitch
            // Assume sample was recorded at C-4, so rate = C-4/C-4 = 1.0
            val c4Freq = 261.63f
            native_triggerNote(255, 0, c4Freq, c4Freq, 1.0f)

            Log.d(TAG, "🔊 Preview sample at C-4: $filePath")
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

        // Calculate target frequency from ROOT + DETUNE
        val targetFreq = calculateInstrumentBaseFrequency(instrument)

        // Use C-4 as standard reference pitch for all samples
        // Sample plays at natural speed when ROOT=C-4, faster/slower when ROOT is higher/lower
        val sampleReferencePitch = 261.63f // C-4

        // Transpose from reference pitch to target pitch
        // Rate = target/reference
        // If ROOT=C-4: rate = C-4/C-4 = 1.0 (natural speed)
        // If ROOT=C-5: rate = C-5/C-4 = 2.0 (one octave up)
        // If ROOT=C-3: rate = C-3/C-4 = 0.5 (one octave down)
        native_triggerNote(sampleId, 0, targetFreq, sampleReferencePitch, 1.0f)

        val rate = targetFreq / sampleReferencePitch
        Log.d(TAG, "🔊 Preview instrument ${instrument.id.toString(16).padStart(2,'0').uppercase()}: root=${instrument.root}, detune=0x${instrument.detune.toString(16).padStart(2,'0').uppercase()}, targetFreq=$targetFreq Hz, rate=$rate")
    }

    /**
     * Calculate the effective base frequency for an instrument
     * Combines ROOT note and DETUNE parameters
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

        val result = rootFreq * detuneMultiplier

        Log.d(TAG, "📐 calculateBaseFreq: root=${instrument.root}, rootFreq=$rootFreq Hz, detune=0x${instrument.detune.toString(16)}, detuneMulti=$detuneMultiplier, result=$result Hz")

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

    // Native methods
    private external fun native_create(): Boolean
    private external fun native_delete()
    private external fun native_loadSample(sampleId: Int, sampleData: FloatArray)
    private external fun native_triggerNote(sampleId: Int, trackId: Int, frequency: Float, baseFrequency: Float, volume: Float)
    private external fun native_stopTrack(trackId: Int)
    private external fun native_stopAll()
    private external fun native_getActiveVoiceCount(): Int
}