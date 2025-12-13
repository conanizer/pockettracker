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

package com.example.pockettracker

import android.util.Log
import androidx.compose.runtime.*
import androidx.compose.ui.Modifier
import androidx.compose.ui.input.key.onKeyEvent
import androidx.compose.ui.input.key.key
import androidx.compose.ui.input.key.Key
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
    val onARight: () -> Unit        // A+RIGHT: Large increment (one octave/0x10)
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
 * @param logInput - If true, logs all button presses to Logcat (useful for debugging)
 */
class InputMapper(
    private val buttonHandlers: ButtonHandlers,
    private val logInput: Boolean = false  // Set to true to see input in Logcat
) {
    // =========================================================================
    // STATE TRACKING
    // =========================================================================

    // Track which modifier buttons are currently held down
    private var isLPressed = false
    private var isRPressed = false
    private var isAPressed = false  // Track A button for A+direction combos

    // Track which buttons are currently held (for combinations)
    private val heldButtons = mutableSetOf<VirtualButton>()

    // Double-tap detection
    private var lastAPress: Long = 0
    private val doubleTapWindow = 300L  // 300ms window for double-tap

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
        Key.Spacebar to VirtualButton.START     // Start = play/pause
    )

    /**
     * Get current modifier state
     */
    fun isLHeld() = isLPressed
    fun isRHeld() = isRPressed
    fun isAHeld() = isAPressed
    fun isBothLRHeld() = isLPressed && isRPressed

    /**
     * Handles a keyboard event from Compose
     *
     * This is called automatically by Compose whenever a key is pressed/released
     * while the component with .inputHandler() modifier has focus.
     *
     * @param keyEvent - The raw keyboard event
     * @return Boolean - true if we handled this key, false if we ignored it
     */
    fun handleKeyEvent(keyEvent: androidx.compose.ui.input.key.KeyEvent): Boolean {
        // Determine if this is a key press or release
        val action = when (keyEvent.type) {
            KeyEventType.KeyDown -> ButtonAction.PRESSED
            KeyEventType.KeyUp -> ButtonAction.RELEASED
            else -> return false  // Unknown event type, ignore
        }

        // Look up the pressed key in our mapping
        val virtualButton = keyboardMapping[keyEvent.key]

        // If this key is mapped to a button...
        return if (virtualButton != null) {
            // Optional logging for debugging
            if (logInput) {
                Log.d(TAG, "Key ${keyEvent.key} → Button ${virtualButton.name} ${action.name}")
            }

            // Trigger the button action
            handleButtonAction(virtualButton, action)

            true  // Return true = "we handled this key"
        } else {
            // This key isn't mapped, let the system handle it
            false
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
            heldButtons.add(button)

            // Update modifier states
            if (button == VirtualButton.L_SHIFT) isLPressed = true
            if (button == VirtualButton.R_SHIFT) isRPressed = true
            if (button == VirtualButton.A) isAPressed = true

        } else if (action == ButtonAction.RELEASED) {
            heldButtons.remove(button)

            // Update modifier states
            if (button == VirtualButton.L_SHIFT) isLPressed = false
            if (button == VirtualButton.R_SHIFT) isRPressed = false
            if (button == VirtualButton.A) isAPressed = false
        }

        // Only handle button presses (not releases) for now
        if (action != ButtonAction.PRESSED) return

        // =====================================================================
        // MODIFIER COMBINATION DETECTION
        // =====================================================================

        // Check for combinations - order matters!
        // More specific combinations should be checked first

        // A + direction combinations (M8-style value editing)
        // When A is held, directions change values instead of moving cursor
        if (isAPressed && !isLPressed && !isRPressed) {
            when (button) {
                VirtualButton.DPAD_UP -> {
                    if (logInput) Log.d(TAG, "A+UP (increment by small step)")
                    buttonHandlers.onAUp()
                    return
                }
                VirtualButton.DPAD_DOWN -> {
                    if (logInput) Log.d(TAG, "A+DOWN (decrement by small step)")
                    buttonHandlers.onADown()
                    return
                }
                VirtualButton.DPAD_RIGHT -> {
                    if (logInput) Log.d(TAG, "A+RIGHT (increment by large step)")
                    buttonHandlers.onARight()
                    return
                }
                VirtualButton.DPAD_LEFT -> {
                    if (logInput) Log.d(TAG, "A+LEFT (decrement by large step)")
                    buttonHandlers.onALeft()
                    return
                }
                else -> { }
            }
        }

        // L + R + button combinations (most specific)
        if (isLPressed && isRPressed) {
            when (button) {
                VirtualButton.SELECT -> {
                    if (logInput) Log.d(TAG, "L+R+SELECT (quit to project)")
                    // TODO: Add handler for L+R+SELECT
                    return
                }
                VirtualButton.A -> {
                    if (logInput) Log.d(TAG, "L+R+A (save snapshot)")
                    // TODO: Add handler for L+R+A
                    return
                }
                VirtualButton.B -> {
                    if (logInput) Log.d(TAG, "L+R+B (load snapshot)")
                    // TODO: Add handler for L+R+B
                    return
                }
                else -> { }
            }
        }

        // L + button combinations
        if (isLPressed && !isRPressed) {
            when (button) {
                VirtualButton.A -> {
                    if (logInput) Log.d(TAG, "L+A (paste)")
                    // TODO: Add handler for L+A (paste)
                    return
                }
                VirtualButton.B -> {
                    if (logInput) Log.d(TAG, "L+B (selection mode)")
                    // TODO: Add handler for L+B (selection mode)
                    return
                }
                VirtualButton.DPAD_UP -> {
                    if (logInput) Log.d(TAG, "L+UP (jump to prev populated)")
                    // TODO: Add handler for L+UP
                    return
                }
                VirtualButton.DPAD_DOWN -> {
                    if (logInput) Log.d(TAG, "L+DOWN (jump to next populated)")
                    // TODO: Add handler for L+DOWN
                    return
                }
                VirtualButton.DPAD_LEFT -> {
                    if (logInput) Log.d(TAG, "L+LEFT (prev chain/phrase)")
                    // TODO: Add handler for L+LEFT
                    return
                }
                VirtualButton.DPAD_RIGHT -> {
                    if (logInput) Log.d(TAG, "L+RIGHT (next chain/phrase)")
                    // TODO: Add handler for L+RIGHT
                    return
                }
                VirtualButton.START -> {
                    if (logInput) Log.d(TAG, "L+START (play all from beginning)")
                    // TODO: Add handler for L+START
                    return
                }
                else -> { }
            }
        }

        // R + button combinations
        if (isRPressed && !isLPressed) {
            when (button) {
                VirtualButton.DPAD_UP -> {
                    if (logInput) Log.d(TAG, "R+UP (navigate screen up)")
                    // TODO: Add handler for R+UP (navigate screens)
                    return
                }
                VirtualButton.DPAD_DOWN -> {
                    if (logInput) Log.d(TAG, "R+DOWN (navigate screen down)")
                    // TODO: Add handler for R+DOWN
                    return
                }
                VirtualButton.DPAD_LEFT -> {
                    if (logInput) Log.d(TAG, "R+LEFT (navigate screen left)")
                    // TODO: Add handler for R+LEFT
                    return
                }
                VirtualButton.DPAD_RIGHT -> {
                    if (logInput) Log.d(TAG, "R+RIGHT (navigate screen right)")
                    // TODO: Add handler for R+RIGHT
                    return
                }
                VirtualButton.A -> {
                    if (logInput) Log.d(TAG, "R+A (clone)")
                    // TODO: Add handler for R+A (clone)
                    return
                }
                VirtualButton.B -> {
                    if (logInput) Log.d(TAG, "R+B (reset to default)")
                    // TODO: Add handler for R+B
                    return
                }
                VirtualButton.START -> {
                    if (logInput) Log.d(TAG, "R+START (play from cursor)")
                    // TODO: Add handler for R+START
                    return
                }
                else -> { }
            }
        }

        // =====================================================================
        // DOUBLE-TAP DETECTION
        // =====================================================================

        if (button == VirtualButton.A && !isLPressed && !isRPressed) {
            val now = System.currentTimeMillis()
            if (now - lastAPress < doubleTapWindow) {
                // Double-tap detected!
                if (logInput) Log.d(TAG, "A,A (insert next unused)")
                lastAPress = 0  // Reset to prevent triple-tap
                // TODO: Add handler for A,A (insert next unused)
                return
            }
            lastAPress = now
        }

        // =====================================================================
        // BASIC BUTTON PRESSES (no modifiers)
        // =====================================================================

        // Only call basic handlers if no modifiers are pressed
        if (!isLPressed && !isRPressed && !isAPressed) {
            when (button) {
                VirtualButton.DPAD_UP -> buttonHandlers.onDPadUp()
                VirtualButton.DPAD_DOWN -> buttonHandlers.onDPadDown()
                VirtualButton.DPAD_LEFT -> buttonHandlers.onDPadLeft()
                VirtualButton.DPAD_RIGHT -> buttonHandlers.onDPadRight()
                VirtualButton.A -> buttonHandlers.onButtonA()
                VirtualButton.B -> buttonHandlers.onButtonB()
                VirtualButton.L_SHIFT -> buttonHandlers.onL()
                VirtualButton.R_SHIFT -> buttonHandlers.onR()
                VirtualButton.SELECT -> buttonHandlers.onSelect()
                VirtualButton.START -> buttonHandlers.onStart()
            }
        }
    }
}

