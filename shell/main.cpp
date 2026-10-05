// PocketTracker — the DESKTOP/HANDHELD entry point. Everything in this file is platform residue;
// the boot sequence, frame loop and teardown are `app.{h,cpp}`, shared by every platform:
//
//   • the COMMAND LINE — a desktop and a PortMaster launch script have one; an APK does not;
//   • the SIGNAL HANDLER — POSIX. Android's lifecycle arrives as `SDL_APP_*` events instead;
//   • `SDL_Init` and `SDL_Quit` — Android's `SDLActivity` owns its own;
//   • the AUDIO BACKEND — `SdlAudioEngine` here, `OboeAudioEngine` there (audio-backend.h);
//   • the ROOT and the FILESYSTEM — `default_app_root()` + `StdFileSystem`;
//   • the CAPS profile, and whether there is a console to print a banner to.

// <cmath> before <SDL.h> — see the note in sdl-audio-engine.h (M_PI, _USE_MATH_DEFINES, C4005).
#include <cmath>
#include <filesystem>
#ifdef _WIN32
#include <io.h>          // _dup2/_fileno — the session log, below
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>     // AttachConsole — PT_CONSOLE, below
#else
#include <unistd.h>      // dup2/isatty  — ditto
#endif

#define SDL_MAIN_HANDLED
#include <SDL.h>

#include "audio-engine.h"
#include "ui/platform_caps.h"
#include "ui/std_filesystem.h"

#include "app.h"
#include "midi-in.h"          // picks the platform's IMidiIn
#include "midi-out.h"         // picks the platform's IMidiOut; the block below has no #ifdef of its own
#include "sdl-audio-engine.h"
#include "audio-outputs-win.h"  // SETTINGS > AUDIO OUT; empty off Windows

#include <csignal>
#include <cstdlib>       // setenv — the window's app id, below
#include <cstdio>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>

namespace ui = pt::ui;

namespace {

// ─── SIGTERM / SIGINT — the launcher taking the process back ─────────────────────────────────────
//
// ⚠️⚠️ THE HANDLER SETS A FLAG AND NOTHING ELSE. A signal handler may only call async-signal-safe
// functions; writing a .ptp (malloc, <filesystem>, ofstream) from one deadlocks on the heap lock if
// the signal lands inside `malloc` — the app hangs, SIGKILL follows, the song is lost. The frame loop
// reads the flag through `AppConfig::terminate_requested` and flushes on the main thread.
//
// ⚠️ OURS, NOT SDL's: on Windows SDL's config carries `/* #undef HAVE_SIGNAL_H */`, so its handlers
// compile to nothing and SIGTERM goes to the CRT's abort(); on Linux an environment variable can
// switch them off (`SDL_NO_SIGNAL_HANDLERS`). SDL leaves a non-SIG_DFL handler in place, so
// installing before `SDL_Init` keeps ours.
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

// Point stdout and stderr at `<app root>/pockettracker-log.txt`, truncated, for this session.
// ⚠️⚠️ THE WINDOWS BUILD HAS NO CONSOLE (a GUI-subsystem app), so without this every line goes
// nowhere. One session per file: the run that just went wrong is the one anybody wants.
// ⚠️ `dup2`, not a second `freopen` — two FILE*s on one path overwrite each other's lines.
// ⚠️ LEFT ALONE when something else already captures us (a PortMaster script's log); only a terminal
// is taken over. Unconditional on Windows. `PT_CONSOLE=1` keeps the output where it was.
void open_session_log(const std::string& appRoot) {
    if (std::getenv("PT_CONSOLE")) {
#ifdef _WIN32
        // ⚠️ On Windows "where it was" is nowhere (a GUI process has no stdout), so the hatch BORROWS
        // the console it was launched from; double-clicked, there is none and it does nothing.
        if (AttachConsole(ATTACH_PARENT_PROCESS)) {
            std::freopen("CONOUT$", "w", stdout);
            std::freopen("CONOUT$", "w", stderr);
        }
#endif
        return;
    }
#ifndef _WIN32
    if (!isatty(fileno(stdout))) return;
#endif
    std::error_code ec;
    std::filesystem::create_directories(appRoot, ec);   // first launch: the root does not exist yet
    const std::string path = appRoot + "/pockettracker-log.txt";
    if (!std::freopen(path.c_str(), "w", stdout)) return;
#ifdef _WIN32
    _dup2(_fileno(stdout), _fileno(stderr));
#else
    dup2(fileno(stdout), fileno(stderr));
#endif
    // ⚠️ Both again: freopen resets the buffering mode main() set, and a file is FULLY buffered by
    // default — which is the exact bug the setvbuf at the top of main() exists to prevent.
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::setvbuf(stderr, nullptr, _IONBF, 0);
}

}  // namespace

