// PocketTracker — the SDL shell. Linux-port plan Phase 2 (the sound) + Phase 3 (the UI).
//
// Everything that decides how a song SOUNDS — the sequencer, effect resolution, the voices, the whole
// DSP chain — is the same C++ the APK ships, linked from native/ and reached through one class:
//
//     songcore::SongcoreHost          (native/songcore/host.h)
//
// which is the same class songcore-jni.cpp marshals for Android. Since Phase 3 began, everything that
// decides how the app LOOKS is shared too — `pt-ui` (native/ui/) draws the screens into a 640×480
// framebuffer and knows nothing about SDL. So what is left in this file is only, and exactly, the
// shell: a window, an audio device, an input source, and a clock.
//
//     ┌─────────────────────────────────────────────┐
//     │  native/ui       the screens, the canvas     │  ← portable, no SDL   (tools/ptshot draws it headless)
//     │  native/songcore the sequencer, the project  │  ← portable, no SDL
//     │  native/         the engine, the DSP         │  ← portable, no SDL
//     ├─────────────────────────────────────────────┤
//     │  linux/          window · audio · input      │  ← the only SDL in the program
//     └─────────────────────────────────────────────┘
//
// The one thing worth stating plainly: the UI edits `host.edit_project()` — the SAME Project object
// the Sequencer is reading — so an edit is live the instant it is made. There is no second copy of the
// document and therefore nothing that can desync. (Android needs a second copy because Compose needs
// an observable object graph; it pushes the whole thing down as JSON on every change. There is no
// Kotlin here.)
//
// ─── WHAT IS HERE ────────────────────────────────────────────────────────────────────────────────
//
// All SIXTEEN screens, as of Phase 3 S9 — SONG, CHAIN, PHRASE, TABLE, GROOVE, INSTRUMENT, INST.POOL,
// MODS, MIXER, EFFECTS, PROJECT, SETTINGS, the FILE BROWSER, the SAMPLE EDITOR, and the EQ and THEME
// editor overlays — with the whole input layer under them: selection, the clipboard, item cycling,
// cloning, the note preview, the FX helper, the QWERTY keyboard, the confirm dialog, the auditions.
// Nothing in the Kotlin dispatcher is unported. There is no "COMING SOON" placeholder left to draw.
//
// S10 added the LIFECYCLE, which is the part of an app that has no screen: the crash-recovery autosave,
// the signal handler that makes a launcher's kill survivable, and the RECOVER WORK? prompt that hands
// the work back. The three things this file does that `pt-ui` cannot are all in that shape — a window,
// a clock, and a process that can be taken away.

// <cmath> before <SDL.h> — see the note in sdl-audio-engine.h (M_PI, _USE_MATH_DEFINES, C4005).
#include <cmath>

#define SDL_MAIN_HANDLED
#include <SDL.h>

#include "audio-engine.h"
#include "songcore/host.h"
#include "ui/app_state.h"
#include "ui/canvas.h"
#include "ui/engine_feed.h"
#include "ui/input_dispatcher.h"
#include "ui/layout.h"
#include "ui/platform_caps.h"
#include "ui/settings_store.h"
#include "ui/std_filesystem.h"

#include "sdl-audio-engine.h"
#include "sdl-input.h"
#include "sdl-video.h"

#include <csignal>
#include <cstdio>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>

using namespace songcore;
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
// the flag, leaves, and flushes on the main thread with the heap intact.
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

/**
 * The mapper's own memory — everything the combo matrix must remember BETWEEN events, and nothing
 * else. Kotlin keeps the same two fields on `InputMapper` itself.
 */
struct MapperState {
    /** The A,A double-tap window. */
    Uint64 lastAPress = 0;

    /**
     * ⚠️ The DEFERRED-A latch (`InputMapper.aPressedAlone`). Set when A goes down on a cell whose A
     * OPENS something — and whose A+DPAD/A+B means something ELSE. The open is then held until A is
     * RELEASED, and CANCELLED outright if any A-combo fires in between.
     *
     * Without it, holding A on the INSTRUMENT NAME cell and pressing B to reset it would open the
     * keyboard first and land the combo on top of it. The dispatcher decides which cells qualify
     * (`defer_a_to_release()`); the mapper owns the latch, because it is the mapper that sees the
     * press, the combo and the release as one gesture.
     */
    bool aPressedAlone = false;

