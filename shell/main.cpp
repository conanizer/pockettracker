// The desktop/handheld entry point: only what differs by platform — the command line, the signal
// handler, SDL_Init/SDL_Quit, the audio backend, the root and filesystem, the caps. The rest is app.cpp.

// <cmath> before <SDL.h> — see sdl-audio-engine.h.
#include <cmath>
#include <filesystem>
#ifdef _WIN32
#include <io.h>          // _dup2/_fileno
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>     // AttachConsole
#else
#include <unistd.h>      // dup2/isatty
#endif

#define SDL_MAIN_HANDLED
#include <SDL.h>

#include "audio-engine.h"
#include "ui/platform_caps.h"
#include "ui/std_filesystem.h"

#include "app.h"
#include "midi/midi-in.h"          // picks the platform's IMidiIn
#include "midi/midi-out.h"         // picks the platform's IMidiOut
#include "sdl/sdl-audio-engine.h"
#include "windows/audio-outputs-win.h"  // SETTINGS > AUDIO OUT; empty off Windows

#include <csignal>
#include <cstdlib>       // setenv
#include <cstdio>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>

namespace ui = pt::ui;

namespace {

// ─── SIGTERM / SIGINT — the launcher taking the process back ─────────────────────────────────────
//
// ⚠️ The handler sets a flag and nothing else: saving from it deadlocks on the heap lock if the signal
// lands inside malloc. The loop reads the flag and flushes on the main thread.
// ⚠️ Ours, not SDL's: Windows SDL compiles its handlers out, and on Linux an env var can turn them
// off. Installed before SDL_Init, SDL leaves ours in place.
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

// A project's relative sample paths are relative to the project file — the default media base dir.
std::string dir_of(const std::string& path) {
    const size_t slash = path.find_last_of("/\\");
    return (slash == std::string::npos) ? std::string(".") : path.substr(0, slash);
}

// stdout and stderr → `<app root>/pockettracker-log.txt`, truncated each session. ⚠️ The Windows build
// has no console, so without this every line goes nowhere. Only a terminal is taken over (a PortMaster
// script's log is left alone); always on Windows. `PT_CONSOLE=1` keeps the output where it was.
// ⚠️ `dup2`, not a second `freopen` — two FILE*s on one path overwrite each other's lines.
void open_session_log(const std::string& appRoot) {
    if (std::getenv("PT_CONSOLE")) {
#ifdef _WIN32
        // A Windows GUI process has no stdout: borrow the launching console, if there is one.
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
    std::filesystem::create_directories(appRoot, ec);   // first launch
    const std::string path = appRoot + "/pockettracker-log.txt";
    if (!std::freopen(path.c_str(), "w", stdout)) return;
#ifdef _WIN32
    _dup2(_fileno(stdout), _fileno(stderr));
#else
    dup2(fileno(stdout), fileno(stderr));
#endif
    // ⚠️ Again: freopen resets the buffering, and a file is fully buffered by default.
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::setvbuf(stderr, nullptr, _IONBF, 0);
}

}  // namespace

int main(int argc, char** argv) {
    // ⚠️ Unbuffered: a piped stdout is fully buffered, so a killed process leaves an empty log.
    // Not `_IOLBF` — the MSVC CRT treats line buffering as full buffering.
    std::setvbuf(stdout, nullptr, _IONBF, 0);

    SDL_SetMainReady();

    // usage: pockettracker-sdl [project.ptp] [media-base-dir] [app-root]
    // All optional — PortMaster passes none, and the app opens a blank project. media-base-dir defaults
    // to the project's folder (else the app root); app-root to $POCKETTRACKER_HOME, else the
    // platform's (ui::default_app_root).
    const bool        hasProject  = (argc > 1);
    const std::string projectPath = hasProject ? argv[1] : std::string();
    const std::string baseDir     = hasProject ? ((argc > 2) ? argv[2] : dir_of(projectPath)) : std::string();

    // ⚠️ Resolved here: the log must open before the first line.
    const std::string appRoot = (argc > 3) ? argv[3] : ui::default_app_root();
    open_session_log(appRoot);

    // Before SDL_Init, so a bad path fails on the console, not behind an open window.
    std::string blob;
    if (hasProject && !read_file(projectPath, blob)) {
        std::fprintf(stderr, "cannot read %s\n", projectPath.c_str());
        return 1;
    }

    // ⚠️ Before SDL_Init, which installs its handlers only over SIG_DFL — and a kill during a slow
    // start-up finds ours rather than abort().
    std::signal(SIGTERM, on_terminate_signal);
    std::signal(SIGINT, on_terminate_signal);

#ifndef _WIN32
    // The window's app id (Wayland app_id, X11 WM_CLASS), matched to `pockettracker.desktop` for the
    // icon. SDL2 reads the WMCLASS variable, SDL3 SDL_APP_ID; a user's own wins.
    setenv("SDL_APP_ID", "pockettracker", 0);
    setenv("SDL_VIDEO_X11_WMCLASS", "pockettracker", 0);
#endif

    // ⚠️ No SDL_INIT_AUDIO: `SdlAudioEngine::openStream` initialises it, so Android can use Oboe with
    // no SDL device to fight over (audio-backend.h).
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER) != 0) {
        std::fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }

    // ⚠️ Heap, not a local: AudioEngine's scratch buffers blow a 1 MB stack.
    auto engine = std::make_unique<AudioEngine>();

    SdlAudioEngine audio(engine.get());
    if (!audio.openStream()) {
        SDL_Quit();
        return 1;
    }

    ui::StdFileSystem filesystem(appRoot);

    ptshell::AppConfig cfg;
    cfg.engine      = engine.get();
    cfg.audio       = &audio;
    cfg.appRoot     = appRoot;
    cfg.filesystem  = &filesystem;
    cfg.projectBlob = blob;
    cfg.projectPath = projectPath;

    // ⚠️ Never empty — it would resolve against whatever the launch script last cd'd to.
    cfg.mediaBaseDir = hasProject ? baseDir : appRoot;

