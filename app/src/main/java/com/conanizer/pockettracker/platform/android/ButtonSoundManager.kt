package com.conanizer.pockettracker.platform.android

import android.content.Context
import android.media.AudioAttributes
import android.media.AudioFormat
import android.media.AudioTrack
import android.os.Build
import android.util.Log
import com.conanizer.pockettracker.input.VirtualButton
import java.util.concurrent.Executors

/**
 * Plays short WAV click sounds on virtual button press/release.
 *
 * A completely separate audio path from the Oboe tracker engine, so button
 * sounds never steal tracker voices.
 *
 * ⚠️ **Why this uses AudioTrack with software gain instead of SoundPool (B1).**
 * The click must be captured on a screen recording *at the level the user hears*.
 * SoundPool applied BTN VOL as a per-stream track gain (`play(sound, vol, vol, …)`),
 * and that gain is applied by AudioFlinger **downstream of the MediaProjection
 * playback-capture tap** — so the speaker honoured BTN VOL but the recording always
 * got unity gain. Confirmed on-device: at BTN VOL = 00 the speaker is silent yet the
 * recording still captures the click at full. `AudioTrack.setVolume` is the *same*
 * downstream track gain, so it would be captured wrong too. The only fix that every
 * capture path reflects is to bake the gain into the PCM the app submits — we scale
 * the decoded samples by BTN VOL in software here, before writing them to the track.
 *
 * ⚠️ **Latency: a POOL of reused tracks, not one per press.** Building an `AudioTrack`
 * (registering a track with AudioFlinger) on every tap is far heavier than SoundPool's
 * fire-and-forget and was audibly laggy. Instead we pre-build a small pool of streaming
 * float tracks once and reuse them (pause → flush → write → play); the buffer holds the
 * whole clip so the write never blocks. When every voice is busy the oldest is stolen.
 *
 * WAV files must be placed in app/src/main/res/raw/ with these names:
 *   ui_sq_press_1.wav, ui_sq_press_2.wav, ...       square button press variants
 *   ui_sq_release_1.wav, ui_sq_release_2.wav, ...   square button release variants
 *   ui_long_press_1.wav, ui_long_press_2.wav, ...   wide button press variants
 *   ui_long_release_1.wav, ui_long_release_2.wav, ... wide button release variants
 *
 * Adding more variants (just drop more files) is automatically picked up.
 * Up to 10 variants per event type are scanned. 16-, 24- and 32-bit-float PCM WAVs
 * (mono or stereo) are supported; the shipped clicks are 48 kHz / stereo / 16-bit. ⚠️ Bit depth here
 * is a PACKAGING choice and nothing else: every arm decodes to the same FloatArray, so a deeper
 * format costs APK bytes (24-bit was +50 % on 2.8 MB of clips) and saves no resident heap at all.
 * What DOES move the 3.5 MB these occupy in RAM is the channel count and the frame count.
 *
 * Button release cuts the in-progress press sound for that specific button,
 * while other simultaneously-held buttons are unaffected.
 */
class ButtonSoundManager(context: Context) {

    var enabled: Boolean = false
    var volume: Float = 1f  // 0f–1f, maps from 00–FF setting

    /** One decoded clip: interleaved float samples in [-1, 1], plus its format. */
    private class Clip(val samples: FloatArray, val sampleRate: Int, val channels: Int)

    /** A track currently sounding: its length (to tell when it finished) and whether it is a
     *  pool-format track (reusable) or a one-off built for an odd clip format (released after). */
    private class Voice(val track: AudioTrack, val frames: Int, val pooled: Boolean)

    private val sqPress     = mutableListOf<Clip>()
    private val sqRelease   = mutableListOf<Clip>()
    private val longPress   = mutableListOf<Clip>()
    private val longRelease = mutableListOf<Clip>()

    private val attrs = AudioAttributes.Builder()
        .setUsage(AudioAttributes.USAGE_GAME)
        .setContentType(AudioAttributes.CONTENT_TYPE_SONIFICATION)
        .build()

    // All track construction / playback / bookkeeping is confined to this single thread,
    // so the collections below need no locking, and it stays off the SDL input thread.
    private val exec = Executors.newSingleThreadExecutor()

    // The pool's fixed format (from the loaded clips) and per-track capacity in frames.
    private var poolSampleRate = 0
    private var poolChannels   = 0
    private var poolFrames     = 0

    // Accessed only on `exec`.
    private val idle           = ArrayDeque<AudioTrack>()          // built, ready to reuse
    private val playing        = mutableListOf<Voice>()            // currently sounding
    private val activeByButton = mutableMapOf<VirtualButton, Voice>()
    private var built          = 0                                 // total tracks constructed

    @Volatile private var released = false

    init {
        loadSounds(context)
    }

