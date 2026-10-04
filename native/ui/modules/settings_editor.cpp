#include "ui/modules/settings_editor.h"

#include <algorithm>

#include "ui/helpers.h"
#include "ui/input_config.h"   // abxy_name - the ABXY row and config.json speak one vocabulary

namespace pt::ui {

namespace {

constexpr int NAME_X     = 10;   // the row label
constexpr int VAL1_X     = 190;  // the primary value
constexpr int SUBLABEL_X = 355;  // the secondary column's own label (STR / VOL / POW)
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

    // ── Scroll ───────────────────────────────────────────────────────────────────────────────────
    // The rows scroll to keep the cursor visible when they overflow (debug builds); in release they
    // fit and `maxScroll` is 0. Derived from the cursor row each frame — no stored scroll.
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

    /** A row with one value. */
    const auto param_row = [&](SettingsRow row, const char* name, const std::string& value) {
        if (!settings_row_visible(row, s.caps)) return;
        const int ry = rowY(row);
        c.draw_text(name, labelX, ry + TEXT_PADDING,
                    on_row(row) ? cursor_mark_ink(t) : t.textParam, CHAR_SPACING, FONT_SCALE);
        draw_cursor_cell(c, value, val1X, ry + TEXT_PADDING, on_cell(row, 1), t.textValue, t);
    };

    /** A row with two values, the second behind its own little label (STR / VOL / POW). */
    const auto dual_row = [&](SettingsRow row, const char* name, const std::string& value1,
                              const char* sublabel, const std::string& value2) {
        if (!settings_row_visible(row, s.caps)) return;
        const int ry = rowY(row);
        c.draw_text(name, labelX, ry + TEXT_PADDING,
                    on_row(row) ? cursor_mark_ink(t) : t.textParam, CHAR_SPACING, FONT_SCALE);
        draw_cursor_cell(c, value1, val1X, ry + TEXT_PADDING, on_cell(row, 1), t.textValue, t);
        // The sublabel is textParam whether or not the cursor is on the row.
        c.draw_text(sublabel, subX, ry + TEXT_PADDING, t.textParam, CHAR_SPACING, FONT_SCALE);
        draw_cursor_cell(c, value2, val2X, ry + TEXT_PADDING, on_cell(row, 2), t.textValue, t);
    };

    const auto on_off = [](bool b) { return std::string(b ? "ON" : "OFF"); };

    // Every row below is drawn through `rowY` (which subtracts `scrollY`); clip them to the viewport so a
    // scrolled row cannot overdraw the title above or spill past the panel edge. RAII, restored at return.
    const Canvas::ClipScope rowsClip(c, x, firstRowY, WIDTH, viewportH);

    // ── LAYOUT — and its skin column, when the layout is a skinned one ───────────────────────────
    if (v.skinCount > 0) {
        dual_row(SettingsRow::LAYOUT, "LAYOUT", s.layoutText, "", s.skinText);
    } else {
        param_row(SettingsRow::LAYOUT, "LAYOUT", s.layoutText);
    }

    param_row(SettingsRow::SCALING, "SCALING", v.scalingBilinear ? "BILINEAR" : "INT");

    dual_row(SettingsRow::OVERLAY, "OVERLAY", s.overlayText, "STR", hex2(v.overlayStrength));

    // A driver's name is the OS's and may not fit: 18 characters to the panel edge.
    param_row(SettingsRow::AUDIO_OUT, "AUDIO OUT",
              Canvas::clip_text(s.audioOutText, (WIDTH - VAL1_X - NAME_X) / CHAR_W));

    dual_row(SettingsRow::BTN_SOUND, "BTN SOUND", on_off(v.buttonSoundEnabled),
             "VOL", hex2(v.buttonSoundVolume));
    // POW is LO/HI, not 00-FF: the target ROMs have no Composition primitives, only EFFECT_TICK and
    // EFFECT_CLICK. LO stores <128 (TICK), HI ≥128 (CLICK).
    dual_row(SettingsRow::BTN_VIBRO, "BTN VIBRO", on_off(v.buttonVibroEnabled),
             "POW", std::string(v.vibroPower >= 128 ? "HI" : "LO"));

    // The face-button swap. Its names come from input_config.h, which is also what parses the same
    // three words out of config.json - one vocabulary, not two.
    param_row(SettingsRow::ABXY, "ABXY",
              abxy_name(static_cast<AbxyLayout>(clamp(v.abxyIndex, 0, 2))));

    dual_row(SettingsRow::METRONOME, "METRONOME", on_off(v.metronomeEnabled),
             "VOL", hex2(v.metronomeVolume));

    param_row(SettingsRow::KB_INSERT, "KB INSERT", v.insertBefore ? "BEFORE" : "AFTER");
    param_row(SettingsRow::CURSOR,    "CURSOR",    v.cursorRemember ? "REMEMBER" : "REFRESH");
    param_row(SettingsRow::NAV,       "NAV",       v.navSongRelative ? "SONG" : "POOL");
    param_row(SettingsRow::NOTE_PREV, "NOTE PREV", on_off(v.notePreviewEnabled));

    {
        const std::vector<std::string>& names = visualizer_names();
        const int index = clamp(static_cast<int>(t.visualizerType), 0,
                                static_cast<int>(names.size()) - 1);
        param_row(SettingsRow::VISUALIZER, "VISUALIZER", names[static_cast<size_t>(index)]);
    }

