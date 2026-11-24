// The preset rows that step a folder — SCALE, GROOVE, the reverb, the delay and THEME. A step
// that would drop values no file holds asks PRESET NOT SAVED first; so does THEME's ROLL.

#include "ui/dispatch/dispatch_common.h"

#include "ui/fx_preset_bank.h"
#include "ui/preset_folder.h"

#include <algorithm>
#include <string>

namespace pt::ui {

namespace {

template <typename T>
std::string path_at(const std::vector<FolderPreset<T>>& list, int index) {
    return index >= 0 && index < static_cast<int>(list.size()) ? list[static_cast<size_t>(index)].path
                                                               : std::string();
}

// An entry's text: its file's, or a factory entry written out as its file would be — so either loads
// through the one parse a file takes.
template <typename T, typename Serialize>
bool entry_text(FileSystem& fs, const std::vector<FolderPreset<T>>& list, const std::string& path,
                Serialize serialize, std::string& text) {
    if (!is_factory_preset(path)) return fs.read_file(path, text);
    for (const FolderPreset<T>& e : list)
        if (e.path == path) {
            text = serialize(e.value);
            return true;
        }
    return false;
}

// What a new project's reverb and delay open on, and so what A+B returns to.
const NamedReverb& reverb_blank() {
    static const NamedReverb c = reverb_preset_cells(kReverbPresets[0]);
    return c;
}
const NamedDelay& delay_blank() {
    static const NamedDelay c = delay_preset_cells(kDelayPresets[0]);
    return c;
}

}  // namespace

// ─── Where the values on screen sit ──────────────────────────────────────────────────────────────

PresetRowView InputDispatcher::preset_row_view(PresetRow row) {
    const Project& p = host_.project();
    switch (row) {
        case PresetRow::SCALE: {
            static const songcore::Scale blank;
            const songcore::Scale& now = p.scales[static_cast<size_t>(s_.currentScale)];
            return pt::ui::preset_row_view(presetLists_.scale(false), now.name, now, same_scale, &blank);
        }
        case PresetRow::GROOVE: {
            static const songcore::Groove blank;
            const songcore::Groove& now = p.grooves[static_cast<size_t>(s_.currentGroove)];
            return pt::ui::preset_row_view(presetLists_.groove(false), now.name, now, same_groove, &blank);
        }
        case PresetRow::REVERB:
            return pt::ui::preset_row_view(presetLists_.reverb(false), p.reverbPreset, reverb_cells_of(p),
                                           same_reverb, &reverb_blank());
        case PresetRow::DELAY:
            return pt::ui::preset_row_view(presetLists_.delay(false), p.delayPreset, delay_cells_of(p), same_delay,
                                           &delay_blank());
        case PresetRow::THEME:
            return pt::ui::preset_row_view<Theme>(presetLists_.theme(false), s_.theme.name, s_.theme, same_theme,
                                                  nullptr);
        default:
            return {};
    }
}

void InputDispatcher::poll_preset_rows() {
    // A file may have come or gone while another screen was up.
    const int screen = theme_editor_up(s_) ? -2 : static_cast<int>(s_.currentScreen);
    presetLists_.forget_on_move(screen);

    const auto refresh = [this](PresetRow row) {
        s_.presetRows[static_cast<size_t>(row)] = preset_row_view(row);
    };
    if (theme_editor_up(s_)) refresh(PresetRow::THEME);
    if (!s_.project) return;
    switch (s_.currentScreen) {
        case ScreenType::SCALE:  refresh(PresetRow::SCALE);  break;
        case ScreenType::GROOVE: refresh(PresetRow::GROOVE); break;
        default:                 break;
    }
    // The send editor draws a section of EFFECTS over other screens.
    if (s_.currentScreen == ScreenType::EFFECTS || s_.sendEditor.isOpen) {
        refresh(PresetRow::REVERB);
        refresh(PresetRow::DELAY);
    }
}

bool InputDispatcher::preset_kept(PresetRow row) {
    const Project& p = host_.project();
    switch (row) {
        case PresetRow::SCALE: {
            // An untouched slot has nothing to lose.
            const songcore::Scale& now = p.scales[static_cast<size_t>(s_.currentScale)];
            return same_scale(now, songcore::Scale{}) ||
                   preset_values_kept(presetLists_.scale(true), now.name, now, same_scale);
        }
        case PresetRow::GROOVE: {
            const songcore::Groove& now = p.grooves[static_cast<size_t>(s_.currentGroove)];
            return same_groove(now, songcore::Groove{}) ||
                   preset_values_kept(presetLists_.groove(true), now.name, now, same_groove);
        }
        case PresetRow::REVERB:
            return preset_values_kept(presetLists_.reverb(true), p.reverbPreset, reverb_cells_of(p), same_reverb);
        case PresetRow::DELAY:
            return preset_values_kept(presetLists_.delay(true), p.delayPreset, delay_cells_of(p), same_delay);
        case PresetRow::THEME:
            // Colours only rolled are not the user's work.
            return s_.themeHandRows == 0 ||
                   preset_values_kept<Theme>(presetLists_.theme(true), s_.theme.name, s_.theme, same_theme);
        default:
            return true;
    }
}

// ─── The step ────────────────────────────────────────────────────────────────────────────────────

bool InputDispatcher::preset_step(PresetRow row, int index) {
    // The target by path, read afresh: a file may have come or gone since the row was drawn.
    std::string path;
    switch (row) {
        case PresetRow::SCALE:  path = path_at(presetLists_.scale(true), index);  break;
        case PresetRow::GROOVE: path = path_at(presetLists_.groove(true), index); break;
        case PresetRow::REVERB: path = path_at(presetLists_.reverb(true), index); break;
        case PresetRow::DELAY:  path = path_at(presetLists_.delay(true), index);  break;
        case PresetRow::THEME:  path = path_at(presetLists_.theme(true), index);  break;
        default:                return false;
    }
    if (path.empty()) return false;

    if (!preset_kept(row)) {
        s_.presetStep = {row, path};
        s_.confirm.open(ConfirmDialogState::Kind::PRESET_UNSAVED);
        return false;
    }
    return preset_apply(row, path);
}

bool InputDispatcher::preset_apply(PresetRow row, const std::string& path) {
    bool changed = false;
    // THEME is a setting, not the song.
    return load_preset_file(row, path, &changed) && changed && row != PresetRow::THEME;
}

bool InputDispatcher::preset_carry_out(const AppState::PendingPresetStep& step) {
    if (step.row == PresetRow::THEME && step.path.empty()) {
        theme_roll_palette(-1);
        return false;
    }
    return preset_apply(step.row, step.path);
}

bool InputDispatcher::load_preset_file(PresetRow row, const std::string& path, bool* changed) {
    bool        moved = false;
    bool        ok    = false;
    std::string text;
    switch (row) {
        case PresetRow::SCALE: {
            // Into a copy that keeps the slot's id, committed only once it parses.
            songcore::Scale& slot   = host_.edit_project().scales[static_cast<size_t>(s_.currentScale)];
            songcore::Scale  loaded = slot;
            ok = entry_text(fs_, presetLists_.scale(false), path, serialize_scale, text) &&
                 parse_scale_text(text, loaded);
            if (ok) {
                moved = loaded.name != slot.name || loaded.enabled != slot.enabled;
                slot  = loaded;
            }
            break;
        }
        case PresetRow::GROOVE: {
            songcore::Groove& slot   = host_.edit_project().grooves[static_cast<size_t>(s_.currentGroove)];
            songcore::Groove  loaded = slot;
            ok = entry_text(fs_, presetLists_.groove(false), path, serialize_groove, text) &&
                 parse_groove_text(text, loaded);
            if (ok) {
                moved = loaded.name != slot.name || loaded.steps != slot.steps ||
                        loaded.ticksPerStep != slot.ticksPerStep;
                slot  = loaded;
            }
            break;
        }
        case PresetRow::REVERB: {
            const NamedReverb now   = reverb_cells_of(host_.project());
            NamedReverb       cells = now;
            ok = entry_text(fs_, presetLists_.reverb(false), path, serialize_reverb_cells, text) &&
                 parse_reverb_cells(text, fx_preset_file_stem(path), cells);
            if (ok) {
                moved = cells.name != now.name || !same_reverb(cells, now);
                apply_reverb_cells(host_.edit_project(), cells);
            }
            break;
        }
        case PresetRow::DELAY: {
            const NamedDelay now   = delay_cells_of(host_.project());
            NamedDelay       cells = now;
            ok = entry_text(fs_, presetLists_.delay(false), path, serialize_delay_cells, text) &&
                 parse_delay_cells(text, fx_preset_file_stem(path), cells);
            if (ok) {
                moved = cells.name != now.name || !same_delay(cells, now);
                apply_delay_cells(host_.edit_project(), cells);
            }
            break;
        }
        case PresetRow::THEME: {
            // ⚠️ A failed parse must not blank the palette. The visualizer is the user's, not the theme's.
            Theme loaded{};
            ok = entry_text(fs_, presetLists_.theme(false), path, serialize_theme, text) &&
                 parse_theme(text, loaded);
            if (ok) {
                loaded.visualizerType = s_.theme.visualizerType;
                moved            = loaded.name != s_.theme.name || !same_theme(loaded, s_.theme);
                s_.theme         = loaded;
                s_.themeHandRows = 0;
            }
            break;
        }
        default:
            break;
    }
    if (changed) *changed = moved;
    return ok;
}

// ─── SAVE ────────────────────────────────────────────────────────────────────────────────────────

void InputDispatcher::preset_open_save(PresetRow row) {
    const Project& p = host_.project();
    // Each is seeded with the sanitized name the row shows: what the file will be called.
    switch (row) {
        case PresetRow::SCALE: {
            const std::string seed =
                safe_name(songcore::scale_display_name(p.scales[static_cast<size_t>(s_.currentScale)]));
            open_qwerty(QwertyContext::SCALE_SAVE, seed.empty() ? "SCALE" : seed, "SAVE SCALE:",
                        fs_.scales_directory(), /*max_length=*/20, /*clear_on_first_b=*/true);
            break;
        }
        case PresetRow::GROOVE: {
            const std::string seed =
                safe_name(songcore::groove_display_name(p.grooves[static_cast<size_t>(s_.currentGroove)]));
            open_qwerty(QwertyContext::GROOVE_SAVE, seed.empty() ? "GROOVE" : seed, "SAVE GROOVE:",
                        fs_.grooves_directory(), /*max_length=*/20, /*clear_on_first_b=*/true);
            break;
        }
        case PresetRow::REVERB: {
            const std::string seed = safe_name(reverb_preset_display_name(p));
            open_qwerty(QwertyContext::REVERB_SAVE, seed.empty() ? "REVERB" : seed, "SAVE REVERB:",
                        fs_.reverbs_directory(), FX_PRESET_NAME_MAX, /*clear_on_first_b=*/true);
            break;
        }
        case PresetRow::DELAY: {
            const std::string seed = safe_name(delay_preset_display_name(p));
            open_qwerty(QwertyContext::DELAY_SAVE, seed.empty() ? "DELAY" : seed, "SAVE DELAY:",
                        fs_.delays_directory(), FX_PRESET_NAME_MAX, /*clear_on_first_b=*/true);
            break;
        }
        case PresetRow::THEME: {
            // ⚠️ Opens without closing the editor (QWERTY is above THEME).
            const std::string seed = safe_name(s_.theme.name);
            open_qwerty(QwertyContext::THEME_SAVE, seed.empty() ? "THEME" : seed, "SAVE THEME:",
                        fs_.themes_directory(), /*max_length=*/20, /*clear_on_first_b=*/true);
            break;
        }
        default:
            break;
    }
}

void InputDispatcher::preset_step_after_save(const AppState::PendingPresetStep& step, PresetRow saved) {
    presetLists_.forget();
    if (step.row != saved) return;
    if (preset_carry_out(step)) mark_modified();
}

}  // namespace pt::ui
