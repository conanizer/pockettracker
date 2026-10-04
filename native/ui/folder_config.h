#pragma once

// ─── config.json — the user-editable configuration file ──────────────────────────────────────────
//
// This header owns the FILE — its schema and the starter template every platform seeds — plus the
// `folders` section. `controller` and `keyboard` are `ui/input_config.h`, read by the shell (only it
// knows an `SDL_Keycode`). Ships and behaves identically on every platform.
//
// The opposite of settings.json: written BY THE USER and never rewritten by the app. `folders` maps a
// load category to where the file browser STARTS for that kind of load. It does NOT redirect saves.
//
//   { "folders": { "samples": "...", "soundfonts": "...", "instruments": "...",
//                  "projects": "...", "themes": "..." } }
//
// Every key is optional; absent, empty, non-string, missing section or unparseable file → defaults. A
// value naming no readable directory is ignored AT USE (`resolve_browse_dir`), costing one category's
// convenience, never a browser that opens on nothing.
// ⚠️ A value is ROOT-RELATIVE unless absolute (`resolve_folder_override`), on every platform:
// "Samples" can be typed and carried between devices; Android's real root (`pt://a1b2c3d4e5f6/…`)
// cannot.

#include "ui/filesystem.h"
#include "ui/input_config.h"

#include <optional>
#include <string>

namespace pt::ui {

/** The five LOAD-browse categories' overrides. `std::nullopt` = "use the built-in default". */
struct FolderConfig {
    std::optional<std::string> samples;
    std::optional<std::string> soundfonts;
    std::optional<std::string> instruments;
    std::optional<std::string> projects;
    std::optional<std::string> themes;
};

/**
 * Read config.json into `out`. False when there is no file (the common case) or it does not parse; `out`
 * is then untouched. A valid file fills only the keys it carries.
 */
bool load_folder_config(FileSystem& fs, FolderConfig& out);

/**
 * A `folders` value → the path it names. Absolute (`/mnt/sd/Packs`, `C:\Music`, `\\server\x`) or a URI
 * (`pt://<id>/Samples`) is taken verbatim; anything else joins onto `media_root` (empty ⇒ unchanged).
 * ⚠️ The STRING half only — `resolve_browse_dir` is the whole rule, and the browser must call that.
 */
std::string resolve_folder_override(const std::string& value, const std::string& media_root);

/**
 * The directory a LOAD browse STARTS in for one category: the override if this filesystem can reach
 * it, the same value RE-ROOTED if authored under another install, else `def` (created on first use).
 * ⚠️⚠️ `is_directory` alone is not the test: on Android a plain path can be stat()ed but not listed. The
 * path's kind (URI vs plain) must match the root's first.
 * The re-rooting is `resolve_media_path`'s (`app_root_relative_tail`), so a project's samples and a
 * config's folders cannot disagree.
 */
std::string resolve_browse_dir(FileSystem& fs, const std::optional<std::string>& value,
                               const std::string& def);

/**
 * Write a STARTER config.json when none exists, so the feature can be found: all three sections, each
 * pre-filled with what the app does now plus a note on changing it — the schema as a no-op config.
 * `keyboardDefaults` is the shell's live key map, spelled as `SDL_GetKeyName` does so an edited line
 * round-trips; never a second copy.
 * ⚠️ NEVER CLOBBERS an existing file. True iff a new file was written.
 */
bool seed_config_template(FileSystem& fs, const KeyboardBindings& keyboardDefaults);

}  // namespace pt::ui
