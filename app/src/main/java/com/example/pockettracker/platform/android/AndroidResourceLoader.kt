package com.example.pockettracker.platform.android

import android.content.Context
import android.util.Log
import com.example.pockettracker.core.resources.IResourceLoader
import com.example.pockettracker.core.resources.SampleData
import java.nio.ByteBuffer
import java.nio.ByteOrder

/**
 * Android implementation of IResourceLoader using Android resources.
 *
 * This class loads WAV files from R.raw.* resources and converts them to
 * platform-agnostic SampleData objects.
 *
 * Architecture:
 * - Kotlin (this class) → Android Resources → WAV parsing → SampleData
 * - Handles stereo → mono conversion
 * - Extracts sample rate from WAV header
 * - No sample rate compensation (that's done by AudioEngine)
 *
 * @param context Android context for accessing resources
 */
class AndroidResourceLoader(
    private val context: Context
) : IResourceLoader {

    private val TAG = "AndroidResourceLoader"

    // Mapping from resource names to resource IDs
    // Currently empty - no default samples included
    // This infrastructure remains in place for future use if needed
    private val resourceMap = mapOf<String, Int>(
        // No default samples - users load their own via file browser
    )

    /**
     * Load a WAV file from Android resources by name.
     *
     * @param name Resource name (e.g., "kick", "snare", "hihat")
     * @return SampleData with float samples (mono), sample rate, and channel count
     * @throws IllegalArgumentException if resource name is unknown
     * @throws Exception if WAV parsing fails
     */
    override fun loadWav(name: String): SampleData {
        val resourceId = resourceMap[name]
            ?: throw IllegalArgumentException("Unknown resource: $name")

        return loadWavFromResource(resourceId, name)
    }

    /**
     * Load WAV file from Android resource ID.
     * Handles WAV parsing, stereo→mono conversion, and sample rate detection.
     */
    private fun loadWavFromResource(resourceId: Int, name: String): SampleData {
        context.resources.openRawResource(resourceId).use { inputStream ->
            val fileSize = inputStream.available()
            val buffer = ByteArray(fileSize)
            inputStream.read(buffer)

            val sampleData = parseWavBuffer(buffer)

            Log.d(TAG, "✅ Loaded resource '$name': ${sampleData.samples.size} samples, ${sampleData.sampleRate}Hz, ${sampleData.channels}ch")

            return sampleData
        }
    }

    /**
     * Parse WAV file buffer into SampleData.
     *
     * WAV Format:
     * - Bytes 22-23: Number of channels (1=mono, 2=stereo)
     * - Bytes 24-27: Sample rate (44100, 48000, etc.)
     * - Bytes 44+: Audio data (16-bit PCM)
     *
     * Processing:
     * - Converts 16-bit PCM to float (-1.0 to 1.0)
     * - If stereo, mixes down to mono by averaging L+R channels
     * - Returns mono float array ready for playback
     */
    private fun parseWavBuffer(buffer: ByteArray): SampleData {
        // Read number of channels from WAV header (bytes 22-23)
        val channels = ByteBuffer.wrap(buffer, 22, 2)
            .order(ByteOrder.LITTLE_ENDIAN)
            .short.toInt()

        // Read sample rate from WAV header (bytes 24-27)
        val sampleRate = ByteBuffer.wrap(buffer, 24, 4)
            .order(ByteOrder.LITTLE_ENDIAN)
            .int

        // Skip WAV header (44 bytes) and read audio data
        val dataStart = 44
        val audioDataSize = buffer.size - dataStart
        val shortBuffer = ByteBuffer.wrap(buffer, dataStart, audioDataSize)
            .order(ByteOrder.LITTLE_ENDIAN)
            .asShortBuffer()

        // Convert 16-bit PCM samples to float (-1.0 to 1.0)
        val rawSamples = FloatArray(shortBuffer.remaining())
        for (i in rawSamples.indices) {
            rawSamples[i] = shortBuffer.get(i) / 32768f
        }

        // If stereo, mix down to mono by averaging L+R channels
        val monoSamples = if (channels == 2) {
            FloatArray(rawSamples.size / 2) { i ->
                (rawSamples[i * 2] + rawSamples[i * 2 + 1]) / 2f
            }
        } else {
            rawSamples
        }

        return SampleData(
            samples = monoSamples,
            sampleRate = sampleRate,
            channels = channels
        )
    }
}
