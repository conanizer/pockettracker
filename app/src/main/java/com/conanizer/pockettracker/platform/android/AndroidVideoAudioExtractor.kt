package com.conanizer.pockettracker.platform.android

import android.media.MediaCodec
import android.media.MediaExtractor
import android.media.MediaFormat
import com.conanizer.pockettracker.core.media.IVideoAudioExtractor
import java.io.File
import java.nio.ByteBuffer
import java.nio.ByteOrder
import java.nio.ShortBuffer

/**
 * Android implementation of IVideoAudioExtractor.
 *
 * Uses Android's MediaExtractor + MediaCodec to decode audio tracks from
 * video/audio container files (MP4, MKV, WebM, 3GP, M4A, MOV).
 *
 * No extra dependencies — MediaExtractor and MediaCodec are Android framework
 * classes available since API 16 (our min is 26).
 */
class AndroidVideoAudioExtractor : IVideoAudioExtractor {

    override fun isSupportedVideo(path: String): Boolean {
        val ext = path.substringAfterLast('.', "").lowercase()
        return ext in IVideoAudioExtractor.SUPPORTED_EXTENSIONS
    }

    override fun extractAudio(path: String, maxDurationSec: Int): Result<IVideoAudioExtractor.ExtractionResult> {
        if (!File(path).exists()) {
            return Result.failure(IVideoAudioExtractor.ExtractionError.FileNotFound(path))
        }

        val extractor = MediaExtractor()
        try {
            extractor.setDataSource(path)

            // Find first audio track
            val audioTrackIndex = (0 until extractor.trackCount).firstOrNull { i ->
                extractor.getTrackFormat(i).getString(MediaFormat.KEY_MIME)?.startsWith("audio/") == true
            } ?: return Result.failure(
                IVideoAudioExtractor.ExtractionError.NoAudioTrack("No audio track found in: $path")
            )

            extractor.selectTrack(audioTrackIndex)
            val format = extractor.getTrackFormat(audioTrackIndex)

            val mime = format.getString(MediaFormat.KEY_MIME) ?: "audio/unknown"
            val sampleRate = format.getInteger(MediaFormat.KEY_SAMPLE_RATE)
            val channelCount = format.getInteger(MediaFormat.KEY_CHANNEL_COUNT)
            val durationUs = if (format.containsKey(MediaFormat.KEY_DURATION))
                format.getLong(MediaFormat.KEY_DURATION) else 0L
            val durationMs = durationUs / 1000

            // Check duration cap
            if (maxDurationSec > 0 && durationUs > maxDurationSec * 1_000_000L) {
                val durationSec = (durationUs / 1_000_000L).toInt()
                return Result.failure(
                    IVideoAudioExtractor.ExtractionError.FileTooLong(durationSec, maxDurationSec)
                )
            }

            val sourceFormat = "$mime ${sampleRate}Hz ${if (channelCount == 1) "mono" else "stereo"}"

            // Decode
            val (left, right) = decode(extractor, format, mime, sampleRate, channelCount, maxDurationSec)
                ?: return Result.failure(
                    IVideoAudioExtractor.ExtractionError.DecodeFailed("MediaCodec decode failed for: $path")
                )

            return Result.success(
                IVideoAudioExtractor.ExtractionResult(
                    samples = left,
                    samplesRight = right,
                    sampleRate = sampleRate,
                    durationMs = durationMs,
                    sourceFormat = sourceFormat
                )
            )
        } catch (e: Exception) {
            return Result.failure(
                IVideoAudioExtractor.ExtractionError.DecodeFailed(e.message ?: "Unknown error")
            )
        } finally {
            extractor.release()
        }
    }

