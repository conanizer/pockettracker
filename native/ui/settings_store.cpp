#include "ui/settings_store.h"

#include <algorithm>
#include <string>

#include "ui/theme_io.h"
#include "vendor/nlohmann/json.hpp"

namespace pt::ui {

namespace {

using nlohmann::json;

// An absent or wrong-typed key leaves the default alone.
bool get_bool(const json& j, const char* key, bool fallback) {
    const auto it = j.find(key);
    if (it == j.end() || !it->is_boolean()) return fallback;
    return it->get<bool>();
}

int get_int(const json& j, const char* key, int fallback) {
    const auto it = j.find(key);
    if (it == j.end() || !it->is_number_integer()) return fallback;
    return it->get<int>();
}

std::string get_string(const json& j, const char* key, const std::string& fallback) {
    const auto it = j.find(key);
    if (it == j.end() || !it->is_string()) return fallback;
    return it->get<std::string>();
}

using V = SettingsValues;

// The settings that are one key and one field, read and written by both halves. The rest are written
// out in each.
constexpr struct { const char* key; bool V::* field; } BOOLS[] = {
    {"scalingBilinear", &V::scalingBilinear},       {"insertBefore", &V::insertBefore},
    {"cursorRemember", &V::cursorRemember},         {"notePreview", &V::notePreviewEnabled},
    {"oneHanded", &V::oneHanded},                   {"rememberFolder", &V::rememberFolder},   // not the path
    {"navSongRelative", &V::navSongRelative},       {"trace", &V::traceEnabled},
    {"touchButtonsWithPad", &V::touchButtonsWithPad}, {"metronome", &V::metronomeEnabled},
    {"midi_sync_out", &V::midiSyncOut},             {"midi_velocity", &V::midiVelocity},
    {"autosaveResumeAuto", &V::autosaveResumeAuto}, {"buttonSound", &V::buttonSoundEnabled},
    {"buttonVibro", &V::buttonVibroEnabled},            {"topPanelAsked", &V::topPanelAsked},
};

constexpr struct { const char* key; int V::* field; int lo, hi; } INTS[] = {
    {"oneHandHoldMs", &V::oneHandHoldMs, 50, 5000},
    {"abxy", &V::abxyIndex, 0, 2},                  // 0 AUTO, 1 XBOX, 2 NINTENDO
    {"metronomeVolume", &V::metronomeVolume, 0, 255},
    {"textCase", &V::textCase, 0, 2},               // 0 UPPER, 1 lower, 2 Title
    {"midi_offset_ms", &V::midiOffsetMs, -99, 99},
    {"midi_sync_in", &V::midiSyncIn, 0, 2},
    {"buttonSoundVolume", &V::buttonSoundVolume, 0, 255},
    {"vibroPower", &V::vibroPower, 0, 255},
    {"overlayStrength", &V::overlayStrength, 0, 255},
};

// Devices by name — port lists reorder on every replug; a missing name resolves to OFF. SKIN and
// OVERLAY are stable names, resolved by the shell.
constexpr struct { const char* key; std::string V::* field; } STRINGS[] = {
    {"midi_out_device", &V::midiOutDevice}, {"midi_in_device", &V::midiInDevice},
    {"audio_output", &V::audioOutput},      // a driver name
    {"portrait_skin", &V::portraitSkin},    {"overlay_name", &V::overlayName},
    {"font", &V::fontName},                 // a UI_FONTS name
};

}  // namespace

// ⚠️ Every key is read and written on every platform, even where its row is not shown: Android's prefs
// import writes them before boot, and a key one side drops would be lost on the first quit.
bool load_settings(FileSystem& fs, SettingsValues& values, Theme& theme) {
    std::string blob;
    if (!fs.read_file(fs.settings_path(), blob)) return false;   // first launch

    const json j = json::parse(blob, nullptr, /*allow_exceptions=*/false);
    if (!j.is_object()) return false;

    for (const auto& s : BOOLS)   values.*s.field = get_bool(j, s.key, values.*s.field);
    for (const auto& s : INTS)    values.*s.field = std::clamp(get_int(j, s.key, values.*s.field), s.lo, s.hi);
    for (const auto& s : STRINGS) values.*s.field = get_string(j, s.key, values.*s.field);

    // ⚠️ After the offset, which its fallback reads: an older file is AUTO only if its offset was 0.
    values.midiOffsetAuto = get_bool(j, "midi_offset_auto", values.midiOffsetMs == 0);
    // 0-15 or `MIDI_CTL_CH_ALL`; out of range (an older build's `-1`) → ALL.
    values.midiControlChannel = get_int(j, "midi_control_channel", values.midiControlChannel);
    if (values.midiControlChannel < 0 || values.midiControlChannel > songcore::MIDI_CTL_CH_ALL)
        values.midiControlChannel = songcore::MIDI_CTL_CH_ALL;
    values.midiInVoices = get_int(j, "midi_in_voices", values.midiInVoices);
    if (values.midiInVoices < 1 || values.midiInVoices > 8) values.midiInVoices = 4;

    // The user's, not the theme's: read first, so the theme load below cannot overwrite it.
    const int viz = std::clamp(get_int(j, "visualizer", static_cast<int>(theme.visualizerType)),
                               0, VISUALIZER_TYPE_COUNT - 1);

    // ⚠️ The whole palette, not its name: an invented palette exists nowhere else. Through the `.ptt`
    // parser, so the two formats cannot drift.
    if (const auto it = j.find("appTheme"); it != j.end() && it->is_object())
        parse_theme(it->dump(), theme);
    else
        theme = theme_by_name(get_string(j, "theme", theme.name), theme.visualizerType);  // an older file

    theme.visualizerType = static_cast<VisualizerType>(viz);
    return true;
}

namespace {

// The exact bytes settings.json should hold — both the comparison and the write use it.
std::string serialize_settings(const SettingsValues& values, const Theme& theme) {
    json j;
    for (const auto& s : BOOLS)   j[s.key] = values.*s.field;
    for (const auto& s : INTS)    j[s.key] = values.*s.field;
    for (const auto& s : STRINGS) j[s.key] = values.*s.field;
    j["midi_offset_auto"]     = values.midiOffsetAuto;
    j["midi_control_channel"] = values.midiControlChannel;   // 0-15 (shown 01-16), or MIDI_CTL_CH_ALL
    j["midi_in_voices"]       = values.midiInVoices;         // 1 = MONO, 2-8 = POLY
    j["visualizer"]           = static_cast<int>(theme.visualizerType);

    j["theme"]    = theme.name;   // for older builds
    j["appTheme"] = json::parse(serialize_theme(theme), nullptr, /*allow_exceptions=*/false);

    // ⚠️ `replace`: the default handler THROWS on a folder path that is not whole UTF-8.
    return j.dump(2, ' ', false, json::error_handler_t::replace) + "\n";
}

}  // namespace

bool save_settings(FileSystem& fs, const SettingsValues& values, const Theme& theme) {
    return fs.write_file(fs.settings_path(), serialize_settings(values, theme));
}

SettingsWrite save_settings_if_changed(FileSystem& fs, const SettingsValues& values,
                                       const Theme& theme) {
    const std::string wanted = serialize_settings(values, theme);

    // A file that cannot be read is written.
    std::string current;
    if (fs.read_file(fs.settings_path(), current) && current == wanted) return SettingsWrite::UNCHANGED;

    return save_settings(fs, values, theme) ? SettingsWrite::SAVED : SettingsWrite::FAILED;
}

}  // namespace pt::ui
