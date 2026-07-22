// app.{h,cpp} — the SHARED shell: the boot sequence, the frame loop, the teardown.
//
// Convergence C0.2. `main.cpp` was 771 lines of which nearly all was portable orchestration and a
// small, nameable remainder was not. This is the portable part, and it is what convergence-plan C1–C4
// lands *against* instead of forking: an Android `SDLActivity`'s `SDL_main` builds an `AppConfig`, and
// the tracker it gets is character-for-character the tracker the desktop gets.
//
//     ┌─────────────────────────────────────────────┐
//     │  native/ui       the screens, the canvas     │  ← portable, no SDL
//     │  native/songcore the sequencer, the project  │  ← portable, no SDL
//     │  native/         the engine, the DSP         │  ← portable, no SDL
//     ├─────────────────────────────────────────────┤
//     │  app.{h,cpp}     boot · frame loop · exit    │  ← SDL, but the SAME on every platform
//     │  sdl-video · sdl-input                       │  ← SDL, and likewise shared
//     ├─────────────────────────────────────────────┤
//     │  main.cpp        argv · signals · console    │  ← the platform residue. Desktop's.
//     │  (C1: android-main.cpp)                      │  ← Android's, and ONLY this much of it
//     └─────────────────────────────────────────────┘
//
// ── WHAT IS DELIBERATELY *NOT* ABSTRACTED HERE ───────────────────────────────────────────────────
//
// Only the audio backend gets an interface (`audio-backend.h`), because it is the one thing already
// known to differ and already written twice. The other three axes convergence-plan C0.2 names — the
// lifecycle source, root resolution, asset access — are NOT modelled yet, on purpose:
//
//   • **root resolution and the filesystem need no new seam.** `pt::ui::FileSystem` has been an
//     abstract interface since S6a, so `run()` takes one and the platform decides what to pass. That
//     is exactly the seam convergence-plan C5's spike is about: if `std::filesystem` works on
//     `/storage/emulated/0` Android passes the same `StdFileSystem`, and if it does not, it passes
//     its own — with nothing in this file changing either way.
//   • **the lifecycle source is one nullable callback** (`terminate_requested`), not a hierarchy.
//     Desktop polls its SIGTERM flag through it. ⚠️ Android will NOT: SDL freezes the native thread
//     when the activity pauses, so a flag consumed by this loop is P4d's never-armed write in a new
//     body — C4's autosave has to flush in an `SDL_AddEventWatch` watcher, which runs on the Java
//     activity thread and never touches this loop at all. Modelling that now would mean designing an
//     interface for a caller that does not exist, against a freezing behaviour nothing here has yet
//     observed. The hook is where it will plug in; the shape is C4's to choose.
//   • **asset access** has no consumer until Phase D's skins and overlays.
//
// The rule this follows is the one the guardrails keep paying for: an assumption that is true when
// it is made and invalidated by the layer built on top of it. Two of these three have no layer on top
// of them yet, so there is nothing to be right about.

#ifndef POCKETTRACKER_APP_H
#define POCKETTRACKER_APP_H

#include "ui/filesystem.h"
#include "ui/platform_caps.h"

#include <functional>
#include <string>

class AudioBackend;
class AudioEngine;

namespace ptshell {

class ButtonFeedback;

/**
 * Everything the shared shell is GIVEN rather than decides. Every field is filled by the platform's
 * `main` before `run()` is called; nothing here is read after it returns.
 */
struct AppConfig {
    /**
     * The portable engine and the platform's device for it. Both are borrowed — the platform owns
     * them, because ⚠️ `AudioEngine` must be heap-allocated (its per-block DSP scratch, spectrum
     * rings and 256-slot table pool are members, and they blow a 1 MB stack instantly, 0xC00000FD)
     * and the shell should not be the thing with an opinion about that.
     *
     * `audio->openStream()` must already have SUCCEEDED. A shell that cannot make sound is a
     * failure the platform reports on its own console, before a window exists to hide it behind.
     */
    AudioEngine*  engine = nullptr;
    AudioBackend* audio  = nullptr;

    /**
     * Where Projects/ Samples/ Soundfonts/ Instruments/ Renders/ Themes live, and the filesystem
     * rooted at it. Two fields rather than one because the HOST is told the root as a string
     * (`set_app_root`, for re-rooting a project authored on another install) while the UI reaches
     * disk only through the interface — see ui/filesystem.h.
     */
    std::string         appRoot;
    pt::ui::FileSystem* filesystem = nullptr;

