#include "ui/modules/settings_editor.h"

#include <algorithm>

#include "ui/helpers.h"
#include "ui/input_config.h"   // abxy_name

namespace pt::ui {

namespace {

constexpr int NAME_X     = 10;   // the row label
constexpr int VAL1_X     = 190;  // the primary value
constexpr int SUBLABEL_X = 355;  // the secondary column's own label (STR / VOL / POW)
constexpr int VAL2_X     = 408;  // the secondary value


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

    // ── Scroll ───────────────────────────────────────────────────────────────────────────────────
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

    // ── LAYOUT, and its skin column when the layout is skinned ───────────────────────────────────
    if (v.skinCount > 0) {
        dual_row(SettingsRow::LAYOUT, "LAYOUT", s.layoutText, "", s.skinText);
    } else {
        param_row(SettingsRow::LAYOUT, "LAYOUT", s.layoutText);
    }

    param_row(SettingsRow::SCALING, "SCALING", v.scalingBilinear ? "BILINEAR" : "INT");

    dual_row(SettingsRow::OVERLAY, "OVERLAY", s.overlayText, "STR", hex2(v.overlayStrength));

    // An OS driver name: 18 characters to the panel edge.
    param_row(SettingsRow::AUDIO_OUT, "AUDIO OUT",
              Canvas::clip_text(s.audioOutText, (WIDTH - VAL1_X - NAME_X) / CHAR_W));

    dual_row(SettingsRow::BTN_SOUND, "BTN SOUND", on_off(v.buttonSoundEnabled),
             "VOL", hex2(v.buttonSoundVolume));
    // POW is LO/HI: the target ROMs have only EFFECT_TICK and EFFECT_CLICK (LO < 128 ≤ HI).
    dual_row(SettingsRow::BTN_VIBRO, "BTN VIBRO", on_off(v.buttonVibroEnabled),
             "POW", std::string(v.vibroPower >= 128 ? "HI" : "LO"));

    // The face-button swap; input_config.h names it, as config.json does.
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

    param_row(SettingsRow::RESUME, "RESUME", v.autosaveResumeAuto ? "AUTO" : "ASK");

    param_row(SettingsRow::TRACE, "TRACE", on_off(v.traceEnabled));

    // FOLDER = REMEMBER / REFRESH, placed by its own offset.
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
    if (!settings_row_visible(row, caps)) return SettingsInputResult{false};
    const SettingsInputResult done{action.type != ActionType::NONE};
    if (action.type != ActionType::SET_VALUE) return done;

    const int  value = action.value;
    const bool on    = value > 0;
    // An index the cycle could not produce falls back to the first (OFF, AUTO, …).
    const auto index_in = [value](int count) { return (value >= 0 && value < count) ? value : 0; };

    switch (row) {
        case SettingsRow::LAYOUT:
            if (cursor_column != 2)  v.layoutIndex = index_in(v.layoutCount);
            else if (v.skinCount > 0) v.skinIndex  = clamp(value, 0, v.skinCount - 1);
            break;

        case SettingsRow::OVERLAY:
            if (cursor_column == 1)      v.overlayIndex    = index_in(v.overlayCount);
            else if (cursor_column == 2) v.overlayStrength = clamp(value, 0, 255);
            break;

        case SettingsRow::BTN_SOUND:
            if (cursor_column == 1)      v.buttonSoundEnabled = on;
            else if (cursor_column == 2) v.buttonSoundVolume  = clamp(value, 0, 255);
            break;

        case SettingsRow::BTN_VIBRO:
            if (cursor_column == 1)      v.buttonVibroEnabled = on;
            // Store 64 (EFFECT_TICK) or 255 (EFFECT_CLICK).
            else if (cursor_column == 2) v.vibroPower         = on ? 255 : 64;
            break;

        case SettingsRow::METRONOME:
            if (cursor_column == 1)      v.metronomeEnabled = on;
            else if (cursor_column == 2) v.metronomeVolume  = clamp(value, 0, 255);
            break;

        case SettingsRow::ABXY: v.abxyIndex = index_in(3); break;
        // The shell opens the output and writes the NAME once it plays.
        case SettingsRow::AUDIO_OUT: v.audioOutIndex = index_in(v.audioOutCount); break;

        case SettingsRow::SCALING:   v.scalingBilinear    = on; break;
        case SettingsRow::KB_INSERT: v.insertBefore       = on; break;
        case SettingsRow::CURSOR:    v.cursorRemember     = on; break;
        case SettingsRow::NOTE_PREV: v.notePreviewEnabled = on; break;
        case SettingsRow::FOLDER:    v.rememberFolder     = on; break;
        case SettingsRow::NAV:       v.navSongRelative    = on; break;
        case SettingsRow::RESUME:    v.autosaveResumeAuto = on; break;
        case SettingsRow::TRACE:     v.traceEnabled       = on; break;

        case SettingsRow::VISUALIZER:
            theme.visualizerType =
                static_cast<VisualizerType>(index_in(static_cast<int>(visualizer_names().size())));
            break;

        case SettingsRow::HELP: v.helpMode = clamp(value, 0, 2); break;

        // A-only rows: the dispatcher's.
        case SettingsRow::THEME:
        case SettingsRow::TEMPLATE:
            break;
    }
    return done;
}

}  // namespace pt::ui
