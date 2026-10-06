// PROJECT and SETTINGS: their buttons, tap tempo, and the B that leaves SETTINGS and the MIDI screens.

#include "ui/dispatch/dispatch_common.h"

#include "ui/navigation.h"

#include <algorithm>

namespace pt::ui {

// ─── The buttons ─────────────────────────────────────────────────────────────────────────────────

GestureResult InputDispatcher::project_screen(Gesture g) {
    switch (g) {
        // The NAME row opens the keyboard on the release; on every other row A is a button.
        case Gesture::A:
            if (!open_sub_screen_at_cursor(/*peek=*/false)) project_action();
            return GestureResult::TAKEN;

        // ⚠️ Tap tempo: a second A inside 300 ms (= 200 BPM) arrives here, not as A. Without this every
        // other fast tap would be lost to the double-tap path and the tempo would halve.
        case Gesture::A_A:
            if (s_.projectCursorRow != static_cast<int>(ProjectRow::TEMPO) ||
                s_.projectCursorColumn != 2)
                return GestureResult::PASS;
            tap_tempo();
            return GestureResult::TAKEN;

        default:
            return GestureResult::PASS;
    }
}

GestureResult InputDispatcher::settings_screen(Gesture g) {
    switch (g) {
        case Gesture::A: settings_action();                  return GestureResult::TAKEN;
        case Gesture::B: leave_to(s_.settingsReturnScreen);  return GestureResult::TAKEN;
        default:         return GestureResult::PASS;
    }
}

// SETTINGS, MIDI and the mapping list are not on the nav grid's main row, so B takes you back to the
// screen stored on the way in. ⚠️ Without leaving the selection: B in one copies, and these screens have
// no clipboard.
void InputDispatcher::leave_to(ScreenType back) {
    s_.selection.exit();
    NavResult nav;
    nav.screen = back;
    nav.column = s_.previousColumn;   // these screens own no column — keep the one they came in with
    go_to_screen(s_, nav);            // not a bare assignment: cursors are saved/restored here
}

// ─── What A does ─────────────────────────────────────────────────────────────────────────────────

void InputDispatcher::project_action() {
    switch (static_cast<ProjectRow>(s_.projectCursorRow)) {
        case ProjectRow::NAME:
            // ⚠️ Empty on purpose: NAME is a deferred cell, already handled by
            // `open_sub_screen_at_cursor`. A silent `default:` would invite a second opener.
            break;

        case ProjectRow::PROJECT:
            switch (s_.projectCursorColumn) {
                case 1: {   // SAVE
                    before_save();
                    const ActionResult r = save_project(host_, fs_, s_);
                    s_.statusMessage = r.message;
                    s_.statusSuccess = r.ok;
                    break;
                }
                case 2:     // LOAD
                    open_file_browser(AppState::BrowserPurpose::LOAD_PROJECT,
                                      browser_dir(BrowserDir::PROJECTS), {"ptp"});
                    break;
                case 3:     // NEW
                    // Only ask if there is something to lose.
                    if (s_.project_dirty()) s_.confirm.open(ConfirmDialogState::Kind::NEW_PROJECT);
                    else                    start_new_project();
                    break;
                default: break;
            }
            break;

        case ProjectRow::EXPORT:
            // Neither button renders directly: both open the RENDER panel, and which was pressed
            // picks stereo WAV or stems.
            if (s_.projectCursorColumn == 1)      open_render_dialog(RenderDialogState::Output::MIX);
            else if (s_.projectCursorColumn == 2) open_render_dialog(RenderDialogState::Output::STEMS);
            break;

        case ProjectRow::COMPACT:
            if (s_.projectCursorColumn == 1)
                s_.confirm.open(ConfirmDialogState::Kind::CLEAN_SEQ);
            else if (s_.projectCursorColumn == 2)
                s_.confirm.open(ConfirmDialogState::Kind::CLEAN_INST);
            break;

        case ProjectRow::SYSTEM: {
            // A shortcut into SETTINGS, which owns no column, so R+UP later returns to the main-row
            // screen you were on. B's way out is captured separately (see AppState::settingsReturnScreen).
            s_.settingsReturnScreen = s_.currentScreen;
            NavResult nav;
            nav.screen = ScreenType::SETTINGS;
            nav.column = s_.previousColumn;
            go_to_screen(s_, nav);
            break;
        }

        case ProjectRow::MIDI: {
            // ⚠️ The row is hidden where the build hides MIDI, and PROJECT is the MIDI screen's only
            // door — so this guard is the real gate, turning back a stale cursor or a future caller.
            if (!s_.caps.midi) break;

            // Like SYSTEM above, minus the nav grid; B is the only way back. ⚠️ Both port lists are
            // enumerated here, on the way in — a port list is only true at the moment it is read.
            refresh_midi_devices();
            refresh_midi_in_devices();
            s_.midiStatusText.clear();   // last visit's "TEST SENT" is not this visit's news
            s_.midiReturnScreen = s_.currentScreen;
            NavResult nav;
            nav.screen = ScreenType::MIDI;
            nav.column = s_.previousColumn;
            go_to_screen(s_, nav);
            break;
        }

        case ProjectRow::EXIT:
            // ⚠️ Only where the platform can exit, and it still asks like NEW: the dialog is the app's
            // one deliberate way to discard a session, and its YES the one clean death.
            if (!s_.caps.appExit) break;
            if (s_.project_dirty()) s_.confirm.open(ConfirmDialogState::Kind::EXIT);
            else                    s_.shouldQuit = true;
            break;

        // TAP — the TEMPO row's second cell, and A on it alone.
        //
        // ⚠️⚠️ The column guard is the feature: the mapper fires plain A on A's own press, so every
        // A+UP used to nudge the BPM was also counted as a tap.
        case ProjectRow::TEMPO:
            if (s_.projectCursorColumn == 2) tap_tempo();
            break;

        // TRANSPOSE is an A+DPAD cell; plain A does nothing.
        default:
            break;
    }
}

void InputDispatcher::tap_tempo() {
    const long long now = now_ms_;

    // A gap this long is a pause, not a beat — start counting again from this tap.
    if (tapTempoLastMs_ == 0 || now - tapTempoLastMs_ > TAP_TEMPO_TIMEOUT_MS) {
        tapTempoLastMs_ = now;
        tapTempoCount_  = 0;
        return;
    }

    const long long gap = now - tapTempoLastMs_;
    // A bounce: keep the anchor, so the next tap measures from the last real one.
    if (gap < TAP_TEMPO_MIN_MS) return;
    tapTempoLastMs_ = now;

    // Shift the ring, newest last; four entries is cheaper than a write cursor.
    for (int i = TAP_TEMPO_KEEP - 1; i > 0; --i) tapTempoGaps_[i] = tapTempoGaps_[i - 1];
    tapTempoGaps_[0] = gap;
    if (tapTempoCount_ < TAP_TEMPO_KEEP) ++tapTempoCount_;

    long long sum = 0;
    for (int i = 0; i < tapTempoCount_; ++i) sum += tapTempoGaps_[i];
    const long long meanMs = sum / tapTempoCount_;
    if (meanMs <= 0) return;

    // Rounded, not truncated, so a 500 ms mean reads 120 and not 119.
    const int bpm = static_cast<int>((60000 + meanMs / 2) / meanMs);

    Project& p = host_.edit_project();
    const int clamped = std::min(999, std::max(20, bpm));
    if (p.tempo == clamped) return;   // no edit, so no dirty bump
    p.tempo = clamped;
    mark_modified();
}

void InputDispatcher::settings_action() {
    switch (static_cast<SettingsRow>(s_.settingsCursorRow)) {
        case SettingsRow::THEME:
            // A opens the theme editor.
            open_theme_editor();
            break;

        case SettingsRow::TEMPLATE: {
            if (s_.settingsCursorColumn != 1 && s_.settingsCursorColumn != 2) break;
            const ActionResult r = (s_.settingsCursorColumn == 1) ? save_template(host_, fs_)
                                                                  : clear_template(fs_);
            s_.statusMessage = r.message;
            s_.statusSuccess = r.ok;
            break;
        }

        // Every other row is a value, changed with A+DPAD; single A is for actions only.
        default:
            break;
    }
}

// ─── The cursor and the edit ─────────────────────────────────────────────────────────────────────

CursorContext InputDispatcher::project_context() const {
    const Project& p = *s_.project;
    ProjectState prs{p};
    prs.cursorRow    = s_.projectCursorRow;
    prs.cursorColumn = s_.projectCursorColumn;
    prs.caps         = s_.caps;
    return project_.cursor_context(prs);
}

bool InputDispatcher::project_edit(const InputAction& action) {
    Project& p = host_.edit_project();
    return project_
        .handle_input(p, s_.projectCursorRow, s_.projectCursorColumn, action)
        .modified;
}

CursorContext InputDispatcher::settings_context() const {
    SettingsState ss{s_.settings};
    ss.cursorRow    = s_.settingsCursorRow;
    ss.cursorColumn = s_.settingsCursorColumn;
    ss.caps         = s_.caps;
    ss.theme        = s_.theme;   // VISUALIZER's value lives on the theme
    return settings_.cursor_context(ss);
}

// ⚠️ SETTINGS edits the SETTINGS, not the project — `false`, so no mark_modified(): a visualizer
// change must not make a song dirty or prompt at NEW / EXIT. The shell writes settings.json.
bool InputDispatcher::settings_edit(const InputAction& action) {
    const bool navBefore = s_.settings.navSongRelative;
    settings_.handle_input(s_.settings, s_.theme, s_.caps, s_.settingsCursorRow,
                           s_.settingsCursorColumn, action);
    // ⚠️ Turning NAV on must land the pointer on a real cell, or the first R+RIGHT is refused
    // silently.
    if (!navBefore && s_.settings.navSongRelative) clamp_song_pointer(s_);
    return false;
}

}  // namespace pt::ui
