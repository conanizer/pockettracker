/**
 * ButtonHandlers.kt
 *
 * This file provides a generic input system for PocketTracker.
 * It translates all input sources (keyboard, gamepad, touch) into unified button events.
 *
 * Architecture:
 * - ButtonHandlers: Simple data class that holds callback functions for each button
 * - InputMapper: Translates raw input (keyboard/gamepad) into virtual button presses
 * - Modifier.inputHandler(): Compose modifier to attach input handling to any UI
 */

package com.conanizer.pockettracker.input

import android.os.Handler
import android.os.Looper
import com.conanizer.pockettracker.core.logging.ILogger
import androidx.compose.ui.Modifier
import androidx.compose.ui.input.key.onKeyEvent
import androidx.compose.ui.input.key.key
import androidx.compose.ui.input.key.Key
import androidx.compose.ui.input.key.KeyEvent
import androidx.compose.ui.input.key.KeyEventType
import androidx.compose.ui.input.key.type

// Logging tag for debugging
private const val TAG = "PocketInput"

/**
 * ButtonHandlers - Holds callback functions for all button actions
 *
 * This is what your tracker screens will provide to define what happens
 * when each button is pressed. Think of it as a "contract" - any screen
 * that wants input just needs to fill in these functions.
 *
 * Example usage:
 * val handlers = ButtonHandlers(
 *     onDPadUp = { moveCursorUp() },
 *     onDPadDown = { moveCursorDown() },
 *     etc...
 * )
 */
data class ButtonHandlers(
    val onDPadUp: () -> Unit,       // Called when UP is pressed
    val onDPadDown: () -> Unit,     // Called when DOWN is pressed
    val onDPadLeft: () -> Unit,     // Called when LEFT is pressed
    val onDPadRight: () -> Unit,    // Called when RIGHT is pressed
    val onButtonA: () -> Unit,      // Called when A (confirm) is pressed
    val onButtonB: () -> Unit,      // Called when B (back) is pressed
    val onSelect: () -> Unit,       // Called when SELECT is pressed
    val onStart: () -> Unit,        // Called when START is pressed
    val onL: () -> Unit,            // Called when L shoulder is pressed
    val onR: () -> Unit,            // Called when R shoulder is pressed

    // A+direction combinations for value editing (M8-style)
    val onAUp: () -> Unit,          // A+UP: Small increment
    val onADown: () -> Unit,        // A+DOWN: Small decrement
    val onALeft: () -> Unit,        // A+LEFT: Large decrement (one octave/0x10)
    val onARight: () -> Unit,       // A+RIGHT: Large increment (one octave/0x10)

    // A+B combination for delete
    val onAB: () -> Unit,           // A+B: Delete/clear value at cursor

    // B+direction combinations for item cycling (chain/phrase/instrument)
    val onBLeft: () -> Unit,        // B+LEFT: Previous chain/phrase/instrument
    val onBRight: () -> Unit,       // B+RIGHT: Next chain/phrase/instrument
    val onBUp: () -> Unit,          // B+UP: Page up (song screen)
    val onBDown: () -> Unit,        // B+DOWN: Page down (song screen)

    // R+direction combinations for screen navigation
    val onRUp: () -> Unit,          // R+UP: Navigate screen up
    val onRDown: () -> Unit,        // R+DOWN: Navigate screen down
    val onRLeft: () -> Unit,        // R+LEFT: Navigate screen left
    val onRRight: () -> Unit,       // R+RIGHT: Navigate screen right

    // L+direction combinations
    val onLLeft: () -> Unit,        // L+LEFT: Browser parent dir / prev item
    val onLRight: () -> Unit,       // L+RIGHT: Next item
    val onLUp: () -> Unit,          // L+UP: Browser sort mode up
    val onLDown: () -> Unit,        // L+DOWN: Browser sort mode down

    // L+button combinations for copy/paste
    val onLA: () -> Unit,           // L+A: Cut (in selection) / Paste (outside selection)
    val onLB: () -> Unit,           // L+B: Enter/cycle selection mode

    // SELECT+button combinations for file operations
    val onSelectA: () -> Unit,      // SELECT+A: Rename file/folder
    val onSelectB: () -> Unit,      // SELECT+B: Delete file/folder
    val onSelectR: () -> Unit,      // SELECT+R: Create new folder

    // L+R combination for exiting selection mode
    val onLR: () -> Unit,           // L+R: Exit selection mode (fixes L+A cut combo)

    // Double-tap A (A,A): insert next unused chain/phrase/note
    val onAA: () -> Unit,           // A,A: Insert next unused item

    // L+B+A: Clone current item to next unused slot
    val onLBA: () -> Unit,          // L+B+A: Clone chain/phrase to next unused ID

    // A button release (for modal overlays that close when A is released)
    val onAReleased: () -> Unit = {},  // Called when A button is released

    // Fired on every "plain" button PRESS — any press that is not START and not made while A is held.
    // Lets the dispatcher silence an in-progress preview from the current screen. A-involved presses
    // (A alone, A+DPAD value edits, A+B, A,A) and START are excluded by the InputMapper so editing —
    // including live EQ-band sweeps — and (re)starting a preview are never interrupted.
    val onStopPreview: () -> Unit = {},

    // Press-vs-release deferral (item 3): when the cursor is on a cell whose single-A opens a
    // sub-screen (deferA) or while a sub-screen wants B = close (deferB), the InputMapper holds the
    // single A/B action until release. This keeps the A+DPAD/A+B (edit/reset) and B+DPAD (preset
    // cycle) combos on the SAME cell from being pre-empted by the single action firing on press.
    val deferAToRelease: () -> Boolean = { false },
    val deferBToRelease: () -> Boolean = { false }
)

