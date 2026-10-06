#pragma once

// ─── THE INPUT DISPATCHER ────────────────────────────────────────────────────────────────────────
//
// Every button, every combo, one place — the only input path for every screen. `cursor_context()`
// answers what the cursor is on, and the generic `on_a` / `on_b` / `on_a_left` / `on_a_right` /
// `on_a_b` turn a button into an `InputAction` without asking which screen is up (ui/cursor.h); what
// lives here is everything else — selection, clipboard, item cycling, cloning, the FX helper, previews.
//
// ── ⚠️ THE MODAL RULE ────────────────────────────────────────────────────────────────────────────
//
// Whatever sits over the screen — a load, the confirm dialogs, the RENDER dialog, the QWERTY keyboard,
// the THEME and EQ editors (partial), the FX and map pickers — OWNS THE BUTTONS while up, and the order
// a press is offered to them is the specification: the keyboard can sit on top of the theme editor
// (its SAVE), and a D-pad press there must move the KEY cursor.
// ⭐ That order is written once, in `LAYERS` (ui/dispatch/route.cpp). Every gesture asks the top layer
// first (`route`), and a gesture a layer does not answer is swallowed there — so a gesture's body runs
// only with no layer up, or under one that deliberately lets it through. Then the screen's handler in
// `SCREENS` is asked; its own buttons live in ui/dispatch/screens/.
// ⚠️ The THEME and EQ editors are PARTIAL: they let START and SELECT through, so a band can be swept
// across a held INSTRUMENT audition. The confirm and the FX picker let the audition's stop through.
//
// ── Input layers ─────────────────────────────────────────────────────────────────────────────────
//
//   • `shell/sdl/sdl-input.h`  — keycodes, controller buttons, axes, key repeat. SDL's.
//   • `ui/button_mapper.h` — the COMBO MATRIX: which named handler a press means. Portable.
//   • here                 — what each named handler DOES.
// No `handle(ButtonEvent)` here: the matrix is a free function so a tool can drive it with a stub,
// and named handlers let the tests drive them one at a time.
//
// ⚠️ The clock is injected (`set_now()` once a frame), so time-dependent behaviour can be tested.

#include "songcore/host.h"
#include "songcore/midi_map.h"
#include "ui/app_state.h"
#include "ui/clipboard.h"
#include "ui/cursor.h"
#include "ui/dispatch/gesture.h"
#include "ui/song_pointer.h"     // NAV = SONG — the ctor clamps the pointer onto a real cell
#include "ui/filesystem.h"
#include "ui/modules/chain_editor.h"
#include "ui/modules/effects_editor.h"
#include "ui/modules/eq_editor.h"
#include "ui/modules/file_browser.h"
#include "ui/modules/groove_editor.h"
#include "ui/modules/scale_editor.h"
#include "ui/modules/instrument_editor.h"
#include "ui/modules/instrument_pool.h"
#include "ui/modules/midi_map_editor.h"
#include "ui/modules/midi_settings.h"
#include "ui/modules/mixer.h"
#include "ui/modules/modulation.h"
#include "ui/modules/phrase_editor.h"
#include "ui/modules/project_editor.h"
#include "ui/modules/qwerty_keyboard.h"
#include "ui/modules/sample_editor.h"
#include "ui/modules/settings_editor.h"
#include "ui/modules/song_editor.h"
#include "ui/modules/table_editor.h"
#include "ui/project_actions.h"

#include <functional>

#include <string>
#include <utility>
#include <vector>

namespace pt::ui {

/** The cursor of a screen that takes a selection: where it is, and how far a selection reaches. */
struct GridCursor {
    int AppState::*row;
    int AppState::*column;
    int maxRow;     // 255 on SONG: a selection spans the arrangement, not the 16 rows shown
    int maxColumn;  // the rightmost selectable column — a ROW-scope selection ends there
};

class InputDispatcher {
  public:
    /**
     * `fs` is a REFERENCE with no null path (unlike the engine): a browser with no filesystem is an
     * empty box. A test points it at a temp directory.
     */
    InputDispatcher(AppState& state, songcore::SongcoreHost& host, FileSystem& fs)
        : s_(state), host_(host), fs_(fs) {
        // The layout's clipboard readout reaches the clipboard through AppState.
        s_.clipboard = &clip_;

        // …and clamp the song-relative pointer for the project the shell pushed before this existed.
        // A no-op under NAV = POOL or with no project (ui/song_pointer.h).
        clamp_song_pointer(s_);
    }

    /**
     * The frame's clock — call once per frame, before the events. It feeds the L+B multi-tap window and
     * runs deferred work that is now due:
     *   • the sample editor's audition restore — the preview's selection window must be put back once
     *     the voice has triggered, 100 frames after scheduling;
     *   • the crash-recovery AUTOSAVE's 3 s debounce (`mark_modified`);
     *   • the status lines' auto-dismiss.
     * Injected so a test can move time.
     */
    void set_now(long long now_ms);

    /**
     * Is there timed work outstanding that will change the SCREEN on its own? The shell skips drawing
     * idle frames, and nothing else would redraw when a status message auto-dismisses
     * (`run_due_status_dismiss`).
     * ⚠️⚠️ It goes false ON the frame the work completes — the frame that must be drawn. The caller
     * owes it an active→idle EDGE (the shell's `timedWorkEdge`), or "PROJECT SAVED" stays on a still
     * screen.
     * Derived from the deadlines themselves; a new screen-changing timer belongs in this expression.
     * `autosavePending_` is included: it ends in a status message.
     */
    bool has_pending_timed_work() const {
        return statusDismissAtMs_ != 0 || browserStatusDismissAtMs_ != 0 || autosavePending_;
    }

    // ═════════════════════════════════════════════════════════════════════════════════════════════
    // THE LIFECYCLE — the three things the SHELL has to say
    // ═════════════════════════════════════════════════════════════════════════════════════════════
    //
    // The rest of the autosave is internal: `mark_modified` arms it, `set_now` fires it, SAVE / LOAD /
    // NEW / EXIT clear it. Only the shell knows where the media is, when the app started, and when it
    // is being taken away.

    /**
     * Where a project's RELATIVE sample paths resolve; call once at start-up with what the shell gave
     * `load_media`. A portable project stores relative paths, and recovering one against the wrong
     * folder plays silence while looking correct.
     */
    void set_media_base_dir(std::string dir) { mediaBaseDir_ = std::move(dir); }

    /** What `boot_recovery()` actually did. Four outcomes, and each one means exactly one thing. */
    enum class BootRecovery {
        NONE,      // no autosave — the last session ended cleanly
        ASKED,     // RESUME=ASK: the RECOVER WORK? dialog is up, and nothing is decided yet.
        RESTORED,  // RESUME=AUTO: the document is back, and DIRTY.
        DROPPED,   // it would not parse. The file is gone, so it cannot be offered again.
    };

    /**
     * START-UP: an autosave that survived to launch means an unclean exit. SETTINGS → RESUME decides:
     * ASK raises RECOVER WORK?, AUTO restores silently. Call AFTER loading settings.json and the
     * shell's own `push_params()`.
     * An enum, not a bool: a bool reported a DROPPED corrupt autosave as restored.
     */
    BootRecovery boot_recovery();

    /**
     * THE KILL: flush the autosave NOW, synchronously, if there is unsaved work — the caller is leaving.
     * ⚠️ From the frame loop's exit path, NEVER a signal handler: it allocates and writes a file, none of
     * it async-signal-safe — a SIGTERM inside `malloc` would deadlock and lose the save it exists for.
     * The shell's handler sets a `volatile sig_atomic_t`; the loop leaves and calls this. An ordinary
     * method, so a test drives it directly.
     */
    void flush_autosave();