    /**
     * ⚠️ The DEFERRED-B latch (`InputMapper.bPressedAlone`), the exact mirror of the above, and it
     * exists for exactly one screen: the EQ EDITOR (S8).
     *
     * There, B is BOTH the close AND the modifier of the slot cycle (B+LEFT/RIGHT walks the 128-preset
     * bank). Fire the close on B's own press and the cycle is unreachable — you would be back on the
     * mixer before LEFT ever arrived. So the close is held until B is RELEASED, and CANCELLED if any
     * B-combo fires in between.
     */
    bool bPressedAlone = false;
};

/**
 * The COMBO MATRIX — a 1:1 port of `InputMapper.handleButtonAction`, and the last piece of the input
 * chain that is allowed to be platform-specific.
 *
 * Android's chain is InputMapper → ButtonHandlers → AppInputDispatcher. This function is the first
 * arrow: it owns nothing but the question "which of the ~30 named handlers does this press mean?",
 * and it answers it from the button plus the modifiers held at the instant it happened. Everything
 * downstream — what a handler DOES — is `pt::ui::InputDispatcher`, which is portable C++ and has never
 * heard of SDL.
 *
 * ⚠️ **THE ORDER OF THESE CHECKS IS THE SPECIFICATION.** They run most-specific-first, and each arm
 * RETURNS. L+B+A must be tested before L+A or a clone would paste; A+B before a plain A or a delete
 * would insert; R+A is consumed and does nothing, so that holding R to change screens cannot also fire
 * an edit underneath. Reordering them silently changes what the tracker does.
 *
 * ⚠️ **The modifiers come from the EVENT, never from `input.is_held()`.** See sdl-input.h: SDL delivers
 * a frame's worth of events at once, so a poll-time read describes the end of the frame rather than the
 * instant of each event — and B-then-A inside one 16 ms frame would fire A+B (delete) on the B press.
 */
