#include "ui/modules/project_editor.h"

#include <algorithm>
#include <initializer_list>

#include "ui/helpers.h"

namespace pt::ui {

namespace {

constexpr int NAME_X   = 10;   // the label column
constexpr int VALUE_X  = 210;  // the value column
constexpr int OPTION_W = 80;   // the stride between the buttons on a multi-button row

constexpr int NAME_MAX_CHARS = 20;

int clamp(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

/** `tempo` as Kotlin prints it: three digits, zero-padded. */
std::string pad3(int v) {
    std::string s = std::to_string(v);
    while (s.size() < 3) s.insert(s.begin(), '0');
    return s;
}

/** Kotlin's String.trimEnd() — drop trailing whitespace, not merely trailing spaces. */
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
        return firstRowY + project_row_offset_y(row, ROW_HEIGHT);
    };

    const bool hasExit  = s.caps.appExit;
    const int  lastRow  = static_cast<int>(project_last_row(s.caps));

    // The row highlight spans the whole module, and it lights from the ROW alone — every column of a
    // row shares it. Only the VALUE under the cursor additionally takes textCursor.
    const auto on_row = [&](ProjectRow row) { return s.cursorRow == static_cast<int>(row); };
    const auto on_cell = [&](ProjectRow row, int column) {
        return on_row(row) && s.cursorColumn == column;
    };

    // A row's label is textCursor while the cursor is anywhere on the row, textParam otherwise —
    // including when the cursor is on column 0, which is why `isCursorOnName || isCursorOnValue`
    // collapses to "on the row".
    const auto row_bg = [&](ProjectRow row) {
        if (on_row(row)) c.fill_rect(x, rowY(row), WIDTH, ROW_HEIGHT, t.rowCursor);
    };
    const auto label = [&](ProjectRow row, const char* text) {
        c.draw_text(text, labelX, rowY(row) + TEXT_PADDING,
                    on_row(row) ? t.textCursor : t.textParam, CHAR_SPACING, FONT_SCALE);
    };

    // ── A single-value row: TEMPO, TRANSPOSE, SYSTEM, EXIT ───────────────────────────────────────
    const auto param_row = [&](ProjectRow row, const char* name, const std::string& value) {
        row_bg(row);
        label(row, name);
        c.draw_text(value, valueX, rowY(row) + TEXT_PADDING,
                    on_cell(row, 1) ? t.textCursor : t.textValue, CHAR_SPACING, FONT_SCALE);
    };

    // ── A button row: PROJECT, EXPORT, COMPACT ───────────────────────────────────────────────────
    const auto button_row = [&](ProjectRow row, const char* name,
                                std::initializer_list<const char*> options) {
        row_bg(row);
        label(row, name);
        int optionX = valueX;
        int index   = 1;
        for (const char* option : options) {
            c.draw_text(option, optionX, rowY(row) + TEXT_PADDING,
                        on_cell(row, index) ? t.textCursor : t.textValue, CHAR_SPACING, FONT_SCALE);
            optionX += OPTION_W;
            ++index;
        }
    };

    param_row(ProjectRow::TEMPO,     "TEMPO",     pad3(p.tempo));
    param_row(ProjectRow::TRANSPOSE, "TRANSPOSE", hex2(p.transpose));

    // ── NAME — 20 characters, one per cursor column ──────────────────────────────────────────────
    {
        const ProjectRow row = ProjectRow::NAME;
        row_bg(row);
        label(row, "NAME");

        const std::string name = p.name.substr(0, std::min<size_t>(p.name.size(), NAME_MAX_CHARS));

        int charX = valueX;
        for (int i = 0; i < NAME_MAX_CHARS; ++i) {
            const bool onThisChar = on_cell(row, i + 1);

            // The per-character cursor is a DARKENED block behind the glyph, not a row highlight —
            // the row is already lit, so the character needs to stand out from within it.
            if (onThisChar) {
                c.fill_rect(charX, rowY(row), 5 * FONT_SCALE, ROW_HEIGHT,
                            darken(t.textCursor, 0.27f));
            }
            if (i < static_cast<int>(name.size())) {
                c.draw_text(std::string(1, name[static_cast<size_t>(i)]), charX,
                            rowY(row) + TEXT_PADDING,
                            onThisChar ? t.textCursor : t.textValue, CHAR_SPACING, FONT_SCALE);
            }
            charX += CHAR_W;
        }
    }

