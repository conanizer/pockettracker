#include "ui/folder_config.h"

#include "common/byte_source.h"        // pt_path_is_uri — one rule for what a URI is
#include "songcore/media_path.h"  // app_root_relative_tail — the same re-rooting as a sample path
#include "vendor/nlohmann/json.hpp"

namespace pt::ui {

namespace {

using nlohmann::json;

/** A key that is absent, non-string or empty leaves the override unset (→ the default). */
std::optional<std::string> get_folder(const json& folders, const char* key) {
    const auto it = folders.find(key);
    if (it == folders.end() || !it->is_string()) return std::nullopt;
    std::string v = it->get<std::string>();
    if (v.empty()) return std::nullopt;
    return v;
}

/**
 * The relative tail of `path` under `root`, or `path` unchanged when it is not under it. Derived from
 * the accessor, not hard-coded, so a seeded value stays true on any platform's layout.
 */
std::string strip_root(const std::string& path, const std::string& root) {
    if (root.empty() || path.size() <= root.size() + 1) return path;
    if (path.compare(0, root.size(), root) != 0 || path[root.size()] != '/') return path;
    return path.substr(root.size() + 1);
}

}  // namespace

std::string resolve_folder_override(const std::string& value, const std::string& media_root) {
    if (value.empty() || media_root.empty()) return value;
    // The same absolute test as `resolve_media_path`, through the same function.
    if (songcore::path_is_absolute(value)) return value;
    return media_root + "/" + value;
}

std::string resolve_browse_dir(FileSystem& fs, const std::optional<std::string>& value,
                               const std::string& def) {
    if (!value || value->empty()) return def;

    // The root is derived from this category's default, so pt-ui never learns whether it is a path or
    // a granted-tree id.
    const std::string root = fs.parent_path(def);
    const std::string dir  = resolve_folder_override(*value, root);

    // ⚠️⚠️ A path of the WRONG KIND is unreachable whatever `is_directory` says: under SAF a plain path
    // can be stat()ed but not listed (a user's whole project library once opened as empty). Plain
    // paths are unreadable through a granted tree and `pt://` through libc, so the kinds must match.
    const bool same_kind = pt_path_is_uri(dir.c_str()) == pt_path_is_uri(root.c_str());
    if (same_kind && fs.is_directory(dir)) return dir;

    // Authored under ANOTHER install's root: re-rooted through the same function as a project's sample
    // paths, so a config and its samples agree. Empty tail = outside the app tree: keep the default.
    const std::string tail = songcore::app_root_relative_tail(dir);
    if (!tail.empty()) {
        const std::string rerooted = root + "/" + tail;
        if (fs.is_directory(rerooted)) return rerooted;
    }
    return def;
}

bool load_folder_config(FileSystem& fs, FolderConfig& out) {
    std::string blob;
    if (!fs.read_file(fs.config_path(), blob)) return false;   // no file: the common case

    json j = json::parse(blob, nullptr, /*allow_exceptions=*/false);
    // The input loader reports what was wrong; this one reads the file the same way, silently.
    if (!j.is_object() && straighten_typographic_punctuation(blob))
        j = json::parse(blob, nullptr, /*allow_exceptions=*/false);
    if (!j.is_object()) return false;

    const auto fit = j.find("folders");
    if (fit == j.end() || !fit->is_object()) return false;
    const json& folders = *fit;

    out.samples     = get_folder(folders, "samples");
    out.soundfonts  = get_folder(folders, "soundfonts");
    out.instruments = get_folder(folders, "instruments");
    out.projects    = get_folder(folders, "projects");
    out.themes      = get_folder(folders, "themes");
    return true;
}

namespace {

/** The starter file and the example share this text, so the two cannot drift apart. */
std::string config_template_text(FileSystem& fs, const ButtonBindings& keyboardDefaults,
                                 const ButtonBindings& gamepadDefaults) {
    // Every key pre-filled with what the app is doing now, so the user sees the schema with real values.
    // The `..._directory()` calls create those folders on first use (harmless). "_README" keys are for
    // the human; load ignores them.
    json j;
    j["_README"] =
        "PocketTracker configuration. config.json is YOURS: the app reads it at startup and never "
        "rewrites it. config.example.json beside it lists every setting at its default and is "
        "rewritten by the app, so a newer version's settings show up there — copy the lines you want "
        "into config.json. Every key is optional — delete a line to use the built-in default.";

    // ⭐ Seeded ROOT-RELATIVE ("Samples"), so the file is portable and typable — on Android the absolute
    // form is a granted-tree id that changes with the home folder.
    const std::string mediaRoot = fs.parent_path(fs.samples_directory());

    j["_README_folders"] =
        "Where a LOAD browse STARTS for each category. A plain name is inside your PocketTracker folder "
        "(\"Samples\", \"Samples/Packs\"); an absolute path (\"/mnt/sdcard/Music\", \"C:\\\\Music\") is "
        "used as given. A folder this device cannot read is ignored, and one written under another "
        "device's PocketTracker folder is re-read against yours. This does not change where anything is "
        "SAVED.";
    j["folders"] = {
        {"samples",     strip_root(fs.samples_directory(),     mediaRoot)},
        {"soundfonts",  strip_root(fs.soundfonts_directory(),  mediaRoot)},
        {"instruments", strip_root(fs.instruments_directory(), mediaRoot)},
        {"projects",    strip_root(fs.projects_directory(),    mediaRoot)},
        {"themes",      strip_root(fs.themes_directory(),      mediaRoot)},
    };

    j["_README_controller"] =
        "abxy: SETTINGS > ABXY is the control for this now, and it appears whenever a controller is "
        "attached. This key SEEDS it: it is used only while ABXY still says AUTO, so a value written "
        "here before that row existed keeps working. Which way round your pad's face buttons are "
        "PRINTED. \"auto\" trusts the controller "
        "(correct for a built-in handheld pad and a real Switch pad). Use \"nintendo\" if A is the "
        "RIGHT button but the app reads it as B — common with 8BitDo pads in XInput mode, which report "
        "themselves as Xbox controllers. \"xbox\" = A is the bottom button. Keyboard keys are never "
        "affected by this.";
    j["controller"] = {{"abxy", abxy_name(AbxyLayout::AUTO)}};

    j["_README_keyboard"] =
        "Keyboard keys per button. A button listed here REPLACES its defaults (so [] unbinds it) and "
        "takes the keys it lists away from any other button; a button left out keeps its defaults. "
        "Names are SDL key names — single characters are capitalised (\"K\"), "
        "and multi-word names use spaces (\"Left Shift\", \"Return\", \"Escape\", \"Space\", the arrows "
        "\"Up\"/\"Down\"/\"Left\"/\"Right\"). An unrecognised name is reported in the app's log and that "
        "one entry is skipped.";

    // From the shell's live table, never restated — the template's job is to be true.
    json keyboard = json::object();
    for (int i = 0; i < static_cast<int>(Button::COUNT); ++i) {
        const Button b = static_cast<Button>(i);
        if (const auto& names = keyboardDefaults[b]) keyboard[button_name(b)] = *names;
    }
    j["keyboard"] = std::move(keyboard);

    j["_README_gamepad"] =
        "Controller buttons per app button, the same rules as \"keyboard\". Names are what is PRINTED "
        "on the pad: a b x y, back, start, leftshoulder, rightshoulder, dpup dpdown dpleft dpright "
        "(also lefttrigger, righttrigger, leftstick, rightstick, guide, misc1, paddle1-4). Example - START and SELECT on Y and X: "
        "\"START\": [\"start\", \"y\"], \"SELECT\": [\"back\", \"x\"].";
    json gamepad = json::object();
    for (int i = 0; i < static_cast<int>(Button::COUNT); ++i) {
        const Button b = static_cast<Button>(i);
        if (const auto& names = gamepadDefaults[b]) gamepad[button_name(b)] = *names;
    }
    j["gamepad"] = std::move(gamepad);

    j["_README_repeat"] =
        "Holding a direction: \"delay\" = ms before it starts repeating, \"interval\" = ms between "
        "repeats at full speed (it starts at twice that and speeds up over about a second). Smaller is "
        "faster. delay 100-2000, interval 16-500.";
    j["repeat"] = {{"delay", RepeatConfig::DEFAULT_DELAY}, {"interval", RepeatConfig::DEFAULT_INTERVAL}};

    return j.dump(2) + "\n";
}

}  // namespace

bool seed_config_template(FileSystem& fs, const ButtonBindings& keyboardDefaults,
                          const ButtonBindings& gamepadDefaults) {
    const std::string path = fs.config_path();
    // ⚠️ Empty: Android with nothing granted yet — nowhere to seed into.
    if (path.empty()) return false;
    if (fs.file_exists(path)) return false;   // the user's file — never rewritten
    return fs.write_file(path, config_template_text(fs, keyboardDefaults, gamepadDefaults));
}

bool write_config_example(FileSystem& fs, const ButtonBindings& keyboardDefaults,
                          const ButtonBindings& gamepadDefaults) {
    const std::string path = config_example_path(fs);
    if (path.empty()) return false;
    const std::string text = config_template_text(fs, keyboardDefaults, gamepadDefaults);
    std::string old;
    if (fs.read_file(path, old) && old == text) return false;   // no write per launch
    return fs.write_file(path, text);
}

std::string config_example_path(FileSystem& fs) {
    const std::string config = fs.config_path();
    const std::string name   = "config.json";
    if (config.size() < name.size() || config.compare(config.size() - name.size(), name.size(), name) != 0)
        return {};
    return config.substr(0, config.size() - name.size()) + "config.example.json";
}

}  // namespace pt::ui