    // ── D-pad alone: move the cursor (or drag a selection's edge) ────────────────────────────────
    void on_dpad_up();
    void on_dpad_down();
    void on_dpad_left();
    void on_dpad_right();

    // ── A + D-pad: edit the cell under the cursor ────────────────────────────────────────────────
    // A+LEFT/RIGHT step by one, A+UP/DOWN by the large step; on an FX-TYPE column A+UP/DOWN open the
    // FX helper.
    void on_a_up();
    void on_a_down();
    void on_a_left();
    void on_a_right();

    /** A+B: delete the cell, or reset it to its default. Over a selection: delete the range. */
    void on_a_b();

    /** A,A (a double-tap on the same cell): insert the next unused chain/phrase. */
    void on_a_a();

    /** Release of A: commits the FX helper's highlighted effect. */
    void on_a_released();

    /**
     * A went down on a cell the mapper DEFERS; nothing fires here but the instant is recorded: the
     * sample editor's row-11 chop tap is aimed by ear at the playhead, and the release comes a
     * reaction time later.
     */
    void on_a_deferred();

    // ── B + D-pad: which item am I looking at? ───────────────────────────────────────────────────
    // B+LEFT/RIGHT cycle the current phrase / chain / table / groove; B+UP/DOWN page SONG.
    void on_b_left();
    void on_b_right();
    void on_b_up();
    void on_b_down();

    // ── R + D-pad: move between screens ──────────────────────────────────────────────────────────
    void on_r_up();
    void on_r_down();
    void on_r_left();
    void on_r_right();

    // ── R + A/B: MUTE and SOLO (SONG and MIXER) ──────────────────────────────────────────────────
    // Acts on the press; undone if its own button comes up before R (the mapper's `rComboArmed`
    // calls one of the two closers below). The revert snapshot is taken on a chord's first toggle.
    /** R+B: toggle MUTE on the cursor's channel, or on every channel in the selection. */
    void on_r_b();
    /** R+A: toggle SOLO, same targeting. */
    void on_r_a();
    /** R went down or up — the arming half of MIDI learn ("hold R, turn a knob"). The only handler
     *  told about a button's state: the other half is a CC, never a button event. */
    void on_r_held(bool down);

    /** R came up first: the chord's changes stand. Drops the snapshot. */
    void on_r_combo_commit();
    /** A/B came up first: put every track's mute and solo back the way the chord found them. */
    void on_r_combo_revert();

    // ── L: selection and the clipboard ───────────────────────────────────────────────────────────
    /** L+B: enter selection, then widen it CELL → ROW → SCREEN on each tap inside 500 ms. */
    void on_l_b();
    /** L+A: cut (inside a selection) or paste (outside one). */
    void on_l_a();
    /** L+R: leave selection mode. */
    void on_l_r();
    /** L+B+A: deep-clone the chain/phrase under the cursor into free slots. */
    void on_l_b_a();

    // ── SELECT + … : the file browser's verbs ────────────────────────────────────────────────────
    // Browser-only chords, each opening the keyboard or a confirm.
    /** SELECT+A: rename the file/folder under the cursor (opens the keyboard). */
    void on_select_a();
    /** SELECT+B: delete it — arms the "A=YES B=NO" confirm, never deletes on the press itself. */
    void on_select_b();
    /** SELECT+R: create a folder here (opens the keyboard). */
    void on_select_r();

    // ── The plain buttons ────────────────────────────────────────────────────────────────────────
    /** B inside a selection COPIES it and exits — the tracker's copy gesture. */
    void on_button_b();
    void on_button_a();
    /**
     * Bare SELECT raises help (the compact panel or full overlay, per SETTINGS > HELP) and aborts the
     * keyboard. ⚠️ Called on the RELEASE, only when nothing else was pressed meanwhile
     * (ui/button_mapper.h).
     */
    void on_select();

    /**
     * A button went down: put help away — the compact panel on any press but SELECT, the full overlay
     * on any press. Called by the mapper, which decides whether the press is also consumed.
     */
    void on_help_dismiss();

    /** Is the full help overlay up? Asked by the mapper before it routes a press anywhere. */
    bool help_full_open() const { return s_.helpFull; }
    /** The mapper closes the overlay on any press and consumes it, so nothing reaches this. */
    GestureResult help_layer(Gesture) { return GestureResult::TAKEN; }
    /** START: play/stop. What it plays depends on the screen you are on. */
    void on_start();

    /**
     * LIVE mode's chords, SONG only: L+START queues the cursor ROW as one scene, R+START queues the
     * cursor's channel to fall silent. Elsewhere they are reserved and do nothing.
     */
    void on_l_start();
    void on_r_start();

  private:
    /**
     * The one gate every LIVE gesture asks: LIVE mode, on SONG, nothing on top. ⚠️ The mapper's
     * L+START / R+START arms are global — ungated, they would queue from inside the sample editor.
     */
    bool live_song_gesture() const {
        return host_.live_mode() && s_.currentScreen == ScreenType::SONG &&
               !top_layer();
    }

    /** Bit N set where track N has a chain on `songRow` — the channels a row launch starts sounding. */
    int  live_row_mask(int songRow) const;

    /** Has this row already been queued? A second L+START then promotes it to the next phrase
     *  boundary. Read back off the slots, not remembered. */
    bool live_row_armed(int songRow) const;

  public:

    /** "Press any button to silence the audition." Called by the mapper on every plain press; whether
     *  this screen has a preview is decided here. Previews have their own voice — song playback is
     *  untouched. */
    void on_stop_preview();

    /**
     * ⚠️ Asked by the MAPPER on every plain A: is the cursor on a cell whose A OPENS something while its
     * A+DPAD / A+B means something else? Then the press waits for A's release and any A-combo cancels
     * it — otherwise A+B to reset such a cell would open the sub-screen first.
     */
    bool defer_a_to_release() const;

    /**
     * ⚠️ Asked by the MAPPER on every plain B: should B wait for its release? True in the EQ editor (B
     * both closes it and modifies the slot cycle) and wherever R+B is a mute (a B landing a frame ahead
     * of its R must still be claimable as the chord, not copy the selection).
     */
    bool defer_b_to_release() const;

    /** The clipboard, for the top-strip readout ("PHR:2x3"). */
    const Clipboard& clipboard() const { return clip_; }

    // ── Opening the sub-screens (the shell uses these at start-up too) ───────────────────────────

    /** Show the browser, filtered, in `directory`, remembering what A will do with the pick.
     *  `previousScreen` (where B returns) is captured here. */
    void open_file_browser(AppState::BrowserPurpose purpose, const std::string& directory,
                           const std::vector<std::string>& extensions);

    /** The load-browse categories a config.json override can redirect. */
    enum class BrowserDir { SAMPLES, SOUNDFONTS, INSTRUMENTS, PROJECTS, THEMES };

    /**
     * The directory a LOAD browser STARTS in for `cat`: the config.json override if it resolves to a
     * real directory, else the built-in default (root-relative unless absolute — not necessarily the
     * file's text). ⚠️ The single resolution point: `open_file_browser` compares against
     * `browser_dir(SAMPLES)`, so an override still counts as a sample load for remember-last-folder.
     */
    std::string browser_dir(BrowserDir cat);

    /**
     * How many of songcore::EFFECT_TYPES an FX cell can reach in this build (the MIDI tail may be
     * hidden, ui/platform_caps.h). ⚠️ Written once because the cell's CursorContext bound and the
     * picker's size must agree, or the picker meets an effect it cannot name.
     */
    int visible_effect_type_count() const;

