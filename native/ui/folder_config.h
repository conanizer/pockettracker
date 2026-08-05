#pragma once

// ─── config.json — the user-editable configuration file ──────────────────────────────────────────
//
// This header owns the FILE — its schema, and the starter template every platform seeds — plus the
// `folders` section. The other two sections, `controller` and `keyboard`, are `ui/input_config.h`:
// they are read by the shell rather than by pt-ui, because only the shell may know what an
// `SDL_Keycode` is.
//
// SHIPS ON EVERY PLATFORM'S RELEASE as of v0.9.4. It was debug-gated when the shape was still being
// tested; the gate is gone from the read, the seed and the dispatcher alike, so what a user edits on
// Windows behaves identically on Android and on a handheld. Nothing about it is conditional any more —
// if you find yourself adding a `caps.debug` back, you are re-opening a decision, not fixing a bug.
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
// SCHEMA (this section only — see ui/input_config.h for `controller` and `keyboard`):
//   { "folders": { "samples": "...", "soundfonts": "...", "instruments": "...",
//                  "projects": "...", "themes": "..." } }
// Every key is OPTIONAL. An absent key → that category keeps its default. An empty string, a
// non-string value, a missing "folders" object, or an unparseable file → the same: defaults stand. A
// path that does not exist on disk is ignored AT USE (the dispatcher checks is_directory), so a typo or
// a folder that has since been deleted costs one category's convenience, never a broken browser.

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
 * Read config.json into `out`. Returns false when there is no file (the common case, and NOT an error)
 * or when the file is present but unparseable — in both cases `out` is left untouched, so the caller's
 * defaults stand. A present, valid file fills only the keys it carries.
 */
bool load_folder_config(FileSystem& fs, FolderConfig& out);

/**
 * Write a STARTER config.json when none exists, so the feature is DISCOVERABLE — the app used not to
 * create the file at all, so a user who wanted to redirect a browse folder had nothing to find or edit
 * (the reported "can't see the folders settings"). The template carries ALL THREE sections, each
 * pre-filled with what the app is doing right now, plus a note per section explaining how to change it.
 * Read as a document, it is the schema; read as a config, it is a no-op.
 *
 * `keyboardDefaults` is the shell's LIVE default key map, passed in rather than restated here — pt-ui
 * cannot name an SDL key, and a second hand-written copy of that table is a copy that drifts. What the
 * template says the keys are is therefore what they actually are, derived from the same table the app
 * dispatches through, in the spelling `SDL_GetKeyName` produces (so it round-trips through
 * `SDL_GetKeyFromName` when the user edits a line rather than replacing it).
 *
 * ⚠️ NEVER CLOBBERS AN EXISTING FILE. config.json is the user's, written by them and never rewritten by
 * the app (the header's contract); this seeds a blank canvas ONCE and then keeps its hands off. Returns
 * true iff it actually wrote a new file (absent before) — false if one was already there or the write
 * failed. Pre-filling with the default dirs also creates those dirs via the FileSystem accessors, which
 * is harmless (they are created on first use anyway).
 */
bool seed_config_template(FileSystem& fs, const KeyboardBindings& keyboardDefaults);

}  // namespace pt::ui
