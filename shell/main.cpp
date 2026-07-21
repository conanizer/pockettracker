// PocketTracker — the DESKTOP/HANDHELD entry point. Everything in this file is platform residue.
//
// Convergence C0.2 split the old 771-line main.cpp in two. The boot sequence, the frame loop and the
// teardown were portable orchestration and moved to `app.{h,cpp}`, which every platform shares; what
// is left here is the small, nameable remainder that genuinely is not:
//
//   • the COMMAND LINE — a desktop and a PortMaster launch script have one; an APK does not;
//   • the SIGNAL HANDLER — POSIX. Android's lifecycle arrives as `SDL_APP_*` events instead (C4);
//   • `SDL_Init` and `SDL_Quit` — Android's `SDLActivity` owns its own;
//   • the AUDIO BACKEND — `SdlAudioEngine` here, `OboeAudioEngine` there (C3, audio-backend.h);
//   • the ROOT and the FILESYSTEM — `default_app_root()` + `StdFileSystem`. C5 decides Android's;
//   • the CAPS profile, and whether there is a console to print a banner to.
//
// That is the whole list, and it is the point of the split: convergence C1–C4 add a second file
// beside this one rather than forking the tracker.
//
// Everything that decides how a song SOUNDS — the sequencer, effect resolution, the voices, the whole
// DSP chain — is the same C++ the APK ships, linked from native/ and reached through one class:
//
//     songcore::SongcoreHost          (native/songcore/host.h)
//
// which is the same class songcore-jni.cpp marshals for Android. Since Phase 3, everything that
// decides how the app LOOKS is shared too — `pt-ui` (native/ui/) draws the screens into a 640×480
// framebuffer and knows nothing about SDL.

// <cmath> before <SDL.h> — see the note in sdl-audio-engine.h (M_PI, _USE_MATH_DEFINES, C4005).
#include <cmath>

#define SDL_MAIN_HANDLED
#include <SDL.h>

#include "audio-engine.h"
#include "ui/platform_caps.h"
#include "ui/std_filesystem.h"

#include "app.h"
#include "sdl-audio-engine.h"

#include <csignal>
#include <cstdio>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>

namespace ui = pt::ui;

namespace {

// ─── SIGTERM / SIGINT — the launcher taking the process back (S10) ────────────────────────────────
//
// ⚠️⚠️ **THE HANDLER SETS A FLAG. IT DOES NOTHING ELSE, AND THAT IS THE WHOLE OF THE DESIGN.**
//
// The port plan says "handle SIGINT/SIGTERM → autosave" (§5), and read literally — serialize the
// project from inside the handler — it is a bug of exactly the kind the autosave exists to prevent.
// A signal handler may only call async-signal-safe functions. Writing a .ptp is ~440 KB of JSON
// through `malloc`, `<filesystem>` and `ofstream`, and not one of those is on the list: a SIGTERM
// landing while the main thread happens to be inside `malloc` deadlocks the handler on the heap lock.
// The app HANGS instead of saving, the launcher's SIGKILL arrives a second later, and the autosave has
// failed in precisely the case it was written for. Writing to a `volatile sig_atomic_t` is the one
// thing the standard actually promises here, so it is the only thing this does. The frame loop reads
// the flag through `AppConfig::terminate_requested`, leaves, and flushes on the main thread with the
// heap intact.
//
// ⚠️ **AND IT IS OURS, NOT SDL's — WHICH IS NOT WHAT S10 FIRST ASSUMED.** SDL_quit.c installs handlers
// for SIGINT and SIGTERM that do exactly this (`send_quit_pending = SDL_TRUE`; its own comment says
// *"We can't send it in signal handler; SDL_malloc() might be interrupted!"*), and it was tempting to
// call the job done and write nothing. **Measured, and it is not done:**
//
//   • on WINDOWS the whole thing is `#ifdef HAVE_SIGNAL_H`, and SDL's generated config carries
//     `/* #undef HAVE_SIGNAL_H */` — so `SDL_EventSignal_Init` compiles to NOTHING. A scratch harness
//     raising SIGTERM after `SDL_Init` found the disposition still `SIG_DFL`, and the raise went
//     straight to the CRT default: `abort()`, exit code 3. No handler, no quit event, no autosave.
//   • on LINUX it IS compiled in — but it is *also* gated on `SDL_HINT_NO_SIGNAL_HANDLERS`, which is
//     readable from the ENVIRONMENT (`SDL_NO_SIGNAL_HANDLERS=1`). Whether a launcher kill saves the
//     user's song would then depend on an env var in somebody else's launch script.
//
// So the guarantee would have rested on how a third party's SDL was compiled and on a variable we do
// not own — for the one path in the app whose failure mode is *losing the user's work*. Ten lines are
// cheaper than that. ⚠️ SDL will not fight us for it: `SDL_EventSignal_Init` puts back any handler it
// finds that is not `SIG_DFL`, so installing before `SDL_Init` leaves ours in place.
volatile std::sig_atomic_t g_terminate = 0;

extern "C" void on_terminate_signal(int) { g_terminate = 1; }

bool read_file(const std::string& path, std::string& out) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    std::ostringstream ss;
    ss << f.rdbuf();
    out = ss.str();
    return true;
}

