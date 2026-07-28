#pragma once

// ─── config.json — user-editable default browse folders (v0.9.4 D2b) ─────────────────────────────
//
// ⚠️ DEBUG BUILDS ONLY for 0.9.4. Gated on `PlatformCaps::debug`, so the read at boot and every use in
// the dispatcher is inert on a release build. It ships behind the cap exactly like the OVERLAY and
// TRACE rows, so the shape can be tested before it goes to every platform's release (plan D2b).
//
// A sibling of settings.json in the PocketTracker root, but the opposite kind of file: settings.json is
// WRITTEN by the app and never meant to be hand-edited; config.json is WRITTEN BY THE USER and never
// rewritten by the app. It maps a load category to the directory the file browser should START in when
// you load that kind of thing — so a user whose samples live outside PocketTracker/Samples does not
// climb out of it every time.
//
// SCOPE (0.9.4): the LOAD-BROWSE start directory for five categories. It does NOT redirect SAVES — the
// sample-editor save, preset save and export destinations keep their built-in folders. That split is
// deliberate: "samples" here means "where a sample LOAD starts browsing", one clear meaning, rather than
// silently also moving where your edits are written. Save-destination overrides (the plan's "renders"
// and "sample-editor saves") are a later increment; they are NOT keys here, so an unknown key is simply
// ignored rather than advertised-and-inert.
//
// SCHEMA:
//   { "folders": { "samples": "...", "soundfonts": "...", "instruments": "...",
//                  "projects": "...", "themes": "..." } }
// Every key is OPTIONAL. An absent key → that category keeps its default. An empty string, a
// non-string value, a missing "folders" object, or an unparseable file → the same: defaults stand. A
// path that does not exist on disk is ignored AT USE (the dispatcher checks is_directory), so a typo or
// a folder that has since been deleted costs one category's convenience, never a broken browser.

#include "ui/filesystem.h"

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
 * Read config.json into `out`. Returns false when there is no file (the common case, and NOT an error)
 * or when the file is present but unparseable — in both cases `out` is left untouched, so the caller's
 * defaults stand. A present, valid file fills only the keys it carries.
 */
bool load_folder_config(FileSystem& fs, FolderConfig& out);

/**
 * Write a STARTER config.json when none exists, so the feature is DISCOVERABLE — until now the app never
 * created the file, so a user who wanted to redirect a browse folder had nothing to find or edit (the
 * reported "can't see the folders settings"). The template lists all five categories, each pre-filled
 * with that category's current default directory, plus a one-line note explaining how to change one.
 *
 * ⚠️ DEBUG BUILDS ONLY — the caller gates this on `PlatformCaps::debug`, exactly like the read, so a
 * release build neither reads nor seeds the file.
 *
 * ⚠️ NEVER CLOBBERS AN EXISTING FILE. config.json is the user's, written by them and never rewritten by
 * the app (the header's contract); this seeds a blank canvas ONCE and then keeps its hands off. Returns
 * true iff it actually wrote a new file (absent before) — false if one was already there or the write
 * failed. Pre-filling with the default dirs also creates those dirs via the FileSystem accessors, which
 * is harmless (they are created on first use anyway).
 */
bool seed_folder_config_template(FileSystem& fs);

}  // namespace pt::ui