    // ⚠️ SAVE is column 1 and LOAD is column 2 — the draw order, not the reading order. The Kotlin
    // list is `listOf("SAVE", "LOAD", "NEW")` while the row's doc comment says "LOAD / SAVE / NEW";
    // the LIST is what the cursor columns are numbered against, so SAVE is what column 1 does.
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

    // ── EXIT — the shell only ────────────────────────────────────────────────────────────────────
    // Android apps never exit; a handheld launcher needs the process back (port plan §5). Drawn like
    // SYSTEM because it is the same kind of thing: a row whose whole content is what A does on it.
    if (hasExit) param_row(ProjectRow::EXIT, "EXIT", "QUIT >");

    // ── USED RAM — a read-only info line, NOT a cursor row ───────────────────────────────────────
    // Integer math in tenths of a MB, as Kotlin does it, to dodge the locale decimal separator.
    if (s.caps.debug) {
        const int ramY = firstRowY +
                         project_row_offset_y(static_cast<ProjectRow>(lastRow), ROW_HEIGHT) +
                         ROW_HEIGHT * 2;
        const int64_t tenths = (s.sampleRamBytes * 10 + 524288) / 1048576;
        c.draw_text("USED RAM", labelX, ramY + TEXT_PADDING, t.textParam, CHAR_SPACING, FONT_SCALE);
        c.draw_text(std::to_string(tenths / 10) + "." + std::to_string(tenths % 10) + " MB",
                    valueX, ramY + TEXT_PADDING, t.textValue, CHAR_SPACING, FONT_SCALE);
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
            // Decimal, not hex — but a HEX_BYTE context, because the type only decides how the value
            // STEPS and 20..999 steps the same way either way. A+LEFT/RIGHT jumps by 10.
            CursorContext c = cc::hex_byte(p.tempo, 20, 999);
            c.largeStep = 10;
            return c;
        }

        case ProjectRow::TRANSPOSE: {
            // Same signed encoding as the chain transpose. A+LEFT/RIGHT = an octave.
            CursorContext c = cc::hex_byte(p.transpose, 0, 255);
            c.largeStep = 12;
            return c;
        }

        case ProjectRow::NAME: {
            const int charIndex = s.cursorColumn - 1;
            if (charIndex >= NAME_MAX_CHARS) return cc::none();
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
            if (action.type == ActionType::SET_VALUE)
                project.tempo = clamp(action.value, 20, 999);
            break;

        case ProjectRow::TRANSPOSE:
            if (action.type == ActionType::SET_VALUE)
                project.transpose = clamp(action.value, 0, 255);
            break;

        case ProjectRow::NAME: {
            const int charIndex = cursor_column - 1;
            if (charIndex < 0 || charIndex >= NAME_MAX_CHARS) return ProjectInputResult{false};

            if (action.type == ActionType::SET_VALUE) {
                // Pad, write, trim — Kotlin's `StringBuilder(name.padEnd(20)).setCharAt(i, c)`, then
                // trimEnd. A name shorter than the cursor column grows to meet it.
                std::string name = project.name;
                if (name.size() < NAME_MAX_CHARS) name.resize(NAME_MAX_CHARS, ' ');
                name[static_cast<size_t>(charIndex)] = static_cast<char>(action.value);
                project.name = trim_end(name);

            } else if (action.type == ActionType::DELETE) {
                // ⚠️ Guarded on the CURRENT length, where SET_VALUE is not: deleting a character
                // beyond the end of the name does nothing, rather than padding the name out to 20
                // spaces and trimming them straight back off. Kotlin's asymmetry, kept.
                if (charIndex < static_cast<int>(project.name.size())) {
                    std::string name = project.name;
                    if (name.size() < NAME_MAX_CHARS) name.resize(NAME_MAX_CHARS, ' ');
                    name[static_cast<size_t>(charIndex)] = ' ';
                    project.name = trim_end(name);
                }
            }
            break;
        }

        // The button rows edit nothing. (Kotlin falls through its `when` here too.)
        default:
            break;
    }

    return ProjectInputResult{action.type != ActionType::NONE};
}

}  // namespace pt::ui
