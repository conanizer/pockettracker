#pragma once
/**
 * native/ui/platform_caps.h — what THIS platform can do: which SETTINGS rows and PROJECT actions exist.
 * Half of SETTINGS is about the DEVICE (touch layouts, a skinned D-pad, button clicks and haptics, a
 * way to QUIT), and devices differ.
 *
 * ⚠️ THE CAPS ARE A VALUE, NOT AN #ifdef. The same code answers every profile:
 *   • `android(debug)` reproduces the recorded golden's row map exactly, hidden rows and all, so
 *     the input tests drive it row for row;
 *   • `sdl(debug)` is what the desktop and handheld shells run;
 *   • `converged(debug)` is what the Android app runs.
 * `debug` is a field rather than `#ifdef NDEBUG` for the same reason.
 */

namespace pt::ui {

struct PlatformCaps {
    /** A developer build. Gates OVERLAY, TRACE and the RAM readouts on PROJECT and INST.POOL — which
     *  `engine_feed.h` then stops sampling, so both byte counts read 0. */
    bool debug = false;

    /** LAYOUT: FULLSCREEN / LANDSCAPE / PORTRAIT, and the skin column the portrait layout gains. */
    bool touchLayouts = false;

    /** A physical controller is attached RIGHT NOW — the face-button-swap row's gate (it configures a
     *  pad, so with none plugged in it would configure nothing). Runtime, since pads are hot-pluggable. */
    bool padAttached = false;

    /** OVERLAY: a PNG laid over the virtual button skin, with a strength. Debug-gated on top. */
    bool skinOverlay = false;

    /** BTN SOUND + BTN VIBRO: the click and the haptic a VIRTUAL button gives back. */
    bool buttonFeedback = false;

    /**
     * RESUME (ASK / AUTO) for a crash-recovery autosave at launch. Both are right on different
     * hardware: ASK where the OS keeps the app warm, AUTO where the launcher kills it at every menu.
     */
    bool autosave = false;

    /** The ENG column of the TRACE row (which sequencer walks the song) — only in the `android()`
     *  golden profile; no shipping build has a second sequencer. */
    bool engineToggle = false;

    /** PROJECT gains an EXIT row: a handheld launcher needs the process back. */
    bool appExit = false;

    /**
     * The MIDI AUTHORING surfaces: PROJECT > MIDI (and the MIDI screen), EXTERNAL in the instrument TYPE
     * cycle, and the six MIDI commands at the end of songcore::EFFECT_TYPES.
     * ⚠️ AUTHORING ONLY — display is unconditional. A .ptp can carry EXTERNAL instruments and MIDI
     * effect codes; hiding their display would draw a silently dead track as a sampler. So a build
     * with this off cannot CREATE MIDI data, and shows what it finds truthfully.
     * On in every shell profile; `android()` keeps it debug-only, as the golden recorded.
     */
    bool midi = false;

    /**
     * The LOOP-WINDOW pair, held back while their behaviour is decided: the `LPO` command (last of the
     * non-MIDI effects) and `osc` in the LOOP cycle.
     * ⚠️ AUTHORING ONLY, as `midi`: existing `LPO` / `osc` keep drawing and playing.
     * ⚠️ It hides `LPO` only while `midi` is off — both are trimmed from the same tail
     * (songcore/effects.h). With MIDI shown (every shipping profile) it hides `osc` alone.
     */
    bool loopWindow = false;

    /** AUDIO OUT: the platform can switch its sound output while running (Windows: ASIO). */
    bool audioOutputs = false;

    /** The recorded golden's row map: every device row, no exit. */
    static PlatformCaps android(bool debug_build) {
        PlatformCaps c;
        c.debug          = debug_build;
        c.touchLayouts   = true;
        c.skinOverlay    = true;
        c.buttonFeedback = true;
        c.autosave       = true;
        c.engineToggle   = true;
        c.appExit        = false;
        c.midi           = debug_build;
        c.loopWindow     = debug_build;
        return c;
    }

    /**
     * The desktop and handheld shells: physical buttons and a way out.
     * (Not named `linux`: that is a predefined MACRO under gcc's gnu++ dialects — `linux()` would
     * expand to `1()`.)
     */
    static PlatformCaps sdl(bool debug_build) {
        PlatformCaps c;
        c.debug          = debug_build;
        c.touchLayouts   = false;
        c.skinOverlay    = false;
        c.buttonFeedback = false;
        c.autosave       = true;
        c.engineToggle   = false;
        c.appExit        = true;
        c.midi           = true;
        c.loopWindow     = debug_build;
        return c;
    }

    /**
     * What the Android app RUNS (`android-main.cpp`): `sdl()` plus the three rows a touch UI brings —
     * not `sdl()` edited in place (those rows would appear on devices with no touch screen) and not
     * `android()` (which carries `engineToggle`). Written as `sdl()` plus flips so the shared fields
     * cannot drift.
     * ⚠️ The LAYOUT row is additionally gated at RUNTIME on a touch screen and no pad (`app.cpp`'s
     * `useTouch`); this is the static half.
     */
    static PlatformCaps converged(bool debug_build) {
        PlatformCaps c   = sdl(debug_build);
        c.touchLayouts   = true;   // the LAYOUT / skin-picker row
        c.buttonFeedback = true;   // BTN SOUND + BTN VIBRO
        c.skinOverlay    = true;   // the CRT screen overlay
        return c;
    }
};

}  // namespace pt::ui