    /**
     * What a render needs that only the SHELL can do. ⚠️ The render is SYNCHRONOUS — the frame loop
     * stops and renders, which also guarantees the audio callback is not reading engine state:
     *   `suspend_audio(true/false)` — pause the audio device for the duration;
     *   `repaint()` — draw the progress (the "43%" on EXPORT).
     * Both may be empty: the tests render a real WAV with neither.
     */
    struct RenderHooks {
        std::function<void(bool)> suspend_audio;
        std::function<void()>     repaint;

        /**
         * A LOAD's hook (`begin_load`): "drain what the platform has queued; did the user cancel?"
         * ⚠️⚠️ It does NOT dispatch what it drains: every press but a cancel is CONSUMED — otherwise SDL
         * would replay them all onto whatever screen the load returns to. (EXPORT still replays.)
         * ⚠️ It is also how `SDL_APP_WILLENTERBACKGROUND` (the autosave flush) is seen during a load.
         */
        std::function<bool()> load_pump;
    };
    void set_render_hooks(RenderHooks hooks) { render_ = std::move(hooks); }

    // ═════════════════════════════════════════════════════════════════════════════════════════════
    // A SLOW LOAD
    // ═════════════════════════════════════════════════════════════════════════════════════════════
    //
    // Most loads finish within a frame or two; a compressed `.sf3` or a long mp3 may not, and then the
    // loop is inside the load.
    // ⚠️ The app never predicts a file's size: `begin_load` starts a clock and the strip shows only once
    // the load outlasts `LOADING_DELAY_MS` — so a slow device shows it exactly when it is slow.

    /** The wait before a running load is drawn. Below it a strip would flash and be worse than none. */
    static constexpr int LOADING_DELAY_MS = 400;

    /** How long a status message stays up before clearing itself (`run_due_status_dismiss`). Long
     *  enough to read, short enough not to look stuck two actions later. Public for the checks. */
    static constexpr long long STATUS_DISMISS_MS = 3000;

    /**
     * How often the strip repaints and the buttons are read during a load. ⚠️ Not per report: a
     * soundfont reports per sample header (1628 times for a 43 MB `.sf3`), and a full redraw each time
     * made the load slower than the decoding.
     */
    static constexpr int LOADING_REPAINT_MS = 33;

    /** Open a load; `detail` is what the strip names. ⚠️ Every load path is wrapped in `LoadScope`
     *  (ui/dispatch/dispatch_common.h), never these calls by hand. */
    void begin_load(long long now_ms, std::string detail);

    /**
     * The report from inside the load (via the sink the shell installs into `pt::set_load_tick`).
     * Returns false when the user cancelled — the engine unwinds on it.
     * ⚠️ Time is passed in, NOT via `set_now()`, which RUNS DUE WORK: the autosave firing mid-load would
     * write a half-loaded document.
     */
    bool load_tick(long long now_ms, float fraction);

    /** Close it. Leaves `loading` clear whether the load finished, failed or was cancelled. */
    void end_load();

    /** Is a load in flight? The shell asks before it does anything that assumes a settled document. */
    bool load_running() const { return s_.loading.running; }

    /** A load answers no button: the shell's pump reads the cancel itself (`load_tick`). */
    GestureResult loading_layer(Gesture) { return GestureResult::TAKEN; }

    /**
     * Open the MIDI port the settings name, once at boot (after `AppState::midiOut` and settings.json).
     * ⚠️ The same two calls the MIDI screen makes, so there is one answer to "which port is open and
     * why". A port already open is left alone — the `POCKETTRACKER_MIDI_OUT` dev override, which the
     * shell has already written into `settings.midiOutDevice`.
     */
    void boot_midi_port();

    /**
     * The same for the INPUT port: resolve `settings.midiInDevice`, wire the host's queue as the sink,
     * open it. Separate from `boot_midi_port` because a build can have one backend and not the other.
     * ⚠️ Sink and open are paired here: an open port with no sink drops every byte, and a sink outliving
     * `set_sink(nullptr)` is a backend thread writing into a dead object.
     */
    void boot_midi_in_port();

    /**
     * The app is back in front: re-list the file browser if it is on screen (keeping and clamping the
     * cursor). Other apps — and Android's folder picker, an activity of its own — change directories
     * while we are in the background. SDL sends `SDL_APP_DIDENTERFOREGROUND` only on Android and iOS,
     * so this is inert on desktop with no `#ifdef`.
     */
    void refresh_browser_on_foreground();

  private:
    AppState&               s_;
    songcore::SongcoreHost& host_;
    FileSystem&             fs_;
    long long               now_ms_ = 0;
    // ── TAP TEMPO (PROJECT > TEMPO, plain A) ─────────────────────────────────────────────────────
    /** How many gaps between taps are averaged. Four taps in, the number has settled. */
    static constexpr int       TAP_TEMPO_KEEP       = 4;
    /** A longer silence starts a new count. 3 s is one beat at 20 BPM, the slowest tempo allowed. */
    static constexpr long long TAP_TEMPO_TIMEOUT_MS = 3000;
    /** Shorter is a bouncing button, not a tap: 60 ms is 1000 BPM, past the ceiling. */
    static constexpr long long TAP_TEMPO_MIN_MS     = 60;
    long long tapTempoLastMs_ = 0;                    // 0 = no tap yet this count
    long long tapTempoGaps_[TAP_TEMPO_KEEP] = {0};    // ring of the most recent gaps, ms
    int       tapTempoCount_ = 0;                     // how many are filled (capped at KEEP)

    /** When the load in flight opened — what `LOADING_DELAY_MS` is measured from. */
    long long               loadStartMs_ = 0;
    /** …and when it was last drawn, which is what `LOADING_REPAINT_MS` paces. */
    long long               lastLoadPaintMs_ = 0;
    RenderHooks             render_{};

    /**
     * Call at the top of anything that holds the UI thread long enough to be heard — a load, a SAMPLE
     * EDITOR operation. The sequencer is only refilled by the frame loop's poll, so a song playing
     * meanwhile runs on what is already scheduled; this fills it deeper than the poll does first. A
     * load also refills at its progress reports (`load_tick`). A missed site shows up as a `STALL` line
     * in the latency probe.
     * ⚠️ The depth outlives the operation: an edit made just after it is heard only past what was
     * filled here. That is why a save does not use it.
     */
    void before_long_operation() { host_.keep_walking(songcore::Sequencer::LONG_OPERATION_MS); }

    /** A save fits inside the normal horizon, so it only tops it up — autosaves come every few seconds
     *  of editing, and a deep fill would leave the next edit late. */
    void before_save() { host_.keep_walking(); }

    /** See set_media_base_dir. Empty means "relative paths stay relative" (resolve_media_path). */
    std::string mediaBaseDir_{};

    // ── Hold acceleration lives in the SHELL ─────────────────────────────────────────────────────
    //
    // ⚠️ A handler here is one step, always. Holding a direction arrives as more presses, closer together
    // (`shell/sdl/sdl-input.h`), never a bigger step — a 2- or 4-row jump is the stutter the ramp avoids.

    // ── The autosave's debounce ──────────────────────────────────────────────────────────────────
    //
    // ⚠️ RE-ARMED on every edit, so the write lands 3 s after the LAST change: a held A+UP edits every
    // 100 ms, and arm-once would write ~440 KB to an SD card several times a second.
    bool      autosavePending_ = false;
    long long autosaveDueAtMs_ = 0;

    /** 3 s after the last edit. */
    static constexpr long long AUTOSAVE_DEBOUNCE_MS = 3000;

