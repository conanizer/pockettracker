// The THEME, SCALE and GROOVE screens: what A does on their action rows, and saving to a file.

#include "ui/dispatch/dispatch_common.h"

#include "ui/groove_io.h"        // .ptg — save_groove_file / load_groove_file / the factory seed
#include "ui/scale_io.h"         // .pts — save_scale_file / load_scale_file / the factory seed
#include "ui/theme_io.h"         // .ptt — save_theme_file / load_theme_file

#include <algorithm>
#include <string>

namespace pt::ui {

// ─── The THEME EDITOR ─────────────────────────────────────────────────────────────────────────────

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

// ─── The SCALE screen's NAME row ─────────────────────────────────────────────────────────────────

void InputDispatcher::scale_row_action() {
    const songcore::Scale& scale =
        host_.project().scales[static_cast<size_t>(s_.currentScale)];

    switch (scale_name_action(s_.scaleCursorRow, s_.scaleCursorColumn)) {
        case ScaleNameAction::SAVE: {
            // Seeded with the sanitized DISPLAY name (a slot that stores none still shows one), so
            // what you see is what the file will be called.
            const std::string seed = sanitize_scale_filename(songcore::scale_display_name(scale));
            open_qwerty(QwertyContext::SCALE_SAVE, seed.empty() ? "SCALE" : seed, "SAVE SCALE:",
                        fs_.scales_directory(), /*max_length=*/20, /*clear_on_first_b=*/true);
            break;
        }
        case ScaleNameAction::LOAD:
            // SCALE is a screen, so the browser replaces it and nothing has to be closed (the theme
            // editor, an overlay, must be). Starts at the built-in folder: config.json's `folders` has
            // no scales key.
            open_file_browser(AppState::BrowserPurpose::LOAD_SCALE, fs_.scales_directory(),
                              {SCALE_FILE_EXT});
            break;
        case ScaleNameAction::NONE:
            break;
    }
}

void InputDispatcher::save_scale_as(const std::string& dir, const std::string& typed_text) {
    // Sanitized FILENAME (survives FAT32), raw name in the file. An empty field keeps the shown name and
    // falls back to "SCALE" — never the dotfile `.pts`.
    const std::string safe = sanitize_scale_filename(typed_text);
    const std::string file = (safe.empty() ? std::string("SCALE") : safe) + ".pts";

    songcore::Scale& slot = host_.edit_project().scales[static_cast<size_t>(s_.currentScale)];

    // The slot adopts the name it was saved under (the theme does not): it is the only thing on screen
    // naming this slot's file, and adopting it clears the `*`.
    const std::string want = !typed_text.empty()          ? typed_text
                           : !slot.name.empty()           ? slot.name
                                                          : songcore::scale_display_name(slot);
    if (slot.name != want) {
        slot.name = want;
        mark_modified();
    }

    const bool ok = save_scale_file(fs_, dir + "/" + file, slot);
    s_.statusMessage = ok ? "SCALE SAVED" : "SAVE FAILED";
    s_.statusSuccess = ok;
}

// ─── The GROOVE screen's SAVE / LOAD cells ───────────────────────────────────────────────────────

void InputDispatcher::groove_row_action() {
    const songcore::Groove& groove =
        host_.project().grooves[static_cast<size_t>(s_.currentGroove)];

    switch (groove_file_action(s_.groovePanelRow, s_.groovePanelColumn)) {
        case GrooveFileAction::SAVE: {
            // Seeded with the sanitized DISPLAY name (a slot that stores none still shows one), so
            // what you see is what the file will be called.
            const std::string seed = sanitize_groove_filename(songcore::groove_display_name(groove));
            open_qwerty(QwertyContext::GROOVE_SAVE, seed.empty() ? "GROOVE" : seed, "SAVE GROOVE:",
                        fs_.grooves_directory(), /*max_length=*/20, /*clear_on_first_b=*/true);
            break;
        }
        case GrooveFileAction::LOAD:
            // GROOVE is a screen, so the browser replaces it and nothing has to be closed. Starts at
            // the built-in folder: config.json's `folders` has no grooves key.
            open_file_browser(AppState::BrowserPurpose::LOAD_GROOVE, fs_.grooves_directory(),
                              {GROOVE_FILE_EXT});
            break;
        case GrooveFileAction::NONE:
            break;
    }
}

void InputDispatcher::save_groove_as(const std::string& dir, const std::string& typed_text) {
    // As for scales: sanitized FILENAME, raw name in the file; an empty field keeps the shown name and
    // falls back to "GROOVE" (never the dotfile `.ptg`).
    const std::string safe = sanitize_groove_filename(typed_text);
    const std::string file = (safe.empty() ? std::string("GROOVE") : safe) + ".ptg";

    songcore::Groove& slot = host_.edit_project().grooves[static_cast<size_t>(s_.currentGroove)];

    // The slot adopts the name it was saved under — the only thing on the panel naming its file — which
    // also clears the `*`.
    const std::string want = !typed_text.empty()          ? typed_text
                           : !slot.name.empty()           ? slot.name
                                                          : songcore::groove_display_name(slot);
    if (slot.name != want) {
        slot.name = want;
        mark_modified();
    }

    const bool ok = save_groove_file(fs_, dir + "/" + file, slot);
    s_.statusMessage = ok ? "GROOVE SAVED" : "SAVE FAILED";
    s_.statusSuccess = ok;
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