int main(int argc, char** argv) {
    // ⚠️ UNBUFFERED stdout: every line here is for a bring-up with no screen yet, and stdout is FULLY
    // buffered when it is not a terminal — pipe to a log, kill the process, and the log is empty.
    // (⚠️ Not `_IOLBF`: the MSVC CRT treats line buffering as full buffering.)
    std::setvbuf(stdout, nullptr, _IONBF, 0);

    SDL_SetMainReady();

    // ⚠️ THE PROJECT IS OPTIONAL, and on the shipping target always absent: PortMaster runs this
    // binary with NO arguments. With no project the app opens the blank document NEW PROJECT makes.
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

    // ⚠️ Resolved here, not beside the FileSystem it feeds: the log must open before the first line.
    const std::string appRoot = (argc > 3) ? argv[3] : ui::default_app_root();
    open_session_log(appRoot);

    // Read it BEFORE SDL_Init, so a bad path fails on the console instead of behind a window that has
    // already opened.
    std::string blob;
    if (hasProject && !read_file(projectPath, blob)) {
        std::fprintf(stderr, "cannot read %s\n", projectPath.c_str());
        return 1;
    }

    // ⚠️ BEFORE SDL_Init: SDL only installs its handlers over a SIG_DFL disposition, so going first
    // keeps ours — and a kill during start-up (a big SF2 off a slow card) finds it rather than abort().
    std::signal(SIGTERM, on_terminate_signal);
    std::signal(SIGINT, on_terminate_signal);

#ifndef _WIN32
    // The window's name to the desktop (Wayland app_id, X11 WM_CLASS). A taskbar or launcher finds the
    // icon by matching it to `pockettracker.desktop`; left alone it is the executable's file name.
    // SDL2 reads the WMCLASS variables, SDL3 (and sdl2-compat over it) SDL_APP_ID. A user's own wins.
    setenv("SDL_APP_ID", "pockettracker", 0);
    setenv("SDL_VIDEO_X11_WMCLASS", "pockettracker", 0);
#endif

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

    // THIS install's app root: `$POCKETTRACKER_HOME` if the launcher says (a PortMaster script points
    // it at the SD card), else by platform — Documents on a desktop, XDG on a handheld.
    ui::StdFileSystem filesystem(appRoot);

    ptshell::AppConfig cfg;
    cfg.engine      = engine.get();
    cfg.audio       = &audio;
    cfg.appRoot     = appRoot;
    cfg.filesystem  = &filesystem;
    cfg.projectBlob = blob;
    cfg.projectPath = projectPath;

    // ⚠️ Never empty — an empty base resolves relative sample paths against the cwd (whatever the
    // launch script last cd'd to). With no project, the app root, where Samples/ lives.
    cfg.mediaBaseDir = hasProject ? baseDir : appRoot;

    // Which SETTINGS rows exist, and whether PROJECT has an EXIT — a VALUE, not an #ifdef
    // (ui/platform_caps.h).
#ifdef NDEBUG
    cfg.caps = ui::PlatformCaps::sdl(/*debug_build=*/false);
#else
    cfg.caps = ui::PlatformCaps::sdl(/*debug_build=*/true);
#endif

    // SETTINGS > AUDIO OUT: the system output or an ASIO driver. The app then talks to the switch,
    // which delegates to whichever is playing.
#ifdef _WIN32
    ptshell::WindowsAudioOutputs audioOutputs(engine.get(), audio);
    cfg.audio             = &audioOutputs;
    cfg.audioOutputs      = &audioOutputs;
    cfg.caps.audioOutputs = true;
#endif

    // A desktop and a handheld both have somewhere for this to go — a terminal, an ssh session, a
    // PortMaster log. The banner and the once-a-second status line are the bring-up instrument.
    cfg.console = true;

    // A real window: resizable, sized to the display. ⚠️ This main only — on Android the flag would
    // also unlock PORTRAIT rotation (app.h). Safe for PortMaster: a 640×480 panel computes 1×, and
    // KMSDRM has no window manager.
    cfg.windowed = true;

    // ⚠️ DEV BRING-UP ONLY: POCKETTRACKER_TOUCH=1 forces the touch skin on a desktop, so the PORTRAIT2
    // skin can be eyeballed on a tall window. The theme PNGs must sit beside the exe (copy
    // `app/src/main/assets/themes/`), or the skin loads 0/10 pieces and draws only casing and labels.
    if (const char* t = SDL_getenv("POCKETTRACKER_TOUCH"); t && t[0] == '1') {
        cfg.touchCapable = true;
        // Also light the LAYOUT row so the skin switch is reachable here, as android-main.cpp does.
        cfg.caps.touchLayouts = true;
        std::printf("input:   TOUCH SKIN forced on (POCKETTRACKER_TOUCH=1) - drag the window tall for PORTRAIT2\n");
    }

    // ── EXTERNAL MIDI out ────────────────────────────────────────────────────────────────────────
    //
    // A DEV OVERRIDE and bring-up console (the device pick lives in settings.json, PROJECT > MIDI):
    //
    //   POCKETTRACKER_MIDI_OUT=<index | name fragment>   open that port
    //   POCKETTRACKER_MIDI_OUT=list                      print the devices and open nothing
    //   POCKETTRACKER_MIDI_OFFSET_MS=<-999..999>         MIDI later (+) or earlier (-) than the audio
    //
    // ⚠️⚠️ THE OVERRIDE DOES NOT OPEN THE PORT: it resolves to a device NAME handed over as a setting,
    // and `InputDispatcher::boot_midi_port()` opens it like the OUTPUT row does — one owner of "which
    // port is open". `open_by_spec` prints the device list unconditionally ("0 devices" vs "3, none
    // matched"). Platform-free: `PlatformMidiOut` is winmm or ALSA rawmidi, both `MidiOutBase`.
#ifdef PT_HAS_PLATFORM_MIDI_OUT
    ptshell::PlatformMidiOut midiOut;
    {
        if (const char* tr = SDL_getenv("POCKETTRACKER_MIDI_TRACE"); tr && tr[0] == '1')
            midiOut.set_trace(true);

        // Attached whether or not anything opens — the MIDI screen needs the ENUMERATOR (see app.h).
        cfg.midiOut = &midiOut;

        const char* spec = SDL_getenv("POCKETTRACKER_MIDI_OUT");
        if (midiOut.open_by_spec(spec ? spec : "")) {
            cfg.midiOutDevice = midiOut.device_name(midiOut.open_index());
            std::printf("midi:    OUT override -> settings device '%s'\n", cfg.midiOutDevice.c_str());
        }
        if (const char* off = SDL_getenv("POCKETTRACKER_MIDI_OFFSET_MS")) {
            cfg.midiOffsetMs = std::atoi(off);
            std::printf("midi:    OFFSET %+d ms\n", cfg.midiOffsetMs);
        }
        // POCKETTRACKER_MIDI_TEST=1 — the TEST row's one-shot, fired before any frame is drawn.
        if (const char* t = SDL_getenv("POCKETTRACKER_MIDI_TEST"); t && t[0] == '1')
            midiOut.test_note(600);
    }
#endif

    // ── MIDI IN ──────────────────────────────────────────────────────────────────────────────────
    //
    //   POCKETTRACKER_MIDI_IN=<index | name fragment>   listen on that port
    //   POCKETTRACKER_MIDI_IN=list                      print the input devices and open nothing
    //   POCKETTRACKER_MIDI_IN_TRACE=1                   print every message as it is drained (app.cpp)
    //
    // ⚠️ IT RESOLVES; IT DOES NOT OPEN: an open input port delivers bytes at once, into a
    // `SongcoreHost` `ptshell::run` has not built yet; the dispatcher opens it and wires the sink.
    // ⭐ Point POCKETTRACKER_MIDI_OUT and _IN at the same loopMIDI port and the sequencer drives its
    // own MIDI in — testable with no hardware.
#ifdef PT_HAS_PLATFORM_MIDI_IN
    ptshell::PlatformMidiIn midiIn;
    {
        // Attached whether or not anything opens: the INPUT row needs the ENUMERATOR.
        cfg.midiIn = &midiIn;

        if (const char* spec = SDL_getenv("POCKETTRACKER_MIDI_IN"))
            cfg.midiInDevice = midiIn.resolve_spec(spec);
    }
#endif

    // POCKETTRACKER_MIDI_SYNC=0|1 — the clock and transport, forced for a scripted run. Outside the
    // backend #ifdef on purpose: the clock decides WHEN a message is released, upstream of any port,
    // so the timing measurement must also run on a build with no MIDI backend.
    if (const char* sy = SDL_getenv("POCKETTRACKER_MIDI_SYNC")) {
        cfg.midiSyncOut = (sy[0] == '0') ? 0 : 1;
        std::printf("midi:    SYNC OUT %s (override)\n", cfg.midiSyncOut ? "ON 24 PPQN" : "off");
    }

    // The launcher's kill, as a question the shared loop can ask once a frame. The handler above only
    // ever sets this flag; everything that has to happen because of it happens in the loop.
    cfg.terminate_requested = [] { return g_terminate != 0; };

    const int rc = ptshell::run(cfg);

    SDL_Quit();
    return rc;
}