    /** The deadline, checked once a frame by set_now(). */
    void run_due_autosave();

    // ── The status line's auto-dismiss ───────────────────────────────────────────────────────────
    //
    // A message clears STATUS_DISMISS_MS after it is SET; re-setting an identical one does not restart
    // it. There are many plain assignments to the field, so set_now WATCHES it for changes rather than
    // trusting a stamp at each site. Detection lands one frame after the set (the tests encode it).
    std::string statusLastSeen_{};
    long long   statusDismissAtMs_ = 0;

    // The FILE BROWSER's line, on the same window. ⚠️ A SEPARATE pair: one pair watching two fields
    // thrashes between them, re-arms every frame, and neither ever expires.
    std::string browserStatusLastSeen_{};
    long long   browserStatusDismissAtMs_ = 0;

    /** The watchers and the deadlines, all run once a frame by set_now(). */
    void run_due_status_dismiss();

    // ── The INSTRUMENT-entry param push ──────────────────────────────────────────────────────────
    //
    // Entering INSTRUMENT pushes its playback params. Screens change by many routes, so set_now watches
    // `currentScreen` and pushes the frame after INSTRUMENT is entered, by any route.
    ScreenType lastScreenSeen_ = ScreenType::SONG;   // the boot screen — see app_state.h

    /** The watcher, run once a frame by set_now(). */
    void run_instrument_entry_push();

    /**
     * A mapped knob wrote the song (from the MIDI drain); notice it once a frame, mark the song modified
     * and re-arm the autosave. A COUNT WATCHED, not a callback — the drain cannot reach the dirty flag,
     * and a whole sweep collapses into one bump per frame.
     */
    void     run_mapped_cc_dirty();
    uint64_t mappedCcSeen_ = 0;

    /** A knob turned while `R` was held: point the cell under the cursor at it. A sweep is ONE learn —
     *  the last controller seen wins, and `learn_mapping` re-points rather than appends. */
    void     run_midi_learn();
    uint64_t learnSeen_ = 0;

    /** What the cell under the cursor is CALLED (beside `cursor_context()`). `NONE` on screens with
     *  nothing a knob can sweep. */
    songcore::MapTarget map_target() const;

    /**
     * Load the autosave into the live document — and LEAVE IT DIRTY. Recovered work is not saved work:
     * the only copy is a crash file the user never named, and a clean flag would say it is safe.
     * ⚠️ And the autosave is NOT cleared: it is still the only copy.
     */
    bool recover_from_autosave();

    Clipboard clip_{};

    // ── The MUTE/SOLO chord's undo ───────────────────────────────────────────────────────────────
    // All TEN channels (8 tracks + 2 returns) as the chord found them — a chord over a selection
    // touches several, and SOLO changes every other channel. ⚠️ Taken on a chord's FIRST toggle, or
    // muting three channels in turn could only undo the last.
    static constexpr int MIX_CHANNELS = 10;
    struct MixSnapshot {
        bool live = false;
        bool mute[MIX_CHANNELS] = {};
        bool solo[MIX_CHANNELS] = {};
    };
    MixSnapshot mixSnapshot_{};

    /** The channels a MUTE/SOLO chord applies to — songcore mixer channel ids (0-7 tracks, 8 REV,
     *  9 DEL): the selection's columns on SONG, else whatever the cursor is on. */
    void mute_solo_targets(int (&out)[8], int& count) const;
    /** Toggle `mute` (or `solo`) on those channels, arm the snapshot, and push the result. */
    void toggle_mute_solo(bool solo);
    /** Every track audible again — L+R's mute rung, and the whole of "restore full playback". */
    void restore_full_playback();
    /** Whether R+A/R+B mean MUTE/SOLO on the screen as it stands — and so whether B must be held. */
    bool mute_solo_chord_live() const;
    /**
     * L+R's recency flag, DERIVED: a selection changes by many paths (L+B, D-pad drag, copy, cut,
     * paste…), so the watcher folds the observable state into one number and notices it move. Run
     * once a frame from `set_now()` AND at the top of `toggle_mute_solo()`, so the order follows the
     * PRESS even when both land in one frame.
     */
    unsigned selection_signature() const;
    void     run_selection_recency();
    unsigned selectionSig_ = 0;

    SongEditorModule       song_{};
    ChainEditorModule      chain_{};
    PhraseEditorModule     phrase_{};
    TableModule            table_{};
    GrooveModule           groove_{};
    ScaleModule            scale_{};
    InstrumentEditorModule instrument_{};
    InstrumentPoolModule   pool_{};
    ModulationModule       mods_{};
    MixerModule            mixer_{};
    EffectModule           effects_{};
    ProjectModule          project_{};
    SettingsModule         settings_{};
    MidiModule             midi_{};
    MidiMapModule          midiMap_{};
    EqModule               eq_{};   // stateful: caches its response curve (eq_editor.h)

    /** A,A only counts if the cursor did not move between the presses: where the first A landed. */
    bool         hasInsertPos_ = false;
    ScreenType   insertScreen_ = ScreenType::PHRASE;
    int          insertRow_    = 0;
    int          insertCol_    = 0;

    // ── The spine ────────────────────────────────────────────────────────────────────────────────

    /** "What is under the cursor?" — asked of the current screen's row in `SCREENS`. */
    CursorContext cursor_context() const;

    /** Apply a resolved action to the live document. True if anything changed. */
    bool apply_edit(const InputAction& action);

    /** Context → action → mutate → echo to the engine. `fn` is one of the five generic handlers. */
    void generic_input(InputAction (*fn)(const CursorContext&));

    /** The same over EVERY ROW of a selection when one is up (A+RIGHT over 4 rows → 4 cells). */
    void selection_or_single(InputAction (*fn)(const CursorContext&));

    /** Move the cursor, or drag the selection's active edge. */
    void dpad_nav(NavDir direction);

    /**
     * The R+LEFT/R+RIGHT deep-link: CAPTURE the ref under the departing cursor into lastEdited*, then
     * APPLY lastEdited* to the arriving screen's current*. ⚠️ Horizontal moves only, and only when the
     * screen actually changes; R+UP/DOWN just save/restore the cursor.
     */
    void sync_last_edited_on_screen_switch(ScreenType from, ScreenType to);

    /** An edit happened: tell the sequencer, so a note already scheduled past the cursor is redone. */
    void mark_modified(bool table_touched = false);

    /** The first HALF of mark_modified — dirty the document, (re-)arm the autosave — for the EQ editor's
     *  band path, whose engine push is two calls, not push_globals. Never bump `projectVersion` bare:
     *  that is a dirty flag with no crash protection. */
    void mark_dirty_and_arm_autosave();

    /**
     * Sound the PHRASE note under the cursor while A is down (SETTINGS NOTE PREV); `on_a_released` is
     * its only end. It gates itself (NOTE column, filled step, no selection).
     * ⚠️ No timed kill: only call from an A handler — the mapper always delivers A's release, focus loss
     * included. A caller without A held would leave it sounding.
     */
    void preview_held_note();
    /** A phrase preview is sounding that the release of A must end. */
    bool heldNotePreview_ = false;

    /** The song cell the cursor last left SONG on, as a 0-based track — only ever a TIE-BREAK for
     *  `songcore::track_of_*` (from the saved songCursorColumn), so a stale one cannot misroute. */
    int remembered_song_track() const;

    /**
     * The mixer channel an audition on the preview lane borrows, or -1 (unity gain). ⚠️ An instrument is
     * not IN the song, only its uses: this is the song cell you came through, while it still holds a
     * chain — never "the first track that plays it".
     */
    int audition_track() const;

