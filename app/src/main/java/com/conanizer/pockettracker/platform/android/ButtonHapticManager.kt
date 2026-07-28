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
 *      — crispest feel, and POW scales it CONTINUOUSLY. Many custom ROMs (e.g. LineageOS) report
 *        arePrimitivesSupported = false even on capable actuators, so this often can't be used.
 *   2. API 29+: VibrationEffect.createPredefined, the EFFECT chosen BY POW (TICK light / CLICK strong)
 *      — a tuned click: the crisp HAPTIC feel, not a buzzy vibration. POW picks the strength (two
 *        discrete steps — see predefinedForPower). Preferred over the one-shot so it stays a HAPTIC.
 *   3. API 26+: VibrationEffect.createOneShot with explicit amplitude (hapticAmplitude)
 *      — honours [power] continuously but feels like a VIBRATION; last resort where predefined
 *        effects are unavailable (API 26–28).
 *   4. API <29: View.performHapticFeedback (VIRTUAL_KEY / VIRTUAL_KEY_RELEASE)
 *      — system-default feel, ignores [power]
 *
 * ⚠️ POW-as-vibration was the first cut of this (createOneShot ahead of predefined): it made POW work
 * but turned every press into a buzz. On an actuator without Composition primitives, crisp-haptic and
 * continuous-POW cannot both be had — the predefined family is the crisp-haptic answer, and POW steps
 * through it. Only virtual-button (touch) layouts reach this; physical-pad devices never call it.
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
        // Predefined effect chosen BY POW: keeps the crisp HAPTIC feel (a tuned click) rather than the
        // buzzy amplitude one-shot, while still letting POW change the pulse — light TICK / strong CLICK.
        if (hapticPredefined(predefinedForPower(power))) return
        // Amplitude one-shot: last resort (API 26–28, no predefined effects). Feels like a vibration.
        if (hapticAmplitude(durationMs = 8, amplitude = power.coerceIn(1, 255))) return
        view.performHapticFeedback(HapticFeedbackConstants.VIRTUAL_KEY)
    }

    fun onRelease(view: View) {
        if (!enabled) return
        val releaseAmplitude = (power * 0.35f).toInt().coerceIn(1, 255)
        val scale = releaseAmplitude / 255f
        if (hapticComposition(VibrationEffect.Composition.PRIMITIVE_TICK, scale, supportsTickPrimitive)) return
        // Predefined, chosen by the (already reduced) release power — a lighter tick, same crisp-haptic
        // reasoning as onPress. The 0.35 scale keeps it in the TICK/CLICK range so the release stays soft.
        if (hapticPredefined(predefinedForPower(releaseAmplitude))) return
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

    /**
     * Map POW (1..255) onto a predefined "click": light below the midpoint, strong above it. Two steps,
     * not three — on the actuators tested (Xiaomi 12T Pro / LineageOS), only EFFECT_TICK (light) and
     * EFFECT_CLICK (strong) are distinct; EFFECT_HEAVY_CLICK collapses onto TICK, so it is not used.
     * This keeps the crisp HAPTIC feel (a tuned click) instead of a buzzy amplitude one-shot, while POW
     * still changes the pulse. No Composition primitives on these ROMs, so continuous POW isn't crisp.
     */
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
