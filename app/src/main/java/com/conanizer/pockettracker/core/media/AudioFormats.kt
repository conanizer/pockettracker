package com.conanizer.pockettracker.core.media

/**
 * Canonical file-extension policy for audio that loads **in place as a sample** — decoded to PCM in
 * RAM, the original file kept on disk, no WAV written. Single source of truth shared by core + UI so
 * the load / reload / preview / browser-filter paths can't drift apart.
 *
 * Lives in `core` (pure data, no Android deps) so `core/logic` can use it without reaching up into the
 * UI layer. True video containers (mp4/mkv/…) are a separate "extract audio → WAV" feature — see
 * [IVideoAudioExtractor.SUPPORTED_EXTENSIONS].
 */
object AudioFormats {
    /** All in-place sample formats (browser filter + "is this loaded as a sample" check). */
    val SAMPLE_EXTENSIONS = listOf("wav", "mp3", "flac", "ogg", "opus", "m4a")

    /** Compressed audio (everything except wav) — routed through `AudioEngine.loadSampleCompressed`. */
    val COMPRESSED_EXTENSIONS = listOf("mp3", "flac", "ogg", "opus", "m4a")

    /** Compressed formats decoded by the bundled native decoders — dr_mp3 / dr_flac / stb_vorbis, plus
     *  libopus for `.opus` and Opus-in-`.ogg`. The remainder (m4a/aac) decode via the OS MediaCodec
     *  extractor (no good native AAC decoder). */
    val NATIVE_EXTENSIONS = listOf("mp3", "flac", "ogg", "opus")

    /** True if [ext] is a compressed sample format (decode to RAM, no WAV, re-decode on reload). */
    fun isCompressed(ext: String) = ext.lowercase() in COMPRESSED_EXTENSIONS

    /** True if [ext] is decoded by a native single-header decoder (vs the MediaCodec extractor). */
    fun isNative(ext: String) = ext.lowercase() in NATIVE_EXTENSIONS
}
