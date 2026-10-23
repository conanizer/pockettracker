#pragma once

// config.json: the file, its starter template, and its `folders` section (the input sections are
// ui/input_config.h). Written by the user, never rewritten by the app.
//   { "folders": { "samples": "...", "soundfonts": "...", "instruments": "...",
//                  "projects": "...", "themes": "..." } }
// Where a load browser starts per category; saves are not redirected. Everything optional, and a
// folder that cannot be read is ignored at use. A value is root-relative unless absolute.

#include "ui/filesystem.h"
#include "ui/input_config.h"

#include <optional>
#include <string>

namespace pt::ui {

// nullopt: the built-in default.
struct FolderConfig {
    std::optional<std::string> samples;
    std::optional<std::string> soundfonts;
    std::optional<std::string> instruments;
    std::optional<std::string> projects;
    std::optional<std::string> themes;
};

// False with no file or one that does not parse (`out` untouched); else fills what it carries.
bool load_folder_config(FileSystem& fs, FolderConfig& out);

// A `folders` value → the path it names: absolute or a URI verbatim, else joined onto `media_root`.
// ⚠️ The string half only; the browser calls `resolve_browse_dir`.
std::string resolve_folder_override(const std::string& value, const std::string& media_root);

// Where a load browser starts: the override if reachable, re-rooted if written under another install,
// else `def`. ⚠️ On Android a plain path can be stat()ed but not listed: the path's kind must match
// the root's first.
std::string resolve_browse_dir(FileSystem& fs, const std::optional<std::string>& value,
                               const std::string& def);

// Instruments, Scales, Grooves, Reverbs and Delays once sat in the root; move what an older install
// left there into Presets/. A name already taken there stays behind, and so does its folder.
// Returns how many entries moved.
int move_old_preset_folders(FileSystem& fs);

// Write a starter config.json when none exists: every section filled with what the app does now, so
// it changes nothing until edited. Never overwrites. True if written.
bool seed_config_template(FileSystem& fs, const ButtonBindings& keyboardDefaults,
                          const ButtonBindings& gamepadDefaults);

// Keep `config.example.json` beside it, the starter text, rewritten when it changes. True if written.
bool write_config_example(FileSystem& fs, const ButtonBindings& keyboardDefaults,
                          const ButtonBindings& gamepadDefaults);
std::string config_example_path(FileSystem& fs);

}  // namespace pt::ui