    override fun extractAudioToSink(
        path: String,
        maxDurationSec: Int,
        sink: IVideoAudioExtractor.PcmSink
    ): Result<IVideoAudioExtractor.StreamInfo> {
        if (!File(path).exists()) {
            return Result.failure(IVideoAudioExtractor.ExtractionError.FileNotFound(path))
        }
        val extractor = MediaExtractor()
        try {
            extractor.setDataSource(path)
            val audioTrackIndex = (0 until extractor.trackCount).firstOrNull { i ->
                extractor.getTrackFormat(i).getString(MediaFormat.KEY_MIME)?.startsWith("audio/") == true
            } ?: return Result.failure(
                IVideoAudioExtractor.ExtractionError.NoAudioTrack("No audio track found in: $path")
            )
            extractor.selectTrack(audioTrackIndex)
            val format = extractor.getTrackFormat(audioTrackIndex)
            val mime = format.getString(MediaFormat.KEY_MIME) ?: "audio/unknown"
            val sampleRate = format.getInteger(MediaFormat.KEY_SAMPLE_RATE)
            val channelCount = format.getInteger(MediaFormat.KEY_CHANNEL_COUNT)
            val durationUs = if (format.containsKey(MediaFormat.KEY_DURATION))
                format.getLong(MediaFormat.KEY_DURATION) else 0L

            // Pre-sizing the native buffer needs the duration. Without it, bail so the caller can fall
            // back to extractAudio (the whole-file path), which doesn't need a size up front.
            if (durationUs <= 0L) {
                return Result.failure(
                    IVideoAudioExtractor.ExtractionError.DecodeFailed("No duration metadata: $path")
                )
            }
            if (maxDurationSec > 0 && durationUs > maxDurationSec * 1_000_000L) {
                return Result.failure(
                    IVideoAudioExtractor.ExtractionError.FileTooLong((durationUs / 1_000_000L).toInt(), maxDurationSec)
                )
            }

            val sourceFormat = "$mime ${sampleRate}Hz ${if (channelCount == 1) "mono" else "stereo"}"
            // Over-estimate by +1 s so encoder delay/padding can't overrun the allocation (excess is clamped).
            val estimatedFrames = ((durationUs / 1_000_000.0) * sampleRate).toInt() + sampleRate
            sink.onFormat(sampleRate, channelCount, estimatedFrames)

            val frames = decodeToSink(extractor, format, mime, channelCount, sink)
                ?: return Result.failure(
                    IVideoAudioExtractor.ExtractionError.DecodeFailed("MediaCodec decode failed for: $path")
                )
            return Result.success(
                IVideoAudioExtractor.StreamInfo(sampleRate, channelCount, frames, sourceFormat)
            )
        } catch (e: Exception) {
            return Result.failure(
                IVideoAudioExtractor.ExtractionError.DecodeFailed(e.message ?: "Unknown error")
            )
        } finally {
            extractor.release()
        }
    }

    /**
     * MediaCodec decode loop that streams each decoded block to [sink] instead of accumulating it.
     * Kept parallel to [decode] (which builds FloatArrays for the WAV-extract path) so the working
     * video/preview path stays untouched. Returns total frames decoded, or null on failure.
     */
    private fun decodeToSink(
        extractor: MediaExtractor,
        format: MediaFormat,
        mime: String,
        channelCount: Int,
        sink: IVideoAudioExtractor.PcmSink
    ): Int? {
        val codec = try {
            MediaCodec.createDecoderByType(mime)
        } catch (e: Exception) {
            return null
        }
        try {
            codec.configure(format, null, null, 0)
            codec.start()
        } catch (e: Exception) {
            codec.release()
            return null
        }
        val bufferInfo = MediaCodec.BufferInfo()
        val timeoutUs = 10_000L
        var inputDone = false
        var outputDone = false
        var isFloat = false
        var totalFrames = 0
        var chunk = ShortArray(8192)   // reused per block; grown if a block is larger
        try {
            while (!outputDone) {
                if (!inputDone) {
                    val inIdx = codec.dequeueInputBuffer(timeoutUs)
                    if (inIdx >= 0) {
                        val inBuf = codec.getInputBuffer(inIdx)!!
                        val size = extractor.readSampleData(inBuf, 0)
                        if (size < 0) {
                            codec.queueInputBuffer(inIdx, 0, 0, 0, MediaCodec.BUFFER_FLAG_END_OF_STREAM)
                            inputDone = true
                        } else {
                            codec.queueInputBuffer(inIdx, 0, size, extractor.sampleTime, 0)
                            extractor.advance()
                        }
                    }
                }
                val outIdx = codec.dequeueOutputBuffer(bufferInfo, timeoutUs)
                when {
                    outIdx == MediaCodec.INFO_OUTPUT_FORMAT_CHANGED -> {
                        val of = codec.outputFormat
                        if (of.containsKey(MediaFormat.KEY_PCM_ENCODING)) {
                            isFloat = of.getInteger(MediaFormat.KEY_PCM_ENCODING) ==
                                    android.media.AudioFormat.ENCODING_PCM_FLOAT
                        }
                    }
                    outIdx >= 0 -> {
                        val outBuf = codec.getOutputBuffer(outIdx)
                        if (outBuf != null && bufferInfo.size > 0) {
                            outBuf.position(bufferInfo.offset)
                            outBuf.limit(bufferInfo.offset + bufferInfo.size)
                            outBuf.order(ByteOrder.LITTLE_ENDIAN)
                            val sampleCount = if (isFloat) bufferInfo.size / 4 else bufferInfo.size / 2
                            if (sampleCount > chunk.size) chunk = ShortArray(sampleCount)
                            if (isFloat) {
                                val fb = outBuf.asFloatBuffer()
                                var i = 0
                                while (fb.hasRemaining()) {
                                    chunk[i++] = (fb.get().coerceIn(-1f, 1f) * 32767f).toInt().toShort()
                                }
                            } else {
                                outBuf.asShortBuffer().get(chunk, 0, sampleCount)
                            }
                            val frameCount = sampleCount / channelCount
                            if (frameCount > 0) {
                                sink.onChunk(chunk, frameCount, channelCount)
                                totalFrames += frameCount
                            }
                        }
                        codec.releaseOutputBuffer(outIdx, false)
                        if (bufferInfo.flags and MediaCodec.BUFFER_FLAG_END_OF_STREAM != 0) outputDone = true
                    }
                }
            }
        } finally {
            codec.stop()
            codec.release()
        }
        return if (totalFrames > 0) totalFrames else null
    }