/**
 * Modifier extension to add input handling to any Composable
 *
 * This is how you attach the input system to your UI.
 *
 * Usage:
 * Box(modifier = Modifier.inputHandler(inputMapper).focusable()) {
 *     // Your tracker UI here
 * }
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

/**
 * =============================================================================
 * USAGE EXAMPLE - How to integrate this into your screens
 * =============================================================================
 */

/*

// In your PhraseEditorModule or any screen that needs input:

@Composable
fun PhraseEditorScreen(
    phrase: Phrase,
    onPhraseChange: (Phrase) -> Unit
) {
    // Track cursor position
    var cursorStep by remember { mutableStateOf(0) }
    var cursorColumn by remember { mutableStateOf(0) }
    
    // Create button handlers specific to phrase editor
    val buttonHandlers = remember {
        ButtonHandlers(
            onDPadUp = {
                // Move cursor up (previous step)
                if (cursorStep > 0) {
                    cursorStep--
                }
            },
            onDPadDown = {
                // Move cursor down (next step)
                if (cursorStep < phrase.steps.size - 1) {
                    cursorStep++
                }
            },
            onDPadLeft = {
                // Move cursor left (previous column)
                if (cursorColumn > 0) {
                    cursorColumn--
                }
            },
            onDPadRight = {
                // Move cursor right (next column)
                if (cursorColumn < 3) {  // Assuming 4 columns: note, instr, vol, fx
                    cursorColumn++
                }
            },
            onButtonA = {
                // Enter edit mode or increase value
                Log.d("PhraseEditor", "A pressed - Edit/Increase")
            },
            onButtonB = {
                // Exit edit mode or decrease value
                Log.d("PhraseEditor", "B pressed - Cancel/Decrease")
            },
            onSelect = {
                // Switch between edit modes
                Log.d("PhraseEditor", "Select pressed - Mode switch")
            },
            onStart = {
                // Play/pause playback
                Log.d("PhraseEditor", "Start pressed - Play/Pause")
            },
            onL = {
                // Quick function (like octave down)
                Log.d("PhraseEditor", "L pressed - Quick function")
            },
            onR = {
                // Quick function (like octave up)
                Log.d("PhraseEditor", "R pressed - Quick function")
            }
        )
    }
    
    // Create the input mapper with these handlers
    // Set logInput = true during development to see what buttons are pressed
    val inputMapper = remember(buttonHandlers) {
        InputMapper(buttonHandlers, logInput = true)
    }
    
    // Focus requester to auto-focus the screen
    val focusRequester = remember { FocusRequester() }
    
    // Main UI container with input handling
    Box(
        modifier = Modifier
            .fillMaxSize()
            .focusRequester(focusRequester)   // Attach focus requester
            .inputHandler(inputMapper)        // Add input handling
            .focusable()                      // Make focusable (REQUIRED!)
    ) {
        // Your phrase editor UI here
        YourPhraseEditorUI(
            phrase = phrase,
            cursorStep = cursorStep,
            cursorColumn = cursorColumn
        )
    }
    
    // Auto-focus when screen appears
    LaunchedEffect(Unit) {
        focusRequester.requestFocus()
    }
}

*/