// The directory a path lives in — the default media base dir, because a project's relative sample
// paths are relative to the project file itself.
std::string dir_of(const std::string& path) {
    const size_t slash = path.find_last_of("/\\");
    return (slash == std::string::npos) ? std::string(".") : path.substr(0, slash);
}

}  // namespace

int main(int argc, char** argv) {
    // ⚠️ UNBUFFERED stdout, and it is a diagnostic decision rather than a stylistic one. Every line this
    // program prints exists for a bring-up where there is no screen yet — "did my samples load?", "where
    // did it put its folders?", "did it find my crash file?" — and stdout is FULLY buffered the moment it
    // is not a terminal. Pipe the shell to a log during a bring-up and then kill it, which is precisely
    // what a bring-up does, and the buffer dies with the process: the log is empty and every question is
    // still unanswered. The once-a-second status line already knew this and carried its own `fflush`;
    // the START-UP banner — the half that says whether anything worked — did not, and lost itself the
    // first time S10 piped it. One `setvbuf` retires the whole class of bug, and at a print volume of a
    // few lines a second it costs nothing.
    //
    // (⚠️ Not `_IOLBF`: the MSVC CRT does not implement line buffering and silently treats it as full
    // buffering, so the dev box would go on losing the output while Linux looked fine.)
    std::setvbuf(stdout, nullptr, _IONBF, 0);

    SDL_SetMainReady();

    // ⚠️ THE PROJECT IS OPTIONAL, and on the shipping target it is always absent: PortMaster launches
    // a port by running its .sh, which invokes this binary with NO arguments at all. A shell that
    // answers `argc < 2` with a usage message and exit 2 is a port that cannot start — the launcher
    // would report nothing but a return to the menu. With no project the app opens the same blank
    // document NEW PROJECT makes, and the file browser is how a handheld user reaches their songs.
    //
    // usage: pockettracker-sdl [project.ptp] [media-base-dir] [app-root]
    //   project.ptp     a song to open at boot   (default: a blank document)
    //   media-base-dir  where the project's relative sample paths resolve against
    //                   (default: the project file's own directory, or the app root if there is no
    //                    project — see the mediaBaseDir note below)
    //   app-root        where Projects/ Samples/ Soundfonts/ Instruments/ live
    //                   (default: $POCKETTRACKER_HOME, else the platform's — Documents on Windows and
    //                    macOS, XDG/~/.local/share on Linux; see ui::default_app_root)
    const bool        hasProject  = (argc > 1);
    const std::string projectPath = hasProject ? argv[1] : std::string();
    const std::string baseDir     = hasProject ? ((argc > 2) ? argv[2] : dir_of(projectPath)) : std::string();

    // Read it BEFORE SDL_Init, so a bad path fails on the console instead of behind a window that has
    // already opened.
    std::string blob;
    if (hasProject && !read_file(projectPath, blob)) {
        std::fprintf(stderr, "cannot read %s\n", projectPath.c_str());
        return 1;
    }

    // ⚠️ BEFORE SDL_Init, and that ordering is load-bearing in two directions. SDL only installs its own
    // SIGINT/SIGTERM handlers over a disposition that is still SIG_DFL — find one of ours and it puts it
    // straight back — so going first is what keeps ours. And a kill arriving during start-up (media
    // loading a big SF2 off a slow SD card is seconds, not milliseconds) then still finds a handler that
    // does the right thing rather than the CRT default, which is abort().
    std::signal(SIGTERM, on_terminate_signal);
    std::signal(SIGINT, on_terminate_signal);

    // ⚠️ NO `SDL_INIT_AUDIO`. `SdlAudioEngine::openStream` initialises the audio subsystem itself, which
    // is what lets Android drop this backend for Oboe without SDL ever opening a device to fight over —
    // see audio-backend.h.
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER) != 0) {
        std::fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }

    // ⚠️ HEAP, not a local. AudioEngine's per-block DSP scratch, spectrum rings and 256-slot table
    // pool are members, and they blow a 1 MB stack instantly (0xC00000FD).
    auto engine = std::make_unique<AudioEngine>();

    SdlAudioEngine audio(engine.get());
    if (!audio.openStream()) {
        SDL_Quit();
        return 1;
    }

    // THIS install's app root — where Projects/ Samples/ Soundfonts/… live, and the FileSystem's root.
    // `$POCKETTRACKER_HOME` if the launcher says (which is how a PortMaster script points the app at the
    // SD card's ports folder) and otherwise by platform — Documents on a desktop, where a file manager
    // is how the user reaches their songs, and XDG on a handheld, which has no Documents folder.
    const std::string appRoot = (argc > 3) ? argv[3] : ui::default_app_root();
    ui::StdFileSystem filesystem(appRoot);

    ptshell::AppConfig cfg;
    cfg.engine      = engine.get();
    cfg.audio       = &audio;
    cfg.appRoot     = appRoot;
    cfg.filesystem  = &filesystem;
    cfg.projectBlob = blob;
    cfg.projectPath = projectPath;

    // ⚠️ Never empty — an empty base resolves every relative sample path against the process's cwd,
    // which on a handheld is whatever the launch script last cd'd to. With no project on the command
    // line there is no project directory to be relative TO, and the app root is the honest answer: it
    // is where Samples/ lives, so it is where a recovered autosave's relative media actually is.
    cfg.mediaBaseDir = hasProject ? baseDir : appRoot;

    // Which SETTINGS rows exist, and whether PROJECT has an EXIT. A VALUE, not an #ifdef — see
    // ui/platform_caps.h for why that is the whole design: the same module compiled with
    // `PlatformCaps::android()` reproduces Kotlin's row map exactly, which is what lets ptinput
    // byte-compare the port's most divergent screen against the Kotlin one it replaces.
