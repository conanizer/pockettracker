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
    /** A developer build. Gates the OVERLAY row, the TRACE row, and PROJECT's USED RAM readout. */
    bool debug = false;

    /** LAYOUT: FULLSCREEN / LANDSCAPE / PORTRAIT, and the skin column the portrait layout gains. */
    bool touchLayouts = false;

    /** OVERLAY: a PNG laid over the virtual button skin, with a strength. Debug-gated on top. */
    bool skinOverlay = false;

    /** BTN SOUND + BTN VIBRO: the click and the haptic a VIRTUAL button gives back. */
    bool buttonFeedback = false;

    /**
     * RESUME (ASK / AUTO). Off on the shell until the autosave itself exists — a setting that
     * configures nothing is a lie told in the user's own UI, and hiding it is cheaper than the lie.
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
        c.autosave       = false;
        c.engineToggle   = false;
        c.appExit        = true;
        return c;
    }
};

}  // namespace pt::ui
