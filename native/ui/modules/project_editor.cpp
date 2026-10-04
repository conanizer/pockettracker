#include "ui/modules/project_editor.h"

#include <algorithm>
#include <initializer_list>

#include "ui/helpers.h"
#include "ui/modules/qwerty_keyboard.h"

namespace pt::ui {

namespace {

constexpr int NAME_X   = 10;                       // the label column
constexpr int OPTION_W = 80;   // the stride between the buttons on a multi-button row

int clamp(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

/** `tempo` as three digits, zero-padded. */
std::string pad3(int v) {
    std::string s = std::to_string(v);
    while (s.size() < 3) s.insert(s.begin(), '0');
    return s;
}

/** Drop trailing whitespace, not merely trailing spaces. */
std::string trim_end(std::string s) {
    while (!s.empty()) {
        const char ch = s.back();
        if (ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r' || ch == '\f' || ch == '\v')
            s.pop_back();
        else
            break;
    }
    return s;
}

}  // namespace

// ─── Draw ────────────────────────────────────────────────────────────────────────────────────────

void ProjectModule::draw(Canvas& c, int x, int y, const ProjectState& s) const {
    const Theme&             t = s.theme;
    const songcore::Project& p = s.project;

    c.fill_rect(x, y, WIDTH, HEIGHT, t.background);

    const int labelX = x + NAME_X;
    const int valueX = x + VALUE_X;

    c.draw_text("PROJECT", labelX, y + TEXT_PADDING, t.textTitle, CHAR_SPACING, FONT_SCALE);

    // The first row's background sits below the title and its 14px of air.
    const int firstRowY = y + TEXT_PADDING + ROW_HEIGHT + 14;
    const auto rowY = [&](ProjectRow row) {
        return firstRowY + project_row_offset_y(row, s.caps, ROW_HEIGHT);
    };

    const bool hasExit = s.caps.appExit;
    const bool hasMidi = s.caps.midi;
    const int  lastRow = static_cast<int>(project_last_row(s.caps));

    // The cursor is the cell it is on; the row's own label takes `cursor_mark_ink` while the cursor
    // is anywhere along the row.
    const auto on_row = [&](ProjectRow row) { return s.cursorRow == static_cast<int>(row); };
    const auto on_cell = [&](ProjectRow row, int column) {
        return on_row(row) && s.cursorColumn == column;
    };

    const auto label = [&](ProjectRow row, const char* text) {
        c.draw_text(text, labelX, rowY(row) + TEXT_PADDING,
                    on_row(row) ? cursor_mark_ink(t) : t.textParam, CHAR_SPACING, FONT_SCALE);
    };

    // ── A single-value row: TEMPO, TRANSPOSE, SYSTEM, EXIT ───────────────────────────────────────
    const auto param_row = [&](ProjectRow row, const char* name, const std::string& value) {
        label(row, name);
        draw_cursor_cell(c, value, valueX, rowY(row) + TEXT_PADDING, on_cell(row, 1), t.textValue, t);
    };

    // ── A DOOR row: MIDI, EXIT — the button alone, no label ──────────────────────────────────────
    // The button names where it goes, so no label. It sits in the value column, where the cursor
    // lands (column 1) as on a labelled row.
    const auto door_row = [&](ProjectRow row, const char* button) {
        draw_cursor_cell(c, button, valueX, rowY(row) + TEXT_PADDING, on_cell(row, 1), t.textValue, t);
    };

    // ── A button row: PROJECT, EXPORT, COMPACT ───────────────────────────────────────────────────
    const auto button_row = [&](ProjectRow row, const char* name,
                                std::initializer_list<const char*> options) {
        label(row, name);
        int optionX = valueX;
        int index   = 1;
        for (const char* option : options) {
            draw_cursor_cell(c, option, optionX, rowY(row) + TEXT_PADDING, on_cell(row, index),
                             t.textValue, t);
            optionX += OPTION_W;
            ++index;
        }
    };

    param_row(ProjectRow::TEMPO,     "TEMPO",     pad3(p.tempo));

    // ── TAP — the TEMPO row's second CELL ────────────────────────────────────────────────────────
    //
    // RIGHT from the value lands here; A on it taps the tempo by feel (the dispatcher owns the
    // arithmetic). It is drawn like the buttons on the rows below, because that is what it is.
    //
    // ⚠️ A CELL, NOT A GESTURE ON THE VALUE: A is also the modifier of A+DPAD and fires on its own
    // press before the direction arrives, so a tap read off the value cell counted every A+UP nudge.
    draw_cursor_cell(c, "TAP", valueX + 80, rowY(ProjectRow::TEMPO) + TEXT_PADDING,
                     on_cell(ProjectRow::TEMPO, 2), t.textValue, t);

    param_row(ProjectRow::TRANSPOSE, "TRANSPOSE", hex2(p.transpose));

    // ── NAME — 20 characters, one per cursor column, in a 17-column window ───────────────────────
    //
    // The field scrolls under the cursor: the row shows `NAME_VISIBLE_CHARS` before the editor clip.
    {
        const ProjectRow row = ProjectRow::NAME;
        label(row, "NAME");

        const std::string name =
            p.name.substr(0, std::min<size_t>(p.name.size(), PROJECT_NAME_MAX_CHARS));

        // ⚠️ The window's content is the name or the cursor, whichever reaches further — not all 20
        // cells: a "…" beside blanks would claim hidden text. The helper adds its own phantom column
        // for an end-of-text cursor, so it is handed one less.
        const int cursorChar = on_row(row) ? s.cursorColumn - 1 : 0;
        const int content    = std::max(static_cast<int>(name.size()), cursorChar + 1);
        const QwertyTextWindow win =
            qwerty_text_window(content - 1, cursorChar, ProjectModule::NAME_VISIBLE_CHARS);

        const std::string ELLIPSIS = "\xE2\x80\xA6";   // U+2026, one column (font5x5 GLYPH_ELLIPSIS)
        const Argb markerColor = darken(t.textValue, 0.5f);
        const int  textX       = valueX + (win.clipLeft ? CHAR_W : 0);

        if (win.clipLeft)
            c.draw_text(ELLIPSIS, valueX, rowY(row) + TEXT_PADDING, markerColor, CHAR_SPACING,
                        FONT_SCALE);

        for (int k = 0; k < win.cols; ++k) {
            const int  i          = win.first + k;
            const int  charX      = textX + k * CHAR_W;
            const bool onThisChar = on_cell(row, i + 1);

            // A space stands in past the end of the name: it draws nothing but gives the cursor a
            // cell's width there, which is how a name gets longer.
            const std::string ch = (i < static_cast<int>(name.size()))
                                       ? std::string(1, name[static_cast<size_t>(i)])
                                       : std::string(" ");
            draw_cursor_cell(c, ch, charX, rowY(row) + TEXT_PADDING, onThisChar, t.textValue, t);
        }

        if (win.clipRight)
            c.draw_text(ELLIPSIS, textX + win.cols * CHAR_W, rowY(row) + TEXT_PADDING, markerColor,
                        CHAR_SPACING, FONT_SCALE);
    }

    // ⚠️ SAVE is column 1, LOAD column 2, NEW column 3 — the order the cursor columns are numbered in.
    button_row(ProjectRow::PROJECT, "PROJECT", {"SAVE", "LOAD", "NEW"});

    // ── EXPORT — MIX / STEMS, plus a live percentage while a render runs ─────────────────────────
    button_row(ProjectRow::EXPORT, "EXPORT", {"MIX", "STEMS"});
    if (s.isRendering) {
        const int percent = clamp(static_cast<int>(s.renderProgress * 100.0f), 0, 100);
        c.draw_text(std::to_string(percent) + "%", valueX + 170,
                    rowY(ProjectRow::EXPORT) + TEXT_PADDING, t.textTitle, CHAR_SPACING, FONT_SCALE);
    }

    button_row(ProjectRow::COMPACT, "COMPACT", {"SEQ", "INST"});

    param_row(ProjectRow::SYSTEM, "SYSTEM", "SETTINGS >");

    // ── MIDI — the door to the MIDI screen ───────────────────────────────────────────────────────
    // Gated on the build, not a device capability (a machine with no port just enumerates none):
    // this row is the only way to the MIDI screen.
    if (hasMidi) door_row(ProjectRow::MIDI, "MIDI >");

    // ── EXIT — the shell only ────────────────────────────────────────────────────────────────────
    // A handheld launcher needs the process back; Android apps never exit. No '>': this one leaves
    // the app rather than opening a screen.
    if (hasExit) door_row(ProjectRow::EXIT, "EXIT");

    // ── USED / FREE RAM — read-only info lines, NOT cursor rows ──────────────────────────────────
    // ⚠️ DEVELOPER BUILDS ONLY: a user can do nothing with the pair — FREE is the machine's, USED the
    // engine's audio, and they sum to nothing. INST.POOL's total is gated the same way, and
    // `poll_sample_ram` stops sampling both when this is off.
    if (s.caps.debug) {
        const int ramY = firstRowY +
                         project_row_offset_y(static_cast<ProjectRow>(lastRow), s.caps, ROW_HEIGHT) +
                         ROW_HEIGHT * 2;
        c.draw_text("USED RAM", labelX, ramY + TEXT_PADDING, t.textParam, CHAR_SPACING, FONT_SCALE);
        c.draw_text(megabytes_str(s.sampleRamBytes),
                    valueX, ramY + TEXT_PADDING, t.textValue, CHAR_SPACING, FONT_SCALE);

        // FREE RAM beneath it, in the same unit. Skipped when the platform cannot answer — "0.0 MB"
        // would say the opposite of "unknown".
        if (s.freeRamBytes > 0) {
            const int freeY = ramY + ROW_HEIGHT;
            c.draw_text("FREE RAM", labelX, freeY + TEXT_PADDING, t.textParam, CHAR_SPACING, FONT_SCALE);
            c.draw_text(megabytes_str(s.freeRamBytes),
                        valueX, freeY + TEXT_PADDING, t.textValue, CHAR_SPACING, FONT_SCALE);
        }
    }

    // The status line (SAVED / EXPORTED! / SEQ CLEANED) is NOT drawn here. It is a global overlay on
    // the visualizer header, so that every screen can report — see TrackerLayout::draw.
}

// ─── Cursor ──────────────────────────────────────────────────────────────────────────────────────

CursorContext ProjectModule::cursor_context(const ProjectState& s) const {
    const songcore::Project& p = s.project;

    // Column 0 is the label on every row. Unreachable (see the header), and read-only if reached.
    if (s.cursorColumn == 0) return cc::read_only();

    switch (static_cast<ProjectRow>(s.cursorRow)) {
        case ProjectRow::TEMPO: {
            // Column 2 is TAP, a button: read-only; plain A is its behaviour (the dispatcher's).
            // Columns 3..20 are unreachable (`project_row_max_column` stops at 2).
            if (s.cursorColumn == 2) return cc::read_only();
            // Decimal, not hex — but a HEX_BYTE context, because the type only decides how the value
            // STEPS and 20..999 steps the same way either way. A+UP/DOWN jumps by 10.
            CursorContext c = cc::hex_byte(p.tempo, 20, 999);
            c.largeStep = 10;
            return c;
        }

        case ProjectRow::TRANSPOSE: {
            // Same signed encoding as the chain transpose. A+UP/DOWN = an octave.
            CursorContext c = cc::hex_byte(p.transpose, 0, 255);
            c.largeStep = 12;
            return c;
        }

        case ProjectRow::NAME: {
            const int charIndex = s.cursorColumn - 1;
            if (charIndex >= PROJECT_NAME_MAX_CHARS) return cc::none();
            // Past the end of the name is a SPACE, not an empty cell — a character always has a value.
            const char ch = (charIndex < static_cast<int>(p.name.size()))
                                ? p.name[static_cast<size_t>(charIndex)]
                                : ' ';
            return cc::character(ch);
        }

        // Everything below is a BUTTON. Read-only to the generic edit path; plain A is the whole of
        // its behaviour, and the dispatcher owns that.
        case ProjectRow::PROJECT:
        case ProjectRow::EXPORT:
        case ProjectRow::COMPACT:
        case ProjectRow::SYSTEM:
            return cc::read_only();

        // The two conditional rows answer `none()` where this build does not draw them — a cursor
        // that cannot get there still has a context taken of it by anything that asks blind.
        case ProjectRow::MIDI:
            return s.caps.midi ? cc::read_only() : cc::none();

        case ProjectRow::EXIT:
            return s.caps.appExit ? cc::read_only() : cc::none();
    }
    return cc::none();
}

// ─── Input ───────────────────────────────────────────────────────────────────────────────────────

ProjectInputResult ProjectModule::handle_input(songcore::Project& project, int cursor_row,
                                               int cursor_column, const InputAction& action) const {
    switch (static_cast<ProjectRow>(cursor_row)) {
        case ProjectRow::TEMPO:
            // ⚠️ Guarded on the column: column 2 is TAP, and a button never writes the cell to its
            // left. `cursor_context` already answers read_only there; this is the second lock.
            if (cursor_column != 2 && action.type == ActionType::SET_VALUE)
                project.tempo = clamp(action.value, 20, 999);
            break;

        case ProjectRow::TRANSPOSE:
            if (action.type == ActionType::SET_VALUE)
                project.transpose = clamp(action.value, 0, 255);
            break;

        case ProjectRow::NAME: {
            const int charIndex = cursor_column - 1;
            if (charIndex < 0 || charIndex >= PROJECT_NAME_MAX_CHARS) return ProjectInputResult{false};

            if (action.type == ActionType::SET_VALUE) {
                // Pad to the cursor column, write, trim trailing whitespace.
                std::string name = project.name;
                if (name.size() < PROJECT_NAME_MAX_CHARS) name.resize(PROJECT_NAME_MAX_CHARS, ' ');
                name[static_cast<size_t>(charIndex)] = static_cast<char>(action.value);
                project.name = trim_end(name);

            } else if (action.type == ActionType::DELETE) {
                // ⚠️ Guarded on the current length: deleting beyond the end does nothing.
                if (charIndex < static_cast<int>(project.name.size())) {
                    std::string name = project.name;
                    if (name.size() < PROJECT_NAME_MAX_CHARS) name.resize(PROJECT_NAME_MAX_CHARS, ' ');
                    name[static_cast<size_t>(charIndex)] = ' ';
                    project.name = trim_end(name);
                }
            }
            break;
        }

        // The button rows edit nothing.
        default:
            break;
    }

    return ProjectInputResult{action.type != ActionType::NONE};
}

}  // namespace pt::ui
