package com.conanizer.pockettracker.platform.android

import android.content.Context
import android.os.Build
import android.os.VibrationEffect
import android.os.Vibrator
import android.os.VibratorManager
import android.view.HapticFeedbackConstants
import android.view.View

/**
 * Short haptic pulses on on-screen button press and release, by the best API the device has:
 *   1. API 30+ Composition primitives (CLICK / TICK) — crispest, POW scales them continuously; many
 *      ROMs (LineageOS) report them unsupported even on capable actuators;
 *   2. API 29+ a predefined effect picked by POW (TICK light, CLICK strong) — still a crisp click;
 *   3. API 26+ an amplitude one-shot — honours POW but feels like a buzz;
 *   4. else `View.performHapticFeedback`, ignoring POW.
 */
class ButtonHapticManager(context: Context) {

    var enabled: Boolean = false
    var power: Int = 255  // 1–255

    private val vibrator: Vibrator? = when {
        Build.VERSION.SDK_INT >= Build.VERSION_CODES.S -> {
            context.getSystemService(VibratorManager::class.java)?.defaultVibrator
        }
        else -> {
            @Suppress("DEPRECATION")
            context.getSystemService(Context.VIBRATOR_SERVICE) as? Vibrator
        }
    }

    // Composition primitive support, asked once (API 30+).
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
        // A predefined click by POW, before the buzzier amplitude one-shot.
        if (hapticPredefined(predefinedForPower(power))) return
        if (hapticAmplitude(durationMs = 8, amplitude = power.coerceIn(1, 255))) return
        view.performHapticFeedback(HapticFeedbackConstants.VIRTUAL_KEY)
    }

    fun onRelease(view: View) {
        if (!enabled) return
        val releaseAmplitude = (power * 0.35f).toInt().coerceIn(1, 255)
        val scale = releaseAmplitude / 255f
        if (hapticComposition(VibrationEffect.Composition.PRIMITIVE_TICK, scale, supportsTickPrimitive)) return
        // The release is 0.35 × POW, which keeps the predefined choice on the light TICK.
        if (hapticPredefined(predefinedForPower(releaseAmplitude))) return
        if (hapticAmplitude(durationMs = 5, amplitude = releaseAmplitude)) return
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O_MR1) {
            view.performHapticFeedback(HapticFeedbackConstants.VIRTUAL_KEY_RELEASE)
        }
    }

    // A short one-shot at an explicit amplitude; needs amplitude control. True if it vibrated.
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

    // POW onto a predefined click. Two steps: on the actuators tried only TICK and CLICK differ
    // (HEAVY_CLICK sounds like TICK).
    private fun predefinedForPower(p: Int): Int =
        if (p < 128) VibrationEffect.EFFECT_TICK else VibrationEffect.EFFECT_CLICK

    /** Plays a predefined effect. Returns true if vibration was triggered. */
    private fun hapticPredefined(effectId: Int): Boolean {
        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.Q) return false
        if (vibrator == null || !vibrator.hasVibrator()) return false
        vibrator.vibrate(VibrationEffect.createPredefined(effectId))
        return true
    }
}
