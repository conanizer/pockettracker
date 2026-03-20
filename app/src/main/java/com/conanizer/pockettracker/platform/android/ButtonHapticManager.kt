package com.conanizer.pockettracker.platform.android

import android.content.Context
import android.os.Build
import android.os.VibrationEffect
import android.os.Vibrator
import android.os.VibratorManager

/**
 * Short haptic pulses on virtual button press and release.
 *
 * Press  → 40 ms pulse
 * Release → 20 ms pulse
 *
 * Durations are tuned for gaming handheld motors (AYANEO etc.) which need
 * longer pulses than phone haptic actuators to produce a perceptible buzz.
 * Fine-tune once tested on a standard Android phone.
 *
 * Uses DEFAULT_AMPLITUDE so the system controls intensity — explicit amplitude
 * values are silently ignored on some custom Android builds.
 *
 * Uses the modern VibrationEffect API (API 26+) with a safe fallback for
 * older devices. VIBRATE permission must be declared in AndroidManifest.xml.
 */
class ButtonHapticManager(context: Context) {

    var enabled: Boolean = false
    var power: Int = 255  // 1–255 vibration amplitude (maps from 00–FF setting)

    private val vibrator: Vibrator? = when {
        Build.VERSION.SDK_INT >= Build.VERSION_CODES.S -> {
            context.getSystemService(VibratorManager::class.java)?.defaultVibrator
        }
        else -> {
            @Suppress("DEPRECATION")
            context.getSystemService(Context.VIBRATOR_SERVICE) as? Vibrator
        }
    }

    fun onPress() {
        vibrate(40)
    }

    fun onRelease() {
        vibrate(20)
    }

    fun hasHardware(): Boolean = vibrator?.hasVibrator() == true

    private fun vibrate(durationMs: Long) {
        if (!enabled || vibrator == null || !vibrator.hasVibrator()) return
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
            vibrator.vibrate(VibrationEffect.createOneShot(durationMs, VibrationEffect.DEFAULT_AMPLITUDE))
        } else {
            @Suppress("DEPRECATION")
            vibrator.vibrate(durationMs)
        }
    }
}
