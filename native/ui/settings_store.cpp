#include "ui/settings_store.h"

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

int clamp(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

constexpr int VISUALIZER_COUNT = 6;   // VisualizerType — SCOPE … SPECTRUM_PEAKS

}  // namespace

bool load_settings(FileSystem& fs, SettingsValues& values, Theme& theme) {
    std::string blob;
    if (!fs.read_file(fs.settings_path(), blob)) return false;   // first launch

    const json j = json::parse(blob, nullptr, /*allow_exceptions=*/false);
    if (!j.is_object()) return false;

    values.scalingBilinear    = get_bool(j, "scalingBilinear",    values.scalingBilinear);
    values.insertBefore       = get_bool(j, "insertBefore",       values.insertBefore);
    values.cursorRemember     = get_bool(j, "cursorRemember",     values.cursorRemember);
    values.notePreviewEnabled = get_bool(j, "notePreview",        values.notePreviewEnabled);

    values.rememberFolder     = get_bool(j, "rememberFolder", values.rememberFolder);   // not the path
    values.navSongRelative    = get_bool(j, "navSongRelative", values.navSongRelative);
    values.traceEnabled       = get_bool(j, "trace",              values.traceEnabled);

    values.abxyIndex          = clamp(get_int(j, "abxy", values.abxyIndex), 0, 2);
    values.touchButtonsWithPad = get_bool(j, "touchButtonsWithPad", values.touchButtonsWithPad);

    values.metronomeEnabled   = get_bool(j, "metronome", values.metronomeEnabled);
    values.metronomeVolume    = clamp(get_int(j, "metronomeVolume", values.metronomeVolume), 0, 255);

    values.helpMode           = clamp(get_int(j, "help", values.helpMode), 0, 2);

    // ── MIDI: the cable's half (the song's half is in the .ptp) ──────────────────────────────────
    // Devices by name — port lists reorder on every replug; a missing name resolves to OFF.
    values.midiOutDevice      = get_string(j, "midi_out_device", values.midiOutDevice);
    values.midiInDevice       = get_string(j, "midi_in_device", values.midiInDevice);
    values.midiOffsetMs       = clamp(get_int(j, "midi_offset_ms", values.midiOffsetMs), -99, 99);
    // ⚠️ After the line above, which its fallback reads: an older file is AUTO only if its offset was 0.
    values.midiOffsetAuto     = get_bool(j, "midi_offset_auto", values.midiOffsetMs == 0);
    values.midiSyncOut        = get_bool(j, "midi_sync_out", values.midiSyncOut);
    values.midiSyncIn         = clamp(get_int(j, "midi_sync_in", values.midiSyncIn), 0, 2);
    // 0-15 or `MIDI_CTL_CH_ALL`; out of range (an older build's `-1`) → ALL.
    values.midiControlChannel = get_int(j, "midi_control_channel", values.midiControlChannel);
    if (values.midiControlChannel < 0 || values.midiControlChannel > songcore::MIDI_CTL_CH_ALL)
        values.midiControlChannel = songcore::MIDI_CTL_CH_ALL;
    values.midiInVoices = get_int(j, "midi_in_voices", values.midiInVoices);
    if (values.midiInVoices < 1 || values.midiInVoices > 8) values.midiInVoices = 4;
    values.midiVelocity = get_bool(j, "midi_velocity", values.midiVelocity);
    values.audioOutput  = get_string(j, "audio_output", values.audioOutput);   // a driver name

    values.autosaveResumeAuto = get_bool(j, "autosaveResumeAuto", values.autosaveResumeAuto);

    // ── The device rows, read and written on every platform even where not shown ─────────────────
    // ⚠️ Android's prefs import writes these keys before boot; a key one side writes and the other drops
    // would be lost on the first quit. SKIN and OVERLAY are stable names, resolved by the shell.
    values.portraitSkin       = get_string(j, "portrait_skin", values.portraitSkin);
    values.overlayName        = get_string(j, "overlay_name",  values.overlayName);
    values.buttonSoundEnabled = get_bool(j, "buttonSound",  values.buttonSoundEnabled);
    values.buttonVibroEnabled = get_bool(j, "buttonVibro",  values.buttonVibroEnabled);
    values.buttonSoundVolume  = clamp(get_int(j, "buttonSoundVolume", values.buttonSoundVolume), 0, 255);
    values.vibroPower         = clamp(get_int(j, "vibroPower",        values.vibroPower),        0, 255);
    values.overlayStrength    = clamp(get_int(j, "overlayStrength",   values.overlayStrength),   0, 255);

    // The user's, not the theme's: read first, so the theme load below cannot overwrite it.
    const int viz = clamp(get_int(j, "visualizer", static_cast<int>(theme.visualizerType)),
                          0, VISUALIZER_COUNT - 1);

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
    j["scalingBilinear"]    = values.scalingBilinear;
    j["insertBefore"]       = values.insertBefore;
    j["cursorRemember"]     = values.cursorRemember;
    j["notePreview"]        = values.notePreviewEnabled;
    j["rememberFolder"]     = values.rememberFolder;
    j["navSongRelative"]    = values.navSongRelative;
    j["abxy"]               = values.abxyIndex;            // 0 AUTO, 1 XBOX, 2 NINTENDO
    j["touchButtonsWithPad"] = values.touchButtonsWithPad;
    j["metronome"]          = values.metronomeEnabled;
    j["metronomeVolume"]    = values.metronomeVolume;
    j["help"]               = values.helpMode;             // 0 OFF, 1 SHORT, 2 FULL
    j["trace"]              = values.traceEnabled;
    j["autosaveResumeAuto"] = values.autosaveResumeAuto;
    j["visualizer"]         = static_cast<int>(theme.visualizerType);

    j["midi_out_device"]    = values.midiOutDevice;
    j["midi_in_device"]     = values.midiInDevice;
    j["midi_offset_ms"]     = values.midiOffsetMs;
    j["midi_offset_auto"]   = values.midiOffsetAuto;
    j["midi_sync_out"]      = values.midiSyncOut;
    j["midi_sync_in"]       = values.midiSyncIn;
    j["midi_control_channel"] = values.midiControlChannel;   // 0-15 (shown 01-16), or MIDI_CTL_CH_ALL
    j["midi_in_voices"]       = values.midiInVoices;         // 1 = MONO, 2-8 = POLY
    j["midi_velocity"]        = values.midiVelocity;
    j["audio_output"]         = values.audioOutput;

    j["portrait_skin"]      = values.portraitSkin;
    j["overlay_name"]       = values.overlayName;
    j["buttonSound"]        = values.buttonSoundEnabled;
    j["buttonSoundVolume"]  = values.buttonSoundVolume;
    j["buttonVibro"]        = values.buttonVibroEnabled;
    j["vibroPower"]         = values.vibroPower;
    j["overlayStrength"]    = values.overlayStrength;

    j["theme"]    = theme.name;   // for older builds
    j["appTheme"] = json::parse(serialize_theme(theme), nullptr, /*allow_exceptions=*/false);

    return j.dump(2) + "\n";
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
