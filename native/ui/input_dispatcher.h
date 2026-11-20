#pragma once

// Every button and combo, for every screen. The generic handlers (ui/cursor.h) turn a press into an
// edit of the cell under the cursor; what lives here is the rest — selection, clipboard, cycling,
// cloning, the FX helper, previews. SDL input → `button_mapper.h` (which handler) → here (what it does).
//
// ⚠️ The modal rule: whatever sits over the screen (a load, a dialog, the keyboard, an editor, a picker)
// owns the buttons while up. Their order is written once, in `OVERLAYS` (ui/overlays.h); a gesture the
// top layer does not answer is swallowed there. Then the screen's row in `SCREENS` is asked.
// The THEME and EQ editors let START and SELECT through, so a band can be swept across an audition.
//
// The clock is injected (`set_now()` once a frame) so tests can move time.

#include "songcore/host.h"
#include "songcore/midi_map.h"
#include "ui/app_state.h"
#include "ui/autosave.h"
#include "ui/clipboard.h"
#include "ui/cursor.h"
#include "ui/dispatch/gesture.h"
#include "ui/grid_cursor.h"
#include "ui/overlays.h"
#include "ui/song_pointer.h"     // NAV = SONG — the ctor clamps the pointer onto a real cell
#include "ui/filesystem.h"
#include "ui/midi_ports.h"
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
#include "ui/preset_lists.h"
#include "ui/project_actions.h"
#include "ui/sample_edit_session.h"
#include "ui/tap_tempo.h"

#include <functional>

#include <string>
#include <utility>
#include <vector>

namespace pt::ui {

class InputDispatcher {
  public:
    // A test points `fs` at a temp directory.
    InputDispatcher(AppState& state, songcore::SongcoreHost& host, FileSystem& fs)
        : s_(state), host_(host), fs_(fs) {
        s_.clipboard = &clip_;   // the layout's clipboard readout
        s_.cellUnderCursor = [this] { return top_overlay(s_) ? cc::none() : cursor_context(); };
        // The project the shell pushed before this existed needs its song pointer clamped.
        clamp_song_pointer(s_);
    }

    // The frame's clock, once per frame before the events. Runs what is due: the sample editor's
    // audition restore, the autosave, the status lines' auto-dismiss.
    void set_now(long long now_ms);

    // The folder-stepped preset rows (`AppState::presetRows`): where the values on screen sit in their
    // folder's list, read again when the screen changed. Once a frame.
    void poll_preset_rows();