/**
 * VirtualButton - Represents all possible buttons on a gamepad
 *
 * This enum creates a unified "virtual gamepad" that all input gets mapped to.
 * Whether input comes from keyboard, physical gamepad, or touch screen,
 * it all gets converted to these button types.
 */
enum class VirtualButton {
    DPAD_UP,      // Directional pad up
    DPAD_DOWN,    // Directional pad down
    DPAD_LEFT,    // Directional pad left
    DPAD_RIGHT,   // Directional pad right
    A,            // Primary action button (confirm/select)
    B,            // Secondary action button (cancel/back)
    L_SHIFT,      // Left shoulder button
    R_SHIFT,      // Right shoulder button
    SELECT,       // Select button (mode switching)
    START         // Start button (play/pause/menu)
}

/**
 * ButtonAction - Describes whether a button was pressed or released
 *
 * We track both so we can later implement:
 * - Press actions (instant response)
 * - Hold actions (continuous while held)
 * - Release actions (trigger on let-go)
 */
enum class ButtonAction {
    PRESSED,      // Button was just pushed down
    RELEASED      // Button was just released
}

/**
 * InputMapper - Core input translation system with modifier support
 *
 * This class:
 * 1. Takes raw input from keyboard/gamepad
 * 2. Maps it to VirtualButton events
 * 3. Tracks modifier states (L/R buttons held)
 * 4. Detects button combinations (L+A, R+arrows, etc.)
 * 5. Supports double-tap detection (A,A)
 * 6. Calls the appropriate ButtonHandlers callback
 *
 * @param buttonHandlers - The ButtonHandlers instance that defines what each button does
 * @param logger - Optional ILogger; pass AndroidLogger() to enable input tracing in Logcat
 */