    /**
     * Run the MediaCodec decode loop.
     * Returns Pair(left, right?) normalized to -1.0..1.0, or null on failure.
     * Stereo sources return separate left/right channels; mono returns Pair(mono, null).
     */
    private fun decode(
        extractor: MediaExtractor,
        format: MediaFormat,
        mime: String,
        sampleRate: Int,
        channelCount: Int,
        maxDurationSec: Int
    ): Pair<FloatArray, FloatArray?>? {
        val codec = try {
            MediaCodec.createDecoderByType(mime)
        } catch (e: Exception) {
            return null
        }
        try {
            codec.configure(format, null, null, 0)
            codec.start()
        } catch (e: Exception) {
            codec.release()
            return null
        }

        // Total interleaved-sample cap as a Long so the no-cap sentinel can't overflow: maxDurationSec = 0
        // means "no limit", and Int.MAX_VALUE * channelCount would wrap to a negative Int and stall the
        // read loop (decode returns empty → every MP3 fails).
        val maxTotalSamples: Long =
            if (maxDurationSec > 0) maxDurationSec.toLong() * sampleRate * channelCount else Long.MAX_VALUE
        // Interleaved PCM shorts. A growing ShortArray, NOT MutableList<Short>: a 60 s stereo
        // 48 kHz extract is ~5.8 M samples, and boxing each one is GC-storm/OOM territory on
        // the 1 GB Miyoo.
        val accumulator = ShortAccumulator()
        val bufferInfo = MediaCodec.BufferInfo()
        val timeoutUs = 10_000L
        var inputDone = false
        var outputDone = false
        // PCM encoding: assume 16-bit (SHORT) until first output buffer tells us otherwise
        var isFloat = false

        try {
            while (!outputDone) {
                // Feed compressed data into decoder
                if (!inputDone) {
                    val inputIndex = codec.dequeueInputBuffer(timeoutUs)
                    if (inputIndex >= 0) {
                        val inputBuffer = codec.getInputBuffer(inputIndex)!!
                        val sampleSize = extractor.readSampleData(inputBuffer, 0)
                        if (sampleSize < 0) {
                            codec.queueInputBuffer(inputIndex, 0, 0, 0, MediaCodec.BUFFER_FLAG_END_OF_STREAM)
                            inputDone = true
                        } else {
                            codec.queueInputBuffer(inputIndex, 0, sampleSize, extractor.sampleTime, 0)
                            extractor.advance()
                        }
                    }
                }

                // Collect decoded PCM output
                val outputIndex = codec.dequeueOutputBuffer(bufferInfo, timeoutUs)
                when {
                    outputIndex == MediaCodec.INFO_OUTPUT_FORMAT_CHANGED -> {
                        // Check actual output encoding after first format change
                        val outFormat = codec.outputFormat
                        if (outFormat.containsKey(MediaFormat.KEY_PCM_ENCODING)) {
                            isFloat = outFormat.getInteger(MediaFormat.KEY_PCM_ENCODING) ==
                                    android.media.AudioFormat.ENCODING_PCM_FLOAT
                        }
                    }
                    outputIndex >= 0 -> {
                        val outputBuffer = codec.getOutputBuffer(outputIndex)
                        if (outputBuffer != null && bufferInfo.size > 0) {
                            outputBuffer.position(bufferInfo.offset)
                            outputBuffer.limit(bufferInfo.offset + bufferInfo.size)
                            readPcmBuffer(outputBuffer, isFloat, accumulator, maxTotalSamples)
                        }
                        codec.releaseOutputBuffer(outputIndex, false)

                        if (bufferInfo.flags and MediaCodec.BUFFER_FLAG_END_OF_STREAM != 0) {
                            outputDone = true
                        }
                        // Stop early if we've hit the duration cap
                        if (accumulator.size >= maxTotalSamples) {
                            outputDone = true
                        }
                    }
                }
            }
        } finally {
            codec.stop()
            codec.release()
        }

        if (accumulator.size == 0) return null

        // Split interleaved shorts into separate channels
        return interleavedShortsToChannels(accumulator.data, accumulator.size, channelCount)
    }

