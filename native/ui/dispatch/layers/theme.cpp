// The THEME EDITOR: a colour list raised from SETTINGS. It owns the buttons while it is up, but lets
// START through (the song plays while you dial) and SELECT (its cells have help).

#include "ui/dispatch/dispatch_common.h"

#include "ui/theme_io.h"         // .ptt — save_theme_file

#include <string>

namespace pt::ui {

GestureResult InputDispatcher::theme_layer(Gesture g) {
    ThemeEditorState& es = s_.themeEditor;
    switch (g) {
        // Both axes WRAP: UP/DOWN walk the rows, LEFT/RIGHT the row's own cells.
        case Gesture::DPAD_UP:    theme_move_cursor(-1, 0); break;
        case Gesture::DPAD_DOWN:  theme_move_cursor(+1, 0); break;
        case Gesture::DPAD_LEFT:  theme_move_cursor(0, -1); break;
        case Gesture::DPAD_RIGHT: theme_move_cursor(0, +1); break;

        // A+LEFT/RIGHT step the THEME row's palettes, or a channel by 0x01; A+UP/DOWN the same, by 0x10.
        case Gesture::A_UP:    theme_dpad_edit(+1, +0x10); break;
        case Gesture::A_DOWN:  theme_dpad_edit(-1, -0x10); break;
        case Gesture::A_LEFT:  theme_dpad_edit(-1, -0x01); break;
        case Gesture::A_RIGHT: theme_dpad_edit(+1, +0x01); break;

        // A means something only on the action rows (SAVE, LOAD, ROLL); colours are A+DPAD.
        case Gesture::A:
            if (theme_color_index(es.cursorRow) < 0) theme_row_action();
            break;

        // ⚠️ No "are you sure": the live theme is the applied theme.
        case Gesture::B: close_theme_editor(); break;

        // On a colour row: L+A locks it against a roll, R+A re-rolls it alone. R+A never asks: it is an
        // edit of the row under the cursor, like A+DPAD.
        case Gesture::L_A: {
            const int color = theme_color_index(es.cursorRow);
            if (color >= 0) es.locks.toggle(color);
            break;
        }
        case Gesture::R_A:
            if (theme_color_index(es.cursorRow) >= 0) theme_roll_palette(theme_color_index(es.cursorRow));
            break;

        case Gesture::START:
        case Gesture::SELECT:
            return GestureResult::PASS;

        default: break;
    }
    return GestureResult::TAKEN;
}
void InputDispatcher::theme_move_cursor(int d_row, int d_channel) {
    if (d_row != 0) {
        const int row = s_.themeEditor.cursorRow;
        const int max = ThemeEditorModule::max_row();
        s_.themeEditor.cursorRow = (d_row < 0) ? (row > 0 ? row - 1 : max)
                                               : (row < max ? row + 1 : 0);
    }
    // The channel count depends on the row (RANDOMIZE has two cells), so it is read after the row moves.
    const int last = theme_channel_count(s_.themeEditor.cursorRow) - 1;
    if (d_channel != 0) {
        const int ch = s_.themeEditor.cursorChannel;
        s_.themeEditor.cursorChannel = (d_channel < 0) ? (ch > 0 ? ch - 1 : last)
                                                       : (ch < last ? ch + 1 : 0);
    }
    if (s_.themeEditor.cursorChannel > last) s_.themeEditor.cursorChannel = last;

    theme_refresh_message();
}

/** A+DPAD in the theme editor, written once for all four directions so none forgets channel 1 is the
 *  style. */
void InputDispatcher::theme_dpad_edit(int cycleDelta, int nudge) {
    ThemeEditorState& es = s_.themeEditor;

    const int color = theme_color_index(es.cursorRow);
    if (color >= 0) {
        theme_adjust_color(s_.theme, color + 1, es.cursorChannel, nudge);
        s_.themeHandRows |= 1u << color;
        theme_refresh_message();
        return;
    }
    if (es.cursorRow == THEME_ROW_RANDOM) {
        // The scheme cell, a ring. ROLL is a button with nothing to dial.
        if (es.cursorChannel == 0) {
            const int cur = static_cast<int>(es.scheme);
            es.scheme = static_cast<ThemeScheme>(
                ((cur + cycleDelta) % THEME_SCHEME_COUNT + THEME_SCHEME_COUNT) % THEME_SCHEME_COUNT);
        }
        return;
    }
    // The THEME row: the NAME cell steps the Themes folder, wrapping; SAVE and LOAD are buttons.
    if (es.cursorChannel != 0) return;
    const PresetRowView v = preset_row_view(PresetRow::THEME);
    if (v.count == 0) return;
    // A palette in no file enters the ring at an end.
    const int at     = v.index >= 0 ? v.index : (cycleDelta >= 0 ? -1 : 0);
    const int target = ((at + (cycleDelta >= 0 ? 1 : -1)) % v.count + v.count) % v.count;
    preset_step(PresetRow::THEME, target);
}

/** Drop a failed roll's message. The clash line is derived in the draw, so only the event is cleared. */
void InputDispatcher::theme_refresh_message() { s_.themeEditor.message.clear(); }

void InputDispatcher::theme_request_roll() {
    // The rows the roll would replace: every unlocked one.
    uint32_t lost = 0;
    for (int i = 0; i < static_cast<int>(s_.themeEditor.locks.row.size()); ++i)
        if (!s_.themeEditor.locks.locked(i)) lost |= 1u << i;

    if ((s_.themeHandRows & lost) != 0 &&
        !preset_values_kept<Theme>(presetLists_.theme(true), s_.theme.name, s_.theme, same_theme)) {
        s_.presetStep = {PresetRow::THEME, std::string()};
        s_.confirm.open(ConfirmDialogState::Kind::PRESET_UNSAVED);
        return;
    }
    theme_roll_palette(-1);
}

/** One row alone (`only` ≥ 0) is far likelier to fail than the whole palette. */
void InputDispatcher::theme_roll_palette(int only) {
    ThemeEditorState& es = s_.themeEditor;
    const bool rowOnly = only >= 0;

    ThemeLocks locks = es.locks;
    if (rowOnly) {
        // "This row only" is "everything else locked", so there is one solver and one set of rules.
        for (size_t i = 0; i < locks.row.size(); ++i) locks.row[i] = (static_cast<int>(i) != only);
    }

    es.seed = es.seed * 1664525u + 1013904223u;
    const std::optional<Theme> rolled = theme_roll(s_.theme, locks, es.scheme, es.seed);
    if (!rolled) {
        es.message = rowOnly ? "ROW UNSOLVABLE" : "LOCKS UNSOLVABLE";   // the palette is left alone
        return;
    }
    s_.theme = *rolled;   // its name kept: the roll carries it
    // The rows rolled are no longer hand work; the locked ones keep what they were.
    for (int i = 0; i < static_cast<int>(locks.row.size()); ++i)
        if (!locks.locked(i)) s_.themeHandRows &= ~(1u << i);
    theme_refresh_message();
}

void InputDispatcher::theme_row_action() {
    if (s_.themeEditor.cursorRow == THEME_ROW_RANDOM) {
        // ROLL — a whole palette, in the scheme the cell beside it is showing.
        if (s_.themeEditor.cursorChannel == 1) theme_request_roll();
        return;
    }
    switch (s_.themeEditor.cursorChannel) {
        case 1:     // SAVE — name it, then write it
            preset_open_save(PresetRow::THEME);
            break;
        case 2: {   // LOAD — browse the Themes folder for a .ptt
            // The editor stays open under the browser (`theme_editor_up`), so B or a load comes back to
            // it with its locks, scheme and cursor.
            open_file_browser(AppState::BrowserPurpose::LOAD_THEME, browser_dir(BrowserDir::THEMES),
                              {"ptt"});
            break;
        }
        default:    // column 0 is the NAME; a bare A on it does nothing
            break;
    }
}

bool InputDispatcher::save_theme_as(const std::string& dir, const std::string& typed_text) {
    // Sanitized filename (empty → THEME.ptt), raw name inside.
    const std::string safe = safe_name(typed_text);
    const std::string file = (safe.empty() ? std::string("THEME") : safe) + ".ptt";
    const std::string path = dir + "/" + file;

    Theme to_save = s_.theme;
    if (!typed_text.empty()) to_save.name = typed_text;

    // The live theme takes the name it was saved under, so the row finds it in the folder.
    const bool ok = save_theme_file(fs_, path, to_save);
    if (ok) {
        s_.theme.name    = to_save.name;
        s_.themeHandRows = 0;
    }

    // ⚠️ A full card or a read-only mount must not report success.
    set_status(s_, ok ? "THEME SAVED" : "SAVE FAILED", ok);
    return ok;
}

}  // namespace pt::ui
