#include "ui/folder_config.h"

#include "common/byte_source.h"        // pt_path_is_uri — one rule for what a URI is
#include "songcore/media_path.h"  // app_root_relative_tail — the same re-rooting as a sample path
#include "vendor/nlohmann/json.hpp"

namespace pt::ui {

namespace {

using nlohmann::json;

// Absent, non-string or empty: unset.
std::optional<std::string> get_folder(const json& folders, const char* key) {
    const auto it = folders.find(key);
    if (it == folders.end() || !it->is_string()) return std::nullopt;
    std::string v = it->get<std::string>();
    if (v.empty()) return std::nullopt;
    return v;
}

// `path` relative to `root`, or unchanged when it is not under it.
std::string strip_root(const std::string& path, const std::string& root) {
    if (root.empty() || path.size() <= root.size() + 1) return path;
    if (path.compare(0, root.size(), root) != 0 || path[root.size()] != '/') return path;
    return path.substr(root.size() + 1);
}

}  // namespace

std::string resolve_folder_override(const std::string& value, const std::string& media_root) {
    if (value.empty() || media_root.empty()) return value;
    if (songcore::path_is_absolute(value)) return value;
    return media_root + "/" + value;
}

std::string resolve_browse_dir(FileSystem& fs, const std::optional<std::string>& value,
                               const std::string& def) {
    if (!value || value->empty()) return def;

    const std::string root = fs.parent_path(def);
    const std::string dir  = resolve_folder_override(*value, root);

    // ⚠️ A path of the wrong kind is unreachable whatever `is_directory` says: on Android a plain path
    // can be stat()ed but not listed.
    const bool same_kind = pt_path_is_uri(dir.c_str()) == pt_path_is_uri(root.c_str());
    if (same_kind && fs.is_directory(dir)) return dir;

    // Written under another install's root: re-rooted as a project's sample paths are.
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
    // The input loader reports what was wrong; this one reads it silently.
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

// The starter file's and the example's text.
std::string config_template_text(FileSystem& fs, const ButtonBindings& keyboardDefaults,
                                 const ButtonBindings& gamepadDefaults) {
    // Every key filled with what the app does now. "_README" keys are for the human.
    json j;
    j["_README"] =
        "PocketTracker configuration. config.json is YOURS: the app reads it at startup and never "
        "rewrites it. config.example.json beside it lists every setting at its default and is "
        "rewritten by the app, so a newer version's settings show up there — copy the lines you want "
        "into config.json. Every key is optional — delete a line to use the built-in default.";

    // Root-relative ("Samples"), so the file is portable — on Android the absolute form is a tree id.
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

    // From the shell's live table, never restated.
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
    if (path.empty()) return false;   // Android with nothing granted yet
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
