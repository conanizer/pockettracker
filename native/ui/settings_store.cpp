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

    // Tolerant by design: a settings file the user has hand-edited into nonsense costs them their
    // settings, not their session.
    const json j = json::parse(blob, nullptr, /*allow_exceptions=*/false);
    if (!j.is_object()) return false;

    values.scalingBilinear    = get_bool(j, "scalingBilinear",    values.scalingBilinear);
    values.insertBefore       = get_bool(j, "insertBefore",       values.insertBefore);
    values.cursorRemember     = get_bool(j, "cursorRemember",     values.cursorRemember);
    values.notePreviewEnabled = get_bool(j, "notePreview",        values.notePreviewEnabled);

    // FOLDER = REMEMBER / REFRESH (D2a). Only the TOGGLE persists; `lastSampleFolder` is deliberately
    // SESSION-ONLY — it resets to the default folder on every launch, exactly like CURSOR remembering.
    // So the choice survives a restart but the remembered path does not (the user's call).
    values.rememberFolder     = get_bool(j, "rememberFolder", values.rememberFolder);

    // NAV = POOL / SONG. ⚠️ A ROW THIS PLATFORM HAS MUST BE PERSISTED, or the setting silently resets
    // on every launch and nothing anywhere says so — the S10 RESUME bug's exact shape, one row later.
    // ⚠️ Absent from an older settings.json → the struct default, which is SONG. A file written before
    // this row existed carries no opinion about NAV, so an upgrading user gets the new default exactly
    // as a fresh install does; once the row is touched the choice is written and sticks.
    values.navSongRelative    = get_bool(j, "navSongRelative", values.navSongRelative);
    values.traceEnabled       = get_bool(j, "trace",              values.traceEnabled);

    // ABXY = AUTO / XBOX / NINTENDO, and the on-screen buttons kept under a pad. Both are absent from
    // every settings.json written before their rows existed, and both struct defaults are what the app
    // already did - AUTO is a no-op and a pad already turned the buttons off - so an upgrade changes
    // nothing until the row is touched.
    values.abxyIndex          = clamp(get_int(j, "abxy", values.abxyIndex), 0, 2);
    values.touchButtonsWithPad = get_bool(j, "touchButtonsWithPad", values.touchButtonsWithPad);

    // ── MIDI (B4.3) — the CABLE's half. The song's half is in the .ptp. ──────────────────────────
    //
    // ⚠️ THE DEVICE IS A NAME AND NOT AN INDEX, and MIDI is the case that rule exists for: a port list
    // is re-enumerated from the OS and reorders on every replug, so an index saved yesterday opens a
    // different synth today. The shell resolves the name against the live list at boot; a name that is
    // not there resolves to OFF, which is the truth rather than a wrong guess (settings_editor.h).
    //
    // Absent from an older settings.json → OFF / 0 ms, which is what every existing file has.
    values.midiOutDevice      = get_string(j, "midi_out_device", values.midiOutDevice);
    // E2 — the INPUT port. Absent from every settings.json written before phase E → "OFF", which is
    // also the right default: an app that opened whatever keyboard it found would hold a port another
    // program wanted, and would do it on the say-so of nobody.
    values.midiInDevice       = get_string(j, "midi_in_device", values.midiInDevice);
    values.midiOffsetMs       = clamp(get_int(j, "midi_offset_ms", values.midiOffsetMs), -99, 99);
    // Phase C. Absent → false, which is both the default and what every settings.json written before
    // phase C says: a file from yesterday must not silently start driving a drum machine today.
    values.midiSyncOut        = get_bool(j, "midi_sync_out", values.midiSyncOut);

    // ⚠️ RESUME (S10). New here because the shell only GAINED the row in S10 — and the session that
    // flips the cap on is the session that must add the key, or the setting resets to ASK on every
    // launch and nothing anywhere says so. See the header: this is S9's theme-by-name bug's shape, and
    // the only check that can catch it is a save → load round trip (ptdispatch §28).
    //
    // Absent from an older settings.json → the default (ASK) stays, which is the right answer for an
    // upgrading user: a prompt they can say no to, not a silent restore they never asked for.
    values.autosaveResumeAuto = get_bool(j, "autosaveResumeAuto", values.autosaveResumeAuto);

    // ── The Android device rows (C6) ─────────────────────────────────────────────────────────────
    //
    // ⚠️⚠️ **THESE ARE READ AND WRITTEN ON EVERY PLATFORM WHILE NO PLATFORM DISPLAYS THEM, AND THAT IS
    // DELIBERATE.** This file's header states the rule they appear to break — "only the rows the shell
    // actually HAS are persisted", because writing a value for a question the platform never asked is
    // inventing an answer. C6 is the case that rule anticipated in its own next sentence: *"if Android
    // ever converges onto this UI it brings its own answers with it — and its own keys."* This is that,
    // arriving one phase before the rows do.
    //
    // The ordering is forced, and it is the whole reason these land now rather than in Phase D. The
    // one-shot prefs import (`SdlActivity.importLegacySettings`) runs BEFORE native boot and writes
    // these keys into settings.json. If `serialize_settings` did not know them, the very first quit
    // would rewrite the file without them and the user's BTN SOUND / VIBRO / overlay strength would be
    // gone — silently, between the import that rescued them and the phase that finally shows them.
    // A key that is written by one component and dropped by another is worse than a key nobody writes.
    //
    // ⚠️ SKIN is now persisted, as a STABLE STRING — this is the "Phase D second pass" the deferral
    // below anticipated. `portrait_skin` (matching Android's SharedPreferences key) survives the shell's
    // skin list being reordered; the shell resolves the name to an index at boot (device_skin.h). An
    // absent key keeps the default, which is what an upgrading user's file has until they touch it.
    //
    // ⚠️ OVERLAY is now persisted too, as a STABLE STRING — the "Phase D second pass" the note below
    // anticipated, landing with D6's CRT overlay. `overlay_name` (matching Android's SharedPreferences
    // key) survives the shell's overlay list being reordered; the shell resolves the name to an index at
    // boot (shell/overlay.h), and "OFF" is the no-overlay choice. An absent key keeps the default (OFF),
    // which is what an upgrading user's file has until they touch it.
    //
    // ⚠️ LAYOUT (the row's *mode selection*) is STILL deliberately not here. There is no shell-side layout
    // MODE override to resolve a name against — the shell auto-selects portrait/landscape by orientation
    // and fullscreen by controller presence — so Android keeps `layout_mode` in SharedPreferences with no
    // consumer on this UI. `SETTINGS_IMPORT_VERSION` keeps that versioned and safe.
    values.portraitSkin       = get_string(j, "portrait_skin", values.portraitSkin);
    values.overlayName        = get_string(j, "overlay_name",  values.overlayName);
    values.buttonSoundEnabled = get_bool(j, "buttonSound",  values.buttonSoundEnabled);
    values.buttonVibroEnabled = get_bool(j, "buttonVibro",  values.buttonVibroEnabled);
    values.buttonSoundVolume  = clamp(get_int(j, "buttonSoundVolume", values.buttonSoundVolume), 0, 255);
    values.vibroPower         = clamp(get_int(j, "vibroPower",        values.vibroPower),        0, 255);
    values.overlayStrength    = clamp(get_int(j, "overlayStrength",   values.overlayStrength),   0, 255);

    // The visualizer is the theme's field but the USER's choice, so it survives the theme load below —
    // which is why it is read first and handed in rather than left to be overwritten.
    const int viz = clamp(get_int(j, "visualizer", static_cast<int>(theme.visualizerType)),
                          0, VISUALIZER_COUNT - 1);

    // ⚠️⚠️ THE WHOLE PALETTE, NOT ITS NAME — AND UNTIL S9 THIS WAS A NAME.
    //
    // S7 wrote `theme = theme_by_name(j["theme"], viz)` and was RIGHT TO, on the state of the world it
    // was written in: the four built-ins were the entire palette set, so a name WAS a palette and
    // re-deriving one from the other was lossless. The THEME EDITOR ends that. A theme is now an
    // arbitrary eighteen-colour palette that exists nowhere but in this file, and storing its NAME threw
    // away every colour the user had dialled — silently, on every quit, with the app cheerfully coming
    // back up in CLASSIC. Android never had the bug: it serializes the entire AppTheme into
    // SharedPreferences (`prefs["app_theme"]`, MainActivity), which is exactly what this now mirrors.
    //
    // ⚠️ NO TOOL IN THE LADDER COULD HAVE SEEN IT. ptinput compares the cell an edit lands in; ptdispatch
    // drives the dispatcher. Neither one QUITS AND RELAUNCHES THE APP — and that is the only place this
    // bug lives. It is the recurring shape, one more time: an assumption that was true when it was made,
    // invalidated by the layer built on top of it, in a channel nothing was pointed at. `ptdispatch` §27
    // is pointed at it now (save → load → the colours are still there).
    //
    // The nested object is a `.ptt` in all but name: it goes through the SAME serializer, so a palette in
    // settings.json and a palette on the SD card cannot drift apart in format.
    if (const auto it = j.find("appTheme"); it != j.end() && it->is_object())
        parse_theme(it->dump(), theme);
    else
        theme = theme_by_name(get_string(j, "theme", theme.name), theme.visualizerType);  // pre-S9 file

    theme.visualizerType = static_cast<VisualizerType>(viz);
    return true;
}

