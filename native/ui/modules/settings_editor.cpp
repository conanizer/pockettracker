#include "ui/modules/settings_editor.h"

#include <algorithm>

#include "ui/helpers.h"

namespace pt::ui {

namespace {

constexpr int NAME_X     = 10;   // the row label
constexpr int VAL1_X     = 190;  // the primary value
constexpr int SUBLABEL_X = 355;  // the secondary column's own label (STR / VOL / POW)
constexpr int VAL2_X     = 408;  // the secondary value

using V = SettingsValues;

// One value cell: a flag shown as two words, a byte shown as hex, or an index into a list of names.
struct Cell {
    enum Kind { NONE, FLAG, BYTE, CHOICE } kind;
    bool V::* flag;
    int  V::* number;
    const char* on;
    const char* off;
    const char* const* names;
    int count;
};

constexpr Cell NO_CELL{Cell::NONE, nullptr, nullptr, nullptr, nullptr, nullptr, 0};
constexpr Cell flag(bool V::* f, const char* on = "ON", const char* off = "OFF") {
    return {Cell::FLAG, f, nullptr, on, off, nullptr, 0};
}
constexpr Cell byte(int V::* n) { return {Cell::BYTE, nullptr, n, nullptr, nullptr, nullptr, 0}; }
template <int N>
constexpr Cell choice(int V::* n, const char* const (&names)[N]) {
    return {Cell::CHOICE, nullptr, n, nullptr, nullptr, names, N};
}

constexpr const char* ABXY_NAMES[] = {"AUTO", "XBOX", "NINTENDO"};   // AbxyLayout
constexpr const char* HELP_NAMES[] = {"OFF", "SHORT", "FULL"};       // HelpMode

// The rows that are only value cells; a second cell sits behind its own little label. The rest of
// the rows are code below.
struct TableRow {
    SettingsRow row;
    const char* label;
    Cell        first;
    const char* sublabel;
    Cell        second;
};

constexpr TableRow ROWS[] = {
    {SettingsRow::SCALING,   "SCALING",   flag(&V::scalingBilinear, "BILINEAR", "INT"), nullptr, NO_CELL},
    {SettingsRow::BTN_SOUND, "BTN SOUND", flag(&V::buttonSoundEnabled), "VOL", byte(&V::buttonSoundVolume)},
    {SettingsRow::ABXY,      "ABXY",      choice(&V::abxyIndex, ABXY_NAMES), nullptr, NO_CELL},
    {SettingsRow::METRONOME, "METRONOME", flag(&V::metronomeEnabled), "VOL", byte(&V::metronomeVolume)},
    {SettingsRow::KB_INSERT, "KB INSERT", flag(&V::insertBefore, "BEFORE", "AFTER"), nullptr, NO_CELL},
    {SettingsRow::CURSOR,    "CURSOR",    flag(&V::cursorRemember, "REMEMBER", "REFRESH"), nullptr, NO_CELL},
    {SettingsRow::NAV,       "NAV",       flag(&V::navSongRelative, "SONG", "POOL"), nullptr, NO_CELL},
    {SettingsRow::FOLDER,    "FOLDER",    flag(&V::rememberFolder, "REMEMBER", "REFRESH"), nullptr, NO_CELL},
    {SettingsRow::NOTE_PREV, "NOTE PREV", flag(&V::notePreviewEnabled), nullptr, NO_CELL},
    {SettingsRow::HELP,      "HELP",      choice(&V::helpMode, HELP_NAMES), nullptr, NO_CELL},
    {SettingsRow::RESUME,    "RESUME",    flag(&V::autosaveResumeAuto, "AUTO", "ASK"), nullptr, NO_CELL},
    {SettingsRow::TRACE,     "TRACE",     flag(&V::traceEnabled), nullptr, NO_CELL},
};

const TableRow* table_row(SettingsRow row) {
    for (const TableRow& r : ROWS)
        if (r.row == row) return &r;
    return nullptr;
}

const Cell& cell_at(const TableRow& r, int column) {
    return (column >= 2 && r.second.kind != Cell::NONE) ? r.second : r.first;
}

std::string cell_text(const Cell& c, const V& v) {
    switch (c.kind) {
        case Cell::FLAG:   return v.*c.flag ? c.on : c.off;
        case Cell::BYTE:   return hex2(v.*c.number);
        case Cell::CHOICE: return c.names[clamp(v.*c.number, 0, c.count - 1)];
        case Cell::NONE:   break;
    }
    return {};
}

CursorContext cell_context(const Cell& c, const V& v) {
    switch (c.kind) {
        case Cell::FLAG:   return cc::toggle_binary(v.*c.flag);
        case Cell::BYTE:   return cc::hex_byte(v.*c.number, 0, 255);
        case Cell::CHOICE: return cc::enum_cycle(v.*c.number, c.count);
        case Cell::NONE:   break;
    }
    return cc::read_only();
}

// An index the cycle could not produce falls back to the first (OFF, AUTO, …).
int index_in(int value, int count) { return (value >= 0 && value < count) ? value : 0; }

void cell_apply(const Cell& c, V& v, int value) {
    switch (c.kind) {
        case Cell::FLAG:   v.*c.flag   = value > 0; break;
        case Cell::BYTE:   v.*c.number = clamp(value, 0, 255); break;
        case Cell::CHOICE: v.*c.number = index_in(value, c.count); break;
        case Cell::NONE:   break;
    }
}

}  // namespace

const std::vector<std::string>& SettingsModule::visualizer_names() {
    static const std::vector<std::string> names = {
        "SCOPE", "FLAT", "OCTA", "OCTA.F", "SPECT", "SPCT.P",
    };
    return names;
}

// ─── Draw ────────────────────────────────────────────────────────────────────────────────────────

void SettingsModule::draw(Canvas& c, int x, int y, const SettingsState& s) const {
    const Theme&          t = s.theme;
    const SettingsValues& v = s.values;

    c.fill_rect(x, y, WIDTH, HEIGHT, t.background);

    const int labelX = x + NAME_X;
    const int val1X  = x + VAL1_X;
    const int subX   = x + SUBLABEL_X;
    const int val2X  = x + VAL2_X;

    c.draw_text("SETTINGS", labelX, y + TEXT_PADDING, t.textTitle, CHAR_SPACING, FONT_SCALE);
    // The running build, right-aligned: what a bug report asks for.
    if (!s.appVersion.empty()) {
        const std::string version = "V" + s.appVersion;
        c.draw_text(version, x + WIDTH - NAME_X - Canvas::text_width(version, CHAR_SPACING, FONT_SCALE),
                    y + TEXT_PADDING, t.textEmpty, CHAR_SPACING, FONT_SCALE);
    }

    const int firstRowY = y + TEXT_PADDING + ROW_HEIGHT + 14;

    // ── Scroll: only debug builds overflow; derived from the cursor row each frame ───────────────
    const int viewportH = HEIGHT - (firstRowY - y);
    const int contentH  = settings_content_height(s.caps, ROW_HEIGHT);
    const int maxScroll = std::max(0, contentH - viewportH);
    const int cursorTop =
        settings_row_offset_y(static_cast<SettingsRow>(s.cursorRow), s.caps, ROW_HEIGHT);
    const int scrollY = clamp(cursorTop + ROW_HEIGHT / 2 - viewportH / 2, 0, maxScroll);

    const auto rowY = [&](SettingsRow row) {
        return firstRowY + settings_row_offset_y(row, s.caps, ROW_HEIGHT) - scrollY;
    };
    const auto on_row  = [&](SettingsRow row) { return s.cursorRow == static_cast<int>(row); };
    const auto on_cell = [&](SettingsRow row, int column) {
        return on_row(row) && s.cursorColumn == column;
    };

    const auto param_row = [&](SettingsRow row, const char* name, const std::string& value) {
        if (!settings_row_visible(row, s.caps)) return;
        const int ry = rowY(row);
        c.draw_text(name, labelX, ry + TEXT_PADDING,
                    on_row(row) ? cursor_mark_ink(t) : t.textParam, CHAR_SPACING, FONT_SCALE);
        draw_cursor_cell(c, value, val1X, ry + TEXT_PADDING, on_cell(row, 1), t.textValue, t);
    };

    // The second value behind its own little label (STR / VOL / POW).
    const auto dual_row = [&](SettingsRow row, const char* name, const std::string& value1,
                              const char* sublabel, const std::string& value2) {
        if (!settings_row_visible(row, s.caps)) return;
        const int ry = rowY(row);
        c.draw_text(name, labelX, ry + TEXT_PADDING,
                    on_row(row) ? cursor_mark_ink(t) : t.textParam, CHAR_SPACING, FONT_SCALE);
        draw_cursor_cell(c, value1, val1X, ry + TEXT_PADDING, on_cell(row, 1), t.textValue, t);
        c.draw_text(sublabel, subX, ry + TEXT_PADDING, t.textParam, CHAR_SPACING, FONT_SCALE);
        draw_cursor_cell(c, value2, val2X, ry + TEXT_PADDING, on_cell(row, 2), t.textValue, t);
    };

    const auto on_off = [](bool b) { return std::string(b ? "ON" : "OFF"); };

    // Every row is drawn through `rowY`; clipped to the viewport.
    const Canvas::ClipScope rowsClip(c, x, firstRowY, WIDTH, viewportH);

    for (const TableRow& r : ROWS) {
        if (r.second.kind == Cell::NONE)
            param_row(r.row, r.label, cell_text(r.first, v));
        else
            dual_row(r.row, r.label, cell_text(r.first, v), r.sublabel, cell_text(r.second, v));
    }

    // ── LAYOUT, and its skin column when the layout is skinned ───────────────────────────────────
    if (v.skinCount > 0) {
        dual_row(SettingsRow::LAYOUT, "LAYOUT", s.layoutText, "", s.skinText);
    } else {
        param_row(SettingsRow::LAYOUT, "LAYOUT", s.layoutText);
    }

    dual_row(SettingsRow::OVERLAY, "OVERLAY", s.overlayText, "STR", hex2(v.overlayStrength));

    // An OS driver name: 18 characters to the panel edge.
    param_row(SettingsRow::AUDIO_OUT, "AUDIO OUT",
              Canvas::clip_text(s.audioOutText, (WIDTH - VAL1_X - NAME_X) / CHAR_W));

    // POW is LO/HI: the target ROMs have only EFFECT_TICK and EFFECT_CLICK (LO < 128 ≤ HI).
    dual_row(SettingsRow::BTN_VIBRO, "BTN VIBRO", on_off(v.buttonVibroEnabled),
             "POW", std::string(v.vibroPower >= 128 ? "HI" : "LO"));

    if (v.oneHanded)
        dual_row(SettingsRow::ONE_HAND, "ONE HAND", on_off(true),
                 "MS", std::to_string(ONE_HAND_HOLD_STEPS[one_hand_hold_index(v.oneHandHoldMs)]));
    else
        param_row(SettingsRow::ONE_HAND, "ONE HAND", on_off(false));

    {
        const std::vector<std::string>& names = visualizer_names();
        const int index = clamp(static_cast<int>(t.visualizerType), 0,
                                static_cast<int>(names.size()) - 1);
        param_row(SettingsRow::VISUALIZER, "VISUALIZER", names[static_cast<size_t>(index)]);
    }

    // THEME: the name and a ">" (A opens the editor). ⚠️ The name is clipped two glyphs short so a
    // long name cannot push the ">" out.
    {
        constexpr int VALUE_COLS = (WIDTH - 10 - VAL1_X) / CHAR_W;
        param_row(SettingsRow::THEME, "THEME",
                  Canvas::clip_text(s.themeName, VALUE_COLS - 2) + " >");
    }

    // TEMPLATE: a button row, two options, no value.
    if (settings_row_visible(SettingsRow::TEMPLATE, s.caps)) {
        const SettingsRow row = SettingsRow::TEMPLATE;
        const int ry = rowY(row);
        c.draw_text("TEMPLATE", labelX, ry + TEXT_PADDING,
                    on_row(row) ? cursor_mark_ink(t) : t.textParam, CHAR_SPACING, FONT_SCALE);
        const char* options[2] = {"SAVE", "CLEAR"};
        for (int i = 0; i < 2; ++i) {
            draw_cursor_cell(c, options[i], val1X + i * 80, ry + TEXT_PADDING,
                             on_cell(row, i + 1), t.textValue, t);
        }
    }
}

// ─── Cursor ──────────────────────────────────────────────────────────────────────────────────────

CursorContext SettingsModule::cursor_context(const SettingsState& s) const {
    const SettingsValues& v = s.values;

    if (s.cursorColumn == 0) return cc::read_only();

    const SettingsRow row = static_cast<SettingsRow>(s.cursorRow);
    if (!settings_row_visible(row, s.caps)) return cc::none();

    if (const TableRow* r = table_row(row)) return cell_context(cell_at(*r, s.cursorColumn), v);

    switch (row) {
        case SettingsRow::LAYOUT:
            if (s.cursorColumn == 2) {
                // The skin column exists only while the layout is skinned.
                if (v.skinCount <= 0) return cc::read_only();
                return cc::enum_cycle(v.skinIndex, v.skinCount);
            }
            return cc::enum_cycle(v.layoutIndex, v.layoutCount);

        case SettingsRow::OVERLAY:
            if (s.cursorColumn == 1) return cc::enum_cycle(v.overlayIndex, v.overlayCount);
            return cc::hex_byte(v.overlayStrength, 0, 255);

        case SettingsRow::BTN_VIBRO:
            if (s.cursorColumn == 1) return cc::toggle_binary(v.buttonVibroEnabled);
            return cc::toggle_binary(v.vibroPower >= 128);  // LO / HI, not a 0..255 knob

        case SettingsRow::AUDIO_OUT:  return cc::enum_cycle(v.audioOutIndex, v.audioOutCount);

        case SettingsRow::ONE_HAND:
            if (s.cursorColumn == 1) return cc::toggle_binary(v.oneHanded);
            if (!v.oneHanded) return cc::read_only();
            return cc::enum_cycle(one_hand_hold_index(v.oneHandHoldMs), ONE_HAND_HOLD_COUNT);

        case SettingsRow::VISUALIZER:
            return cc::enum_cycle(static_cast<int>(s.theme.visualizerType),
                                  static_cast<int>(visualizer_names().size()));

        // A opens the theme editor, or triggers SAVE or CLEAR.
        case SettingsRow::THEME:
        case SettingsRow::TEMPLATE:   return cc::read_only();

        default:                      break;
    }
    return cc::none();
}

// ─── Input ───────────────────────────────────────────────────────────────────────────────────────

void SettingsModule::handle_input(SettingsValues& v, Theme& theme, const PlatformCaps& caps,
                                  int cursor_row, int cursor_column, const InputAction& action) const {
    const SettingsRow row = static_cast<SettingsRow>(cursor_row);
    if (!settings_row_visible(row, caps) || action.type != ActionType::SET_VALUE) return;

    const int  value = action.value;
    const bool on    = value > 0;

    if (const TableRow* r = table_row(row)) {
        if (cursor_column >= 1) cell_apply(cell_at(*r, cursor_column), v, value);
        return;
    }

    switch (row) {
        case SettingsRow::LAYOUT:
            if (cursor_column != 2)  v.layoutIndex = index_in(value, v.layoutCount);
            else if (v.skinCount > 0) v.skinIndex  = clamp(value, 0, v.skinCount - 1);
            break;

        case SettingsRow::OVERLAY:
            if (cursor_column == 1)      v.overlayIndex    = index_in(value, v.overlayCount);
            else if (cursor_column == 2) v.overlayStrength = clamp(value, 0, 255);
            break;

        case SettingsRow::BTN_VIBRO:
            if (cursor_column == 1)      v.buttonVibroEnabled = on;
            // Store 64 (EFFECT_TICK) or 255 (EFFECT_CLICK).
            else if (cursor_column == 2) v.vibroPower         = on ? 255 : 64;
            break;

        // The shell opens the output and writes the NAME once it plays.
        case SettingsRow::AUDIO_OUT: v.audioOutIndex = index_in(value, v.audioOutCount); break;

        case SettingsRow::ONE_HAND:
            if (cursor_column == 1) v.oneHanded = on;
            else if (cursor_column == 2)
                v.oneHandHoldMs = ONE_HAND_HOLD_STEPS[index_in(value, ONE_HAND_HOLD_COUNT)];
            break;

        case SettingsRow::VISUALIZER:
            theme.visualizerType = static_cast<VisualizerType>(
                index_in(value, static_cast<int>(visualizer_names().size())));
            break;

        // A-only rows (THEME, TEMPLATE) are the dispatcher's.
        default: break;
    }
}

}  // namespace pt::ui