#ifdef NDEBUG
    cfg.caps = ui::PlatformCaps::sdl(/*debug_build=*/false);
#else
    cfg.caps = ui::PlatformCaps::sdl(/*debug_build=*/true);
#endif

    // SETTINGS > AUDIO OUT: the switch delegates to the system output or an ASIO driver.
#ifdef _WIN32
    ptshell::WindowsAudioOutputs audioOutputs(engine.get(), audio);
    cfg.audio             = &audioOutputs;
    cfg.audioOutputs      = &audioOutputs;
    cfg.caps.audioOutputs = true;
#endif

    // A terminal, an ssh session or a PortMaster log — always somewhere to print to.
    cfg.console = true;

    // ⚠️ This main only — on Android the flag would also unlock portrait rotation (app.h).
    cfg.windowed = true;

    // Dev only: POCKETTRACKER_TOUCH=1 forces the touch skin on a desktop (drag the window tall). The
    // theme PNGs must sit beside the exe (`app/src/main/assets/themes/`), or only casing and labels draw.
    if (const char* t = SDL_getenv("POCKETTRACKER_TOUCH"); t && t[0] == '1') {
        cfg.touchCapable = true;
        // …and the LAYOUT row, so the skin switch is reachable.
        cfg.caps.touchLayouts = true;
        std::printf("input:   TOUCH SKIN forced on (POCKETTRACKER_TOUCH=1) - drag the window tall for PORTRAIT2\n");
    }

    // ── EXTERNAL MIDI out ────────────────────────────────────────────────────────────────────────
    //
    // Dev overrides (the device pick lives in settings.json):
    //   POCKETTRACKER_MIDI_OUT=<index | name fragment>   that port; `list` prints the devices
    //   POCKETTRACKER_MIDI_OFFSET_MS=<-999..999>         MIDI later (+) or earlier (-) than the audio
    // ⚠️ The override does not open the port: it resolves to a NAME, and `boot_midi_port()` opens it
    // like the OUTPUT row does — one owner of "which port is open".
#ifdef PT_HAS_PLATFORM_MIDI_OUT
    ptshell::PlatformMidiOut midiOut;
    {
        if (const char* tr = SDL_getenv("POCKETTRACKER_MIDI_TRACE"); tr && tr[0] == '1')
            midiOut.set_trace(true);

        // Attached even when nothing opens — the MIDI screen needs the enumerator.
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
        // POCKETTRACKER_MIDI_TEST=1 — the TEST row's one-shot, before the first frame.
        if (const char* t = SDL_getenv("POCKETTRACKER_MIDI_TEST"); t && t[0] == '1')
            midiOut.test_note(600);
    }
#endif

    // ── MIDI IN ──────────────────────────────────────────────────────────────────────────────────
    //
    //   POCKETTRACKER_MIDI_IN=<index | name fragment>   that port; `list` prints the devices
    //   POCKETTRACKER_MIDI_IN_TRACE=1                   print every message as it is drained
    // ⚠️ Resolved, not opened: an open port delivers bytes before `SongcoreHost` exists. Point _OUT and
    // _IN at one loopback port and the sequencer drives its own MIDI in — testable with no hardware.
#ifdef PT_HAS_PLATFORM_MIDI_IN
    ptshell::PlatformMidiIn midiIn;
    {
        // Attached even when nothing opens: the INPUT row needs the enumerator.
        cfg.midiIn = &midiIn;

        if (const char* spec = SDL_getenv("POCKETTRACKER_MIDI_IN"))
            cfg.midiInDevice = midiIn.resolve_spec(spec);
    }
#endif

    // POCKETTRACKER_MIDI_SYNC=0|1 forces the clock. Outside the backend #ifdef: the clock decides when a
    // message is released, so the timing measurement runs on a build with no backend too.
    if (const char* sy = SDL_getenv("POCKETTRACKER_MIDI_SYNC")) {
        cfg.midiSyncOut = (sy[0] == '0') ? 0 : 1;
        std::printf("midi:    SYNC OUT %s (override)\n", cfg.midiSyncOut ? "ON 24 PPQN" : "off");
    }

    // The handler only ever sets this flag; the loop does the rest.
    cfg.terminate_requested = [] { return g_terminate != 0; };

    const int rc = ptshell::run(cfg);

    SDL_Quit();
    return rc;
}