#ifdef NDEBUG
    cfg.caps = ui::PlatformCaps::sdl(/*debug_build=*/false);
#else
    cfg.caps = ui::PlatformCaps::sdl(/*debug_build=*/true);
#endif

    // A desktop and a handheld both have somewhere for this to go — a terminal, an ssh session, a
    // PortMaster log. The banner and the once-a-second status line are the bring-up instrument.
    cfg.console = true;

    // A real window: resizable, sized to the display, and the reason SETTINGS > SCALING has a visible
    // effect here at all (at exactly 640×480, FIT and INTEGER compute the same rect).
    //
    // ⚠️ Set on THIS main only. Android's `android-main.cpp` deliberately leaves it false — the flag
    // becomes SDL_WINDOW_RESIZABLE, which on that platform also unlocks PORTRAIT rotation. See app.h.
    //
    // ⚠️ This binary is also PortMaster's, and that is safe rather than overlooked: the window size is
    // derived from the panel, so a 640×480 handheld gets 1× — the size that shipped — and KMSDRM has
    // no window manager for "resizable" to mean anything to.
    cfg.windowed = true;

    // ⚠️ DEV BRING-UP ONLY: POCKETTRACKER_TOUCH=1 forces the on-screen touch skin on a desktop that has
    // no touchscreen, so the PORTRAIT2 device skin can be eyeballed on a tall resizable window before it
    // is flashed to a phone — drag the window taller than it is wide and app.cpp switches into it. The
    // shipping desktop/handheld leaves it off: it has real buttons and wants no panels over the frame.
    // ⚠️ The theme PNGs must sit beside the exe for `read_asset` to find them off-device (assets.cpp):
    // copy `app/src/main/assets/themes/` next to the binary, or the skin logs 0/10 pieces loaded and the
    // compositor draws only the casing colour and the button labels.
    if (const char* t = SDL_getenv("POCKETTRACKER_TOUCH"); t && t[0] == '1') {
        cfg.touchCapable = true;
        std::printf("input:   TOUCH SKIN forced on (POCKETTRACKER_TOUCH=1) - drag the window tall for PORTRAIT2\n");
    }

    // The launcher's kill, as a question the shared loop can ask once a frame. The handler above only
    // ever sets this flag; everything that has to happen because of it happens in the loop.
    cfg.terminate_requested = [] { return g_terminate != 0; };

    const int rc = ptshell::run(cfg);

    SDL_Quit();
    return rc;
}
