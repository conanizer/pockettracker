package com.example.pockettracker.core.resources

/**
 * Platform-agnostic resource loader interface.
 *
 * This interface abstracts resource loading so it works on any platform.
 * The actual implementation is platform-specific (Android resources, Linux filesystem).
 *
 * Implementations:
 * - Android: AndroidResourceLoader (loads from R.raw.*)
 * - Linux: LinuxResourceLoader (future - will load from /usr/share/pockettracker/samples/)
 *
 * Design Philosophy:
 * - Keep interface minimal (only what's needed for loading samples)
 * - No platform-specific types (no Context, no Resources, no resourceId)
 * - All methods return platform-agnostic data (FloatArray, Int, etc.)
 * - Resource names are strings (not Android resource IDs)
 */
interface IResourceLoader {
    /**
     * Load a WAV file resource by name.
     *
     * Returns sample data and metadata needed for playback.
     *
     * @param name Resource name (e.g., "kick", "snare", "hihat")
     * @return SampleData containing float samples, sample rate, and channel count
     * @throws Exception if resource doesn't exist or can't be loaded
     */
    fun loadWav(name: String): SampleData
}

/**
 * Container for loaded sample data.
 *
 * This is returned by IResourceLoader.loadWav() and contains all the information
 * needed to use the sample for playback.
 *
 * @property samples Float array of audio samples (-1.0 to 1.0), already converted to mono
 * @property sampleRate Sample rate in Hz (e.g., 44100, 48000)
 * @property channels Original channel count before conversion (1=mono, 2=stereo)
 */
data class SampleData(
    val samples: FloatArray,
    val sampleRate: Int,
    val channels: Int
) {
    override fun equals(other: Any?): Boolean {
        if (this === other) return true
        if (javaClass != other?.javaClass) return false

        other as SampleData

        if (!samples.contentEquals(other.samples)) return false
        if (sampleRate != other.sampleRate) return false
        if (channels != other.channels) return false

        return true
    }

    override fun hashCode(): Int {
        var result = samples.contentHashCode()
        result = 31 * result + sampleRate
        result = 31 * result + channels
        return result
    }
}
