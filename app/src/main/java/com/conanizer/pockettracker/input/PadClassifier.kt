package com.conanizer.pockettracker.input

import android.view.InputDevice
import android.view.KeyEvent
import android.view.MotionEvent

/**
 * Is this input device a real game controller? Behind `MainActivity.hasPhysicalGameButtons()`, which
 * the shell asks at boot and on every controller add/remove to choose the touch UI or FULL.
 *
 * ⚠️ A wrong `true` locks a phone user out: FULL draws no buttons, so nothing can reach SETTINGS. A
 * wrong `false` only gives a handheld the touch skin. A judgement call goes the way that keeps input.
 *
 * ⚠️ The `sources` bits are not enough: Android calls any device with `BTN_0`…`BTN_9` keys a gamepad
 * and joystick, and fingerprint readers (`uinput-fpc`, `uinput-goodixfp`) declare exactly those — a
 * Redmi 10C reports `sources=0x1000511` with no buttons anywhere.
 */
object PadClassifier {

    /** The verdict and why; the reason goes into the boot log. */
    data class Verdict(val isPad: Boolean, val reason: String)

    // Fingerprint readers that claim to be pads. ⚠️ Applied BEFORE the control test, which a sensor
    // with swipe axes would pass. Fingerprint-specific, so a real pad arriving over `uinput` passes.
    private val SENSOR_NAME_FRAGMENTS =
        arrayOf("fingerprint", "goodixfp", "fpc", "uinput-fp", "fpsensor")

    /** Names in the `NAME` position of a keyboard-classed device that really is a controller. */
    private val CONTROLLER_NAME_FRAGMENTS = arrayOf("xbox", "controller", "gamepad")

    /**
     * Buttons a real controller has and a pseudo-device does not; parallel to [KEYCODE_NAMES].
     * ⚠️ `KEYCODE_BUTTON_1`…`_16` are left out: `BTN_0`… map onto them, and those are what the
     * fingerprint readers declare.
     */
    val QUERY_KEYCODES = intArrayOf(
        KeyEvent.KEYCODE_BUTTON_A,      KeyEvent.KEYCODE_BUTTON_B,
        KeyEvent.KEYCODE_BUTTON_X,      KeyEvent.KEYCODE_BUTTON_Y,
        KeyEvent.KEYCODE_BUTTON_L1,     KeyEvent.KEYCODE_BUTTON_R1,
        KeyEvent.KEYCODE_BUTTON_L2,     KeyEvent.KEYCODE_BUTTON_R2,
        KeyEvent.KEYCODE_BUTTON_THUMBL, KeyEvent.KEYCODE_BUTTON_THUMBR,
        KeyEvent.KEYCODE_BUTTON_START,  KeyEvent.KEYCODE_BUTTON_SELECT,
        KeyEvent.KEYCODE_BUTTON_MODE,
    )
    val KEYCODE_NAMES = arrayOf(
        "A", "B", "X", "Y", "L1", "R1", "L2", "R2", "THUMBL", "THUMBR", "START", "SELECT", "MODE",
    )

    /** Stick and hat axes, always queried on `SOURCE_JOYSTICK`. Parallel to [AXIS_NAMES]. */
    val QUERY_AXES = intArrayOf(
        MotionEvent.AXIS_X,     MotionEvent.AXIS_Y,
        MotionEvent.AXIS_Z,     MotionEvent.AXIS_RZ,
        MotionEvent.AXIS_HAT_X, MotionEvent.AXIS_HAT_Y,
    )
    val AXIS_NAMES = arrayOf("X", "Y", "Z", "RZ", "HAT_X", "HAT_Y")

    /**
     * The rules, over plain data (`keysPresent` indexed like [QUERY_KEYCODES], `axesPresent` like
     * [QUERY_AXES]). In this order:
     *  1. virtual devices (the emulator's id −1) — never;
     *  2. a pad must claim `GAMEPAD` or `JOYSTICK` — this keeps the emulator's keyboard, whose
     *     keylayout maps `BUTTON_A`, out of step 4;
     *  3. the fingerprint veto;
     *  4. a named button, or a joystick stick or hat axis;
     *  5. keyboard-classed devices by name only — some Xbox pads report no gamepad source at all.
     */
    fun classify(
        name: String,
        isVirtual: Boolean,
        sources: Int,
        keysPresent: BooleanArray,
        axesPresent: BooleanArray,
    ): Verdict {
        if (isVirtual || name == "Virtual") return Verdict(false, "virtual")

        val lowerName   = name.lowercase()
        val hasGamepad  = (sources and InputDevice.SOURCE_GAMEPAD)  == InputDevice.SOURCE_GAMEPAD
        val hasJoystick = (sources and InputDevice.SOURCE_JOYSTICK) == InputDevice.SOURCE_JOYSTICK
        val isKeyboard  = (sources and InputDevice.SOURCE_KEYBOARD) == InputDevice.SOURCE_KEYBOARD

        if (hasGamepad || hasJoystick) {
            if (SENSOR_NAME_FRAGMENTS.any { lowerName.contains(it) }) {
                return Verdict(false, "claims pad, name says fingerprint sensor")
            }

            val buttons  = KEYCODE_NAMES.filterIndexed { i, _ -> keysPresent.getOrElse(i) { false } }
            val axes     = AXIS_NAMES.filterIndexed    { i, _ -> axesPresent.getOrElse(i) { false } }
            val evidence = "keys=[${buttons.joinToString(" ")}] axes=[${axes.joinToString(" ")}]"

            return if (buttons.isNotEmpty() || axes.isNotEmpty()) {
                Verdict(true, "PAD: $evidence")
            } else {
                Verdict(false, "claims pad, has no pad controls: $evidence")
            }
        }

        if (isKeyboard && CONTROLLER_NAME_FRAGMENTS.any { lowerName.contains(it) }) {
            return Verdict(true, "PAD: keyboard-classed, name says controller")
        }

        return Verdict(false, "not a pad")
    }

    /** Gathers the facts from Android and applies the rules above. */
    fun classify(device: InputDevice): Verdict = classify(
        device.name,
        device.isVirtual,
        device.sources,
        device.hasKeys(*QUERY_KEYCODES),
        BooleanArray(QUERY_AXES.size) {
            device.getMotionRange(QUERY_AXES[it], InputDevice.SOURCE_JOYSTICK) != null
        },
    )
}