    /** True on the three screens that edit an INSTRUMENT rather than the arrangement. */
    bool on_instrument_screen() const;

    /** True on the two that edit the GLOBALS — the mixer, the master bus, the send buses. */
    bool on_globals_screen() const;

    // ── The FX helper (ui/dispatch/layers/fx_helper.cpp) ───────────────────────────────────────
    GestureResult fx_helper_layer(Gesture g);
    /** Raise the helper on the effect the cursor's FX column holds. */
    void open_fx_helper();
    /** True when the cursor is on an FX-TYPE column (PHRASE 4/6/8, TABLE 3/5/7). */
    bool on_fx_type_column() const;
    /** The effect CODE the cursor's FX column currently holds — where the picker opens. */
    int  current_fx_type_code() const;
    /** Write an effect CODE into the FX column under the cursor. */
    void apply_fx_type_change(int effect_code);

    // ── The mapping destination picker (ui/dispatch/layers/map_picker.cpp) ──────────────────────
    GestureResult map_picker_layer(Gesture g);
    /** Raise the picker on the destination the mapping under the cursor already has. */
    void open_map_picker();
    /** On a mapping row's GROUP or PARAMETER cell — what the picker stands in for. Not the ADD row. */
    bool on_map_dest_cell() const;
    /** Point the mapping under the cursor at the picked destination, and close. */
    void apply_map_picker_choice();

    // ── The modal guards (see THE MODAL RULE at the top) ─────────────────────────────────────────
    bool qwerty_open() const { return s_.qwerty.isOpen; }
    bool fx_helper_open() const { return s_.fxHelper.isOpen; }
    bool map_picker_open() const { return s_.mapPicker.isOpen; }

    // ═════════════════════════════════════════════════════════════════════════════════════════════
    // THE LAYER STACK — `LAYERS` is the ONE place its order is written
    // ═════════════════════════════════════════════════════════════════════════════════════════════
    //
    // ⭐ The default for a gesture a layer does not answer is SWALLOW, so a new layer is one row in
    // `LAYERS` plus its handler (ui/dispatch/layers/): until it answers a gesture, that gesture does
    // nothing under it rather than editing the screen behind.
    // The FX picker is not a modal, but it owns the buttons the same way, so it is a layer here.
    // `modal_backdrop_active` (ui/app_state.h) is the separate scrim question.

    /** One row of the stack: whether the layer is up, and what it does with a gesture. */
    struct Layer {
        bool (InputDispatcher::*isOpen)() const;
        GestureResult (InputDispatcher::*handle)(Gesture);
    };
    /** Top first. The order, and why, is in ui/dispatch/route.cpp. */
    static const Layer LAYERS[];

    /** The topmost open layer, or null. */
    const Layer* top_layer() const;

    /** One screen (ui/dispatch/screens/): its buttons, the cell under its cursor, an edit applied there,
     *  what a knob would map there (null: nothing), and its grid (null: no selection). */
    struct ScreenHandler {
        ScreenType id;
        GestureResult (InputDispatcher::*handle)(Gesture);
        CursorContext (InputDispatcher::*context)() const;
        bool (InputDispatcher::*edit)(const InputAction&);
        songcore::MapTarget (InputDispatcher::*knob)() const;
        const GridCursor* grid;
    };
    static const ScreenHandler SCREENS[];

    /** The current screen's row in `SCREENS`, or null. */
    const ScreenHandler* screen() const;
    /** The current screen's grid cursor, or null where there is no selection. */
    const GridCursor* grid() const;

    /** Offer `g` to the top layer, then to the screen's handler. True when one of them took it and the
     *  gesture's body must not run; false when it is the body's to answer. */
    bool route(Gesture g);

    /** The confirm dialog: A and B answer it; every other button is inert while it is up, except the
     *  audition's stop (ui/dispatch/layers/confirm.cpp). */
    bool confirm_open() const { return s_.confirm.is_open(); }
    GestureResult confirm_layer(Gesture g);

    /** A on the dialog: do the thing it asked about. */
    void confirm_accept();

    /**
     * B on the dialog: don't. ⚠️ Not always a pure close — for RECOVER it means "discard my unsaved
     * work" and DELETES the autosave, or the prompt would return on every launch. The tests pin
     * both: the other questions leave the filesystem untouched.
     */
    void confirm_cancel();

    // ═════════════════════════════════════════════════════════════════════════════════════════════
    // THE EQ EDITOR (ui/dispatch/layers/eq.cpp)
    // ═════════════════════════════════════════════════════════════════════════════════════════════
    //
    // ⚠️ An OVERLAY, so `currentScreen` still names the screen underneath.

    bool eq_open() const { return s_.eq.isOpen; }
    GestureResult eq_layer(Gesture g);

    /** A+DPAD and A+B: the edit, through the EQ module's own cursor rather than the screen's. */
    void eq_edit(InputAction (*fn)(const CursorContext&));

    /** B+LEFT/RIGHT: step the slot 0..127 (CLAMPED — a bank index). */
    void eq_step_slot(int delta);

    /** Raise the editor on `slot`, remembering WHICH cell asked (the slot cycle has to write back). */
    void open_eq_editor(int slot, EqCallerContext caller);
    void close_eq_editor() { s_.eq = EqEditorState{}; }

    /** The D-pad: LEFT/RIGHT change band, UP/DOWN change param. Both CLAMP; neither wraps. */
    void eq_move_cursor(int d_band, int d_param);

    /** Re-point the cell that opened the editor at `new_slot`; the caller tag says which of five
     *  fields. */
    void apply_caller_eq_slot_change(int new_slot);

    /** ⚠️ After EVERY band nudge — two engine calls: writing the bank changes nothing until the
     *  consumer is re-handed the slot (SongcoreHost::set_eq_band). */
    void push_eq_band_to_engine();

    /**
     * The ONE list of cells whose plain A opens something — both what A DOES (peek = false) and what
     * the mapper must DEFER (peek = true). One function, or a cell drifts into openable-not-deferred
     * (its A+DPAD pre-empted) or deferred-not-openable (the press eaten).
     * ⚠️ `defer_a_to_release` has one more arm (the sample editor's chop tap, which opens nothing); the
     * same rule applies: only an A the RELEASE can still perform may be deferred.
     */
    bool open_sub_screen_at_cursor(bool peek);

    // ── The THEME EDITOR (ui/dispatch/layers/theme.cpp) ──────────────────────────────────────────
    //
    // Raised from SETTINGS, and `currentScreen` STAYS `SETTINGS` underneath it.
    // ⚠️ SAVE raises the keyboard ON TOP of it, so QWERTY sits above THEME in the layer stack.

    bool theme_open() const { return s_.themeEditor.isOpen; }
    GestureResult theme_layer(Gesture g);

    // The roll's seed starts from the frame clock, so each opening walks a different sequence.
    void open_theme_editor() {
        s_.themeEditor = ThemeEditorState{};
        s_.themeEditor.isOpen = true;
        s_.themeEditor.seed ^= static_cast<uint32_t>(now_ms_);
    }
    void close_theme_editor() { s_.themeEditor = ThemeEditorState{}; }

    /** Is a port actually OPEN? A backend can exist with no port, and "TEST SENT" into a closed handle
     *  would lie about the one thing the button is for. */
    bool port_open() const { return s_.midiOut != nullptr && s_.midiOut->is_open(); }

    /** The D-pad: UP/DOWN walk the rows (WRAPPING), LEFT/RIGHT the row's own channels (WRAPPING). */
    void theme_move_cursor(int d_row, int d_channel);