    private fun loadSounds(context: Context) {
        val res = context.resources
        val pkg = context.packageName
        for (i in 1..10) {
            loadClip(res, pkg, "ui_sq_press_$i")    ?.let { sqPress.add(it) }
            loadClip(res, pkg, "ui_sq_release_$i")   ?.let { sqRelease.add(it) }
            loadClip(res, pkg, "ui_long_press_$i")   ?.let { longPress.add(it) }
            loadClip(res, pkg, "ui_long_release_$i") ?.let { longRelease.add(it) }
        }
        // Fix the pool format/size from the loaded clips (the shipped set is uniform;
        // if a stray clip has a longer or different-format buffer it plays one-off).
        val all = sqPress + sqRelease + longPress + longRelease
        val ref = all.firstOrNull() ?: return
        poolSampleRate = ref.sampleRate
        poolChannels   = ref.channels
        poolFrames     = all.filter { it.sampleRate == ref.sampleRate && it.channels == ref.channels }
                            .maxOf { it.samples.size / it.channels }
    }

    private fun loadClip(res: android.content.res.Resources, pkg: String, name: String): Clip? {
        val id = res.getIdentifier(name, "raw", pkg)
        if (id == 0) return null
        return try {
            val bytes = res.openRawResource(id).use { it.readBytes() }
            decodeWav(bytes)
        } catch (e: Exception) {
            Log.w(TAG, "failed to decode $name", e)
            null
        }
    }

    fun onPress(button: VirtualButton) {
        if (!enabled || released) return
        val clip = (if (button.isLong()) longPress else sqPress).randomOrNull() ?: return
        val gain = volume
        submit {
            reapFinished()
            activeByButton.remove(button)?.let { recycle(it) }
            play(clip, gain)?.let { v ->
                activeByButton[button] = v
                playing.add(v)
            }
        }
    }

    fun onRelease(button: VirtualButton) {
        if (!enabled || released) return
        val clip = (if (button.isLong()) longRelease else sqRelease).randomOrNull()
        val gain = volume
        submit {
            // Cut the press sound for exactly this button, then play its release click.
            activeByButton.remove(button)?.let { recycle(it) }
            reapFinished()
            if (clip != null) play(clip, gain)?.let { playing.add(it) }
        }
    }

    fun release() {
        released = true
        submit {
            for (v in playing) release(v.track)
            for (t in idle) release(t)
            playing.clear(); idle.clear(); activeByButton.clear()
        }
        exec.shutdown()
    }

    // ── internals (all run on `exec`) ─────────────────────────────────────────────

    private inline fun submit(crossinline task: () -> Unit) {
        try {
            exec.execute { try { task() } catch (e: Exception) { Log.w(TAG, "button sound", e) } }
        } catch (_: java.util.concurrent.RejectedExecutionException) {
            // Shutting down — drop the click.
        }
    }

    /** Scale by gain in software (so the emitted PCM is already at BTN VOL) and start a voice. */
    private fun play(clip: Clip, gain: Float): Voice? {
        val src = clip.samples
        val scaled = FloatArray(src.size)
        for (i in src.indices) scaled[i] = src[i] * gain   // |src| ≤ 1, gain ≤ 1 → no clip
        val frames = scaled.size / clip.channels

        val poolable = clip.sampleRate == poolSampleRate &&
                       clip.channels   == poolChannels   && frames <= poolFrames
        val track = (if (poolable) obtainPooled() else null)
            ?: buildTrack(clip.sampleRate, clip.channels, frames) ?: return null

        return try {
            try { track.pause() } catch (_: Exception) {}
            try { track.flush() } catch (_: Exception) {}   // reset playhead + drop old data
            track.write(scaled, 0, scaled.size, AudioTrack.WRITE_BLOCKING)  // buffer fits → fast
            track.play()
            Voice(track, frames, poolable)
        } catch (e: Exception) {
            Log.w(TAG, "play failed", e); release(track); null
        }
    }

    /** A ready-to-reuse pooled track: an idle one, a freshly built one, or the oldest voice stolen. */
    private fun obtainPooled(): AudioTrack? {
        idle.removeLastOrNull()?.let { return it }
        if (built < POOL_MAX) buildTrack(poolSampleRate, poolChannels, poolFrames)?.let {
            built++; return it
        }
        // Pool exhausted — steal the oldest still-sounding voice.
        val oldest = playing.removeFirstOrNull() ?: return null
        unmap(oldest)
        return oldest.track
    }

