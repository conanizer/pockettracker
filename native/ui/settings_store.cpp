#include "ui/settings_store.h"

#include <string>

#include "ui/theme_io.h"   // serialize_theme / parse_theme — one palette format, two files
#include "vendor/nlohmann/json.hpp"

namespace pt::ui {

namespace {

using nlohmann::json;

/** A key that is absent, null, or of the wrong type leaves the default alone. */
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

    // Tolerant: a hand-mangled settings file costs the user their settings, not their session.
    const json j = json::parse(blob, nullptr, /*allow_exceptions=*/false);
    if (!j.is_object()) return false;

    values.scalingBilinear    = get_bool(j, "scalingBilinear",    values.scalingBilinear);
    values.insertBefore       = get_bool(j, "insertBefore",       values.insertBefore);
    values.cursorRemember     = get_bool(j, "cursorRemember",     values.cursorRemember);
    values.notePreviewEnabled = get_bool(j, "notePreview",        values.notePreviewEnabled);

    // FOLDER: only the TOGGLE persists; `lastSampleFolder` is session-only and resets every launch.
    values.rememberFolder     = get_bool(j, "rememberFolder", values.rememberFolder);

    // NAV. ⚠️ A row this platform HAS must be persisted, or it silently resets every launch.
    // Absent (an older file) → the struct default, SONG, as on a fresh install.
    values.navSongRelative    = get_bool(j, "navSongRelative", values.navSongRelative);
    values.traceEnabled       = get_bool(j, "trace",              values.traceEnabled);

    // ABXY and the on-screen buttons under a pad. Absent → defaults that change nothing (AUTO; a pad
    // already hides the buttons).
    values.abxyIndex          = clamp(get_int(j, "abxy", values.abxyIndex), 0, 2);
    values.touchButtonsWithPad = get_bool(j, "touchButtonsWithPad", values.touchButtonsWithPad);

    // METRONOME — persisted like NAV. Absent → no click, as before the row existed.
    values.metronomeEnabled   = get_bool(j, "metronome", values.metronomeEnabled);
    values.metronomeVolume    = clamp(get_int(j, "metronomeVolume", values.metronomeVolume), 0, 255);

    // HELP — OFF / SHORT / FULL. Absent → SHORT.
    values.helpMode           = clamp(get_int(j, "help", values.helpMode), 0, 2);

    // ── MIDI — the CABLE's half (the song's half is in the .ptp) ─────────────────────────────────
    //
    // ⚠️ The device is a NAME, not an index: port lists reorder on every replug. The shell resolves the
    // name against the live list at boot; a missing name resolves to OFF — the truth, not a wrong guess.
    // Absent → OFF / 0 ms.
    values.midiOutDevice      = get_string(j, "midi_out_device", values.midiOutDevice);
    // The INPUT port. Absent → "OFF": opening whatever keyboard was found would take a port another
    // program may want.
    values.midiInDevice       = get_string(j, "midi_in_device", values.midiInDevice);
    values.midiOffsetMs       = clamp(get_int(j, "midi_offset_ms", values.midiOffsetMs), -99, 99);
    // ⚠️ MUST FOLLOW THE LINE ABOVE — its fallback reads that value. Absent → AUTO only if the offset was
    // 0: a non-zero offset was dialled in by ear against this desk's cable, and an upgrade must not undo it.
    values.midiOffsetAuto     = get_bool(j, "midi_offset_auto", values.midiOffsetMs == 0);
    // Absent → false: an old file must not suddenly start driving a drum machine.
    values.midiSyncOut        = get_bool(j, "midi_sync_out", values.midiSyncOut);
    // The knob channel: 0-15, or `MIDI_CTL_CH_ALL`. Absent → ALL, safe because a CC is CLAIMED, not
    // reserved (songcore/midi_map.h): with no mappings, no byte behaves differently.
    // ⚠️ Out of range (including a `-1` an older build could write) → ALL.
    values.midiControlChannel = get_int(j, "midi_control_channel", values.midiControlChannel);
    if (values.midiControlChannel < 0 || values.midiControlChannel > songcore::MIDI_CTL_CH_ALL)
        values.midiControlChannel = songcore::MIDI_CTL_CH_ALL;
    values.midiInVoices = get_int(j, "midi_in_voices", values.midiInVoices);
    if (values.midiInVoices < 1 || values.midiInVoices > 8) values.midiInVoices = 4;
    values.midiVelocity = get_bool(j, "midi_velocity", values.midiVelocity);
    // AUDIO OUT, by driver NAME — resolved by the shell against the drivers installed now.
    values.audioOutput  = get_string(j, "audio_output", values.audioOutput);

    // ⚠️ RESUME. A row this platform HAS, so it must be persisted; only a save → load round trip catches
    // the omission (tested). Absent → ASK: a prompt the user can refuse, not a silent restore.
    values.autosaveResumeAuto = get_bool(j, "autosaveResumeAuto", values.autosaveResumeAuto);

