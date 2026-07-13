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
// Five screens are real (SONG, CHAIN, PHRASE, TABLE, GROOVE) and R+DPAD moves between all of them.
// The rest draw the "COMING SOON" placeholder that the Android app itself used while its screens were
// being written, and they land session by session: the full input dispatcher (selection, clipboard,
// note preview, the FX helper) next, then the instrument, mixer, file and settings screens.

// <cmath> before <SDL.h> — see the note in sdl-audio-engine.h (M_PI, _USE_MATH_DEFINES, C4005).
#include <cmath>

#define SDL_MAIN_HANDLED
#include <SDL.h>

#include "audio-engine.h"
#include "songcore/host.h"
#include "ui/app_state.h"
#include "ui/canvas.h"
#include "ui/cursor.h"
#include "ui/cursor_move.h"
#include "ui/engine_feed.h"
#include "ui/layout.h"
#include "ui/navigation.h"
#include "ui/modules/chain_editor.h"
#include "ui/modules/groove_editor.h"
#include "ui/modules/phrase_editor.h"
#include "ui/modules/song_editor.h"
#include "ui/modules/table_editor.h"

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

/** Every grid editor the shell can currently show. One instance each, exactly as TrackerLayout holds. */
struct Editors {
    ui::SongEditorModule   song;
    ui::ChainEditorModule  chain;
    ui::PhraseEditorModule phrase;
    ui::TableModule        table;
    ui::GrooveModule       groove;
};

/**
 * "What is under the cursor, and what can be done to it" — for whichever screen is up.
 *
 * The ONE place the shell asks which screen it is on. Everything downstream (the five button handlers,
 * the value stepping, the wrap-vs-clamp rules) is written against the CursorContext this returns and
 * never learns the answer. That is the whole point of the cursor-context system, and it is why adding
 * a screen below is four lines rather than a new branch in every handler.
 */
ui::CursorContext cursor_context_for(const ui::AppState& s, const Project& p, const Editors& ed) {
    switch (s.currentScreen) {
        case ui::ScreenType::SONG: {
            ui::SongEditorState ss{p};
            ss.cursorRow   = s.cursorRow;
            ss.cursorTrack = s.cursorColumn;  // on SONG the column IS the track
            return ed.song.cursor_context(ss);
        }
        case ui::ScreenType::CHAIN: {
            ui::ChainEditorState cs{p.chains[static_cast<size_t>(s.currentChain)]};
            cs.cursorRow    = s.cursorRow;
            cs.cursorColumn = s.cursorColumn;
            return ed.chain.cursor_context(cs);
        }
        case ui::ScreenType::PHRASE: {
            ui::PhraseEditorState ps{p.phrases[static_cast<size_t>(s.currentPhrase)]};
            ps.cursorRow    = s.cursorRow;
            ps.cursorColumn = s.cursorColumn;
            return ed.phrase.cursor_context(ps);
        }
        case ui::ScreenType::TABLE: {
            ui::TableState ts{p.tables[static_cast<size_t>(s.currentTable)]};
            ts.cursorRow    = s.tableCursorRow;
            ts.cursorColumn = s.tableCursorColumn;
            return ed.table.cursor_context(ts);
        }
        case ui::ScreenType::GROOVE: {
            ui::GrooveState gs{p.grooves[static_cast<size_t>(s.currentGroove)]};
            gs.cursorRow    = s.grooveCursorRow;
            gs.cursorColumn = 1;
            return ed.groove.cursor_context(gs);
        }
        default:
            return ui::cc::none();  // a placeholder screen has nothing to edit
    }
}

/** Apply an action to the live document. Returns true if anything changed. */
bool apply_edit(const ui::AppState& s, SongcoreHost& host, const Editors& ed,
                const ui::InputAction& action) {
    Project& p = host.edit_project();  // the SAME Project the Sequencer is reading

    switch (s.currentScreen) {
        case ui::ScreenType::SONG:
            return ed.song.handle_input(p, s.cursorRow, s.cursorColumn, action).modified;

        case ui::ScreenType::CHAIN:
            return ed.chain
                .handle_input(p.chains[static_cast<size_t>(s.currentChain)], s.cursorRow,
                              s.cursorColumn, action)
                .modified;

        case ui::ScreenType::PHRASE:
            return ed.phrase
                .handle_input(p.phrases[static_cast<size_t>(s.currentPhrase)], s.cursorRow,
                              s.cursorColumn, action)
                .modified;

        case ui::ScreenType::TABLE: {
            const bool modified =
                ed.table
                    .handle_input(p.tables[static_cast<size_t>(s.currentTable)], s.tableCursorRow,
                                  s.tableCursorColumn, action)
                    .modified;
            // ⚠️ The consumer caches which tables it has already pushed to the engine. An in-place
            // edit is invisible to that cache — push_project invalidates it, an edit cannot.
            if (modified) host.invalidate_tables();
            return modified;
        }

        case ui::ScreenType::GROOVE:
            return ed.groove
                .handle_input(p.grooves[static_cast<size_t>(s.currentGroove)], s.grooveCursorRow,
                              /*cursor_column=*/1, action)
                .modified;

        default:
            return false;
    }
}

