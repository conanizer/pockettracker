package com.conanizer.pockettracker.input

/**
 * VirtualButton - the unified "virtual gamepad" every input source maps to.
 *
 * ⚠️ **Order is load-bearing and matches `pt::ui::Button` (native/ui/buttons.h) exactly.** The shell
 * passes a virtual button across JNI as its ORDINAL, and `MainActivity.onButtonFeedback` is the one
 * place that turns it back into an enum (`VirtualButton.entries[ordinal]`) before handing it to the
 * sound and haptic managers, which take the enum and never see the number. So a reorder here — or in
 * `Button` — silently mis-maps every button click, and nothing on either side would report it.
 *
 * The button LOGIC is all C++; only the two feedback managers still need this enum on the Kotlin side.
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
