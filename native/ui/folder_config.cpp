#include "ui/folder_config.h"

#include "byte_source.h"        // pt_path_is_uri — ONE rule about what a URI is, shared with pt_fopen
#include "songcore/media_path.h"  // app_root_relative_tail — the SAME re-rooting a sample path gets
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
 * The relative tail of `path` under `root`, or `path` unchanged when it is not under it.
 *
 * ⚠️ DERIVED from the accessor rather than hard-coded as "Samples", so a platform whose default for a
 * category is NOT a direct child of the root seeds a value that still means what it says. The template
 * has one job — to be true.
 */
std::string strip_root(const std::string& path, const std::string& root) {
    if (root.empty() || path.size() <= root.size() + 1) return path;
    if (path.compare(0, root.size(), root) != 0 || path[root.size()] != '/') return path;
    return path.substr(root.size() + 1);
}

}  // namespace

std::string resolve_folder_override(const std::string& value, const std::string& media_root) {
    if (value.empty() || media_root.empty()) return value;
    // The same absolute test `resolve_media_path` makes, through the same function — a Windows drive
    // (`C:\…`) has no `//` and so is not a URI, and is caught by its drive-letter clause.
    if (songcore::path_is_absolute(value)) return value;
    return media_root + "/" + value;
}

std::string resolve_browse_dir(FileSystem& fs, const std::optional<std::string>& value,
                               const std::string& def) {
    if (!value || value->empty()) return def;

    // The root is DERIVED from this category's own default rather than named, so pt-ui never learns
    // whether a root is a path or a granted-tree id.
    const std::string root = fs.parent_path(def);
    const std::string dir  = resolve_folder_override(*value, root);

    // ⚠️⚠️ **A PATH OF THE WRONG KIND IS UNREACHABLE HOWEVER GOOD `is_directory` SAYS IT LOOKS, and
    // that is not hypothetical — it is the bug this function exists for.** Under SAF the app holds no
    // storage permission, yet `stat("/storage/emulated/0/Documents/PocketTracker/Projects")` still
    // SUCCEEDS while listing it is denied: `is_directory` answered yes, the browser opened on the
    // user's own config value, and every one of their sixteen projects was invisible. A plain path
    // cannot be read through a granted tree and a `pt://` path cannot be read through libc, so the
    // kinds must match before the answer means anything.
    const bool same_kind = pt_path_is_uri(dir.c_str()) == pt_path_is_uri(root.c_str());
    if (same_kind && fs.is_directory(dir)) return dir;

    // Authored under ANOTHER install's root — a config carried off a phone, or written before this
    // device's root became a granted tree. Re-rooted through the very function a project's absolute
    // sample paths go through, so a config and the samples it points at cannot disagree about where
    // the app's folders are. Empty tail = a folder genuinely outside the app tree: keep the default
    // rather than invent a location.
    const std::string tail = songcore::app_root_relative_tail(dir);
    if (!tail.empty()) {
        const std::string rerooted = root + "/" + tail;
        if (fs.is_directory(rerooted)) return rerooted;
    }
    return def;
}

bool load_folder_config(FileSystem& fs, FolderConfig& out) {
    std::string blob;
    if (!fs.read_file(fs.config_path(), blob)) return false;   // no file: the common case, not an error

    const json j = json::parse(blob, nullptr, /*allow_exceptions=*/false);
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

bool seed_config_template(FileSystem& fs, const KeyboardBindings& keyboardDefaults) {
    const std::string path = fs.config_path();
    // ⚠️ Empty is Android with nothing granted yet: there is no tree to seed INTO, and the write would
    // fail anyway. Saying so here rather than letting it fail keeps "no config yet" one condition.
    if (path.empty()) return false;
    if (fs.file_exists(path)) return false;   // the user's file — never rewrite it (header contract)

    // Every key pre-filled with what the app is doing RIGHT NOW, so the user sees the schema AND a real,
    // editable value rather than a blank they have to guess the shape of. `..._directory()` creates the
    // folder on first use (StdFileSystem::ensure_dir), which is fine — those dirs exist the moment the
    // browser opens anyway. The "_README" keys are not part of the schema (load ignores what it does not
    // recognise); they are there for the human who opens the file.
    json j;
    j["_README"] =
        "PocketTracker configuration. This file is YOURS: the app reads it at startup and never "
        "rewrites it. Every key is optional — delete a line to use the built-in default. Values below "
        "are the defaults, so the file as seeded changes nothing.";

    // ⭐ **Seeded ROOT-RELATIVE, and that is what makes the file portable.** The values below are what
    // the app is doing right now, written the way a human would write them ("Samples") rather than as
    // the absolute path the accessor returned — which on Android is a granted-tree id nobody can type
    // and which stops meaning anything the day the home folder changes. The root is DERIVED from an
    // accessor, not named, so pt-ui never has to know what a platform's root string looks like.
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
        "Keyboard keys per button. A button listed here REPLACES its defaults (so [] unbinds it); a "
        "button left out keeps them. Names are SDL key names — single characters are capitalised (\"K\"), "
        "and multi-word names use spaces (\"Left Shift\", \"Return\", \"Escape\", \"Space\", the arrows "
        "\"Up\"/\"Down\"/\"Left\"/\"Right\"). An unrecognised name is reported in the app's log and that "
        "one entry is skipped.";

    // Derived from the shell's live table — never restated here. See the header: a second copy of the
    // key map is a copy that drifts, and the template's whole job is to be true.
    json keyboard = json::object();
    for (int i = 0; i < static_cast<int>(Button::COUNT); ++i) {
        const Button b = static_cast<Button>(i);
        if (const auto& names = keyboardDefaults[b]) keyboard[button_name(b)] = *names;
    }
    j["keyboard"] = std::move(keyboard);

    return fs.write_file(path, j.dump(2) + "\n");
}

}  // namespace pt::ui
