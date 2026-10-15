#include "ui/modules/project_editor.h"

#include <algorithm>
#include <initializer_list>

#include "ui/helpers.h"
#include "ui/modules/qwerty_keyboard.h"

namespace pt::ui {

namespace {

constexpr int NAME_X   = 10;                       // the label column
constexpr int OPTION_W = 80;   // the stride between the buttons on a multi-button row


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

    // The cursor is the cell; the row's label takes `cursor_mark_ink` while the cursor is on the row.
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
    // The button names where it goes; it sits in the value column, where the cursor lands.
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

    // ── TAP — the TEMPO row's second cell; A taps the tempo (the dispatcher's arithmetic) ────────
    // ⚠️ A cell, not a gesture on the value: A is also A+DPAD's modifier and fires first, so a tap
    // read off the value counted every A+UP nudge.
    draw_cursor_cell(c, "TAP", valueX + 80, rowY(ProjectRow::TEMPO) + TEXT_PADDING,
                     on_cell(ProjectRow::TEMPO, 2), t.textValue, t);

    param_row(ProjectRow::TRANSPOSE, "TRANSPOSE", hex2(p.transpose));

    // ── NAME — 20 characters, one per cursor column, scrolling in a 17-column window ─────────────
    {
        const ProjectRow row = ProjectRow::NAME;
        label(row, "NAME");

        const std::string name =
            p.name.substr(0, std::min<size_t>(p.name.size(), PROJECT_NAME_MAX_CHARS));

        // ⚠️ The window spans the name or the cursor, whichever reaches further — a "…" beside blanks
        // would claim hidden text. The helper adds a column for an end-of-text cursor: hand it one less.
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

            // A space past the end gives the cursor a cell there, which is how a name grows.
            const std::string ch = (i < static_cast<int>(name.size()))
                                       ? std::string(1, name[static_cast<size_t>(i)])
                                       : std::string(" ");
            draw_cursor_cell(c, ch, charX, rowY(row) + TEXT_PADDING, onThisChar, t.textValue, t);
        }

        if (win.clipRight)
            c.draw_text(ELLIPSIS, textX + win.cols * CHAR_W, rowY(row) + TEXT_PADDING, markerColor,
                        CHAR_SPACING, FONT_SCALE);
    }

    // SAVE, LOAD, NEW are cursor columns 1, 2, 3.
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
    // Gated on the build, not the device: this row is the only way to the MIDI screen.
    if (hasMidi) door_row(ProjectRow::MIDI, "MIDI >");

    // ── EXIT — the shell only ────────────────────────────────────────────────────────────────────
    // A handheld launcher needs the process back; Android apps never exit. No '>': it leaves the app.
    if (hasExit) door_row(ProjectRow::EXIT, "EXIT");

    // ── USED / FREE RAM — info lines, not cursor rows; developer builds only ─────────────────────
    // A user can do nothing with them; `poll_sample_ram` stops sampling when this is off.
    if (s.caps.debug) {
        const int ramY = firstRowY +
                         project_row_offset_y(static_cast<ProjectRow>(lastRow), s.caps, ROW_HEIGHT) +
                         ROW_HEIGHT * 2;
        c.draw_text("USED RAM", labelX, ramY + TEXT_PADDING, t.textParam, CHAR_SPACING, FONT_SCALE);
        c.draw_text(megabytes_str(s.sampleRamBytes),
                    valueX, ramY + TEXT_PADDING, t.textValue, CHAR_SPACING, FONT_SCALE);

        // Skipped when the platform cannot answer: "0.0 MB" would say the opposite of unknown.
        if (s.freeRamBytes > 0) {
            const int freeY = ramY + ROW_HEIGHT;
            c.draw_text("FREE RAM", labelX, freeY + TEXT_PADDING, t.textParam, CHAR_SPACING, FONT_SCALE);
            c.draw_text(megabytes_str(s.freeRamBytes),
                        valueX, freeY + TEXT_PADDING, t.textValue, CHAR_SPACING, FONT_SCALE);
        }
    }

    // The status line is the global overlay's (TrackerLayout::draw).
}

// ─── Cursor ──────────────────────────────────────────────────────────────────────────────────────

CursorContext ProjectModule::cursor_context(const ProjectState& s) const {
    const songcore::Project& p = s.project;

    // Column 0 is the label: unreachable, read-only if reached.
    if (s.cursorColumn == 0) return cc::read_only();

    switch (static_cast<ProjectRow>(s.cursorRow)) {
        case ProjectRow::TEMPO: {
            // Column 2 is TAP, a button (the dispatcher's); `project_row_max_column` stops at 2.
            if (s.cursorColumn == 2) return cc::read_only();
            // Decimal, but HEX_BYTE steps 20..999 the same. A+UP/DOWN jumps by 10.
            CursorContext c = cc::hex_byte(p.tempo, 20, 999);
            c.largeStep = 10;
            return c;
        }

        case ProjectRow::TRANSPOSE: {
            // Signed, as the chain transpose. A+UP/DOWN = an octave.
            CursorContext c = cc::hex_byte(p.transpose, 0, 255);
            c.largeStep = 12;
            return c;
        }

        case ProjectRow::NAME: {
            const int charIndex = s.cursorColumn - 1;
            if (charIndex >= PROJECT_NAME_MAX_CHARS) return cc::none();
            // Past the end is a space: a character always has a value.
            const char ch = (charIndex < static_cast<int>(p.name.size()))
                                ? p.name[static_cast<size_t>(charIndex)]
                                : ' ';
            return cc::character(ch);
        }

        // Everything below is a button: plain A is the dispatcher's.
        case ProjectRow::PROJECT:
        case ProjectRow::EXPORT:
        case ProjectRow::COMPACT:
        case ProjectRow::SYSTEM:
            return cc::read_only();

        // The conditional rows answer `none()` where this build does not draw them.
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
            // ⚠️ Guarded on the column: TAP (column 2) never writes the cell to its left.
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
                // ⚠️ Deleting past the end does nothing.
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