namespace {

/**
 * The exact bytes `settings.json` should hold for this state.
 *
 * ⚠️ ONE writer, deliberately: `save_settings_if_changed` COMPARES against this and `save_settings`
 * WRITES it, so the two can never disagree about format. A second copy of the layout would make the
 * comparison drift from the write, and the file would then be rewritten on every quit (harmless) or
 * never (silent loss) depending on which way it drifted.
 */
std::string serialize_settings(const SettingsValues& values, const Theme& theme) {
    json j;
    j["scalingBilinear"]    = values.scalingBilinear;
    j["insertBefore"]       = values.insertBefore;
    j["cursorRemember"]     = values.cursorRemember;
    j["notePreview"]        = values.notePreviewEnabled;
    j["rememberFolder"]     = values.rememberFolder;      // D2a — FOLDER row (the toggle only; the
                                                          // remembered PATH is session-only, not saved)
    j["navSongRelative"]    = values.navSongRelative;   // the NAV row — POOL / SONG
    j["abxy"]               = values.abxyIndex;            // the ABXY row - 0 AUTO, 1 XBOX, 2 NINTENDO
    j["touchButtonsWithPad"] = values.touchButtonsWithPad; // LAYOUT s FULL / PORTRAIT under a pad
    j["trace"]              = values.traceEnabled;
    j["autosaveResumeAuto"] = values.autosaveResumeAuto;   // S10 — the RESUME row
    j["visualizer"]         = static_cast<int>(theme.visualizerType);

    // B4.3 — the MIDI screen's two cable settings. `midi_out_device` is a stable NAME string, for the
    // same reason `portrait_skin` and `overlay_name` below are (see load_settings).
    j["midi_out_device"]    = values.midiOutDevice;
    j["midi_in_device"]     = values.midiInDevice;   // E2 — the INPUT port, a NAME for the same reason
    j["midi_offset_ms"]     = values.midiOffsetMs;
    j["midi_sync_out"]      = values.midiSyncOut;   // phase C — the clock + transport switch

    // The Android device rows — see the matching block in load_settings for why these are written on
    // every platform a full phase before any of them is displayed. On the shell they are simply their
    // defaults; on Android they are what the C6 import rescued out of SharedPreferences. `portrait_skin`
    // is the one selection now LIVE on the shell (Phase D), written as a stable id string.
    j["portrait_skin"]      = values.portraitSkin;
    j["overlay_name"]       = values.overlayName;   // D6 — the overlay SELECTION, a stable id string
    j["buttonSound"]        = values.buttonSoundEnabled;
    j["buttonSoundVolume"]  = values.buttonSoundVolume;
    j["buttonVibro"]        = values.buttonVibroEnabled;
    j["vibroPower"]         = values.vibroPower;
    j["overlayStrength"]    = values.overlayStrength;

    // The palette itself, through the `.ptt` serializer (see load_settings). `theme` is still written
    // beside it, and NOT as a leftover: it is what an OLDER build reads, and what a human scanning the
    // file sees. The reader prefers `appTheme` and falls back to it.
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

    // ⚠️ A file that cannot be READ is a file that must be WRITTEN — first launch (no file at all), and
    // an unreadable or hand-mangled one, both land here and both want the current state put down. The
    // `!=` is what makes an untouched session a no-op without anyone having to have remembered to say so.
    std::string current;
    if (fs.read_file(fs.settings_path(), current) && current == wanted) return SettingsWrite::UNCHANGED;

    return save_settings(fs, values, theme) ? SettingsWrite::SAVED : SettingsWrite::FAILED;
}

}  // namespace pt::ui