void handle_button(const ButtonEvent& e, ui::InputDispatcher& d, MapperState& ms, Uint64 now) {
    const ButtonMods& m = e.mods;

    // ── RELEASE ──────────────────────────────────────────────────────────────────────────────────
    if (e.action != ButtonAction::PRESSED) {
        if (e.button == Button::A) {
            // The DEFERRED single-A: it went down on a sub-screen-opening cell and no A-combo
            // intervened, so the open fires NOW, on the release, rather than on the press.
            if (ms.aPressedAlone) {
                ms.aPressedAlone = false;
                d.on_button_a();
            }
            // The FX helper commits on the RELEASE of A — which is what lets you hold A, read your way
            // through the effect grid, and let go on the one you want.
            d.on_a_released();
        }
        if (e.button == Button::B) {
            // The DEFERRED single-B: it went down inside the EQ editor and no B-combo intervened, so
            // the CLOSE fires now rather than on the press. Mirror of the A latch above.
            if (ms.bPressedAlone) {
                ms.bPressedAlone = false;
                d.on_button_b();
            }
        }
        return;
    }

    // Silence a ringing audition on any "plain" press. START is exempt (it starts playback), and so is
    // anything pressed while A is held — that covers every edit combo, and an edit should stay audible.
    if (e.button != Button::START && !m.a) d.on_stop_preview();

    // ── A + … : the tracker's core editing gesture ───────────────────────────────────────────────
    if (m.a && !m.l && !m.r) {
        switch (e.button) {
            // ⚠️ Every arm CANCELS the deferred A first: the gesture turned out to be a combo, so the
            // open it was holding must not fire when A comes back up.
            case Button::B:          ms.aPressedAlone = false; d.on_a_b();     return;   // delete / reset
            case Button::DPAD_UP:    ms.aPressedAlone = false; d.on_a_up();    return;   // +1 (or the FX helper)
            case Button::DPAD_DOWN:  ms.aPressedAlone = false; d.on_a_down();  return;   // −1
            case Button::DPAD_RIGHT: ms.aPressedAlone = false; d.on_a_right(); return;   // +16 / +1 octave
            case Button::DPAD_LEFT:  ms.aPressedAlone = false; d.on_a_left();  return;   // −16 / −1 octave
            default: break;
        }
    }

    // ── SELECT + … : the file browser's file-management chords ───────────────────────────────────
    //
    // ⚠️ `(!m.r || e.button == R_SHIFT)`, not `!m.r`, and this is `ButtonHandlers.kt:621` to the
    // character. **The mods snapshot INCLUDES the button being pressed** (that is what makes the L+R
    // arm below work at all, and why the plain-SELECT arm has to be checked ahead of the no-modifier
    // guard). So on the SELECT+R chord, R's own press has already set `m.r` — a flat `!m.r` would
    // reject the very gesture it is trying to match. R may be held here only when R IS the button.
    if (m.select && !m.l && (!m.r || e.button == Button::R_SHIFT)) {
        switch (e.button) {
            case Button::A:       d.on_select_a(); return;   // rename     (opens the keyboard)
            case Button::B:       d.on_select_b(); return;   // delete     (arms the confirm)
            case Button::R_SHIFT: d.on_select_r(); return;   // new folder (opens the keyboard)
            default: break;
        }
    }

    // ── B + DPAD: WHICH item am I looking at? ────────────────────────────────────────────────────
    if (m.b && !m.l && !m.r && !m.a) {
        switch (e.button) {
            // ⚠️ Every arm CANCELS the deferred B, for the same reason the A-combos cancel the deferred
            // A: the gesture turned out to be a combo, so the CLOSE it was holding must not fire when B
            // comes back up. Without this, cycling the EQ slot with B+RIGHT would shut the editor the
            // moment you let go of B.
            case Button::DPAD_LEFT:  ms.bPressedAlone = false; d.on_b_left();  return;   // prev item / EQ slot −1
            case Button::DPAD_RIGHT: ms.bPressedAlone = false; d.on_b_right(); return;   // next item / EQ slot +1
            case Button::DPAD_UP:    ms.bPressedAlone = false; d.on_b_up();    return;   // SONG / pool: page up
            case Button::DPAD_DOWN:  ms.bPressedAlone = false; d.on_b_down();  return;   // SONG / pool: page down
            default: break;
        }
    }

    // ── L+R: leave selection mode ────────────────────────────────────────────────────────────────
    if (m.l && m.r) {
        switch (e.button) {
            // Reserved chords: consumed so they cannot fall through to a single-button handler
            // mid-chord and do something the user never asked for.
            case Button::SELECT:
            case Button::A:
            case Button::B:       return;
            case Button::L_SHIFT:
            case Button::R_SHIFT: d.on_l_r(); return;
            default: break;
        }
    }

    // ── L+B+A: clone. BEFORE the L+button block, or L+A would paste instead. ─────────────────────
    if (m.l && m.b && !m.r && e.button == Button::A) {
        d.on_l_b_a();
        return;
    }

    // ── L + … : selection and the clipboard ──────────────────────────────────────────────────────
    if (m.l && !m.r) {
        switch (e.button) {
            case Button::A:     d.on_l_a(); return;   // cut (in a selection) / paste (outside one)
            case Button::B:     d.on_l_b(); return;   // enter selection, then widen it
            case Button::START: return;               // reserved — START must not toggle playback here
            default: break;                           // L+DPAD is the file browser's; it has no screen yet
        }
    }

    // ── R + DPAD: move between screens ───────────────────────────────────────────────────────────
    if (m.r && !m.l) {
        switch (e.button) {
            case Button::DPAD_UP:    d.on_r_up();    return;
            case Button::DPAD_DOWN:  d.on_r_down();  return;
            case Button::DPAD_LEFT:  d.on_r_left();  return;
            case Button::DPAD_RIGHT: d.on_r_right(); return;
            // Reserved, and consumed: R is held to navigate, and a stray A must not edit underneath it.
            case Button::A:
            case Button::B:
            case Button::START:      return;
            default: break;
        }
    }

    // ── A, and the A,A double-tap ────────────────────────────────────────────────────────────────
    // 300 ms. The window belongs to the MAPPER on Android (`InputMapper.lastAPress`), so it lives
    // here rather than in the dispatcher — and it is passed in rather than kept in a function-local
    // static, because a mapper that hides its own state cannot be driven by a test.
    if (e.button == Button::A && !m.l && !m.r) {
        // ⚠️ The DEFER, first: on a cell whose A opens a sub-screen, hold the action until A comes back
        // up, so an A+DPAD or A+B on the same cell is not pre-empted by an immediate open. A deferred
        // cell is never a double-tap cell — there is nothing there to insert — so the A,A path below is
        // skipped for it, and `lastAPress` is cleared so the NEXT A press cannot read as the second
        // half of a double-tap that never happened.
        if (d.defer_a_to_release()) {
            ms.aPressedAlone = true;
            ms.lastAPress    = 0;
            return;
        }
        if (now - ms.lastAPress < 300) {
            ms.lastAPress = 0;   // …so a triple-tap does not read as two double-taps
            d.on_a_a();          // insert the next UNUSED chain/phrase
        } else {
            ms.lastAPress = now;
            d.on_button_a();     // insert the LAST-EDITED one
        }
        return;
    }

    // ── B, and the deferred close ────────────────────────────────────────────────────────────────
    if (e.button == Button::B && !m.l && !m.r && !m.a) {
        // ⚠️ The DEFER, exactly as A's above: inside the EQ editor B is a CLOSE, but it is also the
        // modifier of the slot cycle — so hold it until B comes back up and let a B+DPAD cancel it.
        if (d.defer_b_to_release()) {
            ms.bPressedAlone = true;
            return;
        }
        d.on_button_b();   // copy a selection / leave the browser / back out of the sample editor
        return;
    }

    // ⚠️ SELECT and START are checked EXPLICITLY, ahead of the "no modifiers" guard below, and the
    // reason is easy to miss: pressing SELECT sets `m.select` — its own press would be swallowed by a
    // guard that rejects any modifier. Kotlin carries the same explicit check for the same reason.
    if (e.button == Button::SELECT && !m.l && !m.r && !m.a && !m.b) { d.on_select(); return; }
    if (e.button == Button::START && !m.l && !m.r && !m.a && !m.b && !m.select) { d.on_start(); return; }

    // ── The D-pad, unmodified: move the cursor (or drag a selection's edge) ──────────────────────
    if (m.l || m.r || m.a || m.b || m.select) return;  // any modifier down: not a plain press

    switch (e.button) {
        case Button::DPAD_UP:    d.on_dpad_up();    break;
        case Button::DPAD_DOWN:  d.on_dpad_down();  break;
        case Button::DPAD_LEFT:  d.on_dpad_left();  break;
        case Button::DPAD_RIGHT: d.on_dpad_right(); break;
        default: break;
    }
}

}  // namespace

