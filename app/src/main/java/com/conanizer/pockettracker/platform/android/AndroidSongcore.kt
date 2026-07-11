package com.conanizer.pockettracker.platform.android

import android.util.Log
import com.conanizer.pockettracker.core.audio.ISongcore

/**
 * Android's [ISongcore] — the JNI half of songcore's platform shell (C++ half: `native/songcore-jni.cpp`).
 *
 * Pure marshalling. The native library is the same `libpockettracker.so` [OboeAudioBackend] loads; the
 * `create()` call must come after the audio engine exists, because songcore reads the engine's frame
 * counter as its transport clock.
 */
class AndroidSongcore : ISongcore {

    companion object {
        private const val TAG = "AndroidSongcore"

        init {
            // Idempotent — OboeAudioBackend has usually loaded it already, but songcore must not
            // depend on that ordering.
            try {
                System.loadLibrary("pockettracker")
            } catch (e: UnsatisfiedLinkError) {
                Log.e(TAG, "❌ Failed to load native library: ${e.message}")
            }
        }
    }

    override fun create() = native_create()
    override fun destroy() = native_destroy()

    override fun pushProject(blob: ByteArray): Boolean = native_pushProject(blob)

    override fun pushRouting(sampleRateRatios: FloatArray, sfSlots: IntArray) =
        native_pushRouting(sampleRateRatios, sfSlots)

    override fun playSong(startRow: Int): Long = native_playSong(startRow)
    override fun playChain(chainId: Int): Long = native_playChain(chainId)
    override fun playPhrase(phraseId: Int): Long = native_playPhrase(phraseId)

    override fun stop() = native_stop()
    override fun poll() = native_poll()
    override fun isPlaying(): Boolean = native_isPlaying()
    override fun getTrackMask(): Int = native_getTrackMask()

    override fun scheduleSongRange(startRow: Int, endRow: Int, trackFilter: IntArray?): Long =
        native_scheduleSongRange(startRow, endRow, trackFilter)

    override fun notifyDataChanged() = native_notifyDataChanged()

    override fun getPlayheads(): IntArray = native_getPlayheads()

    override fun setTrace(enabled: Boolean, path: String) = native_setTrace(enabled, path)

    // ── native ───────────────────────────────────────────────────────────────────────────────────
    private external fun native_create()
    private external fun native_destroy()

    /** The project as raw UTF-8 bytes, not a String: JNI strings are *modified* UTF-8, and the trace
     *  header's sha must be over the real UTF-8 bytes Kotlin hashes (and this skips transcoding ~440 KB). */
    private external fun native_pushProject(blob: ByteArray): Boolean

    private external fun native_pushRouting(sampleRateRatios: FloatArray, sfSlots: IntArray)

    private external fun native_playSong(startRow: Int): Long
    private external fun native_playChain(chainId: Int): Long
    private external fun native_playPhrase(phraseId: Int): Long
    private external fun native_stop()
    private external fun native_poll()
    private external fun native_isPlaying(): Boolean
    private external fun native_getTrackMask(): Int
    private external fun native_scheduleSongRange(startRow: Int, endRow: Int, trackFilter: IntArray?): Long
    private external fun native_notifyDataChanged()
    private external fun native_getPlayheads(): IntArray
    private external fun native_setTrace(enabled: Boolean, path: String)
}