    /** A on the THEME row: 1 = roll a palette, 2 = SAVE (the keyboard), 3 = LOAD (the browser). */
    void theme_row_action();

    /** A+DPAD: the built-in cycle, the style ring, or a colour channel — one per cell. */
    void theme_dpad_edit(int cycleDelta, int nudge);

    /** Generate a palette. `rowOnly` holds every row but the cursor's. L+A locks, R+A rolls one row. */
    void theme_roll_palette(bool rowOnly);

    /** Drop a failed roll's message. The clash line is derived in the draw, not stored. */
    void theme_refresh_message();

    /**
     * Apply the typed name and write `<dir>/<name>.ptt` (the QWERTY's THEME_SAVE arm).
     * ⚠️ `dir` is a PARAMETER: `qwerty_apply()` clears `s_.qwerty` before dispatching, so the live
     * keyboard's `contextExtra` is already gone (reading it wrote to the filesystem ROOT).
     */
    void save_theme_as(const std::string& dir, const std::string& typed_text);

    // ── MIDI: the ports ──────────────────────────────────────────────────────────────────────────
    /**
     * Re-enumerate the ports and re-resolve the saved device NAME — on every screen entry and once a
     * second (`run_midi_hotplug`). A saved name not in the list resolves to 0 = OFF: the row shows
     * what is OPEN, never what was once wanted (midi_settings.h).
     */
    void refresh_midi_devices();

    /**
     * Make the port match `settings.midiOutDevice`.
     * ⚠️ PANICS FIRST: the shell hands over one `IMidiOut` for the session and this swaps the DEVICE
     * under it, so `ExternalConsumer::set_out` never sees a change — the owed note-offs must go out
     * before the old port closes.
     */
    void apply_midi_device();

    /** `refresh_midi_devices`' twin for the INPUT list. */
    void refresh_midi_in_devices();

    /**
     * Make the input port match `settings.midiInDevice`. ⚠️ The ORDER is the function: `set_sink(nullptr)`
     * before `close()` (no byte from the old device mid-swap); `host_.reset_midi_in()` between (stale
     * running status would complete a phantom note); `set_sink` before `open` (an open port delivers).
     */
    void apply_midi_in_device();

    /** Close the port and forget which device it was. OUT panics first — see `apply_midi_device`. */
    void close_midi_out();
    void close_midi_in();
    /** Open what the setting asks for (a device, or AUTO's first); false if nothing took. */
    bool open_midi_out();
    bool open_midi_in();

    /** The once-a-second rescan: close a device that has gone, open one that has arrived. */
    void run_midi_hotplug();

    static constexpr long long MIDI_SCAN_MS = 1000;
    long long                  midiScanDueMs_ = 0;
    // Devices that refused to open (another app holds them) — not retried until they leave the list.
    std::vector<std::string>   midiOutRefused_;
    std::vector<std::string>   midiInRefused_;

    /**
     * Is the port we listen on the one we send on? MIDI thru on a LOOPBACK port is an amplifying
     * feedback loop. One function called from both device rows, derived from the two names the
     * screen shows.
     */
    void update_midi_thru();

    /** NEW, and the engine sync a fresh project needs (SongcoreHost::new_project). */
    void start_new_project();

    /** Slot 0 across the board, and no selection. Shared by NEW and LOAD. */
    void reset_editing_context();

    /** A .ptp replaced the document: clean, no selection, browser closed. */
    void load_project_done(const std::string& path);

    /** EXPORT. Renders SYNCHRONOUSLY; `on_render_progress_` repaints the frame from inside it. */
    void export_song(bool stems);

    // ── The RENDER dialog (ui/modules/render_dialog.h) ──────────────────────────────────────────

    /**
     * PROJECT → EXPORT → MIX / STEMS. The range starts on the section the SONG cursor is in and SONG END
     * on AUTO — "export the part I am looking at" with no dialling. REPEAT is kept between openings (a
     * preference about the file, not a place in the song).
     */
    void open_render_dialog(RenderDialogState::Output output);

    bool render_dialog_open() const { return s_.renderDialog.isOpen; }

    /** Its buttons, as a layer (ui/dispatch/layers/render_dialog.cpp). */
    GestureResult render_dialog_layer(Gesture g);

    /** UP/DOWN on the panel. Clamps at both ends — four rows are not a ring worth wrapping. */
    void render_dialog_move_cursor(int delta);

    /** A+DPAD on the panel. `delta` is the step; the row under the cursor decides what it means. */
    void render_dialog_edit(int delta);

    /** R+UP/DOWN: move the whole range to the previous or next section of the song. */
    void render_dialog_step_section(int delta);

    /** A on the RENDER row. Fires the output the dialog was opened for, then closes. */
    void render_dialog_fire();

    /**
     * SONG-selection RESAMPLE (the RESAMPLE keyboard's APPLY): render the selection to a WAV in
     * Resampled/ and load it into a fresh instrument. SYNCHRONOUS like export_song. `customBaseName`
     * empty ⇒ `Resample_NNNN`. A no-op while rendering or with no selection.
     */
    void resample_selection(const std::string& customBaseName);

    // ── The FILE BROWSER (ui/dispatch/screens/file_browser.cpp) ─────────────────────────────────
    bool on_browser() const { return s_.currentScreen == ScreenType::FILE_BROWSER; }
    /** The browser's own buttons. */
    GestureResult file_browser_screen(Gesture g);
    /** B: disarm a pending SELECT chord, copy a selection, or leave. */
    void browser_back();
    /** START: audition the file under the cursor, if it is audible. */
    void browser_audition();
    /** L+B: start a selection, select all on a quick second tap, or re-anchor. */
    void browser_select();
    /** L+A: cut the selection to the file clipboard, or paste it here. */
    void browser_cut_or_paste();
    /** SELECT+A: rename what is under the cursor (opens the keyboard); on a granted folder, SET HOME. */
    void browser_rename();
    /** SELECT+B: arm the delete confirm, never delete on the press; on a granted folder, FORGET. */
    void browser_arm_delete();
    /** SELECT+R: create a folder here (opens the keyboard). */
    void browser_new_folder();
    /** Leave the browser for the screen it was opened from, dropping the audition on the way out. */
    void close_file_browser();
    /** Re-list the current directory in place — after a rename, a create, a delete or a paste. */
    void refresh_browser();
    /** R+UP / R+DOWN: step through the six sort modes, rebuilding the listing under the cursor. */
    void browser_cycle_sort(int delta);
    /** Move the cursor, keeping the 19-row window around it. `page` = the D-pad's LEFT/RIGHT jump. */
    void browser_move_cursor(int delta, bool page);
    /** A: open a folder, go up, or LOAD the file — which depends on `browserPurpose`. */
    void browser_confirm();
    /** The paths inside the live selection, minus the ".." entry, which is not a file. */
    std::vector<std::string> browser_selected_paths() const;
    /** Copy or move the clipboard into the directory on screen, de-duplicating names. */
    void browser_paste();

    // ── The QWERTY keyboard (ui/dispatch/layers/qwerty.cpp) ─────────────────────────────────────
    GestureResult qwerty_layer(Gesture g);
    void open_qwerty(QwertyContext context, const std::string& initial_text,
                     const std::string& field_label, const std::string& context_extra,
                     int max_length = 20, bool clear_on_first_b = false);
    /** APPLY — what START, and A on the APPLY button, do. Acts on the context it was opened with. */
    void qwerty_apply();
    /** ABORT — SELECT, and A on the ABORT button. Discards the text. */
    void qwerty_cancel() { s_.qwerty = QwertyKeyboardState{}; }

