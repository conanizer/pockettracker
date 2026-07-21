// PocketTracker — the ANDROID entry point. Everything in this file is platform residue.
//
// Convergence C3, and the file `app.h`'s diagram has had a placeholder for since C0.2. It is the
// sibling of `main.cpp`: the shared shell (`app.cpp`) boots, runs and tears down identically on both,
// and what differs is exactly the list C0.2 named. Set the two side by side and the difference IS the
// port:
//
//   main.cpp (desktop/handheld)              android-main.cpp (this file)
//   ─────────────────────────────────────    ──────────────────────────────────────────────────────
//   argv: project, media dir, app root       argv: the app root, handed down by SDLActivity
//   SIGTERM/SIGINT → a flag the loop polls   nothing (⚠️ C4 — see the terminate_requested note below)
//   SDL_Init / SDL_Quit, here                SDL_Init here too; SDLActivity owns the surface, not this
//   SdlAudioEngine                           OboeAudioEngine  ← the whole of C3's audio work
//   default_app_root() + StdFileSystem       the root from Java + StdFileSystem (⚠️ C5, see below)
//   PlatformCaps::sdl(debug), console on     PlatformCaps::sdl(debug) for now (⚠️ C6 replaces it)
//
// ⚠️ **NO `SDL_MAIN_HANDLED` HERE, AND THAT IS THE OPPOSITE OF `main.cpp`.** SDL_main.h defines
// `SDL_MAIN_NEEDED` on `__ANDROID__` and with it `#define main SDL_main`, so the `main` below is
// compiled as `SDL_main` — which is the symbol `SDLActivity` looks up by name (`getMainFunction()`)
// with `dlsym` in the last library `getLibraries()` names. Define SDL_MAIN_HANDLED as the desktop
// does and the rename does not happen, the symbol is not there, and the app dies at start-up with a
// message about a missing entry point rather than anything about this file.

// <cmath> before <SDL.h> — see the note in sdl-audio-engine.h (M_PI, _USE_MATH_DEFINES, C4005).
#include <cmath>

#include <SDL.h>

#include "audio-engine.h"
#include "oboe-audio-engine.h"
#include "ui/platform_caps.h"
#include "ui/std_filesystem.h"

#include "app.h"

#include <android/log.h>
#include <pthread.h>
#include <unistd.h>

#include <cstdio>
#include <memory>
#include <string>

namespace ui = pt::ui;

namespace {

constexpr const char* kLogTag = "PocketTrackerSDL";

// ⚠️ **THE APP ROOT COMES FROM JAVA, AND IT IS NOT A STYLE CHOICE.** `ui::default_app_root()` walks
// `POCKETTRACKER_HOME` → `XDG_DATA_HOME` → `HOME`, and on Android all three miss — so it would fall
// through to the RELATIVE path "PocketTracker", i.e. beside whatever the process's cwd happens to be.
// That is character-for-character the A1 bug, which was found on Windows for the same reason: the
// platform nobody resolved a root for is already broken. Only Java knows where
// `Environment.getExternalStoragePublicDirectory(DIRECTORY_DOCUMENTS)` actually is on this device and
// this OS version, so the activity resolves it and passes it down through `getArguments()`.
//
// The fallback below exists so a bring-up cannot be blocked by a missing argument, and it SAYS SO in
// the log rather than quietly guessing — a silently wrong root would present as "all my projects are
// gone", which is the worst possible way to discover an argv change.
constexpr const char* kFallbackAppRoot = "/storage/emulated/0/Documents/PocketTracker";

// ─── stdout/stderr → logcat ───────────────────────────────────────────────────────────────────────
//
// The shared shell's boot banner and its once-a-second status line are THE bring-up instrument — the
// half of this app that answers "did my samples load?", "where did it put its folders?", "did it find
// my crash file?". On Android they go to a stdout that is `/dev/null` unless somebody has set
// `log.redirect-stdio`, which needs root on most devices. So the two lessons this project has already
// paid for both apply here and neither is satisfied by default: `main.cpp`'s `setvbuf` note (a
// buffered stdout loses everything when the process is killed, which is how a bring-up ends) and P4a's
// (an instrument that is not pointed at the thing tells you nothing about it).
//
// Twenty lines of pipe-and-pump fixes both, and it is platform residue in the strictest sense —
// nothing above this file knows it happened.
void* log_pump(void* arg) {
    const int   fd = static_cast<int>(reinterpret_cast<intptr_t>(arg));
    std::string line;
    char        buf[256];
    ssize_t     n;
    while ((n = read(fd, buf, sizeof(buf))) > 0) {
        for (ssize_t i = 0; i < n; ++i) {
            if (buf[i] == '\n') {
                __android_log_write(ANDROID_LOG_INFO, kLogTag, line.c_str());
                line.clear();
            } else if (buf[i] != '\r') {
                line.push_back(buf[i]);
            }
        }
        // A status line that never ends in '\n' would otherwise accumulate forever. Flush long
        // fragments rather than growing without bound.
        if (line.size() > 1024) {
            __android_log_write(ANDROID_LOG_INFO, kLogTag, line.c_str());
            line.clear();
        }
    }
    return nullptr;
}

void redirect_stdio_to_logcat() {
    // Unbuffered for the same reason main.cpp is: a buffer that dies with the process takes the only
    // record of the boot with it.
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::setvbuf(stderr, nullptr, _IONBF, 0);

    int pfd[2];
    if (pipe(pfd) != 0) return;  // No console is a degraded bring-up, not a failure to launch.
    dup2(pfd[1], STDOUT_FILENO);
    dup2(pfd[1], STDERR_FILENO);
    close(pfd[1]);

    pthread_t t;
    if (pthread_create(&t, nullptr, log_pump, reinterpret_cast<void*>((intptr_t)pfd[0])) == 0) {
        pthread_detach(t);
    }
}

}  // namespace

