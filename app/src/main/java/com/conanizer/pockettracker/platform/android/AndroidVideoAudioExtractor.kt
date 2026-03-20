package com.conanizer.pockettracker.platform.android

import android.media.MediaCodec
import android.media.MediaExtractor
import android.media.MediaFormat
import com.conanizer.pockettracker.core.media.IVideoAudioExtractor
import java.io.File
import java.nio.ByteBuffer
import java.nio.ByteOrder

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
            val pcmSamples = decode(extractor, format, mime, sampleRate, channelCount, maxDurationSec)
                ?: return Result.failure(
                    IVideoAudioExtractor.ExtractionError.DecodeFailed("MediaCodec decode failed for: $path")
                )

            return Result.success(
                IVideoAudioExtractor.ExtractionResult(
                    samples = pcmSamples,
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

    /**
     * Run the MediaCodec decode loop.
     * Returns mono FloatArray normalized to -1.0..1.0, or null on failure.
     */
    private fun decode(
        extractor: MediaExtractor,
        format: MediaFormat,
        mime: String,
        sampleRate: Int,
        channelCount: Int,
        maxDurationSec: Int
    ): FloatArray? {
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

        val maxSamples = if (maxDurationSec > 0) maxDurationSec * sampleRate else Int.MAX_VALUE
        val accumulator = mutableListOf<Short>()  // interleaved PCM shorts
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
                            readPcmBuffer(outputBuffer, isFloat, accumulator, maxSamples * channelCount)
                        }
                        codec.releaseOutputBuffer(outputIndex, false)

                        if (bufferInfo.flags and MediaCodec.BUFFER_FLAG_END_OF_STREAM != 0) {
                            outputDone = true
                        }
                        // Stop early if we've hit the duration cap
                        if (accumulator.size >= maxSamples * channelCount) {
                            outputDone = true
                        }
                    }
                }
            }
        } finally {
            codec.stop()
            codec.release()
        }

        if (accumulator.isEmpty()) return null

        // Convert interleaved shorts → mono float
        return stereoShortsToMonoFloat(accumulator, channelCount)
    }

    /**
     * Read PCM data from a ByteBuffer into the accumulator list.
     * Handles both 16-bit short and 32-bit float codec output.
     */
    private fun readPcmBuffer(
        buffer: ByteBuffer,
        isFloat: Boolean,
        accumulator: MutableList<Short>,
        maxTotalSamples: Int
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
            while (sb.hasRemaining() && accumulator.size < maxTotalSamples) {
                accumulator.add(sb.get())
            }
        }
    }

    /**
     * Convert interleaved 16-bit PCM (any channel count) to mono FloatArray.
     * Uses simple average mix for stereo/multi-channel downmix.
     */
    private fun stereoShortsToMonoFloat(shorts: List<Short>, channelCount: Int): FloatArray {
        val monoCount = shorts.size / channelCount
        val result = FloatArray(monoCount)
        for (i in 0 until monoCount) {
            var sum = 0f
            for (ch in 0 until channelCount) {
                sum += shorts[i * channelCount + ch] / 32768f
            }
            result[i] = (sum / channelCount).coerceIn(-1f, 1f)
        }
        return result
    }
}