    {
        static constexpr const char* HELP_NAMES[3] = {"OFF", "SHORT", "FULL"};
        param_row(SettingsRow::HELP, "HELP", HELP_NAMES[clamp(v.helpMode, 0, 2)]);
    }

    // THEME shows the name and a ">": A opens the theme editor (theme_editor.cpp).
    // ⚠️ The name is clipped two glyphs short of the column so the ">" — the only sign this row
    // opens a screen — cannot be pushed out by a long user-typed name.
    {
        constexpr int VALUE_COLS = (WIDTH - 10 - VAL1_X) / CHAR_W;
        param_row(SettingsRow::THEME, "THEME",
                  Canvas::clip_text(s.themeName, VALUE_COLS - 2) + " >");
    }

    // TEMPLATE is a BUTTON row, like PROJECT's — two options, no value.
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

    param_row(SettingsRow::RESUME, "RESUME", v.autosaveResumeAuto ? "AUTO" : "ASK");

    param_row(SettingsRow::TRACE, "TRACE", on_off(v.traceEnabled));

    // FOLDER = REMEMBER / REFRESH. It positions itself by its own offset_y, so drawing it last is
    // source order only.
    param_row(SettingsRow::FOLDER, "FOLDER", v.rememberFolder ? "REMEMBER" : "REFRESH");
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
            return cc::toggle_binary(v.vibroPower >= 128);  // LO / HI, not a 0..255 knob

        case SettingsRow::METRONOME:
            if (s.cursorColumn == 1) return cc::toggle_binary(v.metronomeEnabled);
            return cc::hex_byte(v.metronomeVolume, 0, 255);

        case SettingsRow::ABXY:       return cc::enum_cycle(v.abxyIndex, 3);
        case SettingsRow::AUDIO_OUT:  return cc::enum_cycle(v.audioOutIndex, v.audioOutCount);
        case SettingsRow::KB_INSERT:  return cc::toggle_binary(v.insertBefore);
        case SettingsRow::CURSOR:     return cc::toggle_binary(v.cursorRemember);
        case SettingsRow::NOTE_PREV:  return cc::toggle_binary(v.notePreviewEnabled);
        case SettingsRow::FOLDER:     return cc::toggle_binary(v.rememberFolder);
        case SettingsRow::NAV:        return cc::toggle_binary(v.navSongRelative);

        case SettingsRow::VISUALIZER:
            return cc::enum_cycle(static_cast<int>(s.theme.visualizerType),
                                  static_cast<int>(visualizer_names().size()));

        case SettingsRow::HELP:       return cc::enum_cycle(v.helpMode, 3);

        // A opens the theme editor.
        case SettingsRow::THEME:      return cc::read_only();
        // A triggers SAVE or CLEAR.
        case SettingsRow::TEMPLATE:   return cc::read_only();

        case SettingsRow::RESUME:     return cc::toggle_binary(v.autosaveResumeAuto);

        case SettingsRow::TRACE:      return cc::toggle_binary(v.traceEnabled);
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
                    // An index the cycle could not produce falls back to the FIRST mode.
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
                    // Out of range means OFF, which is index 0.
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
                // LO/HI switch: store 64 (→EFFECT_TICK) or 255 (→EFFECT_CLICK). See the draw note.
                else if (cursor_column == 2) v.vibroPower         = (action.value > 0) ? 255 : 64;
            }
            break;

        case SettingsRow::METRONOME:
            if (set) {
                if (cursor_column == 1)      v.metronomeEnabled = action.value > 0;
                else if (cursor_column == 2) v.metronomeVolume  = clamp(action.value, 0, 255);
            }
            break;

        // Out of range falls back to AUTO - index 0, the same shape LAYOUT and OVERLAY use.
        case SettingsRow::ABXY:
            if (set) v.abxyIndex = (action.value >= 0 && action.value < 3) ? action.value : 0;
            break;
        // The shell opens the output and writes the NAME once it plays.
        case SettingsRow::AUDIO_OUT:
            if (set) v.audioOutIndex = (action.value >= 0 && action.value < v.audioOutCount) ? action.value : 0;
            break;
        case SettingsRow::KB_INSERT: if (set) v.insertBefore       = action.value > 0; break;
        case SettingsRow::CURSOR:    if (set) v.cursorRemember     = action.value > 0; break;
        case SettingsRow::NOTE_PREV: if (set) v.notePreviewEnabled = action.value > 0; break;
        case SettingsRow::FOLDER:    if (set) v.rememberFolder     = action.value > 0; break;
        case SettingsRow::NAV:       if (set) v.navSongRelative    = action.value > 0; break;

        case SettingsRow::VISUALIZER:
            if (set) {
                const int count = static_cast<int>(visualizer_names().size());
                const int index = (action.value >= 0 && action.value < count) ? action.value : 0;
                theme.visualizerType = static_cast<VisualizerType>(index);
            }
            break;

        case SettingsRow::HELP:
            if (set) v.helpMode = clamp(action.value, 0, 2);
            break;

        // A-only rows. Nothing to set — the dispatcher owns what A does.
        case SettingsRow::THEME:
        case SettingsRow::TEMPLATE:
            break;

        case SettingsRow::RESUME:
            if (set) v.autosaveResumeAuto = action.value > 0;
            break;

        case SettingsRow::TRACE:
            if (set) v.traceEnabled = action.value > 0;
            break;
    }

    return SettingsInputResult{action.type != ActionType::NONE};
}

}  // namespace pt::ui