/**
 * The editing half of the input loop.
 *
 * A deliberately SMALL stand-in for `AppInputDispatcher` (~3200 lines of Kotlin), covering exactly the
 * slice these two sessions set out to prove: reach every screen, move the cursor, edit the cell under
 * it, hear the result. What matters is that it is written THROUGH the real architecture rather than
 * around it — `cursor_context()` answers what the cursor is on, and the generic `on_a` / `on_b` /
 * `on_a_left` / `on_a_right` / `on_a_b` handlers turn a button into an `InputAction` without ever
 * asking which screen is up. The dispatcher session replaces this function and keeps everything under
 * it.
 *
 * Not here yet, and each belongs to that session: the selection/clipboard combos (L+B, L+A), the
 * item-cycling B+DPAD (which is how you change WHICH phrase/chain/table you are looking at), the A,A
 * double-tap, the note preview an edit should play, and the FX-helper overlay that intercepts A+UP on
 * an FX-type column.
 */
void handle_button(const ButtonEvent& e, ui::AppState& state, SongcoreHost& host, const Editors& ed,
                   const SdlInput& input) {
    if (e.action != ButtonAction::PRESSED) return;

    const bool aHeld = input.is_held(Button::A);
    const bool bHeld = input.is_held(Button::B);
    const bool rHeld = input.is_held(Button::R_SHIFT);

    // ── R + DPAD: move between screens ───────────────────────────────────────────────────────────
    // Checked FIRST: R is a screen-navigation modifier, and while it is down a DPAD press must not
    // also move the cursor underneath.
    if (rHeld) {
        const ui::NavState ns = ui::nav_state_of(state);
        switch (e.button) {
            case Button::DPAD_UP:    ui::go_to_screen(state, ui::navigate_up(ns));    break;
            case Button::DPAD_DOWN:  ui::go_to_screen(state, ui::navigate_down(ns));  break;
            case Button::DPAD_LEFT:  ui::go_to_screen(state, ui::navigate_left(ns));  break;
            case Button::DPAD_RIGHT: ui::go_to_screen(state, ui::navigate_right(ns)); break;
            default: break;
        }
        return;
    }

    const ui::CursorContext ctx = cursor_context_for(state, host.edit_project(), ed);

    ui::InputAction action = ui::InputAction::none();

    // ── A + DPAD: the tracker's core editing gesture ─────────────────────────────────────────────
    // A alone steps the value by one; A+LEFT/RIGHT by the large step (16 for a hex byte, an octave
    // for a note). The same DPAD press therefore means two different things depending on a modifier,
    // which is exactly why the input mapper reports HELD STATE rather than pre-baked combos.
    if (aHeld) {
        switch (e.button) {
            case Button::DPAD_UP:    action = ui::on_a(ctx);       break;
            case Button::DPAD_DOWN:  action = ui::on_b(ctx);       break;
            case Button::DPAD_LEFT:  action = ui::on_a_left(ctx);  break;
            case Button::DPAD_RIGHT: action = ui::on_a_right(ctx); break;
            case Button::B:          action = ui::on_a_b(ctx);     break;  // A+B: delete / reset
            default: break;
        }
    } else if (bHeld && e.button == Button::A) {
        action = ui::on_a_b(ctx);  // the same combo, pressed in the other order
    } else {
        // ── DPAD alone: move the cursor ──────────────────────────────────────────────────────────
        switch (e.button) {
            case Button::DPAD_UP:    ui::move_cursor_up(state);    break;
            case Button::DPAD_DOWN:  ui::move_cursor_down(state);  break;
            case Button::DPAD_LEFT:  ui::move_cursor_left(state);  break;
            case Button::DPAD_RIGHT: ui::move_cursor_right(state); break;
            case Button::START:
                // Kotlin's togglePlayback(): what START plays depends on the screen you are ON — the
                // phrase you are editing, the chain you are editing, or the whole song from row 0
                // (`playSong(project)` takes the startRow=0 default; it does NOT start at the cursor).
                // Every other screen falls back to the phrase, so START always makes a sound.
                if (host.is_playing()) {
                    host.stop();
                } else {
                    switch (state.currentScreen) {
                        case ui::ScreenType::SONG:  host.play_song(0); break;
                        case ui::ScreenType::CHAIN: host.play_chain(state.currentChain); break;
                        default:                    host.play_phrase(state.currentPhrase); break;
                    }
                }
                break;
            default:
                break;
        }
    }

    if (action.type == ui::ActionType::NONE) return;

    // An edit made WHILE PLAYING has to reach the lookahead that has already been scheduled past it —
    // otherwise it is not heard until the buffer happens to roll over it. Kotlin calls exactly this,
    // from exactly here.
    if (apply_edit(state, host, ed, action) && host.is_playing()) host.notify_data_changed();
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
    Editors           editors;

    std::printf("\nWASD/arrows move   K/Enter = A   J/Esc = B   SPACE = START (play/stop)   F10 quit\n");
    std::printf("A+UP/DOWN edit the cell   A+LEFT/RIGHT edit fast   A+B clear\n");
    std::printf("I(R)+DPAD moves between screens: SONG CHAIN PHRASE TABLE GROOVE\n\n");

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

        ButtonEvent be;
        while (input.poll(be)) {
            handle_button(be, state, host, editors, input);
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
