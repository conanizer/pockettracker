#pragma once
/**
 * native/ui/platform_caps.h — what THIS platform can actually do.
 *
 * SETTINGS is the first screen in the whole port where the two platforms genuinely DIFFER. Every
 * screen before it edits the PROJECT or the filesystem, and a project is a project everywhere. But
 * half of SETTINGS is about the DEVICE: Android has touch layouts, a skinned virtual D-pad, button
 * click sounds and haptics, and a KT/C++ sequencer switch. A handheld running the SDL shell has
 * physical buttons, no Kotlin, and — the one thing Android has never needed — a way to QUIT.
 *
 * ⚠️ THE CAPS ARE A VALUE, NOT AN #ifdef, AND THAT IS THE ENTIRE DESIGN.
 *
 * An `#ifdef __ANDROID__` would have been three lines shorter and would have cost the session its
 * measuring stick. Every module in this port is goldened by byte-comparing it against the REAL
 * Kotlin module it replaces (`tools/ptinput`), and a settings screen compiled with Android's rows
 * *removed* cannot be compared against a Kotlin settings screen that HAS them — there would be
 * nothing to compare, and the port would be taking its most divergent screen entirely on trust.
 *
 * As a value, the same C++ code answers both questions:
 *
 *   • `PlatformCaps::android(debug)` reproduces Kotlin's row map EXACTLY, hidden rows and all — so
 *     ptinput can drive it row-for-row against the recorded Kotlin golden.
 *   • `PlatformCaps::sdl(debug)` is what the shell actually runs.
 *
 * The divergence becomes a parameter, and the port is measured against Kotlin with the divergence
 * turned OFF. That is the only honest way to ship a screen that is deliberately different.
 *
 * (`debug` is in here too, rather than as a `#ifdef NDEBUG`, for the same reason and one more: it is
 * what Kotlin ALREADY does — `BuildConfig.DEBUG` is read inline in SettingsModule's draw, in its
 * cursor movement and in ProjectModule's RAM readout. The port plan §5 asks for exactly this: "rows
 * filtered by a small PlatformCaps flags struct (also cleans up the existing debug-only rows
 * pattern)".)
 */

namespace pt::ui {

struct PlatformCaps {
    /**
     * A developer build. Gates the OVERLAY row, the TRACE row, and the RAM readouts on PROJECT and
     * INST.POOL — which `engine_feed.h` then stops sampling, so the two byte counts read 0 here.
     */
    bool debug = false;

    /** LAYOUT: FULLSCREEN / LANDSCAPE / PORTRAIT, and the skin column the portrait layout gains. */
    bool touchLayouts = false;

    /** A physical controller is attached RIGHT NOW. It is the face-button-swap row s gate: a pad
     *  that misreports which button is PRINTED A is the only thing that setting is for, so on a
     *  device with nothing plugged in the row would configure nothing. RUNTIME, like touchLayouts
     *  and for the same reason - the fact it rests on is hot-pluggable. */
    bool padAttached = false;

    /** OVERLAY: a PNG laid over the virtual button skin, with a strength. Debug-gated on top. */
    bool skinOverlay = false;

    /** BTN SOUND + BTN VIBRO: the click and the haptic a VIRTUAL button gives back. */
    bool buttonFeedback = false;

    /**
     * RESUME (ASK / AUTO): what to do with a crash-recovery autosave found at launch.
     *
     * ⚠️ The two answers are both right, on different hardware, which is why this is a setting rather
     * than a policy. ASK suits a device the app is left running on and the OS keeps warm; AUTO suits
     * one whose launcher kills the process every time the user opens a menu, where a prompt on every
     * return is noise rather than a safeguard.
     */
    bool autosave = false;

    /**
     * The ENG column of the TRACE row: which sequencer walks the song. Meaningless here — there is
     * no Kotlin in this process to switch TO.
     */
    bool engineToggle = false;

    /**
     * PROJECT gains an EXIT row. Android apps never exit (the launcher owns that); a handheld
     * launcher needs to be given the process back. Port plan §5.
     */
    bool appExit = false;

    /**
     * The MIDI AUTHORING surfaces: the PROJECT > MIDI row (and so the MIDI screen behind it),
     * EXTERNAL in the instrument TYPE cycle, and the six MIDI commands at the end of
     * songcore::EFFECT_TYPES (in the FX picker and in the FX column's coarse step).
     *
     * ⚠️ IT GATES AUTHORING ONLY — DISPLAY IS UNCONDITIONAL, AND THE DIFFERENCE IS THE WHOLE POINT.
     * Unlike OVERLAY or TRACE, which hide a row and nothing else, MIDI has state that PERSISTS: a
     * .ptp carries `InstrumentType::EXTERNAL`, per-instrument channel/bank/program/CC, and MIDI
     * effect codes in phrase and table cells. A build that also hid the DISPLAY would open such a
     * project and draw an EXTERNAL instrument as a sampler and an `MPG` cell as `---`, while
     * engine_consumer's routing gate still raised no voice for it — a silently dead track with
     * nothing on screen to explain it. So a build with this off cannot CREATE MIDI data, and renders
     * whatever it finds truthfully.
     *
     * Kept separate from `debug` (rather than read off it at each site) so that the release the
     * feature ships in flips one line here, without also un-hiding the developer rows.
     */
    bool midi = false;

    /** Kotlin's world: every device row, no exit. */
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
        return c;
    }

    /**
     * The SDL shell. Physical buttons, no Kotlin, and a way out.
     *
     * (Named `sdl` and not `linux` on purpose: `linux` is a predefined MACRO under gcc's gnu++
     * dialects — `-std=gnu++17`, which is CMake's default when CXX_EXTENSIONS is left alone — and a
     * function called `linux()` would expand to `1()`. It is also the truer name: convergence onto
     * this UI would give Android-on-SDL these same caps, minus the exit.)
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
        c.midi           = debug_build;
        return c;
    }

    /**
     * What the Android app RUNS (`android-main.cpp`): the shell profile plus the three device rows a
     * touch UI brings.
     *
     * It is deliberately neither of the two above:
     *   • not `sdl()` edited in place — that is what the desktop and handheld shells run, and turning
     *     these rows on there would light SETTINGS rows on a device with no touch screen, which is
     *     the "a row that configures nothing is a lie" rule this file opens with;
     *   • not `android()` — that one reproduces the recorded golden's row map, so it carries
     *     `engineToggle`, and there is no second sequencer in this process to switch to.
     *
     * Written as `sdl()` plus three flips rather than a fresh field list, so the two profiles cannot
     * drift apart in the fields they are supposed to share.
     *
     * ⚠️ The LAYOUT row's *existence* is additionally gated at RUNTIME on there being a touch screen
     * and no physical pad (`app.cpp`'s `useTouch`) — a handheld with buttons has this cap true and no
     * touch layout to configure. This cap is the STATIC half of that AND; the pad is the other half.
     */
    static PlatformCaps converged(bool debug_build) {
        PlatformCaps c   = sdl(debug_build);
        c.touchLayouts   = true;   // Phase D  — the LAYOUT / skin-picker row
        c.buttonFeedback = true;   // Phase D  — BTN SOUND + BTN VIBRO
        c.skinOverlay    = true;   // Phase D6 — the CRT screen overlay
        return c;
    }
};

}  // namespace pt::ui
