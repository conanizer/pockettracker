package com.conanizer.pockettracker.input

/**
 * VirtualButton - the unified "virtual gamepad" every input source maps to.
 *
 * ⚠️ **Order is load-bearing and matches `pt::ui::Button` (native/ui/buttons.h) exactly.** The
 * shell passes a virtual button across JNI as its ORDINAL (`MainActivity.onButtonFeedback`), and
 * `ButtonSoundManager` reads it back through `VirtualButton.values()[ordinal]` — so a reorder here
 * silently mis-maps every button click. This is the whole of what survives Phase E's deletion of the
 * old Kotlin input layer (it lived in `ButtonHandlers.kt` until then); the button LOGIC is all C++
 * now, and only the sound/haptic feedback managers still need the enum on the Kotlin side.
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
