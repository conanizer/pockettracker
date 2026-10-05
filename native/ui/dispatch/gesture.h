// The gestures a layer or a screen can own — one per `on_*` entry point that is routed
// (ui/dispatch/route.cpp). Never stored, so the order means nothing.
#pragma once

namespace pt::ui {

enum class Gesture {
    DPAD_UP, DPAD_DOWN, DPAD_LEFT, DPAD_RIGHT,
    A_UP, A_DOWN, A_LEFT, A_RIGHT,
    A_B, A_A, A_RELEASE,
    B_UP, B_DOWN, B_LEFT, B_RIGHT,
    R_UP, R_DOWN, R_LEFT, R_RIGHT,
    R_A, R_B,
    L_A, L_B, L_R, L_B_A,
    SELECT_A, SELECT_B, SELECT_R,
    A, B, SELECT, START,
    L_START, R_START,
    STOP_PREVIEW,   // any plain press: silence the audition
};

/**
 * What a layer or a screen did with a gesture.
 * • A LAYER: ⚠️ TAKEN is also the answer for a gesture it ignores — a button under a dialog must never
 *   reach the screen behind. PASS is the deliberate exception (START under the theme and EQ editors, so
 *   the song plays while you dial).
 * • A SCREEN: PASS is the usual answer — the gesture's generic path runs, as on a screen with no handler.
 */
enum class GestureResult { TAKEN, PASS };

}  // namespace pt::ui