    // ═════════════════════════════════════════════════════════════════════════════════════════════
    // PROJECT AND SETTINGS (ui/dispatch/screens/project.cpp)
    // ═════════════════════════════════════════════════════════════════════════════════════════════

    GestureResult project_screen(Gesture g);
    GestureResult settings_screen(Gesture g);
    CursorContext project_context() const;
    bool          project_edit(const InputAction& action);
    CursorContext settings_context() const;
    bool          settings_edit(const InputAction& action);
    /** B on SETTINGS, MIDI and the mapping list: back to `back`, the screen stored on the way in. */
    void leave_to(ScreenType back);

    /** A on PROJECT: SAVE / LOAD / NEW / MIX / STEMS / SEQ / INST / SETTINGS> / EXIT. */
    void project_action();

    /**
     * TAP TEMPO — A on the TEMPO row, in time. The tempo averages the last `TAP_TEMPO_KEEP` gaps (one
     * gap jumps on every uneven tap). The first tap sets nothing; a gap past `TAP_TEMPO_TIMEOUT_MS`
     * starts a new count.
     */
    void tap_tempo();
    /** A on SETTINGS: only THEME (row 9) and TEMPLATE (row 10) do anything — the rest are A+DPAD. */
    void settings_action();

    // ═════════════════════════════════════════════════════════════════════════════════════════════
    // MIDI AND THE MAPPING LIST (ui/dispatch/screens/midi.cpp)
    // ═════════════════════════════════════════════════════════════════════════════════════════════

    GestureResult midi_screen(Gesture g);
    GestureResult midi_map_screen(Gesture g);
    CursorContext midi_context() const;
    bool          midi_edit(const InputAction& action);
    CursorContext midi_map_context() const;
    bool          midi_map_edit(const InputAction& action);

    /** A on MIDI: only PANIC and TEST do anything — OUTPUT / OFFSET / PROG CHG are A+DPAD. */
    void midi_action();

    /** A on the mapping list — the ADD row, and nothing else on that screen answers a bare press. */
    void midi_map_action();

    /** Put the mapping cursor back inside a list whose length changed — owed on the way IN too (a
     *  different project's list under a remembered cursor). */
    void clamp_midi_map_cursor();

    // ═════════════════════════════════════════════════════════════════════════════════════════════
    // INSTRUMENT, THE POOL AND MODS (ui/dispatch/screens/instrument.cpp)
    // ═════════════════════════════════════════════════════════════════════════════════════════════

    GestureResult instrument_screen(Gesture g);
    GestureResult pool_screen(Gesture g);
    GestureResult mods_screen(Gesture g);
    CursorContext instrument_context() const;
    bool          instrument_edit(const InputAction& action);
    songcore::MapTarget instrument_knob() const;
    CursorContext pool_context() const;
    bool          pool_edit(const InputAction& action);
    CursorContext mods_context() const;
    bool          mods_edit(const InputAction& action);
    /** What the three share: START auditions the instrument, any plain press stops it. */
    GestureResult instrument_audition(Gesture g);

    /**
     * A on one of INSTRUMENT's four cells that open something: the source LOAD and EDIT on row 0, the
     * preset SAVE and LOAD on row 5. True if it handled the press.
     */
    bool instrument_open_at_cursor();
    /** A on the pool's NAME column of an EMPTY slot: browse for a source. True if it opened. */
    bool pool_load_into_empty_slot();

    /**
     * INSTRUMENT row 0, A+DPAD on TYPE: switch at once on an EMPTY slot, else ask first (the source is
     * dropped). `delta` (+1 / −1) rides through the dialog in `ConfirmDialogState::arg`.
     */
    void request_instrument_type_toggle(int delta);
    void toggle_instrument_type(int delta);

    /** True when the cursor is on INSTRUMENT's TYPE cell — where A+DPAD toggles rather than steps. */
    bool on_instrument_type_cell() const;

    // ═════════════════════════════════════════════════════════════════════════════════════════════
    // SONG, CHAIN, PHRASE AND TABLE (ui/dispatch/screens/sequencer.cpp)
    // ═════════════════════════════════════════════════════════════════════════════════════════════

    GestureResult song_screen(Gesture g);
    GestureResult chain_screen(Gesture g);
    GestureResult phrase_screen(Gesture g);
    GestureResult table_screen(Gesture g);
    CursorContext song_context() const;
    bool          song_edit(const InputAction& action);
    CursorContext chain_context() const;
    bool          chain_edit(const InputAction& action);
    CursorContext phrase_context() const;
    bool          phrase_edit(const InputAction& action);
    CursorContext table_context() const;
    bool          table_edit(const InputAction& action);

    /** What a selection's clipboard does to the marked block. */
    enum class ClipOp { COPY, CUT, DELETE };
    /** One screen's three clipboard operations, on the item it shows. */
    using ClipFn = void (InputDispatcher::*)(ClipOp, const SelectionBounds&);
    void song_clip(ClipOp op, const SelectionBounds& b);
    void chain_clip(ClipOp op, const SelectionBounds& b);
    void phrase_clip(ClipOp op, const SelectionBounds& b);
    void table_clip(ClipOp op, const SelectionBounds& b);

    /**
     * What the four share: L+B marks a selection; inside one B copies, A+B deletes and L+A cuts it.
     * Outside one L+A pastes at the cursor into item `pasteTarget`, and A+B is the cell's own delete.
     */
    GestureResult grid_clipboard(Gesture g, ClipFn clip, int pasteTarget);

    /** A on an EMPTY cell: insert the item last edited, and arm A,A. On PHRASE, the NOTE column only,
     *  and A on a filled note auditions it instead. */
    void song_insert();
    void chain_insert();
    void phrase_insert();
    /** A,A: insert the next UNUSED chain or phrase; on PHRASE, keep the note and re-point it at the next
     *  FREE instrument. */
    void song_insert_next();
    void chain_insert_next();
    void phrase_next_instrument();
    /** Remember where an inserting A landed, so a second A there counts as A,A. */
    void arm_double_tap();
    /** Was the first A of this A,A an insert on this same cell? Consumes the arm. */
    bool take_double_tap();

    /** L+B+A. SONG deep-clones the chain under the cursor (its phrases too), CHAIN the phrase under the
     *  cursor, PHRASE itself — and follows the copy. Each into a FREE slot, or not at all. */
    void song_clone_chain();
    void chain_clone_phrase();
    void phrase_clone();

    /** B+LEFT/RIGHT under NAV = SONG: step along the song row. False off NAV = SONG, where B+LEFT/RIGHT
     *  steps through the pool instead (ui/song_pointer.h). */
    bool song_relative_b_horizontal(int delta);
    /** B+UP/DOWN under NAV = SONG: CHAIN walks the song column, PHRASE the chain's filled rows. Off NAV =
     *  SONG it does nothing. */
    void song_relative_b_vertical(int delta);

    /** LIVE on SONG: START launches the cursor's cell, L+START its row, R+START stops its track — each a
     *  second time promotes the launch from the chain's end to the next phrase boundary. */
    void live_launch_cell();
    void live_launch_row();
    void live_stop_track();

    // ═════════════════════════════════════════════════════════════════════════════════════════════
    // GROOVE AND SCALE (ui/dispatch/screens/scale_groove.cpp)
    // ═════════════════════════════════════════════════════════════════════════════════════════════

    GestureResult groove_screen(Gesture g);
    GestureResult scale_screen(Gesture g);
    CursorContext groove_context() const;
    bool          groove_edit(const InputAction& action);
    CursorContext scale_context() const;
    bool          scale_edit(const InputAction& action);
    /** The GROOVE screen's cursor state, assembled once for both the context and the edit. */
    GrooveState groove_state(const songcore::Project& p) const;

