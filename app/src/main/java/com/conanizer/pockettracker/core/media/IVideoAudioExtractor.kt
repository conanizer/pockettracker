package com.conanizer.pockettracker.core.media

/**
 * Platform-agnostic interface for extracting audio from video files.
 *
 * Android: Uses MediaExtractor + MediaCodec
 * Linux:   Uses FFmpeg (future)
 *
 * ✅ PLATFORM-AGNOSTIC - No Android dependencies!
 */
interface IVideoAudioExtractor {

    data class ExtractionResult(
        val samples: FloatArray,            // left channel (or mono), normalized -1.0..1.0
        val samplesRight: FloatArray?,      // right channel if stereo, null if mono
        val sampleRate: Int,                // e.g. 48000
        val durationMs: Long,               // total audio duration
        val sourceFormat: String            // e.g. "AAC 48000Hz stereo"
    )

    sealed class ExtractionError : Exception() {
        data class NoAudioTrack(override val message: String) : ExtractionError()
        data class DecodeFailed(override val message: String) : ExtractionError()
        data class FileTooLong(val durationSec: Int, val maxSec: Int) : ExtractionError() {
            override val message = "Audio too long: ${durationSec}s (max ${maxSec}s)"
        }
        data class FileNotFound(val path: String) : ExtractionError() {
            override val message = "File not found: $path"
        }
    }

    /**
     * Extract audio from a video (or audio container) file.
     * @param path Absolute path to the file
     * @param maxDurationSec Maximum duration to extract in seconds (0 = no limit)
     * @return Result with audio data or error
     */
    fun extractAudio(path: String, maxDurationSec: Int = 60): Result<ExtractionResult>

    /**
     * Streaming sink for [extractAudioToSink]: receives decoded interleaved 16-bit PCM block-by-block so
     * the whole file never has to be buffered at once.
     */
    interface PcmSink {
        /** Called once before any chunk, with the source format and an over-estimate of the total frame count. */
        fun onFormat(sampleRate: Int, channels: Int, estimatedFrames: Int)
        /** One decoded block: [interleaved] holds [frameCount]*[channels] samples. The buffer is reused — copy, don't retain. */
        fun onChunk(interleaved: ShortArray, frameCount: Int, channels: Int)
    }

    /** Final metadata returned by [extractAudioToSink] ([frames] = actual frames streamed). */
    data class StreamInfo(val sampleRate: Int, val channels: Int, val frames: Int, val sourceFormat: String)

    /**
     * Decode [path] straight to [sink] without buffering the whole file in one place. Mirrors
     * [extractAudio]'s format handling. Requires duration metadata (to pre-size the destination);
     * returns [ExtractionError.DecodeFailed] if it is absent so the caller can fall back to [extractAudio].
     * @param maxDurationSec 0 = no length limit.
     */
    fun extractAudioToSink(path: String, maxDurationSec: Int, sink: PcmSink): Result<StreamInfo>

    /**
     * Quick check whether a file is a supported video/audio container format.
     * Based on extension only — does not read file contents.
     */
    fun isSupportedVideo(path: String): Boolean

    companion object {
        // True video containers offered "extract audio → WAV". m4a is intentionally absent: it's
        // handled as an in-place audio sample (decoded via MediaCodec, no WAV) — see
        // AudioFormats.SAMPLE_EXTENSIONS. The extractor can still DECODE m4a (extractAudio* run
        // MediaCodec on any path); this list only gates the video→WAV feature.
        val SUPPORTED_EXTENSIONS = listOf("mp4", "mkv", "webm", "3gp", "mov")
    }
}
