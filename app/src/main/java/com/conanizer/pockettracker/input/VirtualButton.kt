package com.conanizer.pockettracker.input

/**
 * The buttons the click and vibration managers take.
 * ⚠️ The order matches `pt::ui::Button` (native/ui/buttons.h): native passes the ordinal over JNI,
 * and a reorder on either side mis-maps every click with nothing reporting it.
 */
enum class VirtualButton {
    DPAD_UP,
    DPAD_DOWN,
    DPAD_LEFT,
    DPAD_RIGHT,
    A,
    B,
    L_SHIFT,
    R_SHIFT,
    SELECT,
    START
}