class InputMapper(
    private val buttonHandlers: ButtonHandlers,
    private val logger: ILogger? = null
) {
    // =========================================================================
    // STATE TRACKING
    // =========================================================================

    // Track which modifier buttons are currently held down
    private var isLPressed = false
    private var isRPressed = false
    private var isAPressed = false  // Track A button for A+direction combos
    private var isBPressed = false  // Track B button to prevent B+direction old behavior
    private var isSelectPressed = false  // Track SELECT button for SELECT+button combos

    // Deferred single-press tracking (item 3). Set on an A/B press that the dispatcher wants held
    // until release (deferAToRelease / deferBToRelease). Cleared the moment an A/B + combo fires; on
    // release, if still set, the single action runs then. See handleButtonAction.
    private var aPressedAlone = false
    private var bPressedAlone = false

    // Track which physical keys are currently pressed (to ignore key repeat)
    // Store native key codes (Int) for proper equality checking
    private val pressedKeys = mutableSetOf<Int>()

    // Double-tap detection
    private var lastAPress: Long = 0
    private val doubleTapWindow = 300L  // 300ms window for double-tap

    // Key repeat system for D-PAD, A+DPAD, B+DPAD
    private val repeatHandler = Handler(Looper.getMainLooper())
    private var repeatRunnable: Runnable? = null
    private var repeatAction: (() -> Unit)? = null
    private companion object {
        const val REPEAT_INITIAL_DELAY = 400L  // ms before first repeat
        const val REPEAT_INTERVAL = 100L       // ms between repeats
    }

    /**
     * Start key repeat for a repeatable action.
     * After initial delay, fires the action repeatedly at fixed interval.
     */
    private fun startKeyRepeat(action: () -> Unit) {
        cancelKeyRepeat()
        repeatAction = action
        repeatRunnable = object : Runnable {
            override fun run() {
                repeatAction?.invoke()
                repeatHandler.postDelayed(this, REPEAT_INTERVAL)
            }
        }
        repeatHandler.postDelayed(repeatRunnable!!, REPEAT_INITIAL_DELAY)
    }

    /**
     * Cancel any active key repeat.
     */
    private fun cancelKeyRepeat() {
        repeatRunnable?.let { repeatHandler.removeCallbacks(it) }
        repeatRunnable = null
        repeatAction = null
    }

    /**
     * Reset all held-button state and cancel key repeat.
     *
     * Call this whenever the layout composable is rebuilt (e.g. on layout mode switch).
     * When Compose destroys a layout, in-flight pointerInput coroutines are cancelled
     * without firing their RELEASED callbacks, leaving isAPressed / isLPressed / etc. stuck.
     * Stuck modifiers make all subsequent DPAD presses route to the wrong combo handlers.
     */
    fun reset() {
        cancelKeyRepeat()
        isLPressed = false
        isRPressed = false
        isAPressed = false
        isBPressed = false
        isSelectPressed = false
        aPressedAlone = false
        bPressedAlone = false
        pressedKeys.clear()
    }

    /**
     * Keyboard to VirtualButton mapping
     *
     * This follows common emulator conventions:
     * - WASD: D-Pad movement (like PC games)
     * - J/K: Face buttons (right hand home row)
     * - U/I: Shoulder buttons (above J/K on keyboard)
     * - Shift/Space: System buttons
     *
     * Later: Make this customizable per user
     */
    private val keyboardMapping = mapOf(
        // D-Pad mapping (WASD cluster - familiar to PC gamers)
        Key.W to VirtualButton.DPAD_UP,
        Key.A to VirtualButton.DPAD_LEFT,
        Key.S to VirtualButton.DPAD_DOWN,
        Key.D to VirtualButton.DPAD_RIGHT,

        // Face buttons (right hand home row position)
        Key.K to VirtualButton.A,           // A = confirm/select (like Enter)
        Key.J to VirtualButton.B,           // B = cancel/back (like Escape)

        // Shoulder buttons (keys above face buttons)
        Key.U to VirtualButton.L_SHIFT,     // Left shoulder
        Key.I to VirtualButton.R_SHIFT,     // Right shoulder

        // System buttons (easily accessible with left hand)
        Key.ShiftLeft to VirtualButton.SELECT,  // Select = mode switching
        Key.Spacebar to VirtualButton.START,    // Start = play/pause

        // Arrow keys (alternative to WASD)
        Key.DirectionUp to VirtualButton.DPAD_UP,
        Key.DirectionLeft to VirtualButton.DPAD_LEFT,
        Key.DirectionDown to VirtualButton.DPAD_DOWN,
        Key.DirectionRight to VirtualButton.DPAD_RIGHT,

        // Alternative confirm/cancel keys
        Key.Enter to VirtualButton.A,
        Key.Escape to VirtualButton.B
    )

    // Mapping for native Android key codes (for gamepad support on handhelds)
    // These are the actual hardware button codes sent by gaming handhelds
    private val nativeGamepadMapping = mapOf(
        // Android gamepad D-pad (KEYCODE_DPAD_*)
        19 to VirtualButton.DPAD_UP,        // KEYCODE_DPAD_UP
        20 to VirtualButton.DPAD_DOWN,      // KEYCODE_DPAD_DOWN
        21 to VirtualButton.DPAD_LEFT,      // KEYCODE_DPAD_LEFT
        22 to VirtualButton.DPAD_RIGHT,     // KEYCODE_DPAD_RIGHT

        // Android gamepad face buttons (KEYCODE_BUTTON_*)
        96 to VirtualButton.A,              // KEYCODE_BUTTON_A
        97 to VirtualButton.B,              // KEYCODE_BUTTON_B
        99 to VirtualButton.A,              // KEYCODE_BUTTON_X (map to A)
        100 to VirtualButton.B,             // KEYCODE_BUTTON_Y (map to B)

        // Android gamepad shoulder buttons
        102 to VirtualButton.L_SHIFT,       // KEYCODE_BUTTON_L1
        103 to VirtualButton.R_SHIFT,       // KEYCODE_BUTTON_R1
        104 to VirtualButton.L_SHIFT,       // KEYCODE_BUTTON_L2 (also L)
        105 to VirtualButton.R_SHIFT,       // KEYCODE_BUTTON_R2 (also R)

        // Android gamepad system buttons
        108 to VirtualButton.START,         // KEYCODE_BUTTON_START
        109 to VirtualButton.SELECT,        // KEYCODE_BUTTON_SELECT

        // Alternative mappings for some handhelds
        82 to VirtualButton.START,          // KEYCODE_MENU (used as START on some devices)
        4 to VirtualButton.B                // KEYCODE_BACK (back button as B)
    )

    /**
     * Get current modifier state
     */
    fun isLHeld() = isLPressed
    fun isRHeld() = isRPressed
    fun isAHeld() = isAPressed
    fun isBothLRHeld() = isLPressed && isRPressed

    /**
     * Entry point for virtual touchscreen buttons.
     * Routes touch press/release through the same combo detection as physical buttons,
     * so L+A, A+DPAD, R+DPAD, etc. work identically on touch and hardware.
     */
    fun onVirtualButton(button: VirtualButton, action: ButtonAction) {
        handleButtonAction(button, action)
    }

    /**
     * Handles a keyboard event from Compose
     *
     * This is called automatically by Compose whenever a key is pressed/released
     * while the component with .inputHandler() modifier has focus.
     *
     * @param keyEvent - The raw keyboard event
     * @return Boolean - true if we handled this key, false if we ignored it
     */
    fun handleKeyEvent(keyEvent: KeyEvent): Boolean {
        // Try keyboard mapping first (for PC), then native gamepad codes (for handhelds)
        val virtualButton = keyboardMapping[keyEvent.key]
            ?: nativeGamepadMapping[keyEvent.nativeKeyEvent.keyCode]
            ?: return false  // Unknown key, ignore

        // Determine if this is a key press or release
        when (keyEvent.type) {
            KeyEventType.KeyDown -> {
                // IMPORTANT: Ignore key repeat!
                // When you hold a key, OS sends repeated KeyDown events
                // We only want to handle the FIRST press
                // Use native keyCode (Int) for proper equality checking
                val nativeKeyCode = keyEvent.nativeKeyEvent.keyCode
                if (pressedKeys.contains(nativeKeyCode)) {
                    // This is a key repeat - ignore it
                    return true
                }

                // Mark this key as pressed
                pressedKeys.add(nativeKeyCode)

                logger?.d(TAG, "Key ${keyEvent.key} DOWN (native=$nativeKeyCode) → Button ${virtualButton.name} PRESSED")

                // Trigger the button action
                handleButtonAction(virtualButton, ButtonAction.PRESSED)
                return true
            }

            KeyEventType.KeyUp -> {
                val nativeKeyCode = keyEvent.nativeKeyEvent.keyCode

                // Remove key from pressed set
                pressedKeys.remove(nativeKeyCode)

                logger?.d(TAG, "Key ${keyEvent.key} UP → Button ${virtualButton.name} RELEASED")

                // Trigger the button action
                handleButtonAction(virtualButton, ButtonAction.RELEASED)
                return true
            }

            else -> return false  // Unknown event type, ignore
        }
    }

    /**
     * Routes a virtual button action to the appropriate handler
     *
     * This is the bridge between input events and your tracker logic.
     * Now supports:
     * - Modifier tracking (L/R buttons)
     * - Button combinations (L+A, R+arrows, etc.)
     * - Double-tap detection (A,A)
     *
     * @param button - Which virtual button was triggered
     * @param action - Was it pressed or released
     */
    private fun handleButtonAction(button: VirtualButton, action: ButtonAction) {
        // Update button state tracking
        if (action == ButtonAction.PRESSED) {
            // Update modifier states
            if (button == VirtualButton.L_SHIFT) isLPressed = true
            if (button == VirtualButton.R_SHIFT) isRPressed = true
            if (button == VirtualButton.A) isAPressed = true
            if (button == VirtualButton.B) isBPressed = true
            if (button == VirtualButton.SELECT) isSelectPressed = true

        } else if (action == ButtonAction.RELEASED) {
            // Update modifier states
            if (button == VirtualButton.L_SHIFT) isLPressed = false
            if (button == VirtualButton.R_SHIFT) isRPressed = false
            if (button == VirtualButton.A) isAPressed = false
            if (button == VirtualButton.B) isBPressed = false
            if (button == VirtualButton.SELECT) isSelectPressed = false
        }

        // Cancel key repeat on any key release (D-PAD, A, B)
        if (action == ButtonAction.RELEASED) {
            if (button == VirtualButton.A) {
                // Deferred single-A (item 3): A was pressed on a sub-screen-opening cell and no
                // A-combo intervened, so fire the open now (on release) instead of on press.
                if (aPressedAlone) {
                    aPressedAlone = false
                    buttonHandlers.onButtonA()
                }
                buttonHandlers.onAReleased()
            }
            if (button == VirtualButton.B) {
                // Deferred single-B (item 3): mirror of A — closes the EQ editor on release so the
                // B+DPAD preset cycle inside it isn't pre-empted by an immediate close.
                if (bPressedAlone) {
                    bPressedAlone = false
                    buttonHandlers.onButtonB()
                }
            }
            when (button) {
                VirtualButton.DPAD_UP, VirtualButton.DPAD_DOWN,
                VirtualButton.DPAD_LEFT, VirtualButton.DPAD_RIGHT,
                VirtualButton.A, VirtualButton.B -> cancelKeyRepeat()
                else -> {}
            }
            return
        }

        // Stop any active preview on a "plain" press, so a long-ringing audition can be silenced
        // without leaving the screen. Exempt: START (it re-starts the preview) and any press made
        // while A is held — which covers A alone plus the A+DPAD / A+B / A,A edit combos (isAPressed
        // is already true here for the A button itself). That keeps editing audible, including live
        // EQ-band sweeps that update the held preview in real time. The dispatcher decides whether the
        // current screen actually has a preview to stop.
        if (button != VirtualButton.START && !isAPressed) {
            buttonHandlers.onStopPreview()
        }

        logger?.d(TAG, "handleButtonAction: button=$button, isA=$isAPressed, isB=$isBPressed, isL=$isLPressed, isR=$isRPressed, isSEL=$isSelectPressed")

        // =====================================================================
        // MODIFIER COMBINATION DETECTION
        // =====================================================================

        // Check for combinations - order matters!
        // More specific combinations should be checked first

        // A + direction combinations (M8-style value editing)
        // When A is held, directions change values instead of moving cursor
        if (isAPressed && !isLPressed && !isRPressed) {
            logger?.d(TAG, "A is held, checking for combos with button=$button")
            // An A-combo consumes the hold, so the single-A action must NOT also fire on release.
            when (button) {
                VirtualButton.B -> {
                    logger?.d(TAG, "A+B (delete)")
                    aPressedAlone = false
                    buttonHandlers.onAB()
                    return
                }
                VirtualButton.DPAD_UP -> {
                    logger?.d(TAG, "A+UP (increment by small step)")
                    aPressedAlone = false
                    buttonHandlers.onAUp()
                    startKeyRepeat { buttonHandlers.onAUp() }
                    return
                }
                VirtualButton.DPAD_DOWN -> {
                    logger?.d(TAG, "A+DOWN (decrement by small step)")
                    aPressedAlone = false
                    buttonHandlers.onADown()
                    startKeyRepeat { buttonHandlers.onADown() }
                    return
                }
                VirtualButton.DPAD_RIGHT -> {
                    logger?.d(TAG, "A+RIGHT (increment by large step)")
                    aPressedAlone = false
                    buttonHandlers.onARight()
                    startKeyRepeat { buttonHandlers.onARight() }
                    return
                }
                VirtualButton.DPAD_LEFT -> {
                    logger?.d(TAG, "A+LEFT (decrement by large step)")
                    aPressedAlone = false
                    buttonHandlers.onALeft()
                    startKeyRepeat { buttonHandlers.onALeft() }
                    return
                }
                else -> { }
            }
        }

        // B + direction combinations (item cycling: chain/phrase/instrument)
        // When B is held, LEFT/RIGHT cycle through items instead of normal navigation
        if (isBPressed && !isLPressed && !isRPressed && !isAPressed) {
            // A B-combo consumes the hold, so the single-B action must NOT also fire on release.
            when (button) {
                VirtualButton.DPAD_LEFT -> {
                    logger?.d(TAG, "B+LEFT (previous item)")
                    bPressedAlone = false
                    buttonHandlers.onBLeft()
                    startKeyRepeat { buttonHandlers.onBLeft() }
                    return
                }
                VirtualButton.DPAD_RIGHT -> {
                    logger?.d(TAG, "B+RIGHT (next item)")
                    bPressedAlone = false
                    buttonHandlers.onBRight()
                    startKeyRepeat { buttonHandlers.onBRight() }
                    return
                }
                VirtualButton.DPAD_UP -> {
                    logger?.d(TAG, "B+UP (page up)")
                    bPressedAlone = false
                    buttonHandlers.onBUp()
                    startKeyRepeat { buttonHandlers.onBUp() }
                    return
                }
                VirtualButton.DPAD_DOWN -> {
                    logger?.d(TAG, "B+DOWN (page down)")
                    bPressedAlone = false
                    buttonHandlers.onBDown()
                    startKeyRepeat { buttonHandlers.onBDown() }
                    return
                }
                else -> { }
            }
        }

        // L + R combination (exit selection mode)
        // Check when R is pressed while L is held, or L is pressed while R is held
        if (isLPressed && isRPressed) {
            // Check for more specific L+R+button combos first
            when (button) {
                // L+R+SELECT / L+R+A / L+R+B: reserved combos — intentionally consumed (no-op)
                // so they never fall through to the single-button handlers mid-chord.
                VirtualButton.SELECT -> return
                VirtualButton.A -> return
                VirtualButton.B -> return
                // L+R alone (when second button of the pair is pressed)
                VirtualButton.L_SHIFT, VirtualButton.R_SHIFT -> {
                    logger?.d(TAG, "L+R (exit selection mode)")
                    buttonHandlers.onLR()
                    return
                }
                else -> { }
            }
        }

        // L+B+A: Clone (A pressed while both L and B are held)
        // Must be checked BEFORE the L+button block to avoid firing onLA (paste) instead
        if (isLPressed && isBPressed && !isRPressed && button == VirtualButton.A) {
            logger?.d(TAG, "L+B+A (clone)")
            buttonHandlers.onLBA()
            return
        }

        // L + button combinations
        if (isLPressed && !isRPressed) {
            logger?.d(TAG, "L is held, checking L+button combo for button=$button")
            when (button) {
                VirtualButton.A -> {
                    logger?.d(TAG, "L+A (cut/paste)")
                    buttonHandlers.onLA()
                    return
                }
                VirtualButton.B -> {
                    logger?.d(TAG, "L+B (selection mode)")
                    buttonHandlers.onLB()
                    return
                }
                VirtualButton.DPAD_UP -> {
                    logger?.d(TAG, "L+UP (sort mode up / jump to prev) → calling onLUp()")
                    buttonHandlers.onLUp()
                    return
                }
                VirtualButton.DPAD_DOWN -> {
                    logger?.d(TAG, "L+DOWN (sort mode down / jump to next) → calling onLDown()")
                    buttonHandlers.onLDown()
                    return
                }
                VirtualButton.DPAD_LEFT -> {
                    logger?.d(TAG, "L+LEFT (prev chain/phrase) → calling onLLeft()")
                    buttonHandlers.onLLeft()
                    return
                }
                VirtualButton.DPAD_RIGHT -> {
                    logger?.d(TAG, "L+RIGHT (next chain/phrase) → calling onLRight()")
                    buttonHandlers.onLRight()
                    return
                }
                // L+START: reserved — intentionally consumed so START doesn't toggle playback mid-chord.
                VirtualButton.START -> return
                else -> {
                    logger?.d(TAG, "L held but button=$button not a combo target")
                }
            }
        }

        // SELECT + button combinations (file operations)
        // Note: !isRPressed is relaxed for R_SHIFT itself since isRPressed is set before this check
        if (isSelectPressed && !isLPressed && (!isRPressed || button == VirtualButton.R_SHIFT)) {
            when (button) {
                VirtualButton.A -> {
                    logger?.d(TAG, "SELECT+A (rename)")
                    buttonHandlers.onSelectA()
                    return
                }
                VirtualButton.B -> {
                    logger?.d(TAG, "SELECT+B (delete)")
                    buttonHandlers.onSelectB()
                    return
                }
                VirtualButton.R_SHIFT -> {
                    logger?.d(TAG, "SELECT+R (create folder)")
                    buttonHandlers.onSelectR()
                    return
                }
                else -> { }
            }
        }

        // R + button combinations
        if (isRPressed && !isLPressed) {
            when (button) {
                VirtualButton.DPAD_UP -> {
                    logger?.d(TAG, "R+UP (navigate screen up)")
                    buttonHandlers.onRUp()
                    return
                }
                VirtualButton.DPAD_DOWN -> {
                    logger?.d(TAG, "R+DOWN (navigate screen down)")
                    buttonHandlers.onRDown()
                    return
                }
                VirtualButton.DPAD_LEFT -> {
                    logger?.d(TAG, "R+LEFT (navigate screen left)")
                    buttonHandlers.onRLeft()
                    return
                }
                VirtualButton.DPAD_RIGHT -> {
                    logger?.d(TAG, "R+RIGHT (navigate screen right)")
                    buttonHandlers.onRRight()
                    return
                }
                // R+A / R+B / R+START: reserved combos — intentionally consumed (no-op) so the
                // single-button actions can't fire while R is held for screen navigation.
                VirtualButton.A -> return
                VirtualButton.B -> return
                VirtualButton.START -> return
                else -> { }
            }
        }

        // =====================================================================
        // DOUBLE-TAP DETECTION
        // =====================================================================

        if (button == VirtualButton.A && !isLPressed && !isRPressed) {
            // Item 3: on a cell whose single-A opens a sub-screen, hold the action until release so a
            // following A+DPAD/A+B (edit/reset) on the same cell isn't pre-empted by an immediate open.
            // Defer cells are never double-tap (insert) cells, so the A,A path is skipped for them.
            if (buttonHandlers.deferAToRelease()) {
                aPressedAlone = true
                lastAPress = 0L  // a deferred tap must not arm a stray double-tap on the next A
                return
            }
            val now = System.currentTimeMillis()
            if (now - lastAPress < doubleTapWindow) {
                // Double-tap detected!
                logger?.d(TAG, "A,A (insert next unused)")
                lastAPress = 0  // Reset to prevent triple-tap
                buttonHandlers.onAA()
                return
            }
            lastAPress = now
            // Single A press - execute action
            logger?.d(TAG, "Single A press")
            buttonHandlers.onButtonA()
            return
        }

        // Handle B button alone (not as modifier)
        if (button == VirtualButton.B && !isLPressed && !isRPressed && !isAPressed) {
            // Item 3: defer to release when a sub-screen wants B = close (EQ editor) so the B+DPAD
            // preset cycle inside it isn't pre-empted by an immediate close.
            if (buttonHandlers.deferBToRelease()) {
                bPressedAlone = true
                return
            }
            logger?.d(TAG, "Single B press")
            buttonHandlers.onButtonB()
            return
        }

        // Handle L button alone (not as modifier)
        if (button == VirtualButton.L_SHIFT && !isRPressed && !isAPressed && !isBPressed) {
            logger?.d(TAG, "Single L press")
            buttonHandlers.onL()
            return
        }

        // Handle R button alone (not as modifier)
        if (button == VirtualButton.R_SHIFT && !isLPressed && !isAPressed && !isBPressed) {
            logger?.d(TAG, "Single R press")
            buttonHandlers.onR()
            return
        }

        // =====================================================================
        // BASIC BUTTON PRESSES (no modifiers)
        // =====================================================================

        // Handle SELECT button alone (before basic buttons check since isSelectPressed is already true)
        // This fixes the bug where SELECT handler was never called because isSelectPressed=true
        // skipped the basic buttons block
        if (button == VirtualButton.SELECT && !isLPressed && !isRPressed && !isAPressed && !isBPressed) {
            logger?.d(TAG, "Single SELECT press")
            buttonHandlers.onSelect()
            return
        }

        // Handle START button (check explicitly to ensure it's called)
        if (button == VirtualButton.START && !isLPressed && !isRPressed && !isAPressed && !isBPressed && !isSelectPressed) {
            logger?.d(TAG, "Single START press → calling onStart()")
            buttonHandlers.onStart()
            return
        }

        // Only call basic handlers if no modifiers are pressed
        if (!isLPressed && !isRPressed && !isAPressed && !isBPressed && !isSelectPressed) {
            when (button) {
                VirtualButton.DPAD_UP -> {
                    buttonHandlers.onDPadUp()
                    startKeyRepeat { buttonHandlers.onDPadUp() }
                }
                VirtualButton.DPAD_DOWN -> {
                    buttonHandlers.onDPadDown()
                    startKeyRepeat { buttonHandlers.onDPadDown() }
                }
                VirtualButton.DPAD_LEFT -> {
                    buttonHandlers.onDPadLeft()
                    startKeyRepeat { buttonHandlers.onDPadLeft() }
                }
                VirtualButton.DPAD_RIGHT -> {
                    buttonHandlers.onDPadRight()
                    startKeyRepeat { buttonHandlers.onDPadRight() }
                }
                // SELECT and START now handled above with explicit logging
                else -> { } // A, B, L, R are handled above
            }
        }
    }
}

/**
 * Modifier extension to add input handling to any Composable.
 *
 * Usage:
 *     Box(modifier = Modifier.inputHandler(inputMapper).focusable()) {
 *         // Your tracker UI here
 *     }
 *
 * IMPORTANT: Must be combined with .focusable() modifier or keyboard won't work!
 *
 * @param inputMapper - The InputMapper instance that handles events
 */
fun Modifier.inputHandler(inputMapper: InputMapper): Modifier {
    return this.onKeyEvent { keyEvent ->
        // Pass all keyboard events to the InputMapper
        inputMapper.handleKeyEvent(keyEvent)
    }
}
