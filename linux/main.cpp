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
// ─── WHAT IS STILL A STUB ────────────────────────────────────────────────────────────────────────
//
// Five screens are real (SONG, CHAIN, PHRASE, TABLE, GROOVE), R+DPAD moves between all of them, and
// since S3 the whole input layer is here: selection, the clipboard, item cycling, cloning, the note
// preview, the FX-helper overlay. The rest draw the "COMING SOON" placeholder that the Android app
// itself used while its own screens were being written, and they land session by session — the
// instrument, mixer, file and settings screens, each bringing its own arm of the dispatcher with it.

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

#include "sdl-audio-engine.h"
#include "sdl-input.h"
#include "sdl-video.h"

#include <cstdio>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>

using namespace songcore;
namespace ui = pt::ui;

namespace {

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

/** The mapper's own memory. Today just the A,A double-tap window — Kotlin's `InputMapper.lastAPress`. */
struct MapperState {
    Uint64 lastAPress = 0;
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
        // The FX helper commits on the RELEASE of A — which is what lets you hold A, read your way
        // through the effect grid, and let go on the one you want.
        if (e.button == Button::A) d.on_a_released();
        return;
    }

    // Silence a ringing audition on any "plain" press. START is exempt (it starts playback), and so is
    // anything pressed while A is held — that covers every edit combo, and an edit should stay audible.
    if (e.button != Button::START && !m.a) d.on_stop_preview();

    // ── A + … : the tracker's core editing gesture ───────────────────────────────────────────────
    if (m.a && !m.l && !m.r) {
        switch (e.button) {
            case Button::B:          d.on_a_b();     return;   // A+B — delete / reset to default
            case Button::DPAD_UP:    d.on_a_up();    return;   // +1  (or open the FX helper)
            case Button::DPAD_DOWN:  d.on_a_down();  return;   // −1
            case Button::DPAD_RIGHT: d.on_a_right(); return;   // +16 / +1 octave
            case Button::DPAD_LEFT:  d.on_a_left();  return;   // −16 / −1 octave
            default: break;
        }
    }

    // ── B + DPAD: WHICH item am I looking at? ────────────────────────────────────────────────────
    if (m.b && !m.l && !m.r && !m.a) {
        switch (e.button) {
            case Button::DPAD_LEFT:  d.on_b_left();  return;   // previous phrase/chain/table/groove
            case Button::DPAD_RIGHT: d.on_b_right(); return;   // next
            case Button::DPAD_UP:    d.on_b_up();    return;   // SONG: page up
            case Button::DPAD_DOWN:  d.on_b_down();  return;   // SONG: page down
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
        if (now - ms.lastAPress < 300) {
            ms.lastAPress = 0;   // …so a triple-tap does not read as two double-taps
            d.on_a_a();          // insert the next UNUSED chain/phrase
        } else {
            ms.lastAPress = now;
            d.on_button_a();     // insert the LAST-EDITED one
        }
        return;
    }

    if (e.button == Button::B && !m.l && !m.r && !m.a) { d.on_button_b(); return; }  // copy a selection

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
    SDL_SetMainReady();

    if (argc < 2) {
        std::fprintf(stderr, "usage: pockettracker-sdl <project.ptp> [media-base-dir]\n");
        return 2;
    }
    const std::string projectPath = argv[1];
    const std::string baseDir     = (argc > 2) ? argv[2] : dir_of(projectPath);

    std::string blob;
    if (!read_file(projectPath, blob)) {
        std::fprintf(stderr, "cannot read %s\n", projectPath.c_str());
        return 1;
    }

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
    if (!host.push_project(blob)) {
        std::fprintf(stderr, "%s did not parse as a .ptp\n", projectPath.c_str());
        SDL_Quit();
        return 1;
    }

    const MediaLoadResult media = host.load_media(baseDir);
    std::printf("project: %s\nmedia:   %d loaded, %d failed (base dir: %s)\n", projectPath.c_str(),
                media.loaded, media.failed, baseDir.c_str());
    if (media.failed > 0) {
        std::fprintf(stderr,
                     "warning: %d sample(s)/SoundFont(s) failed to load — those instruments will be "
                     "silent\n",
                     media.failed);
    }

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

    ui::Canvas        canvas;
    ui::TrackerLayout layout;
    ui::EngineFeed    feed;

    // The whole input layer, in one object. It edits `host.edit_project()` — the SAME Project the
    // Sequencer is reading — so an edit is live the instant it is made.
    ui::InputDispatcher dispatch(state, host);
    MapperState         mapper;

    std::printf("\nWASD/arrows move   K/Enter = A   J/Esc = B   U/I = L/R   LShift = SELECT   SPACE = START   F10 quit\n");
    std::printf("A+UP/DOWN edit   A+LEFT/RIGHT edit fast   A+B clear   A,A insert next unused\n");
    std::printf("B+LEFT/RIGHT change WHICH phrase/chain/table   B+UP/DOWN page the song\n");
    std::printf("L+B select (tap again to widen)   B copies   L+A cut/paste   L+R deselect   L+B+A clone\n");
    // ASCII only, deliberately: this goes to a console whose encoding is not ours to choose (a
    // handheld's serial/ssh terminal, a Windows box on a legacy code page), and a stray em-dash
    // arrives there as mojibake.
    std::printf("A+UP on an FX-TYPE column opens the effect picker - release A to choose\n");
    std::printf("R+DPAD moves between screens: SONG CHAIN PHRASE TABLE GROOVE\n\n");

    bool   running    = true;
    Uint64 lastStatus = 0;

    while (running) {
        // One clock reading per frame, handed to everything that needs it. The input layer's repeat
        // is a function of time, so it takes the clock rather than reaching for it.
        const Uint64 now = SDL_GetTicks64();

        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_QUIT) {
                running = false;
            } else if (e.type == SDL_KEYDOWN && e.key.keysym.sym == SDLK_F10) {
                // Dev-only quit, and NOT part of the button model: on Android the app never exits, and
                // on a handheld the launcher kills the port. The real EXIT lands on the PROJECT screen
                // with a SIGTERM autosave beside it (port plan §4.5 lifecycle).
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
        // notes, the table's playing row. AFTER the transport fields above — the waveform decay is a
        // function of isPlaying, and the table row is only resolved on the TABLE screen.
        feed.poll(*engine, state);

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

    host.stop();
    engine->onResumeRequested = nullptr;
    audio.closeStream();
    input.close_controllers();
    video.close();
    SDL_Quit();
    return 0;
}
