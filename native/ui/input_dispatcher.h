#pragma once

// Every button and combo, for every screen. The generic handlers (ui/cursor.h) turn a press into an
// edit of the cell under the cursor; what lives here is the rest — selection, clipboard, cycling,
// cloning, the FX helper, previews. SDL input → `button_mapper.h` (which handler) → here (what it does).
//
// ⚠️ The modal rule: whatever sits over the screen (a load, a dialog, the keyboard, an editor, a picker)
// owns the buttons while up. Their order is written once, in `LAYERS` (ui/dispatch/route.cpp); a gesture
// the top layer does not answer is swallowed there. Then the screen's row in `SCREENS` is asked.
// The THEME and EQ editors let START and SELECT through, so a band can be swept across an audition.
//
// The clock is injected (`set_now()` once a frame) so tests can move time.

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
#include "ui/phrase_fill.h"
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
    // A test points `fs` at a temp directory.
    InputDispatcher(AppState& state, songcore::SongcoreHost& host, FileSystem& fs)
        : s_(state), host_(host), fs_(fs) {
        s_.clipboard = &clip_;   // the layout's clipboard readout
        // The project the shell pushed before this existed needs its song pointer clamped.
        clamp_song_pointer(s_);
    }

    // The frame's clock, once per frame before the events. Runs what is due: the sample editor's
    // audition restore, the autosave, the status lines' auto-dismiss.
    void set_now(long long now_ms);

    // Will timed work change the screen on its own? The shell skips drawing idle frames.
    // ⚠️ It goes false on the frame that must be drawn: the caller owes the active→idle edge a redraw.
    // A new screen-changing timer belongs in this expression.
    bool has_pending_timed_work() const {
        return statusDismissAtMs_ != 0 || browserStatusDismissAtMs_ != 0 || autosavePending_;
    }

    // ── The lifecycle: what only the shell knows. The autosave is otherwise internal ──

    // Where relative sample paths resolve; once at start-up, the folder the shell gave `load_media`.
    // ⚠️ The wrong folder recovers a portable project that plays silence while looking correct.
    void set_media_base_dir(std::string dir) { mediaBaseDir_ = std::move(dir); }

    enum class BootRecovery {
        NONE,      // no autosave — the last session ended cleanly
        ASKED,     // RESUME=ASK: the RECOVER WORK? dialog is up
        RESTORED,  // RESUME=AUTO: the document is back, and dirty
        DROPPED,   // it would not parse; the file is gone
    };

    // An autosave that survived to launch means an unclean exit; SETTINGS → RESUME decides what to do.
    // Call after loading settings.json and the shell's `push_params()`.
    BootRecovery boot_recovery();

    // Write the autosave now if there is unsaved work — the app is leaving.
    // ⚠️ From the frame loop's exit path, never a signal handler: it allocates and writes a file.
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

    // A+B: delete the cell or reset it to its default; over a selection, delete the range.
    void on_a_b();

    // A,A on the same cell: insert the next unused chain/phrase.
    void on_a_a();

    // Release of A: commits the FX helper's highlighted effect.
    void on_a_released();

    // A went down or up, whatever it then does.
    void on_a_held(bool down) { s_.aHeld = down; }

    // A+START: audition the instrument on the three instrument screens and TABLE.
    void on_a_start();

    // A went down on a cell the mapper defers. Records the instant: the sample editor's chop tap is
    // aimed by ear at the playhead, and the release comes a reaction time later.
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
    // Acts on the press; undone if its own button comes up before R.
    // R+B: toggle MUTE on the cursor's channel, or every channel in the selection.
    void on_r_b();
    // R+A: toggle SOLO, same targeting.
    void on_r_a();
    // R down or up — the arming half of MIDI learn ("hold R, turn a knob").
    void on_r_held(bool down);
    // Another button went down while R is held: a combo, so this hold never learns.
    void on_r_combo_press();

    // R came up first: the chord's changes stand.
    void on_r_combo_commit();
    // A/B came up first: every track's mute and solo back the way the chord found them.
    void on_r_combo_revert();

    // ── L: selection and the clipboard ───────────────────────────────────────────────────────────
    // L+B: enter selection, then widen it CELL → ROW → SCREEN on each tap inside 500 ms.
    void on_l_b();
    // L+A: cut inside a selection, paste outside one.
    void on_l_a();
    // L+R: leave selection mode.
    void on_l_r();
    // L+B+A: deep-clone the chain/phrase under the cursor into free slots.
    void on_l_b_a();
    // L + a direction: fill rows from the selection's top row (or the cursor's cell on PHRASE).
    // RIGHT walks the spacings, LEFT the even spreads, UP rolls random values, DOWN scatters.
    void on_l_up();
    void on_l_down();
    void on_l_left();
    void on_l_right();

    // ── SELECT + … : the file browser's verbs ────────────────────────────────────────────────────
    // SELECT+A: rename the file/folder under the cursor (opens the keyboard).
    void on_select_a();
    // SELECT+B: arm the delete confirm; never deletes on the press itself.
    void on_select_b();
    // SELECT+R: create a folder here (opens the keyboard).
    void on_select_r();

    // ── The plain buttons ────────────────────────────────────────────────────────────────────────
    // B inside a selection copies it and exits.
    void on_button_b();
    void on_button_a();
    // Bare SELECT raises help (per SETTINGS > HELP) and aborts the keyboard. Called on the release,
    // only when nothing else was pressed meanwhile.
    void on_select();

    // A button went down: put help away (the compact panel on any press but SELECT, the overlay on any).
    void on_help_dismiss();

    bool help_full_open() const { return s_.helpFull; }
    // The mapper closes the overlay on any press and consumes it, so nothing reaches this.
    GestureResult help_layer(Gesture) { return GestureResult::TAKEN; }
    // START: play/stop; what it plays depends on the screen.
    void on_start();

    // LIVE on SONG: L+START queues the cursor row as one scene, R+START silences the cursor's channel.
    // Below SONG, R+START plays the song from the row you came through; L+START does nothing.
    void on_l_start();
    void on_r_start();

  private:
    // The gate every LIVE gesture asks: LIVE mode, on SONG, nothing on top.
    // ⚠️ The mapper's L+START / R+START are global; ungated they would queue from the sample editor.
    bool live_song_gesture() const {
        return host_.live_mode() && s_.currentScreen == ScreenType::SONG &&
               !top_layer();
    }

    // Bit N set where track N has a chain on `songRow`.
    int  live_row_mask(int songRow) const;

    // Already queued? Then a second L+START promotes it to the next phrase boundary. Read off the slots.
    bool live_row_armed(int songRow) const;

  public:

    // Any plain press silences an audition; whether this screen has one is decided here.
    void on_stop_preview();

    // Asked by the mapper on every plain A: does this cell's A open something while A+DPAD / A+B mean
    // something else? Then A waits for its release and any A-combo cancels it.
    bool defer_a_to_release() const;

    // Asked by the mapper on every plain B: wait for the release? True in the EQ editor and wherever
    // R+B is a mute (a B a frame ahead of its R must still become the chord).
    bool defer_b_to_release() const;

    // For the top-strip readout ("PHR:2x3").
    const Clipboard& clipboard() const { return clip_; }

    // ── Opening the sub-screens (the shell uses these at start-up too) ───────────────────────────

    // Show the browser in `directory`, filtered; `purpose` is what A does with the pick. Captures
    // `previousScreen` (where B returns).
    void open_file_browser(AppState::BrowserPurpose purpose, const std::string& directory,
                           const std::vector<std::string>& extensions);

    // The load-browse categories a config.json override can redirect.
    enum class BrowserDir { SAMPLES, SOUNDFONTS, INSTRUMENTS, PROJECTS, THEMES };

    // Where a load browser starts: the config.json override if it is a real directory, else the default.
    // ⚠️ The one resolution point — `open_file_browser` compares against it for remember-last-folder.
    std::string browser_dir(BrowserDir cat);

    // How many EFFECT_TYPES an FX cell can reach in this build (the MIDI tail may be hidden).
    // ⚠️ The cell's bound and the picker's size both come from here, or the picker meets an unnamed effect.
    int visible_effect_type_count() const;

    // What a render needs from the shell. The render is synchronous: the frame loop stops for it.
    // Both may be empty (the tests render with neither).
    struct RenderHooks {
        std::function<void(bool)> suspend_audio;
        std::function<void()>     repaint;   // draws the progress

        // A load's pump: drain the platform's events; did the user cancel?
        // ⚠️ Every press but the cancel is consumed, or SDL would replay them onto the next screen.
        // It is also how the background event (the autosave flush) is seen during a load.
        std::function<bool()> load_pump;
    };
    void set_render_hooks(RenderHooks hooks) { render_ = std::move(hooks); }

    // ── A slow load ──────────────────────────────────────────────────────────────────────────────
    // A big `.sf3` or a long mp3 can take seconds; the loop is then inside the load. The strip shows
    // only once a load outlasts `LOADING_DELAY_MS`, so a slow device shows it exactly when it is slow.

    // Below this a strip would only flash.
    static constexpr int LOADING_DELAY_MS = 400;

    // How long a status message stays up. Public for the checks.
    static constexpr long long STATUS_DISMISS_MS = 3000;

    // How often the strip repaints and the buttons are read during a load.
    // ⚠️ Not per report: a soundfont reports per sample header, and redrawing each time was slower
    // than the decoding.
    static constexpr int LOADING_REPAINT_MS = 33;

    // Open a load; `detail` is what the strip names. ⚠️ Use `LoadScope` (dispatch_common.h), never by hand.
    void begin_load(long long now_ms, std::string detail);

    // The report from inside the load; false when the user cancelled (the engine unwinds on it).
    // ⚠️ Time is passed in, not via `set_now()`: that runs due work, and an autosave mid-load would
    // write a half-loaded document.
    bool load_tick(long long now_ms, float fraction);

    // Close it, whether the load finished, failed or was cancelled.
    void end_load();

    bool load_running() const { return s_.loading.running; }

    // A load answers no button: the pump reads the cancel itself.
    GestureResult loading_layer(Gesture) { return GestureResult::TAKEN; }

    // Open the MIDI port the settings name, once at boot — the same calls the MIDI screen makes.
    // A port already open (the dev override) is left alone.
    void boot_midi_port();

    // The same for the input port.
    // ⚠️ Sink and open are paired here: an open port with no sink drops every byte.
    void boot_midi_in_port();

    // The app is back in front: re-list the file browser if it is up — other apps (and Android's folder
    // picker) change directories meanwhile. Only Android and iOS send the event.
    void refresh_browser_on_foreground();

  private:
    AppState&               s_;
    songcore::SongcoreHost& host_;
    FileSystem&             fs_;
    long long               now_ms_ = 0;
    // ── Tap tempo (PROJECT > TEMPO, plain A) ─────────────────────────────────────────────────────
    static constexpr int       TAP_TEMPO_KEEP       = 4;      // gaps averaged
    static constexpr long long TAP_TEMPO_TIMEOUT_MS = 3000;   // one beat at 20 BPM; longer starts anew
    static constexpr long long TAP_TEMPO_MIN_MS     = 60;     // shorter is a bouncing button
    long long tapTempoLastMs_ = 0;                    // 0 = no tap yet this count
    long long tapTempoGaps_[TAP_TEMPO_KEEP] = {0};    // ring of the most recent gaps, ms
    int       tapTempoCount_ = 0;                     // how many are filled (capped at KEEP)

    long long               loadStartMs_ = 0;       // when the load in flight opened
    long long               lastLoadPaintMs_ = 0;   // …and when it was last drawn
    RenderHooks             render_{};

    // At the top of anything that holds the UI thread long enough to be heard (a load, a sample editor
    // op): fills the sequencer deeper than the frame poll does, so a playing song keeps going.
    // ⚠️ The depth outlives the operation — an edit just after is heard late. So a save does not use it.
    void before_long_operation() { host_.keep_walking(songcore::Sequencer::LONG_OPERATION_MS); }

    // A save only tops the normal horizon up: autosaves are frequent, and a deep fill makes edits late.
    void before_save() { host_.keep_walking(); }

    // Empty means relative paths stay relative.
    std::string mediaBaseDir_{};

    // ⚠️ A handler here is one step, always. A held direction arrives from the shell as more presses,
    // closer together — never a bigger step.

    // ── The autosave ─────────────────────────────────────────────────────────────────────────────
    // ⚠️ Re-armed on every edit, so it writes 3 s after the last change; a held A+UP edits every 100 ms.
    bool      autosavePending_ = false;
    long long autosaveDueAtMs_ = 0;

    static constexpr long long AUTOSAVE_DEBOUNCE_MS = 3000;

    void run_due_autosave();

    // ── The status lines' auto-dismiss ───────────────────────────────────────────────────────────
    // A message clears STATUS_DISMISS_MS after it is set. Many sites assign the field, so set_now
    // watches it for changes; detection lands one frame after the set.
    std::string statusLastSeen_{};
    long long   statusDismissAtMs_ = 0;

    // The file browser's line. ⚠️ A separate pair: one pair watching two fields re-arms every frame.
    std::string browserStatusLastSeen_{};
    long long   browserStatusDismissAtMs_ = 0;

    void run_due_status_dismiss();

    // Entering INSTRUMENT, by any route, pushes its playback params the frame after.
    ScreenType lastScreenSeen_ = ScreenType::SONG;   // the boot screen
    void run_instrument_entry_push();

    // A mapped knob wrote the song (from the MIDI drain): mark it modified, once a frame. A count is
    // watched because the drain cannot reach the dirty flag.
    void     run_mapped_cc_dirty();
    uint64_t mappedCcSeen_ = 0;

    // A knob turned while R was held: point the cell under the cursor at it. The last controller wins.
    void     run_midi_learn();
    uint64_t learnSeen_ = 0;
    // MIDI learn arms once R has been held ALONE this long. A sequencer's controllers stream in while
    // R+DPAD switches screens, and the moment between the two presses would learn one.
    static constexpr long long MIDI_LEARN_HOLD_MS = 500;
    long long rAloneSinceMs_ = -1;   // when R went down with nothing pressed since; −1 = not armable

    // What a knob would map on the cell under the cursor; `NONE` where nothing can be swept.
    songcore::MapTarget map_target() const;

    // Load the autosave into the document and leave it dirty — recovered work is not saved work.
    // ⚠️ The autosave is not cleared: it is still the only copy.
    bool recover_from_autosave();

    Clipboard clip_{};

    // ── The MUTE/SOLO chord's undo: all ten channels (8 tracks + 2 returns) as the chord found them ──
    // ⚠️ Taken on the chord's first toggle, or muting three channels in turn could only undo the last.
    static constexpr int MIX_CHANNELS = 10;
    struct MixSnapshot {
        bool live = false;
        bool mute[MIX_CHANNELS] = {};
        bool solo[MIX_CHANNELS] = {};
    };
    MixSnapshot mixSnapshot_{};

    // The channels a chord applies to (0-7 tracks, 8 REV, 9 DEL): the selection's columns on SONG,
    // else the cursor's.
    void mute_solo_targets(int (&out)[8], int& count) const;
    void toggle_mute_solo(bool solo);
    // Every track audible again (L+R's mute rung).
    void restore_full_playback();
    // Do R+A/R+B mean MUTE/SOLO here — and so must B be held?
    bool mute_solo_chord_live() const;
    // L+R's recency flag: the selection's state folded into one number, watched for a change. Run from
    // `set_now()` and at the top of `toggle_mute_solo()`, so the order follows the press.
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

    // The A still held inserted the PHRASE note under the cursor, so an A+B now acts on an empty cell.
    bool         insertHeld_ = false;

    // L + direction on PHRASE: the phrase before the first press (`base`, the undo), the fill laid over
    // it (`placed`), and that with any random roll (`shown`).
    // ⚠️ Good only while the phrase still holds `shown`; any other edit or move makes it the new `base`.
    struct FillMemory {
        bool             live     = false;
        int              phraseId = -1;
        FillRange        range{};
        songcore::Phrase base{}, placed{}, shown{};
        int              stop   = 0;       // index into the line it is on
        bool             spread = false;   // that line: fill_spread_stops (L+LEFT), else fill_stops
    };
    FillMemory    fill_{};
    songcore::Rng fillRng_{};
    void phrase_fill(Gesture g);

    // Where the first A of an A,A landed; it counts only if the cursor did not move.
    bool         hasInsertPos_ = false;
    ScreenType   insertScreen_ = ScreenType::PHRASE;
    int          insertRow_    = 0;
    int          insertCol_    = 0;

    // ── The spine ────────────────────────────────────────────────────────────────────────────────

    // "What is under the cursor?", asked of the current screen's row in `SCREENS`.
    CursorContext cursor_context() const;

    // Apply a resolved action to the document. True if anything changed.
    bool apply_edit(const InputAction& action);

    // Context → action → mutate → echo to the engine; `fn` is one of the generic handlers.
    void generic_input(InputAction (*fn)(const CursorContext&));

    // The same over every row of a selection when one is up.
    void selection_or_single(InputAction (*fn)(const CursorContext&));

    // Move the cursor, or drag the selection's active edge.
    void dpad_nav(NavDir direction);

    // R+LEFT/RIGHT: capture the ref under the departing cursor into lastEdited*, then apply it to the
    // arriving screen's current*. Only when the screen actually changes.
    void sync_last_edited_on_screen_switch(ScreenType from, ScreenType to);

    // An edit happened: dirty the document, arm the autosave, tell the sequencer.
    void mark_modified(bool table_touched = false);

    // Dirty the document and arm the autosave, without the engine push (the EQ band path does its own).
    // ⚠️ Never bump `projectVersion` bare: that is a dirty flag with no autosave behind it.
    void mark_dirty_and_arm_autosave();

    // Sound the PHRASE note under the cursor while A is down (SETTINGS NOTE PREV). Gates itself.
    // ⚠️ No timed kill — A's release is its only end, so call it only from an A handler.
    void preview_held_note();
    bool heldNotePreview_ = false;

    // The track the cursor last left SONG on — only ever a tie-break, so a stale one cannot misroute.
    int remembered_song_track() const;

    // The mixer channel an audition borrows, or -1 (unity gain): the song cell you came through, while
    // it still holds a chain — never "the first track that plays it".
    int audition_track() const;

    // The three screens that edit an instrument rather than the arrangement.
    bool on_instrument_screen() const;

    // MIXER and EFFECTS, and any screen while the send editor is open over it.
    bool on_globals_screen() const;

    // ── The FX helper (ui/dispatch/layers/fx_helper.cpp) ───────────────────────────────────────
    GestureResult fx_helper_layer(Gesture g);
    void open_fx_helper();
    // On an FX-TYPE column (PHRASE 4/6/8, TABLE 3/5/7)?
    bool on_fx_type_column() const;
    // The effect code the cursor's FX column holds — where the picker opens.
    int  current_fx_type_code() const;
    void apply_fx_type_change(int effect_code);
    // The cursor's FX slot (0-2) on PHRASE or TABLE, from either of its cells; -1 elsewhere.
    int  fx_slot_at_cursor() const;
    // After an edit: the command in the cursor's FX slot is what a plain A lays down next.
    void remember_fx_at_cursor();
    // A on an empty FX command cell: lay down the last command entered. `peek`: would it? (The mapper
    // holds this A for the release, so A+UP/DOWN still opens the list.)
    bool fx_insert_last(bool peek);

    // ── The mapping destination picker (ui/dispatch/layers/map_picker.cpp) ──────────────────────
    GestureResult map_picker_layer(Gesture g);
    void open_map_picker();
    // On a mapping row's GROUP or PARAMETER cell (not the ADD row)?
    bool on_map_dest_cell() const;
    void apply_map_picker_choice();

    bool qwerty_open() const { return s_.qwerty.isOpen; }
    bool fx_helper_open() const { return s_.fxHelper.isOpen; }
    bool map_picker_open() const { return s_.mapPicker.isOpen; }

    // ── The layer stack ──────────────────────────────────────────────────────────────────────────
    // A gesture a layer does not answer is swallowed, so a new layer is one row in `LAYERS` plus its
    // handler. The FX picker is not a modal but owns the buttons the same way.
    struct Layer {
        bool (InputDispatcher::*isOpen)() const;
        GestureResult (InputDispatcher::*handle)(Gesture);
    };
    // Top first; the order, and why, is in ui/dispatch/route.cpp.
    static const Layer LAYERS[];

    const Layer* top_layer() const;   // or null

    // One screen: its buttons, the cell under its cursor, an edit there, what a knob would map
    // (null: nothing), and its grid (null: no selection).
    struct ScreenHandler {
        ScreenType id;
        GestureResult (InputDispatcher::*handle)(Gesture);
        CursorContext (InputDispatcher::*context)() const;
        bool (InputDispatcher::*edit)(const InputAction&);
        songcore::MapTarget (InputDispatcher::*knob)() const;
        const GridCursor* grid;
    };
    static const ScreenHandler SCREENS[];

    const ScreenHandler* screen() const;   // the current screen's row, or null
    const GridCursor* grid() const;        // null where there is no selection

    // Offer `g` to the top layer, then the screen. True when one took it and the gesture's body must
    // not run.
    bool route(Gesture g);

    // The confirm dialog: A and B answer it; everything else is inert but the audition's stop.
    bool confirm_open() const { return s_.confirm.is_open(); }
    GestureResult confirm_layer(Gesture g);

    void confirm_accept();

    // B on the dialog. ⚠️ For RECOVER it means "discard my work" and deletes the autosave, or the
    // prompt would return on every launch.
    void confirm_cancel();

    // ── The EQ editor (ui/dispatch/layers/eq.cpp) — an overlay: `currentScreen` is still the one under ──

    bool eq_open() const { return s_.eq.isOpen; }
    GestureResult eq_layer(Gesture g);

    // A+DPAD and A+B, through the EQ module's own cursor.
    void eq_edit(InputAction (*fn)(const CursorContext&));

    // B+LEFT/RIGHT: step the slot 0..127, clamped.
    void eq_step_slot(int delta);

    // Raise the editor on `slot`, remembering which cell asked (the slot cycle writes back to it).
    void open_eq_editor(int slot, EqCallerContext caller);
    void close_eq_editor() { s_.eq = EqEditorState{}; }

    // LEFT/RIGHT change band, UP/DOWN param; both clamp.
    void eq_move_cursor(int d_band, int d_param);

    // Re-point the cell that opened the editor at `new_slot`.
    void apply_caller_eq_slot_change(int new_slot);

    // ⚠️ After every band nudge: writing the bank changes nothing until the slot is re-handed.
    void push_eq_band_to_engine();

    // The one list of cells whose plain A opens something (or switches the delay's TIME sync): what A
    // does (peek = false) and what the mapper defers (peek = true). One function, so the two cannot drift.
    // ⚠️ Only an A the release can still perform may be deferred.
    bool open_sub_screen_at_cursor(bool peek);

    // ── The send editor (ui/dispatch/layers/send_editor.cpp) — an overlay too ──

    bool send_editor_open() const { return send_editor_up(s_); }
    // …with no keyboard or EQ editor over it.
    bool send_editor_on_top() const {
        const Layer* top = top_layer();
        return top && top->handle == &InputDispatcher::send_editor_layer;
    }
    GestureResult send_editor_layer(Gesture g);

    void open_send_editor(EffectsSection section);
    void close_send_editor() { s_.sendEditor = SendEditorState{}; }

    // A+DPAD and A+B, through the editor's own cursor.
    void send_editor_edit(InputAction (*fn)(const CursorContext&));

    // ── The theme editor (ui/dispatch/layers/theme.cpp), over SETTINGS ──
    // ⚠️ SAVE raises the keyboard on top of it, so QWERTY sits above THEME in the layer stack.

    bool theme_open() const { return s_.themeEditor.isOpen; }
    GestureResult theme_layer(Gesture g);

    // The roll's seed starts from the clock, so each opening walks a different sequence.
    void open_theme_editor() {
        s_.themeEditor = ThemeEditorState{};
        s_.themeEditor.isOpen = true;
        s_.themeEditor.seed ^= static_cast<uint32_t>(now_ms_);
    }
    void close_theme_editor() { s_.themeEditor = ThemeEditorState{}; }

    // A backend can exist with no port open; "TEST SENT" into a closed one would lie.
    bool port_open() const { return s_.midiOut != nullptr && s_.midiOut->is_open(); }

    // UP/DOWN walk the rows, LEFT/RIGHT the row's channels; both wrap.
    void theme_move_cursor(int d_row, int d_channel);

    // A on the THEME row: 1 = roll a palette, 2 = SAVE (the keyboard), 3 = LOAD (the browser).
    void theme_row_action();

    // A+DPAD: the built-in cycle, the style ring, or a colour channel — one per cell.
    void theme_dpad_edit(int cycleDelta, int nudge);

    // Generate a palette; `rowOnly` keeps every row but the cursor's.
    void theme_roll_palette(bool rowOnly);

    // Drop a failed roll's message.
    void theme_refresh_message();

    // Write `<dir>/<name>.ptt` (the keyboard's THEME_SAVE).
    // ⚠️ `dir` is a parameter because `qwerty_apply()` clears the keyboard state before dispatching.
    void save_theme_as(const std::string& dir, const std::string& typed_text);

    // ── MIDI ports ───────────────────────────────────────────────────────────────────────────────
    // Re-list the ports and re-resolve the saved device name (screen entry, and once a second). A name
    // not in the list shows OFF: the row shows what is open, never what was once wanted.
    void refresh_midi_devices();

    // Make the port match `settings.midiOutDevice`.
    // ⚠️ Panics first: the device changes under the same `IMidiOut`, so the owed note-offs must go out
    // before the old port closes.
    void apply_midi_device();

    void refresh_midi_in_devices();

    // Make the input port match `settings.midiInDevice`. ⚠️ The order matters: unhook the sink before
    // closing, reset the parser between (stale running status), hook the sink before opening.
    void apply_midi_in_device();

    // Close the port and forget the device. OUT panics first.
    void close_midi_out();
    void close_midi_in();
    // Open what the setting asks for (a device, or AUTO's first); false if nothing took.
    bool open_midi_out();
    bool open_midi_in();

    // Once a second: close a device that has gone, open one that has arrived.
    void run_midi_hotplug();

    static constexpr long long MIDI_SCAN_MS = 1000;
    long long                  midiScanDueMs_ = 0;
    // Devices that refused to open (another app holds them) — not retried until they leave the list.
    std::vector<std::string>   midiOutRefused_;
    std::vector<std::string>   midiInRefused_;

    // MIDI thru on a loopback port (listening where we send) is a feedback loop; derived from the two
    // names the screen shows, called from both device rows.
    void update_midi_thru();

    // NEW, with the engine sync a fresh project needs.
    void start_new_project();

    // Slot 0 across the board, and no selection. Shared by NEW and LOAD.
    void reset_editing_context();

    // A .ptp replaced the document: clean, no selection, browser closed.
    void load_project_done(const std::string& path);

    // EXPORT, synchronously; the render hook repaints the progress from inside it.
    void export_song(bool stems);

    // COLLECT into Songs/<folder>, its percentage on the PROJECT screen while it copies.
    void collect_song(const std::string& folder);

    // ── The RENDER dialog (ui/modules/render_dialog.h) ──────────────────────────────────────────

    // PROJECT → EXPORT → MIX / STEMS. The range starts on the SONG cursor's section, SONG END on AUTO.
    // REPEAT is kept between openings.
    void open_render_dialog(RenderDialogState::Output output);

    bool render_dialog_open() const { return s_.renderDialog.isOpen; }

    GestureResult render_dialog_layer(Gesture g);

    // UP/DOWN; clamps at both ends.
    void render_dialog_move_cursor(int delta);

    // A+DPAD; the row under the cursor decides what `delta` means.
    void render_dialog_edit(int delta);

    // R+UP/DOWN: move the whole range to the previous or next section.
    void render_dialog_step_section(int delta);

    // A on the RENDER row: fire the output the dialog was opened for, then close.
    void render_dialog_fire();

    // RESAMPLE's APPLY: render the SONG selection to a WAV in Resampled/ and load it into a fresh
    // instrument, synchronously. Empty `customBaseName` ⇒ `Resample_NNNN`.
    void resample_selection(const std::string& customBaseName);

    // ── The file browser (ui/dispatch/screens/file_browser.cpp) ─────────────────────────────────
    bool on_browser() const { return s_.currentScreen == ScreenType::FILE_BROWSER; }
    GestureResult file_browser_screen(Gesture g);
    // B: disarm a pending SELECT chord, copy a selection, or leave.
    void browser_back();
    // START: audition the file under the cursor, if it is audible.
    void browser_audition();
    // L+B: start a selection, select all on a quick second tap, or re-anchor.
    void browser_select();
    // L+A: cut the selection to the file clipboard, or paste it here.
    void browser_cut_or_paste();
    // SELECT+A: rename (opens the keyboard); on a granted folder, SET HOME.
    void browser_rename();
    // SELECT+B: arm the delete confirm; on a granted folder, FORGET.
    void browser_arm_delete();
    // SELECT+R: create a folder here.
    void browser_new_folder();
    // Back to the screen it was opened from, dropping the audition.
    void close_file_browser();
    // Re-list the current directory in place.
    void refresh_browser();
    // R+UP/DOWN: step through the sort modes.
    void browser_cycle_sort(int delta);
    // Move the cursor, keeping the window around it; `page` is LEFT/RIGHT's jump.
    void browser_move_cursor(int delta, bool page);
    // A: open a folder, go up, or load the file — per `browserPurpose`.
    void browser_confirm();
    // The selection's paths, without "..".
    std::vector<std::string> browser_selected_paths() const;
    // Copy or move the clipboard into the directory on screen, de-duplicating names.
    void browser_paste();

    // ── The QWERTY keyboard (ui/dispatch/layers/qwerty.cpp) ─────────────────────────────────────
    GestureResult qwerty_layer(Gesture g);
    void open_qwerty(QwertyContext context, const std::string& initial_text,
                     const std::string& field_label, const std::string& context_extra,
                     int max_length = 20, bool clear_on_first_b = false);
    // APPLY (START, or A on APPLY): acts on the context it was opened with.
    void qwerty_apply();
    // ABORT (SELECT, or A on ABORT): discards the text.
    void qwerty_cancel() { s_.qwerty = QwertyKeyboardState{}; }

    // ── PROJECT and SETTINGS (ui/dispatch/screens/project.cpp) ───────────────────────────────────

    GestureResult project_screen(Gesture g);
    GestureResult settings_screen(Gesture g);
    CursorContext project_context() const;
    bool          project_edit(const InputAction& action);
    CursorContext settings_context() const;
    bool          settings_edit(const InputAction& action);
    // B on SETTINGS, MIDI and the mapping list: back to the screen stored on the way in.
    void leave_to(ScreenType back);

    // A on PROJECT: SAVE / LOAD / NEW / MIX / STEMS / SEQ / INST / SETTINGS> / EXIT.
    void project_action();

    // A on the TEMPO row, in time: averages the last `TAP_TEMPO_KEEP` gaps. The first tap sets nothing.
    void tap_tempo();
    // A on SETTINGS: only THEME (row 9) and TEMPLATE (row 10) answer it.
    void settings_action();

    // ── MIDI and the mapping list (ui/dispatch/screens/midi.cpp) ─────────────────────────────────

    GestureResult midi_screen(Gesture g);
    GestureResult midi_map_screen(Gesture g);
    CursorContext midi_context() const;
    bool          midi_edit(const InputAction& action);
    CursorContext midi_map_context() const;
    bool          midi_map_edit(const InputAction& action);

    // A on MIDI: only PANIC and TEST answer it.
    void midi_action();

    // A on the mapping list: only the ADD row answers it.
    void midi_map_action();

    // Keep the mapping cursor inside the list — also on the way in (another project's list).
    void clamp_midi_map_cursor();

    // ── INSTRUMENT, the pool and MODS (ui/dispatch/screens/instrument.cpp) ───────────────────────

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
    // What the three share: A+START auditions the instrument, any plain press stops it.
    GestureResult instrument_audition(Gesture g);

    // A on INSTRUMENT's cells that open something: source LOAD/EDIT (row 0), preset SAVE/LOAD (row 5).
    // `peek`: would it? ⚠️ Deferred to A's release, or A+START would open a browser before auditioning.
    bool instrument_open_at_cursor(bool peek);
    // A on the pool's NAME column of an empty slot: browse for a source. Deferred the same way.
    bool pool_load_into_empty_slot(bool peek);

    // A+DPAD on TYPE: switch at once on an empty slot, else ask first (the source is dropped). `delta`
    // rides through the dialog in `ConfirmDialogState::arg`.
    void request_instrument_type_toggle(int delta);
    void toggle_instrument_type(int delta);

    bool on_instrument_type_cell() const;

    // ── SONG, CHAIN, PHRASE and TABLE (ui/dispatch/screens/sequencer.cpp) ────────────────────────

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

    enum class ClipOp { COPY, CUT, DELETE };
    using ClipFn = void (InputDispatcher::*)(ClipOp, const SelectionBounds&);
    void song_clip(ClipOp op, const SelectionBounds& b);
    void chain_clip(ClipOp op, const SelectionBounds& b);
    void phrase_clip(ClipOp op, const SelectionBounds& b);
    void table_clip(ClipOp op, const SelectionBounds& b);

    // What the four share: L+B marks a selection; inside one B copies, A+B deletes, L+A cuts.
    // Outside one L+A pastes into item `pasteTarget`, and A+B is the cell's own delete.
    GestureResult grid_clipboard(Gesture g, ClipFn clip, int pasteTarget);

    // A on an empty cell: insert the item last edited, and arm A,A. On PHRASE, the NOTE column only;
    // A on a filled note auditions it.
    void song_insert();
    void chain_insert();
    void phrase_insert();
    // A+B on an empty note cell: put an OFF there. True if it did.
    // ⚠️ The chord's A has already inserted a note, so "empty" means `insertHeld_` too.
    bool phrase_put_note_off();
    // A,A: insert the next unused chain or phrase; on PHRASE, re-point the note at the next free instrument.
    void song_insert_next();
    void chain_insert_next();
    void phrase_next_instrument();
    void arm_double_tap();
    // Was the first A of this A,A an insert on this same cell? Consumes the arm.
    bool take_double_tap();

    // L+B+A: SONG deep-clones the chain under the cursor (its phrases too), CHAIN the phrase, PHRASE
    // itself — into a free slot or not at all — and follows the copy.
    void song_clone_chain();
    void chain_clone_phrase();
    void phrase_clone();

    // B+LEFT/RIGHT under NAV = SONG: step along the song row. False off NAV = SONG (the pool steps instead).
    bool song_relative_b_horizontal(int delta);
    // B+UP/DOWN under NAV = SONG: CHAIN walks the song column, PHRASE the chain's filled rows.
    void song_relative_b_vertical(int delta);

    // LIVE on SONG: START launches the cursor's cell, L+START its row, R+START stops its track. A second
    // press promotes the launch from the chain's end to the next phrase boundary.
    void live_launch_cell();
    void live_launch_row();
    void live_stop_track();

    // ── GROOVE and SCALE (ui/dispatch/screens/scale_groove.cpp) ──────────────────────────────────

    GestureResult groove_screen(Gesture g);
    GestureResult scale_screen(Gesture g);
    CursorContext groove_context() const;
    bool          groove_edit(const InputAction& action);
    CursorContext scale_context() const;
    bool          scale_edit(const InputAction& action);
    // The GROOVE cursor's state, shared by the context and the edit.
    GrooveState groove_state(const songcore::Project& p) const;

    // A on SCALE's NAME row: column 1 = SAVE, column 2 = LOAD.
    void scale_row_action();

    // Write `<dir>/<name>.pts` (the keyboard's SCALE_SAVE).
    void save_scale_as(const std::string& dir, const std::string& typed_text);

    // A on GROOVE's panel: the SAVE and LOAD cells only.
    void groove_row_action();

    // Write `<dir>/<name>.ptg` (the keyboard's GROOVE_SAVE).
    void save_groove_as(const std::string& dir, const std::string& typed_text);

    // ── MIXER and EFFECTS (ui/dispatch/screens/mixer.cpp) ────────────────────────────────────────

    GestureResult mixer_screen(Gesture g);
    GestureResult effects_screen(Gesture g);
    CursorContext mixer_context() const;
    bool          mixer_edit(const InputAction& action);
    songcore::MapTarget mixer_knob() const;
    CursorContext effects_context() const;
    bool          effects_edit(const InputAction& action);
    songcore::MapTarget effects_knob() const;

    // A on a preset line's SAVE or LOAD cell; `row` is EFFECTS' cursor or the send editor's.
    bool effects_preset_action(int row);

    // A, on its release, on a send's cell (`peek`: would it answer?). INP EQ opens the EQ editor on
    // that send's input; the delay's TIME switches free time ↔ note sync.
    bool effects_release_action(int row, bool peek);

    // Write `<dir>/<name>.ptr` (the keyboard's REVERB_SAVE).
    void save_reverb_preset_as(const std::string& dir, const std::string& typed_text);

    // Write `<dir>/<name>.ptd` (the keyboard's DELAY_SAVE).
    void save_delay_preset_as(const std::string& dir, const std::string& typed_text);

    // ── The sample editor (ui/dispatch/screens/sample_editor.cpp) ────────────────────────────────

    bool on_sample_editor() const { return s_.currentScreen == ScreenType::SAMPLE_EDITOR; }

    GestureResult sample_editor_screen(Gesture g);
    CursorContext sample_editor_context() const;
    bool          sample_editor_edit(const InputAction& action);

    // "ARE YOU SURE?" on B over a modified sample: A discards and leaves, B stays.
    bool sample_close_open() const { return on_sample_editor() && s_.sampleEditor.showConfirmClose; }
    GestureResult sample_close_layer(Gesture g);

    // START toggles the audition of the selection, with the FX row's effect on it (SYNC included).
    // The one audition a plain press does not silence.
    void sample_editor_audition();

    // Rows 3..8: the D-pad drags the selection instead of moving a cursor.
    bool on_sample_selection_row() const {
        return on_sample_editor() && s_.sampleEditor.cursorRow >= 3 && s_.sampleEditor.cursorRow <= 8;
    }

    // INSTRUMENT's EDIT button. Samplers only.
    void open_sample_editor();
    // B on an unmodified sample: free the undo, drop the scratch slots, go back.
    void close_sample_editor();

    // Build the editor's session from the sample the engine holds; separate because the editor's LOAD
    // re-enters on different audio. ⚠️ Must not touch `previousScreen` (the editor's return target).
    void init_sample_editor_state();

    // A+DPAD on rows 3..8 (why those rows have no CursorContext).
    void nudge_selection_edge(int64_t delta);

    // Row 11's POSITION cell: A+DPAD drags the boundary under the cursor. Col 0 keeps choosing the marker.
    bool on_sample_slice_marker_row() const {
        return on_sample_editor() && s_.sampleEditor.cursorRow == 11 &&
               s_.sampleEditor.cursorCol == 1 &&
               s_.sampleEditor.sliceMethod != SampleEditorModule::SLICE_OFF;
    }

    void nudge_slice_marker(int64_t delta);

    // Row 11 under MANUAL: a plain A is the chop tap, held by the mapper until release.
    // ⚠️ Every other gesture here starts with A held; a tap on the press would add an unasked boundary.
    bool on_slice_tap_cell() const {
        return on_sample_editor() && s_.sampleEditor.cursorRow == 11 &&
               s_.sampleEditor.sliceMethod == SampleEditorModule::SLICE_MANUAL;
    }

    // Put the selection on the slice row 11's cursor points at; every boundary gesture ends here.
    void select_current_slice();

    // Copy the method's computed boundaries into the hand-placed list, so a nudge has a home.
    void materialise_manual_markers();

    // A+B on row 11: the marker back where its method would put it.
    void reset_slice_marker();

    // A on row 11 under MANUAL: cut a boundary at the playhead — START, listen, tap on each hit.
    void tap_slice_marker();

    // RATE and BIT rebuild the buffer.
    void apply_sample_rate_and_bits();

    // A on rows 13/14/16/18/19 — the ops, the FX apply, the name, the save buttons.
    void sample_editor_confirm();

    // What SYNC would do to the sample now (NONE off SYNC, or when it already fits). APPLY and the
    // audition both ask here, so START plays what APPLY will write.
    songcore::SampleSync planned_sample_sync() const;

    // SYNC's APPLY: fit the sample to DURATION by RPITCH or TSTRETCH. The pending PITCH is spent.
    void apply_sample_sync();

    // ── The TSTRETCH box (ui/dispatch/layers/stretch_editor.cpp) ─────────────────────────────────

    bool stretch_editor_open() const { return stretch_editor_up(s_); }
    GestureResult stretch_editor_layer(Gesture g);
    void open_stretch_editor();
    void stretch_editor_edit(InputAction (*fn)(const CursorContext&));

    // Bake the pending pitch into the buffer and rescale everything in frames — every save calls it,
    // since a sample is saved as it sounds.
    void bake_pending_pitch();

    // The slice boundaries as WAV cue points: the markers, or DIVIDE's computed cuts.
    std::vector<int> compute_slice_cue_points() const;

    // Re-read length and waveform after an op. `reset_selection`: the length changed, so the selection,
    // the sample and loop windows and ⚠️ every slice marker are reset, not clamped.
    void refresh_sample_view(bool reset_selection);

    // SAVE / SAVE-AS / OVERWRITE: write the WAV, adopt it, leave the editor.
    void save_sample_to(const std::string& path, bool adopt_name);

    // CHOP: every slice to `Samples/Chops/<name>/` as its own WAV.
    void sample_editor_chop();

    // The slice (start, end) pairs the current method defines.
    std::vector<std::pair<int64_t, int64_t>> current_slices() const;

    // ⚠️ The audition's deferred half: the preview's window and stripped EQ, sends and mods are read
    // when the voice triggers, so they are restored later. A second START runs it at once (`force`),
    // or the first restore would undo the second.
    void run_due_sample_preview_restore(bool force = false);

    // What the editor's buttons leave for later (not drawn, so not in `s_.sampleEditor`).
    struct SamplePending {
        bool      previewRestore     = false;   // run_due_sample_preview_restore
        long long previewRestoreAtMs = 0;
        int       previewRestoreInst = 0;

        // The playhead when A went down on the tap cell (0..1, −1 = nothing sounding). Consumed by
        // `tap_slice_marker`, so a press that became a combo leaves nothing.
        float sliceTapPlayhead = -1.0f;
    };
    SamplePending samplePending_{};

    SampleEditorModule sample_{};
};

}  // namespace pt::ui
