package com.conanizer.pockettracker.core.storage

import java.io.File
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
        sampleRate: Int = 44100,
        cuePoints: IntArray = intArrayOf()
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
        val dataSize = numSamples.toLong() * blockAlign  // Long to avoid Int overflow on large renders

        // Reject renders that would exceed the 32-bit WAV size field (~2GB limit)
        if (dataSize > Int.MAX_VALUE.toLong()) {
            return false
        }

        // cue chunk: "cue " (4) + size (4) + count (4) + n * 24 bytes per cue point
        val numCue = cuePoints.size
        val cueChunkDataSize = if (numCue > 0) 4 + numCue * 24 else 0
        val cueChunkBytes    = if (numCue > 0) 8 + cueChunkDataSize else 0

        val riffContentSize = 36L + dataSize + cueChunkBytes   // after "RIFF" + size
        val totalFileBytes  = 8L  + riffContentSize            // = 44 + dataSize + cueChunkBytes

        // Allocate buffer for entire file
        val buffer = ByteBuffer.allocate(totalFileBytes.toInt())
        buffer.order(ByteOrder.LITTLE_ENDIAN)

        // ===================================
        // RIFF HEADER (12 bytes)
        // ===================================
        buffer.put("RIFF".toByteArray(Charsets.US_ASCII))
        buffer.putInt(riffContentSize.toInt())
        buffer.put("WAVE".toByteArray(Charsets.US_ASCII))

        // ===================================
        // FMT CHUNK (24 bytes)
        // ===================================
        buffer.put("fmt ".toByteArray(Charsets.US_ASCII))
        buffer.putInt(16)                                  // PCM
        buffer.putShort(1)                                 // AudioFormat = PCM
        buffer.putShort(numChannels.toShort())
        buffer.putInt(sampleRate)
        buffer.putInt(byteRate)
        buffer.putShort(blockAlign.toShort())
        buffer.putShort(bitsPerSample.toShort())

        // ===================================
        // DATA CHUNK (8 + dataSize bytes)
        // ===================================
        buffer.put("data".toByteArray(Charsets.US_ASCII))
        buffer.putInt(dataSize.toInt())

        for (i in 0 until numSamples) {
            buffer.putShort(floatToInt16(leftChannel[i]))
            buffer.putShort(floatToInt16(rightChannel[i]))
        }

        // ===================================
        // CUE CHUNK (optional, 12 + n*24 bytes)
        // Each cue point marks a slice boundary frame.
        // ===================================
        if (numCue > 0) {
            buffer.put("cue ".toByteArray(Charsets.US_ASCII))
            buffer.putInt(cueChunkDataSize)
            buffer.putInt(numCue)
            cuePoints.forEachIndexed { i, frame ->
                buffer.putInt(i + 1)   // ID (1-based)
                buffer.putInt(frame)   // position (play-order; same as sample offset for no playlist)
                buffer.put("data".toByteArray(Charsets.US_ASCII))  // data chunk ID
                buffer.putInt(0)       // chunk start (byte offset of data chunk from RIFF start, 0 = unknown)
                buffer.putInt(0)       // block start
                buffer.putInt(frame)   // sample offset within data chunk
            }
        }

        return fileSystem.writeBytes(path, buffer.array())
    }

    /**
     * Write mono audio data to a stereo WAV file (duplicated to both channels).
     */
    fun writeWavMono(
        fileSystem: IFileSystem,
        path: String,
        samples: FloatArray,
        sampleRate: Int = 44100,
        cuePoints: IntArray = intArrayOf()
    ): Boolean {
        return writeWav(fileSystem, path, samples, samples, sampleRate, cuePoints)
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
     * Read cue point frame positions from a WAV file's cue chunk.
     * Returns an empty array if the file has no cue chunk or cannot be read.
     * Frame 0 is excluded (it's the implicit sample start, not a slice boundary).
     */
    fun readCuePoints(path: String): IntArray {
        return try {
            val bytes = File(path).readBytes()
            if (bytes.size < 12) return intArrayOf()
            val buf = ByteBuffer.wrap(bytes).order(ByteOrder.LITTLE_ENDIAN)

            // Verify RIFF/WAVE header
            val riffId = ByteArray(4).also { buf.get(it) }
            if (String(riffId, Charsets.US_ASCII) != "RIFF") return intArrayOf()
            buf.getInt()  // RIFF chunk size
            val waveId = ByteArray(4).also { buf.get(it) }
            if (String(waveId, Charsets.US_ASCII) != "WAVE") return intArrayOf()

            // Scan subchunks until we find "cue "
            while (buf.remaining() >= 8) {
                val chunkId = ByteArray(4).also { buf.get(it) }
                val chunkSize = buf.getInt()
                if (String(chunkId, Charsets.US_ASCII) == "cue ") {
                    if (buf.remaining() < 4) break
                    val count = buf.getInt()
                    val frames = mutableListOf<Int>()
                    repeat(count) {
                        if (buf.remaining() < 24) return@repeat
                        buf.getInt()              // ID
                        val pos = buf.getInt()    // position (frame number)
                        buf.getInt()              // data chunk ID ("data")
                        buf.getInt()              // chunk start
                        buf.getInt()              // block start
                        buf.getInt()              // sample offset
                        if (pos > 0) frames.add(pos)
                    }
                    return frames.toIntArray()
                } else {
                    // Skip chunk, padding to even boundary
                    val skip = chunkSize + (chunkSize and 1)
                    if (buf.remaining() < skip) break
                    buf.position(buf.position() + skip)
                }
            }
            intArrayOf()
        } catch (e: Exception) {
            intArrayOf()
        }
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
