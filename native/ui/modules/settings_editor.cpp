#include "ui/modules/settings_editor.h"

#include <algorithm>

#include "ui/helpers.h"

namespace pt::ui {

namespace {

constexpr int NAME_X     = 10;   // the row label
constexpr int VAL1_X     = 190;  // the primary value
constexpr int SUBLABEL_X = 355;  // the secondary column's own label (STR / VOL / POW / ENG)
constexpr int VAL2_X     = 408;  // the secondary value

int clamp(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

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

    const int firstRowY = y + TEXT_PADDING + ROW_HEIGHT + 14;
    const auto rowY = [&](SettingsRow row) {
        return firstRowY + settings_row_offset_y(row, s.caps, ROW_HEIGHT);
    };
    const auto on_row  = [&](SettingsRow row) { return s.cursorRow == static_cast<int>(row); };
    const auto on_cell = [&](SettingsRow row, int column) {
        return on_row(row) && s.cursorColumn == column;
    };

    /** A row with one value. */
    const auto param_row = [&](SettingsRow row, const char* name, const std::string& value) {
        if (!settings_row_visible(row, s.caps)) return;
        const int ry = rowY(row);
        if (on_row(row)) c.fill_rect(x, ry, WIDTH, ROW_HEIGHT, t.rowCursor);
        c.draw_text(name, labelX, ry + TEXT_PADDING,
                    on_row(row) ? t.textCursor : t.textParam, CHAR_SPACING, FONT_SCALE);
        c.draw_text(value, val1X, ry + TEXT_PADDING,
                    on_cell(row, 1) ? t.textCursor : t.textValue, CHAR_SPACING, FONT_SCALE);
    };

    /** A row with two values, the second behind its own little label (STR / VOL / POW / ENG). */
    const auto dual_row = [&](SettingsRow row, const char* name, const std::string& value1,
                              const char* sublabel, const std::string& value2) {
        if (!settings_row_visible(row, s.caps)) return;
        const int ry = rowY(row);
        if (on_row(row)) c.fill_rect(x, ry, WIDTH, ROW_HEIGHT, t.rowCursor);
        c.draw_text(name, labelX, ry + TEXT_PADDING,
                    on_row(row) ? t.textCursor : t.textParam, CHAR_SPACING, FONT_SCALE);
        c.draw_text(value1, val1X, ry + TEXT_PADDING,
                    on_cell(row, 1) ? t.textCursor : t.textValue, CHAR_SPACING, FONT_SCALE);
        // The sublabel is textParam whether or not the cursor is on the row — Kotlin's ternary picks
        // textParam on both arms, which is a tell that it was written and then thought better of.
        c.draw_text(sublabel, subX, ry + TEXT_PADDING, t.textParam, CHAR_SPACING, FONT_SCALE);
        c.draw_text(value2, val2X, ry + TEXT_PADDING,
                    on_cell(row, 2) ? t.textCursor : t.textValue, CHAR_SPACING, FONT_SCALE);
    };

    const auto on_off = [](bool b) { return std::string(b ? "ON" : "OFF"); };

    // ── LAYOUT — and its skin column, when the layout is a skinned one ───────────────────────────
    if (v.skinCount > 0) {
        dual_row(SettingsRow::LAYOUT, "LAYOUT", s.layoutText, "", s.skinText);
    } else {
        param_row(SettingsRow::LAYOUT, "LAYOUT", s.layoutText);
    }

    param_row(SettingsRow::SCALING, "SCALING", v.scalingBilinear ? "BILINEAR" : "INT");

    dual_row(SettingsRow::OVERLAY, "OVERLAY", s.overlayText, "STR", hex2(v.overlayStrength));

    dual_row(SettingsRow::BTN_SOUND, "BTN SOUND", on_off(v.buttonSoundEnabled),
             "VOL", hex2(v.buttonSoundVolume));
    dual_row(SettingsRow::BTN_VIBRO, "BTN VIBRO", on_off(v.buttonVibroEnabled),
             "POW", hex2(v.vibroPower));

    param_row(SettingsRow::KB_INSERT, "KB INSERT", v.insertBefore ? "BEFORE" : "AFTER");
    param_row(SettingsRow::CURSOR,    "CURSOR",    v.cursorRemember ? "REMEMBER" : "REFRESH");
    param_row(SettingsRow::NOTE_PREV, "NOTE PREV", on_off(v.notePreviewEnabled));

    {
        const std::vector<std::string>& names = visualizer_names();
        const int index = clamp(static_cast<int>(t.visualizerType), 0,
                                static_cast<int>(names.size()) - 1);
        param_row(SettingsRow::VISUALIZER, "VISUALIZER", names[static_cast<size_t>(index)]);
    }

    // ⚠️ THEME shows the name and a ">" — the arrow is the promise that A opens something. It does
    // not yet: the theme editor is its own module (port plan §10, Phase 3), and until it lands this
    // row is inert, exactly as every EQ cell in the app dials a slot number it cannot yet open.
    param_row(SettingsRow::THEME, "THEME", s.themeName + " >");

    // TEMPLATE is a BUTTON row, like PROJECT's — two options, no value.
    if (settings_row_visible(SettingsRow::TEMPLATE, s.caps)) {
        const SettingsRow row = SettingsRow::TEMPLATE;
        const int ry = rowY(row);
        if (on_row(row)) c.fill_rect(x, ry, WIDTH, ROW_HEIGHT, t.rowCursor);
        c.draw_text("TEMPLATE", labelX, ry + TEXT_PADDING,
                    on_row(row) ? t.textCursor : t.textParam, CHAR_SPACING, FONT_SCALE);
        const char* options[2] = {"SAVE", "CLEAR"};
        for (int i = 0; i < 2; ++i) {
            c.draw_text(options[i], val1X + i * 80, ry + TEXT_PADDING,
                        on_cell(row, i + 1) ? t.textCursor : t.textValue, CHAR_SPACING, FONT_SCALE);
        }
    }

    param_row(SettingsRow::RESUME, "RESUME", v.autosaveResumeAuto ? "AUTO" : "ASK");

    // ── TRACE (+ ENG) ────────────────────────────────────────────────────────────────────────────
    // ⚠️ The only row whose COLUMN COUNT is caps-dependent. ENG picks which sequencer walks the song
    // — and in this process there is no other one to pick. Both live on one row because the twelve
    // above them already consume 395 of the panel's 392 pixels, so a fourteenth row would draw
    // underneath it and be invisible.
    if (s.caps.engineToggle) {
        dual_row(SettingsRow::TRACE, "TRACE", on_off(v.traceEnabled),
                 "ENG", v.engineCpp ? "C++" : "KT");
    } else {
        param_row(SettingsRow::TRACE, "TRACE", on_off(v.traceEnabled));
    }
}

// ─── Cursor ──────────────────────────────────────────────────────────────────────────────────────

CursorContext SettingsModule::cursor_context(const SettingsState& s) const {
    const SettingsValues& v = s.values;

    if (s.cursorColumn == 0) return cc::read_only();

    const SettingsRow row = static_cast<SettingsRow>(s.cursorRow);
    if (!settings_row_visible(row, s.caps)) return cc::none();

    switch (row) {
        case SettingsRow::LAYOUT:
            if (s.cursorColumn == 2) {
                // The skin column exists only while the layout is skinned.
                if (v.skinCount <= 0) return cc::read_only();
                return cc::enum_cycle(v.skinIndex, v.skinCount);
            }
            return cc::enum_cycle(v.layoutIndex, v.layoutCount);

        case SettingsRow::SCALING:
            return cc::toggle_binary(v.scalingBilinear);

        case SettingsRow::OVERLAY:
            if (s.cursorColumn == 1) return cc::enum_cycle(v.overlayIndex, v.overlayCount);
            return cc::hex_byte(v.overlayStrength, 0, 255);

        case SettingsRow::BTN_SOUND:
            if (s.cursorColumn == 1) return cc::toggle_binary(v.buttonSoundEnabled);
            return cc::hex_byte(v.buttonSoundVolume, 0, 255);

        case SettingsRow::BTN_VIBRO:
            if (s.cursorColumn == 1) return cc::toggle_binary(v.buttonVibroEnabled);
            return cc::hex_byte(v.vibroPower, 0, 255);

        case SettingsRow::KB_INSERT:  return cc::toggle_binary(v.insertBefore);
        case SettingsRow::CURSOR:     return cc::toggle_binary(v.cursorRemember);
        case SettingsRow::NOTE_PREV:  return cc::toggle_binary(v.notePreviewEnabled);

        case SettingsRow::VISUALIZER:
            return cc::enum_cycle(static_cast<int>(s.theme.visualizerType),
                                  static_cast<int>(visualizer_names().size()));

        // A opens the theme editor.
        case SettingsRow::THEME:      return cc::read_only();
        // A triggers SAVE or CLEAR.
        case SettingsRow::TEMPLATE:   return cc::read_only();

        case SettingsRow::RESUME:     return cc::toggle_binary(v.autosaveResumeAuto);

        case SettingsRow::TRACE:
            if (s.cursorColumn == 1) return cc::toggle_binary(v.traceEnabled);
            // Column 2 is ENG. Unreachable without the cap — the cursor cannot move right onto it —
            // but answered honestly rather than guessed at, the way MIXER answers its dead cells.
            if (!s.caps.engineToggle) return cc::none();
            return cc::toggle_binary(v.engineCpp);
    }
    return cc::none();
}

// ─── Input ───────────────────────────────────────────────────────────────────────────────────────

SettingsInputResult SettingsModule::handle_input(SettingsValues& v, Theme& theme,
                                                 const PlatformCaps& caps,
                                                 int cursor_row, int cursor_column,
                                                 const InputAction& action) const {
    const SettingsRow row = static_cast<SettingsRow>(cursor_row);
    const bool        set = (action.type == ActionType::SET_VALUE);

    if (!settings_row_visible(row, caps)) return SettingsInputResult{false};

    switch (row) {
        case SettingsRow::LAYOUT:
            if (set) {
                if (cursor_column == 2) {
                    if (v.skinCount > 0) v.skinIndex = clamp(action.value, 0, v.skinCount - 1);
                } else {
                    // Kotlin: `modes.getOrElse(action.value) { modes.first() }` — an index the cycle
                    // could not have produced falls back to the FIRST mode, not to the nearest one.
                    v.layoutIndex = (action.value >= 0 && action.value < v.layoutCount)
                                        ? action.value : 0;
                }
            }
            break;

        case SettingsRow::SCALING:
            if (set) v.scalingBilinear = action.value > 0;
            break;

        case SettingsRow::OVERLAY:
            if (set) {
                if (cursor_column == 1) {
                    // Kotlin: `options.getOrElse(action.value) { "OFF" }` — out of range means OFF,
                    // which IS index 0, so the fallback is the same shape as LAYOUT's.
                    v.overlayIndex = (action.value >= 0 && action.value < v.overlayCount)
                                         ? action.value : 0;
                } else if (cursor_column == 2) {
                    v.overlayStrength = clamp(action.value, 0, 255);
                }
            }
            break;

        case SettingsRow::BTN_SOUND:
            if (set) {
                if (cursor_column == 1)      v.buttonSoundEnabled = action.value > 0;
                else if (cursor_column == 2) v.buttonSoundVolume  = clamp(action.value, 0, 255);
            }
            break;

        case SettingsRow::BTN_VIBRO:
            if (set) {
                if (cursor_column == 1)      v.buttonVibroEnabled = action.value > 0;
                else if (cursor_column == 2) v.vibroPower         = clamp(action.value, 0, 255);
            }
            break;

        case SettingsRow::KB_INSERT: if (set) v.insertBefore       = action.value > 0; break;
        case SettingsRow::CURSOR:    if (set) v.cursorRemember     = action.value > 0; break;
        case SettingsRow::NOTE_PREV: if (set) v.notePreviewEnabled = action.value > 0; break;

        case SettingsRow::VISUALIZER:
            if (set) {
                const int count = static_cast<int>(visualizer_names().size());
                // Kotlin: `types.getOrNull(action.value) ?: types[0]`.
                const int index = (action.value >= 0 && action.value < count) ? action.value : 0;
                theme.visualizerType = static_cast<VisualizerType>(index);
            }
            break;

        // A-only rows. Nothing to set — the dispatcher owns what A does.
        case SettingsRow::THEME:
        case SettingsRow::TEMPLATE:
            break;

        case SettingsRow::RESUME:
            if (set) v.autosaveResumeAuto = action.value > 0;
            break;

        case SettingsRow::TRACE:
            if (set) {
                if (cursor_column == 1)                            v.traceEnabled = action.value > 0;
                else if (cursor_column == 2 && caps.engineToggle)  v.engineCpp    = action.value > 0;
            }
            break;
    }

    return SettingsInputResult{action.type != ActionType::NONE};
}

}  // namespace pt::ui
