package com.example.pockettracker.platform.android

import android.content.Context
import android.media.AudioAttributes
import android.media.SoundPool
import com.example.pockettracker.VirtualButton

/**
 * Plays short WAV click sounds on virtual button press/release.
 *
 * Uses Android SoundPool — a completely separate audio stream from the Oboe
 * tracker engine, so button sounds never steal tracker voices.
 *
 * WAV files must be placed in app/src/main/res/raw/ with these names:
 *   ui_sq_press_1.wav, ui_sq_press_2.wav, ...       square button press variants
 *   ui_sq_release_1.wav, ui_sq_release_2.wav, ...   square button release variants
 *   ui_long_press_1.wav, ui_long_press_2.wav, ...   wide button press variants
 *   ui_long_release_1.wav, ui_long_release_2.wav, ... wide button release variants
 *
 * Adding more variants (just drop more files) is automatically picked up.
 * Up to 10 variants per event type are scanned.
 *
 * Button release cuts the in-progress press sound for that specific button,
 * while other simultaneously-held buttons are unaffected.
 */
class ButtonSoundManager(context: Context) {

    var enabled: Boolean = false
    var volume: Float = 1f  // 0f–1f, maps from 00–FF setting

    private val soundPool: SoundPool

    // Sound ID lists (SoundPool integer IDs, 0 = not loaded)
    private val sqPressSounds    = mutableListOf<Int>()
    private val sqReleaseSounds  = mutableListOf<Int>()
    private val longPressSounds  = mutableListOf<Int>()
    private val longReleaseSounds = mutableListOf<Int>()

    // Active press stream per button — allows release to stop the right stream
    private val activeStreams = mutableMapOf<VirtualButton, Int>()

    init {
        val attrs = AudioAttributes.Builder()
            .setUsage(AudioAttributes.USAGE_GAME)
            .setContentType(AudioAttributes.CONTENT_TYPE_SONIFICATION)
            .build()

        soundPool = SoundPool.Builder()
            .setMaxStreams(16)
            .setAudioAttributes(attrs)
            .build()

        loadSounds(context)
    }

    private fun loadSounds(context: Context) {
        val res     = context.resources
        val pkg     = context.packageName

        for (i in 1..10) {
            loadRaw(res, pkg, "ui_sq_press_$i")  ?.let { sqPressSounds.add(it) }
            loadRaw(res, pkg, "ui_sq_release_$i") ?.let { sqReleaseSounds.add(it) }
            loadRaw(res, pkg, "ui_long_press_$i") ?.let { longPressSounds.add(it) }
            loadRaw(res, pkg, "ui_long_release_$i")?.let { longReleaseSounds.add(it) }
        }
    }

    /** Returns SoundPool sound ID for a res/raw resource name, or null if missing. */
    private fun loadRaw(res: android.content.res.Resources, pkg: String, name: String): Int? {
        val id = res.getIdentifier(name, "raw", pkg)
        if (id == 0) return null
        return try {
            soundPool.load(res.openRawResourceFd(id), 1)
        } catch (_: Exception) {
            null
        }
    }

    fun onPress(button: VirtualButton) {
        if (!enabled) return
        val sounds = if (button.isLong()) longPressSounds else sqPressSounds
        if (sounds.isEmpty()) return
        val streamId = soundPool.play(sounds.random(), volume, volume, 1, 0, 1f)
        activeStreams[button] = streamId
    }

    fun onRelease(button: VirtualButton) {
        if (!enabled) return
        // Cut the press sound for exactly this button
        activeStreams.remove(button)?.let { soundPool.stop(it) }
        // Play release sound
        val sounds = if (button.isLong()) longReleaseSounds else sqReleaseSounds
        if (sounds.isEmpty()) return
        soundPool.play(sounds.random(), volume, volume, 1, 0, 1f)
    }

    fun release() {
        soundPool.release()
    }
}

/** Buttons classified as "long/wide" get the ui_long_* samples; others get ui_sq_*. */
fun VirtualButton.isLong(): Boolean = when (this) {
    VirtualButton.L_SHIFT,
    VirtualButton.R_SHIFT -> true
    else                  -> false
}