int main(int argc, char** argv) {
    // ⚠️ UNBUFFERED stdout, and it is a diagnostic decision rather than a stylistic one. Every line this
    // program prints exists for a bring-up where there is no screen yet — "did my samples load?", "where
    // did it put its folders?", "did it find my crash file?" — and stdout is FULLY buffered the moment it
    // is not a terminal. Pipe the shell to a log during a bring-up and then kill it, which is precisely
    // what a bring-up does, and the buffer dies with the process: the log is empty and every question is
    // still unanswered. The once-a-second status line below already knew this and carried its own
    // `fflush`; the START-UP banner — the half that says whether anything worked — did not, and lost
    // itself the first time S10 piped it. One `setvbuf` retires the whole class of bug, and at a print
    // volume of a few lines a second it costs nothing.
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
    //                    project — see set_media_base_dir below)
    //   app-root        where Projects/ Samples/ Soundfonts/ Instruments/ live
    //                   (default: $POCKETTRACKER_HOME, else XDG, else ~/.local/share)
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
    engine->onResumeRequested = [&audio] { audio.resumeStream(); };

    SongcoreHost host(engine.get(), audio.sampleRate());
    if (hasProject) {
        if (!host.push_project(blob)) {
            std::fprintf(stderr, "%s did not parse as a .ptp\n", projectPath.c_str());
            SDL_Quit();
            return 1;
        }

        const MediaLoadResult media = host.load_media(baseDir);
        std::printf("project: %s\nmedia:   %d loaded, %d failed (base dir: %s)\n", projectPath.c_str(),
                    media.loaded, media.failed, baseDir.c_str());
        if (media.failed > 0) {
            // ASCII, like every other line this program prints — see the banner below. (This one was NOT:
            // it carried an em-dash, on the one path you most want legible when something has already gone
            // wrong on a device with no screen. Found by grepping the file against its own stated rule.)
            std::fprintf(stderr,
                         "warning: %d sample(s)/SoundFont(s) failed to load - those instruments will be "
                         "silent\n",
                         media.failed);
        }
    } else {
        // The same document NEW PROJECT builds. There is no media to load: a blank project references
        // no samples, so load_media would walk an empty instrument pool and report 0/0.
        host.new_project();
        std::printf("project: (none given) - starting on a blank document\n");
    }

    // ⚠️ **Load the media, then push the PARAMS.** The engine holds a great deal of state on its own
    // behalf that no note ever carries — the mixer, the master bus, reverb, delay, the 128-slot EQ bank,
    // and every instrument's drive / crush / filter / sample window / loop — and until Phase 3 S4 the
    // only thing in the whole tree that pushed any of it was the offline renderer. So this shell would
    // RENDER a project correctly and PLAY the same project on the engine's factory defaults. See
    // songcore::push_live_params: it survived Phase 2 and three Phase-3 sessions because it is invisible
    // to every conformance tool (none of it is an event, none of it is made by a note, and the one tool
    // that compares audio renders — through the path that was already right).
    host.push_params();

    // ── The file system (S6a) ────────────────────────────────────────────────────────────────────
    //
    // The seven app directories — Projects, Samples, Soundfonts, Instruments, Renders, Themes — live
    // under ONE root, and the root is the only per-platform fact about files. Android hard-codes
    // `Documents/PocketTracker` because that is the one place scoped storage lets it write and the user
    // browse; a handheld has no Documents directory, so it is chosen here (`$POCKETTRACKER_HOME`, then
    // XDG, then `$HOME/.local/share` — see default_app_root), and a PortMaster launch script sets the
    // env var to point at the SD card's ports folder.
    //
    // The SUB-directory names are identical to Android's on purpose: a user who copies their
    // `PocketTracker/` folder off a phone onto an SD card must find their projects where the app looks.
    const std::string appRoot = (argc > 3) ? argv[3] : ui::default_app_root();
    ui::StdFileSystem filesystem(appRoot);

    // ⚠️ Create them NOW, at boot, rather than lazily on the first browse. `ensure_dir` runs inside each
    // getter, so the folders would otherwise not exist until the user opened the browser once — and on a
    // handheld that is exactly backwards. The first thing anyone does with a new port is plug the SD
    // card into a PC and copy their samples in, and they cannot do that if the app has not yet said
    // where. (Android gets this for free: it calls the getters all through start-up.)
    filesystem.projects_directory();
    filesystem.samples_directory();
    filesystem.renders_directory();
    filesystem.instruments_directory();
    filesystem.soundfonts_directory();
    filesystem.themes_directory();
    std::printf("files:   %s\n", appRoot.c_str());

    SdlVideo video;
    if (!video.open("PocketTracker", ui::DESIGN_W, ui::DESIGN_H)) {
        SDL_Quit();
        return 1;
    }

    SdlInput input;
    input.open_controllers();

    // The UI state points at the host's live project — one document, edited in place.
    ui::AppState state;
    state.project       = &host.edit_project();
    state.currentScreen = ui::ScreenType::PHRASE;

    // ── What THIS platform can do (S7) ───────────────────────────────────────────────────────────
    //
    // Which SETTINGS rows exist, and whether PROJECT has an EXIT. A VALUE, not an #ifdef — see
    // ui/platform_caps.h for why that is the whole design: the same module compiled with
    // `PlatformCaps::android()` reproduces Kotlin's row map exactly, which is what lets ptinput
    // byte-compare the port's most divergent screen against the Kotlin one it replaces.
#ifdef NDEBUG
    state.caps = ui::PlatformCaps::sdl(/*debug_build=*/false);
#else
    state.caps = ui::PlatformCaps::sdl(/*debug_build=*/true);
#endif

    // ── settings.json ────────────────────────────────────────────────────────────────────────────
    // SharedPreferences, as a file. No file = first launch = the factory settings, which is not an
    // error and gets no complaint.
    if (ui::load_settings(filesystem, state.settings, state.theme))
        std::printf("settings: %s\n", filesystem.settings_path().c_str());

    ui::Canvas        canvas;
    ui::TrackerLayout layout;
    ui::EngineFeed    feed;

    // The whole input layer, in one object. It edits `host.edit_project()` — the SAME Project the
    // Sequencer is reading — so an edit is live the instant it is made.
    ui::InputDispatcher dispatch(state, host, filesystem);
    MapperState         mapper;

    // ── What a RENDER needs from the shell (S7) ──────────────────────────────────────────────────
    //
    // The render is SYNCHRONOUS — the frame loop stops and renders. Android hands it to a coroutine
    // because Compose would ANR; there is nothing here to hand it to, and stopping is the safer
    // answer anyway, because the ONE thing that must not touch the engine while an offline render is
    // driving it is the audio callback.
    //
    // ⚠️ So the device is PAUSED, not merely stopped. Kotlin stops PLAYBACK and leaves its Oboe stream
    // open and idle, which is a race it happens to win (an idle callback reads a silent engine). A
    // paused SDL device is a guarantee instead of a coincidence, and it costs one call.
    ui::InputDispatcher::RenderHooks hooks;
    hooks.suspend_audio = [&audio](bool suspend) { audio.setPaused(suspend); };
    hooks.repaint       = [&]() {
        layout.draw(canvas, state);
        video.present(canvas);
    };
    dispatch.set_render_hooks(std::move(hooks));

    // ── THE LIFECYCLE (S10) ──────────────────────────────────────────────────────────────────────
    //
    // Where a RELATIVE sample path resolves. Absolute paths (everything the browser loads) ignore it;
    // a portable project — every golden, and anything this build ships — stores its media relative, and
    // recovering one of those against the wrong folder brings the song back looking perfect and playing
    // silence. So the dispatcher is TOLD the session's media dir rather than guessing one.
    //
    // ⚠️ With no project on the command line there is no project directory to be relative TO, and an
    // empty base resolves every relative path against the process's cwd — which on a handheld is
    // whatever the launch script last cd'd to. The app root is the honest answer: it is where Samples/
    // lives, so it is where a recovered autosave's relative media actually is.
    dispatch.set_media_base_dir(hasProject ? baseDir : appRoot);

    // An autosave that survived to launch means the last session did not end cleanly — a launcher's
    // kill, a flat battery, a crash. SETTINGS → RESUME decides what happens next: ASK raises the
    // RECOVER WORK? dialog, AUTO restores in silence.
    //
    // ⚠️ AFTER load_settings (RESUME is the setting being read) and AFTER push_params (a recovery
    // re-pushes everything anyway). If there is no autosave — the common case, and the one that means
    // everything went fine last time — this does nothing at all.
    // Said out loud for the same reason the once-a-second status line below is: during a handheld
    // bring-up there is no screen yet, and "did it find my crash file?" is not a question you can answer
    // by looking at a window that is not there.
    //
    // ⚠️ ASCII, and that is this file's own rule being obeyed rather than a preference — the help banner
    // below states it: the console's encoding is not ours to choose (a handheld's serial console, an ssh
    // session, a Windows box on a legacy code page), and an em-dash arrives there as mojibake. S10 wrote
    // one into this very line and watched it come back as `вЂ”` on the first run.
    switch (dispatch.boot_recovery()) {
        using BR = ui::InputDispatcher::BootRecovery;
        case BR::NONE:     break;   // the common case, and it deserves no line of its own
        case BR::ASKED:    std::printf("autosave: FOUND - asking (SETTINGS > RESUME = ASK)\n"); break;
        case BR::RESTORED: std::printf("autosave: FOUND - restored (SETTINGS > RESUME = AUTO)\n"); break;
        case BR::DROPPED:  std::printf("autosave: FOUND but UNREADABLE - dropped\n"); break;
    }

    std::printf("\nWASD/arrows move   K/Enter = A   J/Esc = B   U/I = L/R   LShift = SELECT   SPACE = START   F10 quit\n");
    std::printf("A+UP/DOWN edit   A+LEFT/RIGHT edit fast   A+B clear   A,A insert next unused\n");
    std::printf("B+LEFT/RIGHT change WHICH phrase/chain/table   B+UP/DOWN page the song\n");
    std::printf("L+B select (tap again to widen)   B copies   L+A cut/paste   L+R deselect   L+B+A clone\n");
    // ASCII only, deliberately: this goes to a console whose encoding is not ours to choose (a
    // handheld's serial/ssh terminal, a Windows box on a legacy code page), and a stray em-dash
    // arrives there as mojibake.
    std::printf("A+UP on an FX-TYPE column opens the effect picker - release A to choose\n");
    std::printf("R+DPAD moves between screens: SONG CHAIN PHRASE INSTRUMENT TABLE MODS INST.POOL\n");
    std::printf("                             GROOVE MIXER EFFECTS PROJECT SETTINGS\n");
    std::printf("PROJECT: A on SAVE/LOAD/NEW, on EXPORT MIX/STEMS, on COMPACT SEQ/INST, on SETTINGS>, on EXIT\n");
    std::printf("         A on NAME opens the keyboard; A+UP/DOWN edits one character in place\n");
    std::printf("         a confirm asks A=YES B=NO before anything destructive\n");
    std::printf("START auditions the instrument on INSTRUMENT/POOL/MODS/TABLE - any button silences it\n");
    std::printf("SELECT on the EFFECTS TIME row toggles delay sync (free ms <-> note divisions)\n");
    std::printf("\nFILE BROWSER (A on INSTRUMENT's LOAD, or on the pool's NAME of an empty slot):\n");
    std::printf("  A opens a folder or LOADS the file   B goes back   START auditions the file\n");
    std::printf("  R+LEFT = up a directory   R+UP/DOWN = sort (name/date/size)   DPAD L/R = page\n");
    std::printf("  SELECT+A rename   SELECT+B delete   SELECT+R new folder\n");
    std::printf("  L+B select (again within 500ms = all)   B copies   L+A cut/paste   L+R cancel\n");
    std::printf("KEYBOARD: DPAD picks a key   A types   B deletes   R+UP/DOWN = ABC/123 layout\n");
    std::printf("          R+LEFT/RIGHT moves the text cursor   SELECT aborts   START applies\n");
    std::printf("\nEQ EDITOR (A on any EQ cell: INSTRUMENT/POOL/MIXER master/EFFECTS REV+DLY/SAMPLE FX):\n");
    std::printf("  DPAD UP/DOWN picks the param, LEFT/RIGHT the band   A+UP/DOWN and A+LEFT/RIGHT dial it\n");
    std::printf("  A+B resets it   B+LEFT/RIGHT changes the EQ SLOT   B or SELECT closes\n");
    std::printf("  START still auditions underneath, so you can sweep a band across a ringing note\n\n");

    bool   running    = true;
    Uint64 lastStatus = 0;

    // ⚠️ `g_terminate` is the launcher's kill, read once per frame. It is a FLAG and not an action — see
    // on_terminate_signal — so this loop condition is where a SIGTERM actually takes effect, and the
    // flush below the loop is where the work is saved, on the main thread, with a heap to do it with.
    //
    // ⚠️ One honest limit, stated rather than discovered later: a kill arriving while the app is inside
    // the SYNCHRONOUS export render is not seen until the render finishes, because the frame loop is not
    // running. The exposure is small — the autosave for everything up to that point fired 3 s after the
    // last edit, long before the user navigated to EXPORT and pressed A — but it is not zero.
    while (running && !state.shouldQuit && !g_terminate) {
        // One clock reading per frame, handed to everything that needs it. The input layer's repeat
        // is a function of time, so it takes the clock rather than reaching for it.
        const Uint64 now = SDL_GetTicks64();

        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_QUIT) {
                // The OTHER way a kill arrives: a window manager's close button, and — where SDL was
                // built with HAVE_SIGNAL_H and nobody set SDL_NO_SIGNAL_HANDLERS — SDL's own SIGINT /
                // SIGTERM translation. Both land here as an ordinary event.
                //
                // ⚠️ It is NOT the guarantee, and S10 assumed it was until it measured. On Windows SDL's
                // signal code is `#undef HAVE_SIGNAL_H`'d out entirely, and on Linux it is gated on an
                // ENVIRONMENT variable. So the shell installs its own handler (see on_terminate_signal)
                // and this arm is the belt to that handler's braces — an UNCLEAN exit either way, so the
                // flush below the loop keeps the work.
                running = false;
            } else if (e.type == SDL_KEYDOWN && e.key.keysym.sym == SDLK_F10) {
                // Dev-only quit, and still NOT part of the button model — it is the desktop escape
                // hatch, and it bypasses the dirty check on purpose (a dev killing a test run does not
                // want to be asked).
                //
                // ⚠️ Not being ASKED is not the same as CHOOSING TO DISCARD, so F10 is an UNCLEAN exit
                // and the flush below keeps the work. That is also the more useful behaviour for the
                // person pressing it: F10 out of a test session, come back, and the session is still
                // there. The one exit that throws work away is the one that says so first.
                //
                // The REAL exit is PROJECT → EXIT (S7): a handheld launcher offers no window chrome to
                // close, so the app has to be able to give the process back from inside. It asks when
                // there is unsaved work, and its YES is the app's ONE clean death — the only path that
                // deletes the autosave rather than writing one.
                running = false;
            } else {
                input.handle_event(e, now);
            }
        }
        input.tick(now);

        // The frame's clock, handed to the dispatcher once. It feeds the L+B multi-tap window — a
        // class whose behaviour is a function of time must be GIVEN the time, not go looking for it.
        dispatch.set_now(static_cast<long long>(now));

        ButtonEvent be;
        while (input.poll(be)) {
            handle_button(be, dispatch, mapper, now);
        }

        // The lookahead pump — the same call, on the same 60 Hz cadence, that PixelPerfectRenderer's
        // loop makes on Android.
        host.poll();

        const PlaybackPosition pos = host.playheads();
        state.isPlaying        = host.is_playing();
        state.playbackRow      = pos.phraseStep;
        state.playbackChainRow = pos.chainRow;
        state.playbackSongRow  = pos.songRow;
        state.trackMask        = host.track_mask();

        // Everything the UI reads back OUT of the engine: the scope's samples, the eight monitored
        // notes, the table's playing row, the SF2 preset list, the mixer's meters. AFTER the transport
        // fields above — the waveform decay is a function of isPlaying, and the table row is only
        // resolved on TABLE. `now` because the meters poll on their own 60 ms cadence, not per frame.
        feed.poll(*engine, host, state, static_cast<long long>(now));

        layout.draw(canvas, state);
        video.present(canvas);

        // A status line once a second, kept from the Phase 2 shell and kept for the same reason: on a
        // headless box, over ssh, or during a handheld bring-up where you cannot yet see or hear
        // anything, these numbers are what tell you the chain is alive. The FRAME COUNTER means the
        // audio device is calling back, the PLAYHEAD means the sequencer is advancing, and VOICES
        // means events are reaching the engine and turning into sound. Any one stuck at zero names the
        // broken link. A window on screen does not answer that question when there is no screen.
        if (now - lastStatus >= 1000) {
            lastStatus = now;
            std::printf(
                "%s  frame %-10lld  song %3d  chain %2d  step %2d   voices %2d   %-10s cursor %X,%d\n",
                host.is_playing() ? "play" : "stop",
                static_cast<long long>(engine->getCurrentFrame()), pos.songRow, pos.chainRow,
                pos.phraseStep, engine->getActiveVoiceCount(), ui::screen_label(state.currentScreen),
                state.cursorRow, state.cursorColumn);
            std::fflush(stdout);  // block-buffered to a pipe otherwise, and then it says nothing
        }
    }

    // ── Leaving ──────────────────────────────────────────────────────────────────────────────────
    //
    // ⚠️ Settings are written HERE, not on every keystroke. Holding A+UP on a hex-byte setting fires
    // an edit every 100 ms (the key-repeat interval), and one file write per repeat is an SD card
    // being hammered for a value that is still moving.
    //
    // ⚠️⚠️ …but NOT behind a dirty FLAG, and that distinction cost a real bug. This used to read
    // `if (state.settingsDirty)`, and the only thing that ever SET that flag was the SETTINGS screen's
    // edit arm — so a palette dialled in the THEME EDITOR (which mutates the theme directly, having no
    // CursorContext to route through) armed nothing and was silently thrown away on quit. The verb below
    // asks the DATA instead: it writes only when the bytes on disk differ from what memory holds, so it
    // is still one write per session at most, and there is no longer anything for the next screen that
    // touches the theme to forget. See ui/settings_store.h, and ptdispatch §27(c) — which fails if the
    // arming ever comes apart again.
    switch (ui::save_settings_if_changed(filesystem, state.settings, state.theme)) {
        using SW = ui::SettingsWrite;
        case SW::UNCHANGED: break;   // nothing moved this session; the file already says so
        case SW::SAVED:     std::printf("settings: saved\n"); break;
        // A full SD card, a read-only mount. S9's lesson, one file over: the only save in the app with
        // no result at all was a dropped error return, and it read as success.
        case SW::FAILED:
            std::printf("settings: SAVE FAILED - %s\n", filesystem.settings_path().c_str());
            break;
    }

    // ⚠️ **THE FLUSH — and every way out of the loop above arrives here, which is the design.**
    //
    // The 3 s debounce can lose the last few edits if the process is taken away before it fires, and on
    // a handheld it very often is: the CFW menu kills the port, the battery goes, the power slider is a
    // switch and not a request. So the exit path flushes synchronously, on the main thread, while there
    // is still a heap and a filesystem to do it with.
    //
    // ⚠️ It is a NO-OP when the document is clean, and that is what keeps the file's meaning intact:
    // "an autosave exists" must mean "the last session ended badly and there is work in it". A confirmed
    // PROJECT → EXIT has already DELETED the autosave (confirm_accept) and made the project clean, so
    // this writes nothing — the one exit the user was asked about is the one exit that leaves no trace.
    // Everything else — SIGTERM, SIGINT, the window's close button, F10 — never asked, so it keeps the
    // work, and the next launch says so.
    dispatch.flush_autosave();

    host.stop();
    engine->onResumeRequested = nullptr;
    audio.closeStream();
    input.close_controllers();
    video.close();
    SDL_Quit();
    return 0;
}
