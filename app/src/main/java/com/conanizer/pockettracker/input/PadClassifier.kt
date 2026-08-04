package com.conanizer.pockettracker.input

import android.view.InputDevice
import android.view.KeyEvent
import android.view.MotionEvent

/**
 * **Is this input device a real game controller?** The single decision behind `MainActivity`'s
 * `hasPhysicalGameButtons()`, which the shared C++ shell asks over JNI once at boot and again on every
 * controller add/remove to choose between the touch UI (on-screen gamepad + PORTRAIT2 skin) and FULL,
 * the bare fullscreen frame with no on-screen buttons.
 *
 * ⚠️ **A WRONG `true` LOCKS A PHONE USER OUT OF THE APP ENTIRELY.** FULL draws no buttons, so a
 * touchscreen device that is wrongly believed to have a pad has no working input at all — not even
 * enough to reach SETTINGS and change the layout. A wrong `false` is merely cosmetic by comparison: a
 * handheld that also has buttons gets the touch skin, and its buttons keep working. **When a test here
 * is a judgement call, it goes the way that leaves input reachable.**
 *
 * ⚠️ **THE `sources` BITMASK IS NOT ENOUGH — SOME PHONES CLAIM `GAMEPAD | JOYSTICK` ON A DEVICE THAT
 * IS NOT ONE.** Android's `EventHub` classes any evdev node carrying keys in the `BTN_MISC..BTN_MOUSE`
 * range (`BTN_0`…`BTN_9`) as a gamepad, and having decided that, calls it a joystick too. Xiaomi's
 * fingerprint reader (`uinput-fpc`; `uinput-goodixfp` is the Goodix part other vendors fit) declares
 * exactly those keys for its navigation gestures, so on a Redmi 10C it arrives as `sources=0x1000511`
 * — gamepad, joystick AND keyboard — on a phone with no buttons anywhere.
 *
 * The split between [classify] the pure function and [classify] the `InputDevice` overload is what
 * makes the policy testable off-device: everything Android-shaped is gathered by the overload, and
 * every rule lives below it in plain data.
 */
object PadClassifier {

    /** The verdict, and *why* — the reason travels into the boot log so a report carries the evidence. */
    data class Verdict(val isPad: Boolean, val reason: String)

    /**
     * Name fragments of devices that claim to be pads and are fingerprint readers.
     *
     * ⚠️ **A VETO, applied BEFORE the control test below, not after.** A sensor that also exposed swipe
     * axes on `SOURCE_JOYSTICK` would pass the control test on axes alone. Kept to fingerprint-specific
     * fragments so a handheld whose genuine pad happens to arrive over `uinput` is not caught by it —
     * see the wrong-`true`/wrong-`false` asymmetry above.
     */
    private val SENSOR_NAME_FRAGMENTS =
        arrayOf("fingerprint", "goodixfp", "fpc", "uinput-fp", "fpsensor")

    /** Names in the `NAME` position of a keyboard-classed device that really is a controller. */
    private val CONTROLLER_NAME_FRAGMENTS = arrayOf("xbox", "controller", "gamepad")

    /**
     * The keycodes a real controller reports and a pseudo-device does not — face, shoulder, thumb and
     * menu buttons. Parallel to [KEYCODE_NAMES]; the index is shared with the `keysPresent` array
     * [classify] takes.
     *
     * ⚠️ **`KEYCODE_BUTTON_1`…`_16` ARE DELIBERATELY ABSENT.** `Generic.kl` maps `BTN_0`…`BTN_15` onto
     * them, and `BTN_0` is precisely what the readers in [SENSOR_NAME_FRAGMENTS] declare — admitting the
     * generic codes would let every device this test exists to reject back in through the front door.
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
     * The rules, over plain data. `keysPresent` is indexed by [QUERY_KEYCODES] and `axesPresent` by
     * [QUERY_AXES]; both are exactly what the `InputDevice` overload below gathers.
     *
     * In order, and the order matters:
     *  1. **virtual** — the emulator's UI pseudo-device (id −1) and anything else synthesised.
     *  2. **`GAMEPAD` or `JOYSTICK` claimed?** The cheap precondition, and the one that keeps the
     *     emulator's `qwerty2` keyboard (`KEYBOARD | DPAD`, and a keylayout that maps `BUTTON_A`) out of
     *     step 4, where it would otherwise pass. Without it this rejects one impostor and admits another.
     *  3. **the fingerprint veto** — never a controller, whatever it claims.
     *  4. **real pad controls** — a named button, or a joystick-sourced stick or hat axis.
     *  5. **the name rescue**, for keyboard-classed devices only: some Xbox pads report with no gamepad
     *     source at all, and for those there is nothing structural left to look at.
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

    /** Gathers the five facts from Android and applies the rules above. The only Android-shaped part. */
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
