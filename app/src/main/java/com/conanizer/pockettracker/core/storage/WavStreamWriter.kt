package com.conanizer.pockettracker.core.storage

import java.io.File
import java.io.RandomAccessFile
import java.nio.ByteBuffer
import java.nio.ByteOrder

/**
 * Streaming 16-bit PCM WAV writer for long renders.
 *
 * WavWriter.writeWav buffers the entire file in memory — fine for samples, but a full-song
 * render at 44.1 kHz held ~4 full-song copies in RAM at peak (float render + jfloatArray +
 * channel splits + the ByteBuffer), which OOM-kills the app on 1 GB devices. This writer keeps
 * peak memory at one render chunk: open → appendInterleaved() per chunk → finish().
 *
 * Writes to "$path.tmp" and renames on finish() — same atomic pattern as AndroidFileSystem —
 * so an aborted/failed render never leaves a half-written .wav behind.
 *
 * Uses java.io directly (like WavWriter.readCuePoints): JVM-portable, no Android dependency.
 */
class WavStreamWriter(
    private val path: String,
    private val sampleRate: Int = 44100,
    private val channels: Int = 2
) {
    private val tmpFile = File("$path.tmp")
    private val raf = RandomAccessFile(tmpFile, "rw")
    private var framesWritten = 0L
    private var done = false

    private val bytesPerFrame = channels * 2  // 16-bit

    init {
        raf.setLength(0)
        raf.write(buildHeader(dataSize = 0))  // placeholder sizes, patched in finish()
    }

    /**
     * Append interleaved float frames ([L0, R0, L1, R1, ...] for stereo), converted to 16-bit.
     * @param frames number of frames to take from the start of [data] (data.size / channels max)
     */
    fun appendInterleaved(data: FloatArray, frames: Int) {
        if (done) return
        val samples = frames * channels
        val buf = ByteBuffer.allocate(samples * 2).order(ByteOrder.LITTLE_ENDIAN)
        for (i in 0 until samples) {
            val clamped = data[i].coerceIn(-1f, 1f)
            buf.putShort((clamped * 32767f).toInt().toShort())
        }
        raf.write(buf.array())
        framesWritten += frames
    }

    /**
     * Patch the RIFF/data sizes, close, and atomically rename to the final path.
     * @return true on success; false (with tmp file removed) on failure or > 2 GB data
     */
    fun finish(): Boolean {
        if (done) return false
        done = true
        return try {
            val dataSize = framesWritten * bytesPerFrame
            if (dataSize > Int.MAX_VALUE.toLong() - 44L) {  // WAV 32-bit size field limit
                raf.close(); tmpFile.delete()
                return false
            }
            raf.seek(0)
            raf.write(buildHeader(dataSize))
            raf.close()
            val target = File(path)
            if (target.exists()) target.delete()
            if (!tmpFile.renameTo(target)) {
                // renameTo can fail across filesystems; fall back to copy
                tmpFile.copyTo(target, overwrite = true)
                tmpFile.delete()
            }
            true
        } catch (e: Exception) {
            try { raf.close() } catch (_: Exception) {}
            tmpFile.delete()
            false
        }
    }

    /** Discard everything written so far (failed/cancelled render). Safe to call anytime. */
    fun abort() {
        if (done) return
        done = true
        try { raf.close() } catch (_: Exception) {}
        tmpFile.delete()
    }

    // Standard 44-byte RIFF/fmt/data header (same layout WavWriter.writeWav produces).
    private fun buildHeader(dataSize: Long): ByteArray {
        val buf = ByteBuffer.allocate(44).order(ByteOrder.LITTLE_ENDIAN)
        buf.put("RIFF".toByteArray(Charsets.US_ASCII))
        buf.putInt((36L + dataSize).toInt())
        buf.put("WAVE".toByteArray(Charsets.US_ASCII))
        buf.put("fmt ".toByteArray(Charsets.US_ASCII))
        buf.putInt(16)                              // PCM fmt chunk size
        buf.putShort(1)                             // AudioFormat = PCM
        buf.putShort(channels.toShort())
        buf.putInt(sampleRate)
        buf.putInt(sampleRate * bytesPerFrame)      // byte rate
        buf.putShort(bytesPerFrame.toShort())       // block align
        buf.putShort(16)                            // bits per sample
        buf.put("data".toByteArray(Charsets.US_ASCII))
        buf.putInt(dataSize.toInt())
        return buf.array()
    }
}
