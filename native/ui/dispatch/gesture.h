// The gestures a layer or a screen can own — one per `on_*` entry point that is routed
// (ui/dispatch/route.cpp). Never stored, so the order means nothing.
#pragma once

namespace pt::ui {

enum class Gesture {
    DPAD_UP, DPAD_DOWN, DPAD_LEFT, DPAD_RIGHT,
    A_UP, A_DOWN, A_LEFT, A_RIGHT,
    A_B, A_A, A_RELEASE, A_START,
    B_UP, B_DOWN, B_LEFT, B_RIGHT,
    R_UP, R_DOWN, R_LEFT, R_RIGHT,
    R_A, R_B,
    L_A, L_B, L_R, L_B_A,
    L_UP, L_DOWN, L_LEFT, L_RIGHT,
    SELECT_A, SELECT_B, SELECT_R,
    A, B, SELECT, START,
    L_START, R_START,
    STOP_PREVIEW,   // any plain press: silence the audition
};

// What a layer or screen did with a gesture. ⚠️ A layer answers TAKEN even for one it ignores, so nothing
// reaches the screen behind; PASS lets one through (START under the theme and EQ editors). For a screen
// PASS is the usual answer: the gesture's generic path runs.
enum class GestureResult { TAKEN, PASS };

}  // namespace pt::ui
