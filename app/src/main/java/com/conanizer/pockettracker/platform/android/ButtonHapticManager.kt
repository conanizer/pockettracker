package com.conanizer.pockettracker.platform.android

import android.content.Context
import android.os.Build
import android.os.VibrationEffect
import android.os.Vibrator
import android.os.VibratorManager
import android.view.HapticFeedbackConstants
import android.view.View

/**
 * Short haptic pulses on virtual button press and release.
 *
 * Uses the best available haptic API in order of preference:
 *   1. API 30+: VibrationEffect.Composition (PRIMITIVE_CLICK / PRIMITIVE_TICK)
 *      — crispest feel, supports amplitude scaling via [power]
 *   2. API 29:  VibrationEffect.createPredefined (EFFECT_CLICK / EFFECT_TICK)
 *      — good feel, ignores [power]
 *   3. API <29: View.performHapticFeedback (VIRTUAL_KEY / VIRTUAL_KEY_RELEASE)
 *      — system-default feel, ignores [power]
 *
 * VIBRATE permission must be declared in AndroidManifest.xml.
 */
class ButtonHapticManager(context: Context) {

    var enabled: Boolean = false
    var power: Int = 255  // 1–255; maps to 0.0–1.0 amplitude scale (Composition API only)

    private val vibrator: Vibrator? = when {
        Build.VERSION.SDK_INT >= Build.VERSION_CODES.S -> {
            context.getSystemService(VibratorManager::class.java)?.defaultVibrator
        }
        else -> {
            @Suppress("DEPRECATION")
            context.getSystemService(Context.VIBRATOR_SERVICE) as? Vibrator
        }
    }

    // Lazily check device support for Composition primitives (API 30+)
    private val supportsClickPrimitive: Boolean by lazy {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R && vibrator != null) {
            vibrator.arePrimitivesSupported(VibrationEffect.Composition.PRIMITIVE_CLICK)[0]
        } else false
    }
    private val supportsTickPrimitive: Boolean by lazy {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R && vibrator != null) {
            vibrator.arePrimitivesSupported(VibrationEffect.Composition.PRIMITIVE_TICK)[0]
        } else false
    }

    fun onPress(view: View) {
        if (!enabled) return
        val scale = power.coerceIn(1, 255) / 255f
        if (hapticComposition(VibrationEffect.Composition.PRIMITIVE_CLICK, scale, supportsClickPrimitive)) return
        if (hapticPredefined(VibrationEffect.EFFECT_CLICK)) return
        if (hapticAmplitude(durationMs = 8, amplitude = power.coerceIn(1, 255))) return
        view.performHapticFeedback(HapticFeedbackConstants.VIRTUAL_KEY)
    }

    fun onRelease(view: View) {
        if (!enabled) return
        val releaseAmplitude = (power * 0.35f).toInt().coerceIn(1, 255)
        val scale = releaseAmplitude / 255f
        if (hapticComposition(VibrationEffect.Composition.PRIMITIVE_TICK, scale, supportsTickPrimitive)) return
        if (hapticPredefined(VibrationEffect.EFFECT_TICK)) return
        if (hapticAmplitude(durationMs = 5, amplitude = releaseAmplitude)) return
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O_MR1) {
            view.performHapticFeedback(HapticFeedbackConstants.VIRTUAL_KEY_RELEASE)
        }
    }

    fun hasHardware(): Boolean = vibrator?.hasVibrator() == true

    /**
     * Short pulse with explicit amplitude — feels like a click when [durationMs] is ≤ 10ms.
     * Requires the device to support amplitude control. Returns true if vibration was triggered.
     */
    private fun hapticAmplitude(durationMs: Long, amplitude: Int): Boolean {
        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.O) return false
        if (vibrator == null || !vibrator.hasVibrator()) return false
        if (!vibrator.hasAmplitudeControl()) return false
        vibrator.vibrate(VibrationEffect.createOneShot(durationMs, amplitude))
        return true
    }

    /** Plays a single Composition primitive at [scale]. Returns true if vibration was triggered. */
    private fun hapticComposition(primitiveId: Int, scale: Float, supported: Boolean): Boolean {
        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.R) return false
        if (vibrator == null || !vibrator.hasVibrator() || !supported) return false
        val effect = VibrationEffect.startComposition()
            .addPrimitive(primitiveId, scale.coerceIn(0f, 1f))
            .compose()
        vibrator.vibrate(effect)
        return true
    }

    /** Plays a predefined effect. Returns true if vibration was triggered. */
    private fun hapticPredefined(effectId: Int): Boolean {
        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.Q) return false
        if (vibrator == null || !vibrator.hasVibrator()) return false
        vibrator.vibrate(VibrationEffect.createPredefined(effectId))
        return true
    }
}
