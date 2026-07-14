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
// BTN VIBRO) are Android's, they are caps-gated off here, and writing zeroes for them would be
// inventing a value for a question this platform never asked. If Android ever converges onto this UI it
// brings its own answers with it — and its own keys.
//
// ⚠️ **RESUME MOVED OUT OF THAT LIST IN S10, AND THAT IS THE ONE LINE OF THIS FILE WORTH RE-READING.**
// The rule is "persist the rows this platform HAS", and S10 gave the shell a row it did not have — so
// the very same session that flips `PlatformCaps::sdl().autosave` on has to add the key here, or the
// setting silently resets to ASK on every single launch.
//
// That is not a hypothetical coupling: it is EXACTLY the bug S9 shipped and then found, one row later.
// S9's `theme` was stored by NAME, which was lossless until the layer above it (the theme editor) made
// a palette something you could invent — and **no tool in the ladder quits and relaunches the app**, so
// the only thing that can ever catch this class is a save → load round trip. `ptdispatch` §28 carries
// one for RESUME for precisely that reason, and its control fires.
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
 * ⚠️ `theme` carries its FULL PALETTE, not just its name — see the long note in the .cpp. Until S9 this
 * stored `theme.name` and rebuilt the palette from the four built-ins, which was lossless right up until
 * the THEME EDITOR made a palette something you can invent. A pre-S9 file (a `theme` string and no
 * `appTheme` object) still loads: the reader falls back to the by-name path.
 *
 * The visualizer is the theme's FIELD but the user's CHOICE, so it rides across a theme load rather than
 * coming from it — exactly as Android carries it across a theme change.
 *
 * Returns false when there was no file to read. That is not an error: it is the first launch.
 */
bool load_settings(FileSystem& fs, SettingsValues& values, Theme& theme);

/** Write `settings.json`. Called on exit, and after every SETTINGS edit. */
bool save_settings(FileSystem& fs, const SettingsValues& values, const Theme& theme);

}  // namespace pt::ui