/**
 * =============================================================================
 * TESTING - Use this simple screen to verify keyboard input works
 * =============================================================================
 */

/*

@Composable
fun InputTestScreen() {
    // Create test handlers that just log messages
    val testHandlers = remember {
        ButtonHandlers(
            onDPadUp = { Log.d("InputTest", "D-Pad UP") },
            onDPadDown = { Log.d("InputTest", "D-Pad DOWN") },
            onDPadLeft = { Log.d("InputTest", "D-Pad LEFT") },
            onDPadRight = { Log.d("InputTest", "D-Pad RIGHT") },
            onButtonA = { Log.d("InputTest", "A Button (Confirm)") },
            onButtonB = { Log.d("InputTest", "B Button (Cancel)") },
            onSelect = { Log.d("InputTest", "SELECT") },
            onStart = { Log.d("InputTest", "START") },
            onL = { Log.d("InputTest", "L Shoulder") },
            onR = { Log.d("InputTest", "R Shoulder") }
        )
    }
    
    // Create mapper with logging enabled
    val inputMapper = remember {
        InputMapper(testHandlers, logInput = true)
    }
    
    val focusRequester = remember { FocusRequester() }
    
    Box(
        modifier = Modifier
            .fillMaxSize()
            .background(Color.Black)
            .focusRequester(focusRequester)
            .inputHandler(inputMapper)
            .focusable()
    ) {
        Column(
            modifier = Modifier.align(Alignment.Center),
            horizontalAlignment = Alignment.CenterHorizontally
        ) {
            Text(
                "⌨️ INPUT TEST",
                color = Color.Green,
                fontSize = 32.sp
            )
            Spacer(modifier = Modifier.height(32.dp))
            Text(
                """
                W/A/S/D = D-Pad
                J = B Button
                K = A Button
                U = L Shoulder
                I = R Shoulder
                Shift = Select
                Space = Start
                
                Press keys and check Logcat!
                Filter: "InputTest" or "PocketInput"
                """.trimIndent(),
                color = Color.White,
                fontSize = 16.sp,
                fontFamily = FontFamily.Monospace
            )
        }
    }
    
    LaunchedEffect(Unit) {
        focusRequester.requestFocus()
    }
}

*/