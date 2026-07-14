#pragma once

// ─── settings.json ───────────────────────────────────────────────────────────────────────────────
//
// SharedPreferences, as a file. The port plan's §4.5 line — "SharedPreferences → settings.json in
// CONFDIR (tiny key-value store)" — and the thing that makes the SETTINGS screen worth having: a
// setting that resets on every launch is a setting nobody will touch twice.
//
// It lives in pt-ui rather than in the shell for the same reason `ui/filesystem.h` does: it is
// portable (nlohmann + a FileSystem, no POSIX), so `ptdispatch` can round-trip it in a temp directory
// and prove that what was written is what comes back.
//
// ⚠️ ONLY THE ROWS THE SHELL ACTUALLY HAS ARE PERSISTED. The device rows (LAYOUT, OVERLAY, BTN SOUND,
// BTN VIBRO, RESUME) are Android's, they are caps-gated off here, and writing zeroes for them would
// be inventing a value for a question this platform never asked. If Android ever converges onto this
// UI it brings its own answers with it — and its own keys.
//
// Missing file, missing key, unparseable value: the DEFAULT stays. A settings file is not a document,
// and losing one is not worth a dialog — it is worth the factory settings and a working app.

#include "ui/filesystem.h"
#include "ui/modules/settings_editor.h"
#include "ui/theme.h"

namespace pt::ui {

/**
 * Read `settings.json` into `values` and `theme`.
 *
 * `theme` is REPLACED (by name, from the built-ins) rather than patched — a theme is a palette, and a
 * half-applied one would be a palette nobody chose. The visualizer rides across the swap, exactly as
 * Android carries it across a theme change.
 *
 * Returns false when there was no file to read. That is not an error: it is the first launch.
 */
bool load_settings(FileSystem& fs, SettingsValues& values, Theme& theme);

/** Write `settings.json`. Called on exit, and after every SETTINGS edit. */
bool save_settings(FileSystem& fs, const SettingsValues& values, const Theme& theme);

}  // namespace pt::ui