    private fun buildTrack(sampleRate: Int, channels: Int, frames: Int): AudioTrack? = try {
        AudioTrack.Builder()
            .setAudioAttributes(attrs)
            .setAudioFormat(
                AudioFormat.Builder()
                    .setEncoding(AudioFormat.ENCODING_PCM_FLOAT)
                    .setSampleRate(sampleRate)
                    .setChannelMask(
                        if (channels == 1) AudioFormat.CHANNEL_OUT_MONO
                        else AudioFormat.CHANNEL_OUT_STEREO
                    )
                    .build()
            )
            .setBufferSizeInBytes(frames * channels * 4)   // whole clip → write() won't block
            .setTransferMode(AudioTrack.MODE_STREAM)
            .apply {
                if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O)
                    setPerformanceMode(AudioTrack.PERFORMANCE_MODE_LOW_LATENCY)
            }
            .build()
    } catch (e: Exception) {
        Log.w(TAG, "AudioTrack build failed", e); null
    }

    /** Return a finished/cut voice's track to the idle pool (or release a non-pooled one-off). */
    private fun recycle(v: Voice) {
        playing.remove(v); unmap(v)
        if (v.pooled) {
            try { v.track.pause() } catch (_: Exception) {}
            idle.addLast(v.track)
        } else {
            release(v.track)
        }
    }

    /** Move tracks that have played to the end back into the idle pool (release one-offs). */
    private fun reapFinished() {
        val it = playing.iterator()
        while (it.hasNext()) {
            val v = it.next()
            if (v.track.playbackHeadPosition >= v.frames) {
                it.remove(); unmap(v)
                if (v.pooled) {
                    try { v.track.pause() } catch (_: Exception) {}
                    idle.addLast(v.track)
                } else {
                    release(v.track)
                }
            }
        }
    }

    private fun unmap(v: Voice) {
        val e = activeByButton.entries.firstOrNull { it.value === v }
        if (e != null) activeByButton.remove(e.key)
    }

    private fun release(t: AudioTrack) {
        try { t.stop() } catch (_: Exception) {}
        try { t.release() } catch (_: Exception) {}
    }

    // ── WAV decoding → normalized interleaved float ──────────────────────────────

    private fun decodeWav(b: ByteArray): Clip? {
        if (b.size < 12 || !tag(b, 0, "RIFF") || !tag(b, 8, "WAVE")) return null

        var channels = 0; var sampleRate = 0; var bits = 0; var audioFormat = 0
        var dataOff = -1; var dataLen = 0

        var p = 12
        while (p + 8 <= b.size) {
            val id = String(b, p, 4, Charsets.US_ASCII)
            val size = leU32(b, p + 4)
            val body = p + 8
            when (id) {
                "fmt " -> {
                    audioFormat = leU16(b, body)
                    channels    = leU16(b, body + 2)
                    sampleRate  = leU32(b, body + 4)
                    bits        = leU16(b, body + 14)
                }
                "data" -> { dataOff = body; dataLen = minOf(size, b.size - body) }
            }
            // Chunks are word-aligned: an odd size carries a pad byte.
            p = body + size + (size and 1)
        }

        if (dataOff < 0 || channels <= 0 || sampleRate <= 0) return null

        val samples: FloatArray = when {
            audioFormat == 3 && bits == 32 -> {  // IEEE float
                val n = dataLen / 4
                FloatArray(n) { i -> Float.fromBits(leU32(b, dataOff + i * 4)) }
            }
            audioFormat == 1 && bits == 16 -> {
                val n = dataLen / 2
                FloatArray(n) { i -> leS16(b, dataOff + i * 2) / 32768f }
            }
            audioFormat == 1 && bits == 24 -> {
                val n = dataLen / 3
                FloatArray(n) { i ->
                    val o = dataOff + i * 3
                    var s = (b[o].toInt() and 0xFF) or
                            ((b[o + 1].toInt() and 0xFF) shl 8) or
                            ((b[o + 2].toInt() and 0xFF) shl 16)
                    if (s and 0x800000 != 0) s = s or -0x1000000   // sign-extend 24→32
                    s / 8388608f
                }
            }
            audioFormat == 1 && bits == 32 -> {  // 32-bit int PCM
                val n = dataLen / 4
                FloatArray(n) { i -> leU32(b, dataOff + i * 4) / 2147483648f }
            }
            else -> { Log.w(TAG, "unsupported WAV fmt=$audioFormat bits=$bits"); return null }
        }
        return Clip(samples, sampleRate, channels)
    }

    private fun tag(b: ByteArray, o: Int, s: String) =
        o + 4 <= b.size && String(b, o, 4, Charsets.US_ASCII) == s
    private fun leU16(b: ByteArray, o: Int) =
        (b[o].toInt() and 0xFF) or ((b[o + 1].toInt() and 0xFF) shl 8)
    private fun leS16(b: ByteArray, o: Int) = leU16(b, o).toShort().toInt()
    private fun leU32(b: ByteArray, o: Int) =
        (b[o].toInt() and 0xFF) or ((b[o + 1].toInt() and 0xFF) shl 8) or
        ((b[o + 2].toInt() and 0xFF) shl 16) or ((b[o + 3].toInt() and 0xFF) shl 24)

    private companion object {
        const val TAG = "ButtonSoundManager"
        const val POOL_MAX = 8
    }
}

/** Buttons classified as "long/wide" get the ui_long_* samples; others get ui_sq_*. */
fun VirtualButton.isLong(): Boolean = when (this) {
    VirtualButton.L_SHIFT,
    VirtualButton.R_SHIFT -> true
    else                  -> false
}