int main(int argc, char** argv) {
    redirect_stdio_to_logcat();

    // argv[0] is the application name SDLActivity supplies; the root is the first real argument.
    std::string appRoot = (argc > 1 && argv[1] && argv[1][0]) ? argv[1] : std::string();
    if (appRoot.empty()) {
        __android_log_print(ANDROID_LOG_ERROR, kLogTag,
                            "no app root in argv - the activity's getArguments() should pass one. "
                            "Falling back to %s",
                            kFallbackAppRoot);
        appRoot = kFallbackAppRoot;
    }

    // ⚠️ **THE BACK BUTTON, TRAPPED BEFORE SDL_Init (C4).** Untrapped, Android's back runs
    // `SDLActivity.onBackPressed()` → `finish()`, which closes the activity out from under the frame
    // loop mid-edit — and it is the easiest button on a phone to hit by accident. Set, the activity
    // ignores it (SDLActivity.java:623 returns early) and the key still reaches native as
    // `SDLK_AC_BACK`, which sdl-input.cpp maps to B, the app's own cancel.
    //
    // ⚠️ This is read by JAVA, through `nativeGetHintBoolean`, at the moment back is pressed — so it
    // is a hint about the activity's behaviour rather than about any subsystem, and setting it before
    // `SDL_Init` is belt-and-braces rather than a requirement.
    //
    // ⚠️ It works because `android:enableOnBackInvokedCallback` is NOT set in the manifest: at
    // targetSdk 34 that defaults to false, so the legacy `onBackPressed` path SDL hooks is still the
    // one Android uses. A future targetSdk bump that opts into predictive back silently un-traps this
    // — the symptom being the app closing on back again, with nothing here having changed.
    SDL_SetHint(SDL_HINT_ANDROID_TRAP_BACK_BUTTON, "1");

    // ⚠️ NO `SDL_INIT_AUDIO`, and on this platform it is load-bearing rather than tidy: Oboe owns the
    // device here. Asking SDL for the audio subsystem as well would put two libraries on one output
    // stream — convergence-plan §1's "SDL and Oboe are not a choice". `SdlAudioEngine::openStream`
    // initialises the subsystem itself on the platforms that use it, which is what lets this line be
    // identical to the desktop's; see native/audio-backend.h.
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER) != 0) {
        __android_log_print(ANDROID_LOG_ERROR, kLogTag, "SDL_Init failed: %s", SDL_GetError());
        return 1;
    }

    // ⚠️ HEAP, not a local — the same 0xC00000FD the desktop would hit, and worse here: Android's
    // default thread stack is smaller than a desktop's, and this runs on SDLActivity's thread rather
    // than a process main. AudioEngine's per-block DSP scratch, spectrum rings and 256-slot table pool
    // are members.
    auto engine = std::make_unique<AudioEngine>();

    OboeAudioEngine audio(engine.get());
    if (!audio.openStream()) {
        __android_log_print(ANDROID_LOG_ERROR, kLogTag, "openStream failed - no audio device");
        SDL_Quit();
        return 1;
    }

    // ⚠️ **THIS LINE IS C5's SPIKE, ANSWERED BY RUNNING RATHER THAN BY READING.** The open question
    // the plan wanted settled in week one is whether `std::filesystem` can reach
    // `/storage/emulated/0` from native code with MANAGE_EXTERNAL_STORAGE granted. `StdFileSystem` is
    // the same implementation the desktop uses, so if the file browser lists projects on device the
    // answer is yes and `AndroidFileSystem.kt` (324 lines) dies with the rest of the Kotlin in Phase
    // E; if it does not, this one line becomes a JNI-backed `pt::ui::FileSystem` and NOTHING ELSE IN
    // THE TREE CHANGES — the interface has been abstract since S6a precisely so that the worst case
    // is a second implementation rather than a redesign.
    //
    // ⚠️ And that is also the SAF contingency. If the fdroiddata review forces Storage Access
    // Framework, `std::filesystem` is off the table regardless of what this spike measures, because
    // SAF is a Java-only API — the answer would then be the same second implementation, arrived at
    // for a different reason. The seam is what makes both outcomes cheap.
    ui::StdFileSystem filesystem(appRoot);

    ptshell::AppConfig cfg;
    cfg.engine     = engine.get();
    cfg.audio      = &audio;
    cfg.appRoot    = appRoot;
    cfg.filesystem = &filesystem;

    // No command line, so no project and no media dir of its own: the app opens the blank document
    // NEW PROJECT makes and the file browser is how the user reaches their songs — exactly as the
    // shipping handheld target already behaves (PortMaster invokes the binary with no arguments).
    // ⚠️ mediaBaseDir is never empty: an empty base resolves relative sample paths against the
    // process's cwd, and the app root is where Samples/ actually lives.
    cfg.mediaBaseDir = appRoot;

    // ⚠️ **`sdl()`, NOT `android()`, AND THAT IS DELIBERATE FOR C3.** The converged profile is C6's
    // to decide and the plan already sketches it (touch/feedback/overlay on, engineToggle off). But
    // none of those three rows configures anything that exists yet: touch is Phase D, the overlay
    // system is D6, and the haptics trigger moves in Phase E. Turning them on now would put rows in
    // SETTINGS that do nothing — which S7 refused to do on exactly these grounds, in this struct's
    // own comments: "a setting which configures nothing is a lie told in the user's own UI". So the
    // caps say what this build can actually do, and C6 changes them in the session that makes them
    // true. ⚠️ ptinput's goldens keep comparing against `PlatformCaps::android()` regardless — they
    // record Kotlin's row map, not this choice.
