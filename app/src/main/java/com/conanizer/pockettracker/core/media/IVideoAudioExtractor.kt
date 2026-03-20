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
        val samples: FloatArray,    // mono, normalized -1.0..1.0
        val sampleRate: Int,        // e.g. 48000
        val durationMs: Long,       // total audio duration
        val sourceFormat: String    // e.g. "AAC 48000Hz stereo"
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
     * Quick check whether a file is a supported video/audio container format.
     * Based on extension only — does not read file contents.
     */
    fun isSupportedVideo(path: String): Boolean

    companion object {
        val SUPPORTED_EXTENSIONS = listOf("mp4", "mkv", "webm", "3gp", "m4a", "mov")
    }
}
