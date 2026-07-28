#include "ui/folder_config.h"

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

}  // namespace

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

bool seed_folder_config_template(FileSystem& fs) {
    const std::string path = fs.config_path();
    if (fs.file_exists(path)) return false;   // the user's file — never rewrite it (header contract)

    // Every key pre-filled with its current default directory, so the user sees the schema AND a real,
    // editable path rather than a blank they have to guess the shape of. `..._directory()` creates the
    // folder on first use (StdFileSystem::ensure_dir), which is fine — those dirs exist the moment the
    // browser opens anyway. The "_README" is not part of the schema (load ignores unknown keys); it is
    // there for the human who opens the file.
    json j;
    j["_README"] =
        "PocketTracker default browse folders (debug builds). Set a path below to change where a LOAD "
        "browse for that category STARTS; delete a line to use the built-in default. A path that does "
        "not exist is ignored. This file is yours — the app reads it at startup and never rewrites it.";
    j["folders"] = {
        {"samples",     fs.samples_directory()},
        {"soundfonts",  fs.soundfonts_directory()},
        {"instruments", fs.instruments_directory()},
        {"projects",    fs.projects_directory()},
        {"themes",      fs.themes_directory()},
    };
    return fs.write_file(path, j.dump(2) + "\n");
}

}  // namespace pt::ui