#ifdef NDEBUG
    cfg.caps = ui::PlatformCaps::sdl(/*debug_build=*/false);
#else
    cfg.caps = ui::PlatformCaps::sdl(/*debug_build=*/true);
#endif

    // On by default and worth it: with the pump above, the banner and the status line land in logcat,
    // which is the only console this platform has.
    cfg.console = true;

    // ⚠️ **PHASE D: this is a phone, so draw the on-screen gamepad** — when no physical controller is
    // plugged and the letterbox bars have room (the shell decides both). This is NOT the same as
    // flipping `PlatformCaps::touchLayouts`: that is the SETTINGS row that lets the user PICK a layout,
    // and it stays off until PORTRAIT and the skinned grid exist to be picked, so the picker never
    // offers a mode that does nothing (platform_caps.h's own rule). Desktop and the handhelds leave
    // this false — main.cpp says nothing, so the default (false) is the safe answer there.
    cfg.touchCapable = true;

    // ⚠️ **`cfg.windowed = true` UNLOCKS PORTRAIT, AND THAT IS AN ORIENTATION DECISION, NOT A COSMETIC
    // ONE.** It becomes `SDL_WINDOW_RESIZABLE`, which SDL hands straight to
    // `SDLActivity.setOrientationBis` (SDL_androidwindow.c:52): a NON-resizable window takes its
    // orientation from `w > h` and locks to SENSOR_LANDSCAPE for the 640x480 design, while a RESIZABLE
    // one becomes SCREEN_ORIENTATION_FULL_USER, free to follow the sensor into PORTRAIT. Through C4 this
    // was deliberately FALSE, because a rotation into portrait had no layout to land on and would have
    // shown a broken letterboxed screen. Phase D's PORTRAIT2 device skin is that layout, so it flips to
    // true here: held LANDSCAPE the phone still gets C4's pixel-exact 2x window (FULL_USER stays
    // landscape while the device is), and held PORTRAIT it now gets the skin — app.cpp switches on the
    // output aspect, with nothing to keep in sync. Read out of the vendored SDL source, not remembered.
    //
    // ⚠️ **THIS ALSO MAKES A LANDSCAPE-NATIVE HANDHELD (the AYANEO) ROTATABLE**, where C4 proved its
    // geometry with the flag false. Landscape is preserved — the 2x integer scale is a function of the
    // OUTPUT SIZE, not this flag — but a deliberate rotate would now show PORTRAIT2 there too. That is
    // the one behaviour change this slice makes to a C4-proven config, and it is worth a re-check on
    // that device.
    cfg.windowed = true;

    // ⚠️ **NULL, AND C4 IS WHERE THIS GETS ITS ANSWER — NOT HERE.** The desktop polls a SIGTERM flag
    // through this hook once a frame. Android must not: SDL freezes the native thread when the
    // activity pauses (`SDL_HINT_ANDROID_BLOCK_ON_PAUSE`, on by default), which is precisely when the
    // process is most likely to be killed, so a flag consumed by this loop is P4d's never-armed write
    // in a new body — it would read correct and never run. C4's autosave flushes in an
    // `SDL_AddEventWatch` watcher, which fires synchronously on the Java activity thread and does not
    // touch this loop at all. Leaving it null is the honest state: nothing asks this app to
    // terminate yet.
    cfg.terminate_requested = nullptr;

    const int rc = ptshell::run(cfg);

    SDL_Quit();
    return rc;
}