    /** A on the SCALE screen's NAME row: column 1 = SAVE, column 2 = LOAD. Column 0 cycles on A+DPAD. */
    void scale_row_action();

    /** Apply the typed name and write `<dir>/<name>.pts`. The QWERTY's SCALE_SAVE arm. */
    void save_scale_as(const std::string& dir, const std::string& typed_text);

    /** A on the GROOVE screen's panel: the SAVE and LOAD cells. Every other panel row ignores it. */
    void groove_row_action();

    /** Apply the typed name and write `<dir>/<name>.ptg`. The QWERTY's GROOVE_SAVE arm. */
    void save_groove_as(const std::string& dir, const std::string& typed_text);

    // ═════════════════════════════════════════════════════════════════════════════════════════════
    // MIXER AND EFFECTS (ui/dispatch/screens/mixer.cpp)
    // ═════════════════════════════════════════════════════════════════════════════════════════════

    GestureResult mixer_screen(Gesture g);
    GestureResult effects_screen(Gesture g);
    CursorContext mixer_context() const;
    bool          mixer_edit(const InputAction& action);
    songcore::MapTarget mixer_knob() const;
    CursorContext effects_context() const;
    bool          effects_edit(const InputAction& action);
    songcore::MapTarget effects_knob() const;

    // ═════════════════════════════════════════════════════════════════════════════════════════════
    // THE SAMPLE EDITOR
    // ═════════════════════════════════════════════════════════════════════════════════════════════

    bool on_sample_editor() const { return s_.currentScreen == ScreenType::SAMPLE_EDITOR; }

    /** The editor's own buttons (ui/dispatch/screens/sample_editor.cpp). */
    GestureResult sample_editor_screen(Gesture g);
    CursorContext sample_editor_context() const;
    bool          sample_editor_edit(const InputAction& action);

    /** "ARE YOU SURE?" — B on a modified sample. A discards and leaves, B stays; nothing else. */
    bool sample_close_open() const { return on_sample_editor() && s_.sampleEditor.showConfirmClose; }
    GestureResult sample_close_layer(Gesture g);

    /**
     * START: TOGGLES the audition of the selection — the only audition that does. Long loops are
     * auditioned here, and "any button silences a preview" is exempt on this screen.
     */
    void sample_editor_audition();

    /** Rows 3..8: the D-pad DRAGS the selection instead of moving a cursor. */
    bool on_sample_selection_row() const {
        return on_sample_editor() && s_.sampleEditor.cursorRow >= 3 && s_.sampleEditor.cursorRow <= 8;
    }

    /** INSTRUMENT's EDIT button (row 0, col 3). Samplers only — an SF2 has no waveform to cut. */
    void open_sample_editor();
    /** B on an unmodified sample: free the undo, drop the scratch slots, go back. */
    void close_sample_editor();

    /**
     * Build the editor's session from the sample the engine holds: length, rate, waveform, the selection
     * the instrument's start/end describe, the file's slice markers. Separate from `open_sample_editor`
     * because the editor's LOAD re-enters on DIFFERENT audio. ⚠️ Must not touch `previousScreen` (the
     * editor's return target).
     */
    void init_sample_editor_state();

    /** A+DPAD on rows 3..8, and the whole reason those rows have no CursorContext. */
    void nudge_selection_edge(int64_t delta);

    /** Row 11's POSITION cell: A+DPAD drags the boundary under the cursor. Col 0 (the index) keeps its
     *  own cell — dragging there would fight choosing the marker. */
    bool on_sample_slice_marker_row() const {
        return on_sample_editor() && s_.sampleEditor.cursorRow == 11 &&
               s_.sampleEditor.cursorCol == 1 &&
               s_.sampleEditor.sliceMethod != SampleEditorModule::SLICE_OFF;
    }

    /** A+DPAD on row 11 col 1. Mirrors `nudge_selection_edge`; what a boundary may DO is what differs. */
    void nudge_slice_marker(int64_t delta);

    /**
     * Row 11 under MANUAL, both columns: a plain A is the lazy-chop tap, so the mapper holds it until
     * release. ⚠️ Every other gesture here starts with A held (A+DPAD steps or drags, A+B deletes); a
     * tap on the PRESS would precede each with an unasked boundary.
     */
    bool on_slice_tap_cell() const {
        return on_sample_editor() && s_.sampleEditor.cursorRow == 11 &&
               s_.sampleEditor.sliceMethod == SampleEditorModule::SLICE_MANUAL;
    }

    /** Put the selection on the slice row 11's cursor is pointing at — every boundary gesture ends here. */
    void select_current_slice();

    /** Copy the method's computed boundaries into the hand-placed list, so a nudge has a home. */
    void materialise_manual_markers();

    /** A+B on row 11: put the marker under the cursor back where its method would have put it. */
    void reset_slice_marker();

    /**
     * A on row 11 under MANUAL: cut a boundary at the PLAYHEAD — START, listen, tap on each hit. It is
     * the same kind of boundary a drag makes: A+DPAD walks it, A+B removes it, the list stays sorted.
     */
    void tap_slice_marker();

    /** RATE (row 1, col 2) and BIT (row 2, col 2) rebuild the buffer — the two cells that change the AUDIO. */
    void apply_sample_rate_and_bits();

    /** A on rows 13/14/16/18/19 — the twelve ops, the FX apply, the name, and the save buttons. */
    void sample_editor_confirm();

    /** Bake the PENDING pitch shift into the buffer and rescale everything in frames — all three saves
     *  call it, since a sample is saved as it SOUNDS. */
    void bake_pending_pitch();

    /** The slice boundaries as WAV cue points: the markers, or DIVIDE's N−1 computed cuts. */
    std::vector<int> compute_slice_cue_points() const;

    /**
     * Re-read length and waveform after an op. `reset_selection` = the op CHANGED THE LENGTH: the
     * selection, the instrument's sample and loop windows and ⚠️ every slice marker describe audio
     * that no longer exists, so they are reset, not clamped.
     */
    void refresh_sample_view(bool reset_selection);

    /** SAVE / SAVE-AS / OVERWRITE all end here: write the WAV, adopt it, and leave the editor. */
    void save_sample_to(const std::string& path, bool adopt_name);

    /** CHOP: every slice out to `Samples/Chops/<name>/`, as its own WAV. */
    void sample_editor_chop();

    /** The slice (start, end) pairs the current method defines — CHOP's work list. */
    std::vector<std::pair<int64_t, int64_t>> current_slices() const;

    /**
     * ⚠️ The deferred half of the audition (set_now): the preview's frame window and stripped EQ, sends
     * and modulation are read when the voice TRIGGERS, 100 frames later, so they are restored then. A
     * second START inside the window runs it IMMEDIATELY, or the first restore would undo the second.
     */
    void run_due_sample_preview_restore(bool force = false);

    /** What the editor's buttons leave for later. Not the editor's session (`s_.sampleEditor`): none of
     *  it is drawn. */
    struct SamplePending {
        bool      previewRestore     = false;   // run_due_sample_preview_restore
        long long previewRestoreAtMs = 0;
        int       previewRestoreInst = 0;

        /** The playhead when A went down on the tap cell (0..1, or −1 = nothing sounding). Written by
         *  `on_a_deferred`, CONSUMED by `tap_slice_marker`, so a press that became a combo leaves nothing. */
        float sliceTapPlayhead = -1.0f;
    };
    SamplePending samplePending_{};

    SampleEditorModule sample_{};
};

}  // namespace pt::ui
