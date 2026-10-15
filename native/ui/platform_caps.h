#pragma once
// What this platform can do: which SETTINGS rows and PROJECT actions exist. A value, not an #ifdef, so
// one build can answer every profile — `android()` is the tests' fixed one, `sdl()` the desktop and
// handhelds, `converged()` the Android app.

namespace pt::ui {

struct PlatformCaps {
    // A developer build: OVERLAY, TRACE and the RAM readouts.
    bool debug = false;

    // LAYOUT, and the portrait layout's skin column.
    bool touchLayouts = false;

    // A pad is attached right now — the ABXY row's gate.
    bool padAttached = false;

    // OVERLAY: a PNG over the button skin (debug only on top).
    bool skinOverlay = false;

    // BTN SOUND + BTN VIBRO for the on-screen buttons.
    bool buttonFeedback = false;

    // RESUME: ASK where the OS keeps the app warm, AUTO where a launcher kills it at every menu.
    bool autosave = false;

    // PROJECT's EXIT row: a handheld launcher needs the process back.
    bool appExit = false;

    // Creating MIDI data: PROJECT > MIDI, EXTERNAL in TYPE, the MIDI commands at EFFECT_TYPES' end.
    // ⚠️ Authoring only: what a file already holds is always shown truthfully.
    bool midi = false;

    // `LPO` and LOOP's `osc`, held back while their behaviour is decided; authoring only, as `midi`.
    // ⚠️ It hides `LPO` only while `midi` is off (both are trimmed from the same tail).
    bool loopWindow = false;

    // AUDIO OUT: the output can be switched while running (Windows: ASIO).
    bool audioOutputs = false;

    // The tests' row map: every device row, no exit.
    static PlatformCaps android(bool debug_build) {
        PlatformCaps c;
        c.debug          = debug_build;
        c.touchLayouts   = true;
        c.skinOverlay    = true;
        c.buttonFeedback = true;
        c.autosave       = true;
        c.appExit        = false;
        c.midi           = debug_build;
        c.loopWindow     = debug_build;
        return c;
    }

    // The desktop and handheld shells. (Not `linux`: gcc predefines it as a macro.)
    static PlatformCaps sdl(bool debug_build) {
        PlatformCaps c;
        c.debug          = debug_build;
        c.touchLayouts   = false;
        c.skinOverlay    = false;
        c.buttonFeedback = false;
        c.autosave       = true;
        c.appExit        = true;
        c.midi           = true;
        c.loopWindow     = debug_build;
        return c;
    }

    // The Android app: `sdl()` plus the touch rows. LAYOUT is also gated at runtime on a touch screen
    // and no pad (`app.cpp`).
    static PlatformCaps converged(bool debug_build) {
        PlatformCaps c   = sdl(debug_build);
        c.touchLayouts   = true;
        c.buttonFeedback = true;
        c.skinOverlay    = true;
        return c;
    }
};

}  // namespace pt::ui
