package com.example.pockettracker.core.storage

import java.nio.ByteBuffer
import java.nio.ByteOrder

/**
 * WAV File Writer
 *
 * Writes 16-bit stereo PCM WAV files.
 * Platform-agnostic - uses IFileSystem for file operations.
 *
 * WAV Format Reference:
 * - RIFF header (12 bytes)
 * - fmt chunk (24 bytes)
 * - data chunk header (8 bytes)
 * - audio data (variable)
 */
object WavWriter {

    /**
     * Write stereo audio data to a WAV file.
     *
     * @param fileSystem Platform file system implementation
     * @param path Absolute path to output file
     * @param leftChannel Left channel samples (-1.0 to 1.0)
     * @param rightChannel Right channel samples (-1.0 to 1.0)
     * @param sampleRate Sample rate in Hz (default 44100)
     * @return true if successful
     */
    fun writeWav(
        fileSystem: IFileSystem,
        path: String,
        leftChannel: FloatArray,
        rightChannel: FloatArray,
        sampleRate: Int = 44100
    ): Boolean {
        require(leftChannel.size == rightChannel.size) {
            "Left and right channels must have the same length"
        }

        val numSamples = leftChannel.size
        val numChannels = 2
        val bitsPerSample = 16
        val bytesPerSample = bitsPerSample / 8
        val blockAlign = numChannels * bytesPerSample
        val byteRate = sampleRate * blockAlign
        val dataSize = numSamples * blockAlign
        val fileSize = 36 + dataSize  // Total file size minus 8 bytes for RIFF header

        // Allocate buffer for entire file
        val buffer = ByteBuffer.allocate(44 + dataSize)
        buffer.order(ByteOrder.LITTLE_ENDIAN)

        // ===================================
        // RIFF HEADER (12 bytes)
        // ===================================
        buffer.put("RIFF".toByteArray(Charsets.US_ASCII))  // ChunkID
        buffer.putInt(fileSize)                             // ChunkSize
        buffer.put("WAVE".toByteArray(Charsets.US_ASCII))  // Format

        // ===================================
        // FMT CHUNK (24 bytes)
        // ===================================
        buffer.put("fmt ".toByteArray(Charsets.US_ASCII))  // Subchunk1ID
        buffer.putInt(16)                                   // Subchunk1Size (16 for PCM)
        buffer.putShort(1)                                  // AudioFormat (1 = PCM)
        buffer.putShort(numChannels.toShort())             // NumChannels
        buffer.putInt(sampleRate)                          // SampleRate
        buffer.putInt(byteRate)                            // ByteRate
        buffer.putShort(blockAlign.toShort())              // BlockAlign
        buffer.putShort(bitsPerSample.toShort())           // BitsPerSample

        // ===================================
        // DATA CHUNK (8 + dataSize bytes)
        // ===================================
        buffer.put("data".toByteArray(Charsets.US_ASCII))  // Subchunk2ID
        buffer.putInt(dataSize)                             // Subchunk2Size

        // Write interleaved samples (L, R, L, R, ...)
        for (i in 0 until numSamples) {
            // Convert float (-1.0 to 1.0) to 16-bit signed integer
            val leftSample = floatToInt16(leftChannel[i])
            val rightSample = floatToInt16(rightChannel[i])

            buffer.putShort(leftSample)
            buffer.putShort(rightSample)
        }

        // Write to file
        return fileSystem.writeBytes(path, buffer.array())
    }

    /**
     * Write mono audio data to a stereo WAV file (duplicated to both channels).
     */
    fun writeWavMono(
        fileSystem: IFileSystem,
        path: String,
        samples: FloatArray,
        sampleRate: Int = 44100
    ): Boolean {
        return writeWav(fileSystem, path, samples, samples, sampleRate)
    }

    /**
     * Write interleaved stereo audio data to a WAV file.
     *
     * @param interleavedData Interleaved samples [L0, R0, L1, R1, ...]
     */
    fun writeWavInterleaved(
        fileSystem: IFileSystem,
        path: String,
        interleavedData: FloatArray,
        sampleRate: Int = 44100
    ): Boolean {
        require(interleavedData.size % 2 == 0) {
            "Interleaved data must have even number of samples"
        }

        val numFrames = interleavedData.size / 2
        val leftChannel = FloatArray(numFrames)
        val rightChannel = FloatArray(numFrames)

        for (i in 0 until numFrames) {
            leftChannel[i] = interleavedData[i * 2]
            rightChannel[i] = interleavedData[i * 2 + 1]
        }

        return writeWav(fileSystem, path, leftChannel, rightChannel, sampleRate)
    }

    /**
     * Convert float sample (-1.0 to 1.0) to 16-bit signed integer.
     * Clamps values outside range to prevent overflow.
     */
    private fun floatToInt16(sample: Float): Short {
        val clamped = sample.coerceIn(-1f, 1f)
        return (clamped * 32767f).toInt().toShort()
    }
}