    // Will timed work change the screen on its own? The shell skips drawing idle frames.
    // ⚠️ It goes false on the frame that must be drawn: the caller owes the active→idle edge a redraw.
    // A new screen-changing timer belongs in this expression.
    bool has_pending_timed_work() const {
        return statusDismissAtMs_ != 0 || browserStatusDismissAtMs_ != 0 || autosave_.pending();
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

    // Once per install: ask whether the top panel should show the guide. Call after `boot_recovery()`
    // — where that one is asking, this waits for its answer.
    void boot_ask_top_panel();

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
    // B and L likewise, for the guide; R's is `on_r_held`.
    void on_b_held(bool down) { s_.bHeld = down; }
    void on_l_held(bool down) { s_.lHeld = down; }

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
    // A channel is on or muted, nothing else. R+B flips the cursor's channel, or every one in the
    // selection. On PHRASE and TABLE it switches commands off instead (`toggle_fx_off`).
    void on_r_b();
    // R+A: the same targets on, the rest of their kind muted — or, pressed on exactly that, all on.
    void on_r_a();
    // R down or up — the arming half of MIDI learn ("hold R, turn a knob").
    void on_r_held(bool down);
    // Another button went down while R is held: a combo, so this hold never learns.
    void on_r_combo_press();

    // R came up first: the chord's changes stand.
    void on_r_combo_commit();
    // A/B came up first: every mute back the way the chord found it.
    void on_r_combo_revert();

    // ── L: selection and the clipboard ───────────────────────────────────────────────────────────
    // L+B: enter selection, then widen it CELL → ROW → SCREEN on each press.
    void on_l_b();
    // L+A: cut inside a selection, paste outside one.
    void on_l_a();
    // L+R: leave selection mode.
    void on_l_r();
    // After an L+R: is there still a mix, a switched-off command or a selection for the next one to
    // clear? The buffer does not count. False where a screen of its own took it.
    bool l_r_has_more();
    // L+B+A: deep-clone the chain/phrase under the cursor into free slots.
    void on_l_b_a();
    // The same played one button at a time: L held, B, then A with B already let go. Where the screen
    // has no clone it is L+A.
    void on_l_b_then_a();
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
    // Bare SELECT raises the full help and aborts the keyboard. Called on the release,
    // only when nothing else was pressed meanwhile.
    void on_select();

    // A button went down with the help overlay up: put it away.
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
        return host_.live_mode() && s_.currentScreen == ScreenType::SONG && !top_overlay(s_);
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
    // R+B is a chord (a B a frame ahead of its R must still become the chord).
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

    // How many EFFECTS an FX cell can reach in this build (the MIDI tail may be hidden).
    // ⚠️ The cell's bound and the picker's size both come from here, or the picker meets an unnamed effect.
    int visible_effect_type_count() const;

    // What a render needs from the shell. The render is synchronous: the frame loop stops for it.
    // Both may be empty (the tests render with neither).
    struct RenderHooks {
        std::function<void(bool)> suspend_audio;
        std::function<void()>     repaint;   // draws the progress

        // Drains the platform's events during a load or a render; true when the user cancelled.
        // ⚠️ Every press is consumed, or SDL would replay them onto the next screen. It is also how
        // the window stays answering and the trip to the background and back is seen.
        std::function<bool()> pump;
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

    // A load answers no button: the pump reads the cancel itself.
    GestureResult loading_layer(Gesture) { return GestureResult::TAKEN; }

    // The shell opens the ports through it at boot.
    MidiPorts& midi_ports() { return midiPorts_; }

    // The app is back in front: re-list the file browser if it is up — other apps (and Android's folder
    // picker) change directories meanwhile. Only Android and iOS send the event.
    void refresh_browser_on_foreground();

  private:
    AppState&               s_;
    songcore::SongcoreHost& host_;
    FileSystem&             fs_;
    long long               now_ms_ = 0;
    TapTempo                tapTempo_{};

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

    Autosave  autosave_{host_, fs_};
    MidiPorts midiPorts_{s_, host_};

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
        bool dry = false;
    };
    MixSnapshot mixSnapshot_{};

    // The channels a chord applies to (0-7 tracks, 8 REV, 9 DEL): the selection's columns on SONG,
    // else the cursor's.
    void mute_solo_targets(int (&out)[8], int& count) const;
    void toggle_mute_solo(bool solo);
    // Every track audible again (L+R's mute rung).
    void restore_full_playback();
    // Is any channel muted?
    bool mix_touched();
    // The last L+R went to a screen's or layer's own handler.
    bool lrRouted_ = false;
    // Do R+A/R+B mean MUTE/SOLO here — and so must B be held?
    bool mute_solo_chord_live() const;

    // ── R+B on PHRASE and TABLE: switch commands off, held like the MUTE chord ──
    // The undo is the whole phrase's or table's off flags, as the chord found them.
    struct FxOffSnapshot {
        bool live  = false;
        bool table = false;
        int  id    = -1;
        bool off[16][3] = {};
    };
    FxOffSnapshot fxOffSnapshot_{};
    bool fx_off_chord_live() const;
    // Flips every command under the cursor, or in the selection.
    void toggle_fx_off();
    // L+R's rung for them: the open phrase or table only, never another one.
    bool open_grid_has_fx_off() const;
    void clear_open_grid_fx_off();
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
    CursorPosition insertAt_{};

    // ── The spine ────────────────────────────────────────────────────────────────────────────────

    // "What is under the cursor?", asked of the current screen's row in `SCREENS`.
    CursorContext cursor_context() const;

    // Apply a resolved action to the document. True if anything changed; a SET of the value already
    // in the cell is not applied at all.
    bool apply_edit(const InputAction& action);

    // Context → action → mutate → echo to the engine; `fn` is one of the generic handlers.
    void generic_input(InputAction (*fn)(const CursorContext&));

    // The same over a selection when one is up: the cursor's column, or every selected one where the
    // grid says so. Only filled cells step; when every covered cell is empty, each gets what A and the
    // step give one empty cell, so the run starts equal.
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
    // The cursor's FX slot (1-3) on PHRASE or TABLE, from either of its cells; 0 elsewhere.
    int  fx_slot_at_cursor() const;
    // After an edit: the command in the cursor's FX slot is what a plain A lays down next.
    void remember_fx_at_cursor();
    // A on an empty FX command cell: lay down the last command entered. `peek`: would it? (The mapper
    // holds this A for the release, so A+UP/DOWN still opens the list.)
    bool fx_insert_last(bool peek);
    // The write alone, under a selection too; marks nothing.
    bool lay_last_fx(bool peek);
    // Holds nothing: an empty cell, a command's cell with no command in its slot, PHRASE's V or I
    // with no note.
    bool cursor_cell_empty() const;

    // ── The mapping destination picker (ui/dispatch/layers/map_picker.cpp) ──────────────────────
    GestureResult map_picker_layer(Gesture g);
    void open_map_picker();
    // On a mapping row's GROUP or PARAMETER cell (not the ADD row)?
    bool on_map_dest_cell() const;
    void apply_map_picker_choice();

    // ── The instrument list over a phrase's instrument cell (ui/dispatch/layers/instrument_picker.cpp)
    GestureResult instrument_picker_layer(Gesture g);
    bool on_phrase_instrument_cell() const;
    void open_instrument_picker();
    // The same list for a SoundFont's PATCH row: the presets in its file.
    GestureResult sf_preset_picker_layer(Gesture g);
    void open_sf_preset_picker();
    // And for MACRO's MODEL row: every model with its note.
    GestureResult macro_model_picker_layer(Gesture g);
    void open_macro_model_picker();

    // ── The layer stack ──────────────────────────────────────────────────────────────────────────
    // A gesture a layer does not answer is swallowed, so a new layer is one row in `OVERLAYS` plus its
    // case here.
    GestureResult layer_handle(Overlay o, Gesture g);

    // One screen: its buttons, the cell under its cursor, an edit there, and what a knob would map
    // (null: nothing).
    struct ScreenHandler {
        ScreenType id;
        GestureResult (InputDispatcher::*handle)(Gesture);
        CursorContext (InputDispatcher::*context)() const;
        bool (InputDispatcher::*edit)(const InputAction&);
        songcore::MapTarget (InputDispatcher::*knob)() const;
    };
    static const ScreenHandler SCREENS[];

    const ScreenHandler* screen() const;   // the current screen's row, or null
    const GridCursor* grid() const { return grid_of(s_.currentScreen); }   // null: no selection

    // Offer `g` to the top layer, then the screen. True when one took it and the gesture's body must
    // not run.
    bool route(Gesture g);

    // The confirm dialog: A and B answer it; everything else is inert but the audition's stop.
    GestureResult confirm_layer(Gesture g);

    void confirm_accept();

    // B on the dialog. ⚠️ For RECOVER it means "discard my work" and deletes the autosave, or the
    // prompt would return on every launch.
    void confirm_cancel();

    // The top panel's question (`boot_ask_top_panel`): owed until no other question is up, and its
    // answer is written to settings.json at once.
    void ask_top_panel_if_owed();
    void top_panel_answered();
    bool topPanelAskOwed_ = false;

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

    // …with no keyboard or EQ editor over it.
    bool send_editor_on_top() const {
        const OverlayRow* top = top_overlay(s_);
        return top && top->id == Overlay::SEND;
    }
    GestureResult send_editor_layer(Gesture g);

    void open_send_editor(EffectsSection section);
    void close_send_editor() { s_.sendEditor = SendEditorState{}; }

    // A+DPAD and A+B, through the editor's own cursor.
    void send_editor_edit(InputAction (*fn)(const CursorContext&));

    // ── The theme editor (ui/dispatch/layers/theme.cpp), over SETTINGS ──
    // ⚠️ SAVE raises the keyboard on top of it, so QWERTY sits above THEME in the layer stack.

    GestureResult theme_layer(Gesture g);

    // The roll's seed starts from the clock, so each opening walks a different sequence.
    void open_theme_editor() {
        s_.themeEditor = ThemeEditorState{};
        s_.themeEditor.isOpen = true;
        s_.themeEditor.seed ^= static_cast<uint32_t>(now_ms_);
    }
    void close_theme_editor() { s_.themeEditor = ThemeEditorState{}; }

    // UP/DOWN walk the rows, LEFT/RIGHT the row's channels; both wrap.
    void theme_move_cursor(int d_row, int d_channel);

    // A on the THEME row: 1 = roll a palette, 2 = SAVE (the keyboard), 3 = LOAD (the browser).
    void theme_row_action();

    // A+DPAD: the built-in cycle, the style ring, or a colour channel — one per cell.
    void theme_dpad_edit(int cycleDelta, int nudge);

    // Generate a palette; `rowOnly` keeps every row but the cursor's.
    // Roll the palette, or only colour row `only` (−1: every unlocked one).
    void theme_roll_palette(int only);
    // ROLL: the whole palette, asking first when it would drop colours dialled by hand that no file holds.
    void theme_request_roll();

    // Drop a failed roll's message.
    void theme_refresh_message();

    // Write `<dir>/<name>.ptt` (the keyboard's THEME_SAVE).
    // ⚠️ `dir` is a parameter because `qwerty_apply()` clears the keyboard state before dispatching.
    bool save_theme_as(const std::string& dir, const std::string& typed_text);

    // NEW, with the engine sync a fresh project needs.
    void start_new_project();

    // Slot 0 across the board, and no selection. Shared by NEW and LOAD.
    void reset_editing_context();

    // A .ptp replaced the document: clean, no selection, browser closed.
    void load_project_done(const std::string& path);

    // EXPORT, synchronously; the render hook repaints the progress from inside it.
    void export_song(bool stems);
    void paint_progress();

    // COLLECT into Songs/<folder>, its percentage on the PROJECT screen while it copies.
    void collect_song(const std::string& folder);

    // ── The RENDER dialog (ui/modules/render_dialog.h) ──────────────────────────────────────────

    // PROJECT → EXPORT → MIX / STEMS. The range starts on the SONG cursor's section, SONG END on AUTO.
    // REPEAT is kept between openings.
    void open_render_dialog(RenderDialogState::Output output);

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
    // A: open a folder, go up, or load the file — per `browserPurpose`. In a SELECT mode, the YES.
    void browser_confirm();
    void browser_confirm_delete();
    void browser_confirm_root();
    // The file `browserPurpose` opened the browser for.
    void browser_load(const BrowserItem& item);
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
    void qwerty_cancel() {
        s_.qwerty     = QwertyKeyboardState{};
        s_.presetStep = {};   // ABORT on PRESET NOT SAVED's SAVE drops the step too
    }

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

    // A on the TEMPO row, in time: the tempo of the last few taps. The first tap sets nothing.
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

    // A+DPAD on TYPE: an untouched slot switches at once and afresh; any other asks DISCARD / KEEP /
    // CANCEL. `delta` rides through the dialog in `ConfirmDialogState::step`.
    void request_instrument_type_toggle(int delta);
    void toggle_instrument_type(int delta, bool keepSettings);
    bool instrument_is_untouched() const;

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
    using ClipFn = int (InputDispatcher::*)(ClipOp, const SelectionBounds&);   // the cells changed
    int  song_clip(ClipOp op, const SelectionBounds& b);
    int  chain_clip(ClipOp op, const SelectionBounds& b);
    int  phrase_clip(ClipOp op, const SelectionBounds& b);
    int  table_clip(ClipOp op, const SelectionBounds& b);

    // What the four share: L+B marks a selection; inside one B copies, A+B deletes, L+A cuts.
    // Outside one L+A pastes into item `pasteTarget`, and A+B is the cell's own delete.
    GestureResult grid_clipboard(Gesture g, ClipFn clip, int pasteTarget);

    // A on an empty cell: insert the item last edited, and arm A,A. On PHRASE, the NOTE column only;
    // A on a filled note auditions it.
    void song_insert();
    void chain_insert();
    void phrase_insert();
    // What that A writes into the cursor's empty cell (PHRASE: a note, or a command), under a selection
    // too; marks nothing. False where A lays nothing or the cell is filled.
    bool lay_last_at_cursor();
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
    // B+UP/DOWN under NAV = SONG: CHAIN walks the song column, PHRASE the chain's filled rows. False off
    // NAV = SONG (the pool pages 16 instead).
    bool song_relative_b_vertical(int delta);

    // LIVE on SONG: START launches the cursor's cell, L+START its row, R+START stops its track. A second
    // press promotes the launch from the chain's end to the next phrase boundary.
    void live_launch_cell();
    void live_launch_row();
    void song_launch_row();   // L+START outside LIVE
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
    bool save_scale_as(const std::string& dir, const std::string& typed_text);

    // A on GROOVE's panel: the SAVE and LOAD cells only.
    void groove_row_action();

    // Write `<dir>/<name>.ptg` (the keyboard's GROOVE_SAVE).
    bool save_groove_as(const std::string& dir, const std::string& typed_text);

    // ── The folder-stepped preset rows (ui/dispatch/preset_rows.cpp) ─────────────────────────────

    // Step `row` to entry `index` of its list — or, when no file holds the values on screen, open
    // PRESET NOT SAVED and hold the step. True when the project changed.
    bool preset_step(PresetRow row, int index);
    // Would a step lose nothing? The values are in a file (or are an untouched slot).
    bool preset_kept(PresetRow row);
    // Load the file at `path` into `row`. True when the project changed.
    bool preset_apply(PresetRow row, const std::string& path);
    // What PRESET NOT SAVED held: a step, or THEME's ROLL. True when the project changed.
    bool preset_carry_out(const AppState::PendingPresetStep& step);
    // The row's own SAVE keyboard.
    void preset_open_save(PresetRow row);
    // After SAVE's keyboard wrote `saved`'s file: carry out the step the dialog held, if it was this row's.
    void preset_step_after_save(const AppState::PendingPresetStep& step, PresetRow saved);
    // A file into the row's values, as LOAD does; false when it does not parse. `changed`: did they move?
    bool load_preset_file(PresetRow row, const std::string& path, bool* changed = nullptr);
    // The row as drawn, from the lists as last read.
    PresetRowView preset_row_view(PresetRow row);

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
    bool save_reverb_preset_as(const std::string& dir, const std::string& typed_text);

    // Write `<dir>/<name>.ptd` (the keyboard's DELAY_SAVE).
    bool save_delay_preset_as(const std::string& dir, const std::string& typed_text);

    // ── The sample editor (ui/dispatch/screens/sample_editor.cpp) ────────────────────────────────
    // What its buttons do to the sample is `sampleSession_`'s; the screen, the keyboard and the browser
    // it opens are here.

    bool on_sample_editor() const { return s_.currentScreen == ScreenType::SAMPLE_EDITOR; }

    GestureResult sample_editor_screen(Gesture g);
    CursorContext sample_editor_context() const;
    bool          sample_editor_edit(const InputAction& action);

    // "ARE YOU SURE?" on B over a modified sample: A discards and leaves, B stays.
    GestureResult sample_close_layer(Gesture g);

    // SELECTION: the D-pad drags the selection instead of moving a cursor.
    bool on_sample_selection_row() const {
        return on_sample_editor() && s_.sampleEditor.cursorRow == SampleRow::SELECTION;
    }

    // INSTRUMENT's EDIT button. Samplers only.
    void open_sample_editor();

    // SLICE_AT's POSITION cell: A+DPAD drags the boundary under the cursor. Col 0 keeps choosing the marker.
    bool on_sample_slice_marker_row() const {
        return on_sample_editor() && s_.sampleEditor.cursorRow == SampleRow::SLICE_AT &&
               s_.sampleEditor.cursorCol == 1 &&
               s_.sampleEditor.sliceMethod != SampleEditorModule::SLICE_OFF;
    }

    // SLICE_AT under MANUAL: a plain A is the chop tap, held by the mapper until release.
    // ⚠️ Every other gesture here starts with A held; a tap on the press would add an unasked boundary.
    bool on_slice_tap_cell() const {
        return on_sample_editor() && s_.sampleEditor.cursorRow == SampleRow::SLICE_AT &&
               s_.sampleEditor.sliceMethod == SampleEditorModule::SLICE_MANUAL;
    }

    // A on SLICE_AT, the op rows, FX, NAME and FILE — the tap, the ops, the apply, the name, the save buttons.
    void sample_editor_confirm();

    // ── The TSTRETCH box (ui/dispatch/layers/stretch_editor.cpp) ─────────────────────────────────

    GestureResult stretch_editor_layer(Gesture g);
    void open_stretch_editor();
    void stretch_editor_edit(InputAction (*fn)(const CursorContext&));

    SampleEditSession  sampleSession_{s_, host_, fs_, [this] { mark_modified(); }};
    SampleEditorModule sample_{};

    PresetLists presetLists_{fs_};
};

}  // namespace pt::ui
