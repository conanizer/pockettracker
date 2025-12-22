package com.example.pockettracker

import android.content.Context
import android.util.Log
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

            // Skip WAV header (44 bytes)
            val dataStart = 44
            val audioDataSize = fileSize - dataStart
            val shortBuffer = ByteBuffer.wrap(buffer, dataStart, audioDataSize)
                .order(ByteOrder.LITTLE_ENDIAN)
                .asShortBuffer()

            // Convert 16-bit samples to float (-1.0 to 1.0)
            val samples = FloatArray(shortBuffer.remaining())
            for (i in samples.indices) {
                samples[i] = shortBuffer.get(i) / 32768f
            }

            return samples
        }
    }

    // Play a note on a specific track
    fun playNote(note: Note, instrumentId: Int, trackId: Int, volume: Float = 1.0f) {
        if (note == Note.EMPTY) return

        // Get sample for this instrument
        val sampleId = instrumentId % 12
        val baseFreq = sampleBaseFrequencies[sampleId] ?: 261.63f

        // Calculate frequency for this note
        val frequency = note.toFrequency()

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