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
    values.traceEnabled       = get_bool(j, "trace",              values.traceEnabled);

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
    // ⚠️ LAYOUT, SKIN and OVERLAY (the row's *selection*) are deliberately NOT here. Those three are
    // INDICES into lists that do not exist yet — the layouts, the skinned D-pads and the overlay PNGs
    // all arrive in Phase D — and an index is meaningless without the list it indexes. Android stores
    // them as stable strings (`layout_mode`, `portrait_skin`, `overlay_name`) and they stay in
    // SharedPreferences, which the OS keeps regardless of what this repo deletes, until Phase D can
    // resolve a name to an index. That is what `SETTINGS_IMPORT_VERSION` exists to make safe: the
    // migration is versioned, so Phase D runs a second pass rather than needing this one to have been
    // clairvoyant.
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
    j["trace"]              = values.traceEnabled;
    j["autosaveResumeAuto"] = values.autosaveResumeAuto;   // S10 — the RESUME row
    j["visualizer"]         = static_cast<int>(theme.visualizerType);

    // The Android device rows — see the matching block in load_settings for why these are written on
    // every platform a full phase before any of them is displayed. On the shell they are simply their
    // defaults; on Android they are what the C6 import rescued out of SharedPreferences.
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