    /**
     * The project to open at boot, ALREADY READ. Empty blob = start on the blank document
     * `NEW PROJECT` makes, which is what the shipping handheld target always does (PortMaster invokes
     * the binary with no arguments at all).
     *
     * `projectPath` is for the console line only; `mediaBaseDir` is where the project's RELATIVE
     * sample paths resolve — see the note on `set_media_base_dir` in the .cpp, which is why an empty
     * one is not allowed to mean "the process's cwd".
     */
    std::string projectBlob;
    std::string projectPath;
    std::string mediaBaseDir;

    /**
     * Which SETTINGS rows exist, and whether PROJECT has an EXIT. A VALUE, not an #ifdef
     * (ui/platform_caps.h). ⚠️ Convergence C6 gives Android a NEW profile — touch/feedback/overlay
     * on, `engineToggle` off (there is no Kotlin sequencer left to switch to) — which is precisely
     * the kind of decision that belongs to the caller and not to this file.
     */
    pt::ui::PlatformCaps caps;

    /**
     * The multi-line help banner and the once-a-second status line. On by default because they are
     * the bring-up instrument for a device with no screen yet; a platform whose stdout goes nowhere
     * (an APK's does) turns them off and pays nothing.
     */
    bool console = true;

    /**
     * A WINDOWED host — one whose window the user can drag to any size. Desktop sets it; the window
     * then opens at the largest integer multiple of the design that fits the display, and SETTINGS >
     * SCALING finally has somewhere to show (FIT and INTEGER agree at exactly 640×480, which is why
     * that setting looked dead on desktop even after C4 wired it).
     *
     * ⚠️⚠️ **ANDROID MUST LEAVE THIS FALSE, AND THE REASON IS ORIENTATION, NOT RESIZING.** It becomes
     * `SDL_WINDOW_RESIZABLE`, which SDL hands to `SDLActivity.setOrientationBis` — and a resizable
     * window there means `SCREEN_ORIENTATION_FULL_USER`, i.e. the activity may rotate into PORTRAIT,
     * undoing C4's device-proven 2× landscape geometry. Defaulting to false is what makes the safe
     * answer the one a platform gets by saying nothing. See sdl-video.h::open.
     *
     * ⚠️ PortMaster is unaffected by construction rather than by exclusion: the size is derived from
     * the panel, so a 640×480 handheld computes 1× — exactly what shipped — and KMSDRM has no window
     * manager for the resizable flag to mean anything to.
     */
    bool windowed = false;

    /**
     * This platform has a touchscreen, so the shell draws the virtual gamepad (Phase D) when there is
     * no physical one and room for it. A phone sets it; desktop and the handhelds leave it false (a
     * handheld has real buttons, and drawing panels over a full-bleed frame would only shrink it).
     * ⚠️ NOT the same as `PlatformCaps::touchLayouts`, which is the SETTINGS row that lets the user
     * PICK a layout — that stays off until PORTRAIT and the skin exist to be picked. This just says the
     * hardware can be touched.
     */
    bool touchCapable = false;

    /**
     * The click + haptic a VIRTUAL button gives back (convergence D). BORROWED and NULLABLE: the
     * platform's `main` owns it, and only Android constructs one — a JNI shim into the surviving thin
     * Kotlin managers, which is the "one outward JNI hook" the Phase-E plan names. Desktop and the
     * handhelds leave it null, and `SdlTouch` treats null as "no feedback", so nothing in the shared
     * touch path learns the word `jni`. Paired with `caps.buttonFeedback`, which is what makes the
     * BTN SOUND / BTN VIBRO rows appear in SETTINGS: the pointer plays the feedback, the cap shows the
     * rows that configure it.
     */
    ButtonFeedback* buttonFeedback = nullptr;

    /**
     * Polled once a frame; true ends the session as an UNCLEAN exit, so the autosave is kept.
     *
     * Desktop hands its SIGTERM/SIGINT flag through here. ⚠️ May be null, and Android's will be —
     * see the header note above on why its lifecycle cannot ride this loop.
     */
    std::function<bool()> terminate_requested;
};

/**
 * Boot, run until something asks to stop, save, and tear down. Returns a process exit code.
 *
 * ⚠️ **Every way out of the loop arrives at the same flush**, which is the whole crash-safety design
 * — a launcher's kill, a window close, F10 and PROJECT → EXIT all leave through one path. See the
 * flush comment in the .cpp for why the one exit that ASKED is the one exit that leaves no autosave.
 */
int run(const AppConfig& cfg);

}  // namespace ptshell

#endif  // POCKETTRACKER_APP_H