    /**
     * Read PCM data from a ByteBuffer into the accumulator.
     * Handles both 16-bit short and 32-bit float codec output.
     */
    private fun readPcmBuffer(
        buffer: ByteBuffer,
        isFloat: Boolean,
        accumulator: ShortAccumulator,
        maxTotalSamples: Long
    ) {
        buffer.order(ByteOrder.LITTLE_ENDIAN)
        if (isFloat) {
            val fb = buffer.asFloatBuffer()
            while (fb.hasRemaining() && accumulator.size < maxTotalSamples) {
                val f = fb.get().coerceIn(-1f, 1f)
                accumulator.add((f * 32767f).toInt().toShort())
            }
        } else {
            val sb = buffer.asShortBuffer()
            val room = (maxTotalSamples - accumulator.size).coerceAtMost(Int.MAX_VALUE.toLong()).toInt()
            accumulator.addFrom(sb, minOf(sb.remaining(), room))
        }
    }

    /**
     * Split interleaved 16-bit PCM into separate channel FloatArrays.
     * Mono → Pair(mono, null). Stereo → Pair(left, right).
     * 3+ channels: left = ch0, right = ch1 (extras discarded).
     */
    private fun interleavedShortsToChannels(
        shorts: ShortArray,
        sampleCount: Int,
        channelCount: Int
    ): Pair<FloatArray, FloatArray?> {
        val frameCount = sampleCount / channelCount
        val left = FloatArray(frameCount) { i -> (shorts[i * channelCount] / 32768f).coerceIn(-1f, 1f) }
        val right = if (channelCount >= 2) {
            FloatArray(frameCount) { i -> (shorts[i * channelCount + 1] / 32768f).coerceIn(-1f, 1f) }
        } else {
            null
        }
        return Pair(left, right)
    }

    /**
     * Growable primitive short buffer (doubling growth). Replaces MutableList<Short> in the
     * whole-file decode path so PCM samples are never boxed.
     */
    private class ShortAccumulator {
        var data = ShortArray(INITIAL_CAPACITY)
            private set
        var size = 0
            private set

        private fun ensureCapacity(needed: Int) {
            if (needed <= data.size) return
            var newCapacity = data.size * 2
            if (newCapacity < needed) newCapacity = needed  // also handles Int overflow of the doubling
            data = data.copyOf(newCapacity)
        }

        fun add(value: Short) {
            ensureCapacity(size + 1)
            data[size++] = value
        }

        /** Bulk-copy [count] shorts out of [sb] (the 16-bit PCM fast path). */
        fun addFrom(sb: ShortBuffer, count: Int) {
            if (count <= 0) return
            ensureCapacity(size + count)
            sb.get(data, size, count)
            size += count
        }

        companion object {
            private const val INITIAL_CAPACITY = 1 shl 16  // 64 K samples = 128 KB
        }
    }
}