    // ── The device rows ──────────────────────────────────────────────────────────────────────────
    //
    // ⚠️⚠️ Read and written on EVERY platform, even where not displayed. Android's one-shot prefs import
    // (`SdlActivity.importLegacySettings`) writes these keys before native boot; if the serializer did not
    // know them, the first quit would drop them. A key one component writes and another drops is worse
    // than a key nobody writes.
    // SKIN and OVERLAY are persisted as STABLE STRINGS (`portrait_skin`, `overlay_name`) that survive the
    // shell's lists being reordered; the shell resolves them at boot (device_skin.h, shell/overlay.h).
    // LAYOUT's mode is NOT here: the shell auto-selects it (orientation, controller presence), so there
    // is nothing to resolve a name against.
    values.portraitSkin       = get_string(j, "portrait_skin", values.portraitSkin);
    values.overlayName        = get_string(j, "overlay_name",  values.overlayName);
    values.buttonSoundEnabled = get_bool(j, "buttonSound",  values.buttonSoundEnabled);
    values.buttonVibroEnabled = get_bool(j, "buttonVibro",  values.buttonVibroEnabled);
    values.buttonSoundVolume  = clamp(get_int(j, "buttonSoundVolume", values.buttonSoundVolume), 0, 255);
    values.vibroPower         = clamp(get_int(j, "vibroPower",        values.vibroPower),        0, 255);
    values.overlayStrength    = clamp(get_int(j, "overlayStrength",   values.overlayStrength),   0, 255);

    // The visualizer is the theme's field but the USER's choice: read first, so a theme load below
    // cannot overwrite it.
    const int viz = clamp(get_int(j, "visualizer", static_cast<int>(theme.visualizerType)),
                          0, VISUALIZER_COUNT - 1);

    // ⚠️⚠️ The WHOLE PALETTE, not its name: a theme can be an invented palette that exists nowhere but
    // here, and a name would lose every dialled colour on quit. Only a save → load round trip can see
    // that (tested). The nested object goes through the `.ptt` serializer, so the two formats
    // cannot drift.
    if (const auto it = j.find("appTheme"); it != j.end() && it->is_object())
        parse_theme(it->dump(), theme);
    else
        theme = theme_by_name(get_string(j, "theme", theme.name), theme.visualizerType);  // an older file

    theme.visualizerType = static_cast<VisualizerType>(viz);
    return true;
}

namespace {

/**
 * The exact bytes `settings.json` should hold. ⚠️ ONE writer: `save_settings_if_changed` compares
 * against this and `save_settings` writes it, so format cannot drift between the comparison and the
 * write.
 */
std::string serialize_settings(const SettingsValues& values, const Theme& theme) {
    json j;
    j["scalingBilinear"]    = values.scalingBilinear;
    j["insertBefore"]       = values.insertBefore;
    j["cursorRemember"]     = values.cursorRemember;
    j["notePreview"]        = values.notePreviewEnabled;
    j["rememberFolder"]     = values.rememberFolder;      // the FOLDER toggle (its path is session-only)
    j["navSongRelative"]    = values.navSongRelative;   // the NAV row — POOL / SONG
    j["abxy"]               = values.abxyIndex;            // the ABXY row - 0 AUTO, 1 XBOX, 2 NINTENDO
    j["touchButtonsWithPad"] = values.touchButtonsWithPad; // LAYOUT s FULL / PORTRAIT under a pad
    j["metronome"]          = values.metronomeEnabled;     // the METRONOME row - the click and its VOL
    j["metronomeVolume"]    = values.metronomeVolume;
    j["help"]               = values.helpMode;             // the HELP row - 0 OFF, 1 SHORT, 2 FULL
    j["trace"]              = values.traceEnabled;
    j["autosaveResumeAuto"] = values.autosaveResumeAuto;   // the RESUME row
    j["visualizer"]         = static_cast<int>(theme.visualizerType);

    // The MIDI cable settings. `midi_out_device` is a stable NAME, like `portrait_skin` (load_settings).
    j["midi_out_device"]    = values.midiOutDevice;
    j["midi_in_device"]     = values.midiInDevice;   // a NAME, for the same reason
    j["midi_offset_ms"]     = values.midiOffsetMs;
    j["midi_offset_auto"]   = values.midiOffsetAuto;
    j["midi_sync_out"]      = values.midiSyncOut;   // the clock + transport switch
    j["midi_control_channel"] = values.midiControlChannel;   // 0-15 (shown 01-16), or MIDI_CTL_CH_ALL
    j["midi_in_voices"]       = values.midiInVoices;         // 1 = MONO, 2-8 = POLY
    j["midi_velocity"]        = values.midiVelocity;
    j["audio_output"]         = values.audioOutput;

    // The device rows — written on every platform (see load_settings).
    j["portrait_skin"]      = values.portraitSkin;
    j["overlay_name"]       = values.overlayName;   // the overlay SELECTION, a stable id string
    j["buttonSound"]        = values.buttonSoundEnabled;
    j["buttonSoundVolume"]  = values.buttonSoundVolume;
    j["buttonVibro"]        = values.buttonVibroEnabled;
    j["vibroPower"]         = values.vibroPower;
    j["overlayStrength"]    = values.overlayStrength;

    // The palette itself, through the `.ptt` serializer. `theme` is written too: older builds read it,
    // and a human scanning the file sees it.
    j["theme"]    = theme.name;
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

    // ⚠️ A file that cannot be READ must be WRITTEN (first launch, or a mangled file). The `!=` makes an
    // untouched session a no-op.
    std::string current;
    if (fs.read_file(fs.settings_path(), current) && current == wanted) return SettingsWrite::UNCHANGED;

    return save_settings(fs, values, theme) ? SettingsWrite::SAVED : SettingsWrite::FAILED;
}

}  // namespace pt::ui
