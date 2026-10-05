// The THEME EDITOR: a colour list raised from SETTINGS. It owns the buttons while it is up, but lets
// START through (the song plays while you dial) and SELECT (its cells have help).

#include "ui/dispatch/dispatch_common.h"

#include "ui/theme_io.h"         // .ptt — save_theme_file

#include <string>

namespace pt::ui {

LayerResult InputDispatcher::theme_layer(Gesture g) {
    ThemeEditorState& es = s_.themeEditor;
    switch (g) {
        // Both axes WRAP: UP/DOWN walk the rows, LEFT/RIGHT the row's own cells.
        case Gesture::DPAD_UP:    theme_move_cursor(-1, 0); break;
        case Gesture::DPAD_DOWN:  theme_move_cursor(+1, 0); break;
        case Gesture::DPAD_LEFT:  theme_move_cursor(0, -1); break;
        case Gesture::DPAD_RIGHT: theme_move_cursor(0, +1); break;

        // A+LEFT / A+RIGHT: the THEME row steps the built-in palettes; a colour channel moves by 0x01.
        // A+UP   / A+DOWN:  the palettes too; a colour channel moves by 0x10.
        case Gesture::A_UP:    theme_dpad_edit(+1, +0x10); break;
        case Gesture::A_DOWN:  theme_dpad_edit(-1, -0x10); break;
        case Gesture::A_LEFT:  theme_dpad_edit(-1, -0x01); break;
        case Gesture::A_RIGHT: theme_dpad_edit(+1, +0x01); break;

        // A means something only on the action rows (SAVE, LOAD, ROLL); colours are A+DPAD.
        case Gesture::A:
            if (theme_color_index(es.cursorRow) < 0) theme_row_action();
            break;

        // ⚠️ B closes with no "are you sure": the live theme IS the applied theme, and it survives the
        // close and the quit.
        case Gesture::B: close_theme_editor(); break;

        // On a colour row: L+A locks it against a roll, R+A re-rolls it alone.
        case Gesture::L_A: {
            const int color = theme_color_index(es.cursorRow);
            if (color >= 0) es.locks.toggle(color);
            break;
        }
        case Gesture::R_A:
            if (theme_color_index(es.cursorRow) >= 0) theme_roll_palette(/*rowOnly=*/true);
            break;

        case Gesture::START:
        case Gesture::SELECT:
            return LayerResult::PASS;

        default: break;
    }
    return LayerResult::TAKEN;
}
void InputDispatcher::theme_move_cursor(int d_row, int d_channel) {
    // Both axes wrap: a list of colours is a ring. The panel scrolls to follow the row.
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

/**
 * A+DPAD in the theme editor, which means three different things depending on the cell. Written once
 * and called from all four directions, so no edge can forget that channel 1 is the style.
 */
void InputDispatcher::theme_dpad_edit(int cycleDelta, int nudge) {
    ThemeEditorState& es = s_.themeEditor;

    const int color = theme_color_index(es.cursorRow);
    if (color >= 0) {
        theme_adjust_color(s_.theme, color + 1, es.cursorChannel, nudge);
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
    // The THEME row: the NAME cell steps the built-in palettes; SAVE and LOAD are buttons.
    if (es.cursorChannel == 0) theme_cycle_builtin(s_.theme, cycleDelta);
}

/** Drop a failed roll's message. The clash line is derived in the draw, so only the event is cleared. */
void InputDispatcher::theme_refresh_message() { s_.themeEditor.message.clear(); }

/** Roll the palette; `rowOnly` re-rolls one row with the others held (far likelier to fail). */
void InputDispatcher::theme_roll_palette(bool rowOnly) {
    ThemeEditorState& es = s_.themeEditor;

    ThemeLocks locks = es.locks;
    if (rowOnly) {
        // "This row only" is "everything else locked", so there is one solver and one set of rules.
        const int target = theme_color_index(es.cursorRow);
        for (size_t i = 0; i < locks.row.size(); ++i) locks.row[i] = (static_cast<int>(i) != target);
    }

    es.seed = es.seed * 1664525u + 1013904223u;
    const ThemeRollResult r = theme_roll(s_.theme, locks, es.scheme, es.seed);

    if (!r.ok) {
        // ⚠️ The palette is left alone: a half-legal best attempt is worse than no change.
        es.message = rowOnly ? "ROW UNSOLVABLE" : "LOCKS UNSOLVABLE";
        return;
    }

    const std::string keepName = s_.theme.name;
    s_.theme      = r.theme;
    s_.theme.name = keepName;
    theme_refresh_message();
}

/**
 * A theme name as a FILENAME: anything outside [A-Za-z0-9_] becomes `_`. ⚠️ An empty name stays empty —
 * both callers apply the "THEME" fallback, since `.ptt` would be a dotfile the browser skips.
 */
static std::string sanitize_theme_filename(const std::string& name) {
    std::string out;
    out.reserve(name.size());
    for (const char c : name) {
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                        (c >= '0' && c <= '9') || c == '_';
        out += ok ? c : '_';
    }
    return out;
}

void InputDispatcher::theme_row_action() {
    if (s_.themeEditor.cursorRow == THEME_ROW_RANDOM) {
        // ROLL — a whole palette, in the scheme the cell beside it is showing.
        if (s_.themeEditor.cursorChannel == 1) theme_roll_palette(/*rowOnly=*/false);
        return;
    }
    switch (s_.themeEditor.cursorChannel) {
        case 1: {   // SAVE — name it, then write it
            // ⚠️ The keyboard opens WITHOUT closing the editor, hence QWERTY above THEME in the layer
            // stack. Seeded with the sanitized name — what the file will be called.
            const std::string seed = sanitize_theme_filename(s_.theme.name);
            open_qwerty(QwertyContext::THEME_SAVE, seed.empty() ? "THEME" : seed, "SAVE THEME:",
                        fs_.themes_directory(), /*max_length=*/20, /*clear_on_first_b=*/true);
            break;
        }
        case 2: {   // LOAD — browse the Themes folder for a .ptt
            // ⚠️ LOAD closes the editor first: the browser is a SCREEN, and a modal left on top would
            // swallow its D-pad. `browser_confirm` re-opens the editor when a theme lands.
            close_theme_editor();
            open_file_browser(AppState::BrowserPurpose::LOAD_THEME, browser_dir(BrowserDir::THEMES),
                              {"ptt"});
            break;
        }
        default:    // column 0 is the NAME; a bare A on it does nothing
            break;
    }
}

void InputDispatcher::save_theme_as(const std::string& dir, const std::string& typed_text) {
    // ⚠️ Two names from one typed string: the FILENAME is sanitized ("My Theme!" → My_Theme_.ptt, so it
    // survives FAT32); the name IN the file stays raw. An empty field keeps the current name and falls
    // back to "THEME" for the file — never `.ptt`, a dotfile the browser hides.
    const std::string safe = sanitize_theme_filename(typed_text);
    const std::string file = (safe.empty() ? std::string("THEME") : safe) + ".ptt";
    const std::string path = dir + "/" + file;

    Theme to_save = s_.theme;
    if (!typed_text.empty()) to_save.name = typed_text;

    // ⚠️ The live theme does NOT adopt the saved name: the built-in cycle keys off the name, and a
    // custom one would change where A+RIGHT lands. Loading the file back does set it.
    const bool ok = save_theme_file(fs_, path, to_save);

    // ⚠️ A failed save must say so — a full card or read-only mount would otherwise report success.
    s_.statusMessage = ok ? "THEME SAVED" : "SAVE FAILED";
    s_.statusSuccess = ok;

    open_theme_editor();
}

}  // namespace pt::ui
