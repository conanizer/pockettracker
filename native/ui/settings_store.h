#pragma once

// ─── settings.json ───────────────────────────────────────────────────────────────────────────────
//
// The app's preferences as a file. In pt-ui (nlohmann + a FileSystem, no POSIX) so the tests can
// round-trip it in a temp directory.
// ⚠️ Every row a platform HAS must be persisted, or it silently resets every launch — and only a save →
// load round trip can catch the omission (no tool quits and relaunches the app).
// Missing file, key or parse: the DEFAULT stays. Losing a settings file is worth the factory settings
// and a working app, never a dialog.

#include "ui/filesystem.h"
#include "ui/modules/settings_editor.h"
#include "ui/theme.h"

namespace pt::ui {

/**
 * Read `settings.json` into `values` and `theme`.
 * ⚠️ `theme` carries its FULL PALETTE (an invented palette exists only here); an older file with only a
 * `theme` name still loads by name. The visualizer is the theme's field but the user's choice, so it
 * rides across a theme load.
 * Returns false when there was no file (the first launch) — ⚠️ AND when the file is there but will not
 * read or parse, which discards the user's settings at the next quit-time save. A caller that cares
 * asks the filesystem itself (`app.cpp` does); this signature cannot tell them apart.
 */
bool load_settings(FileSystem& fs, SettingsValues& values, Theme& theme);

/** Write `settings.json` unconditionally. */
bool save_settings(FileSystem& fs, const SettingsValues& values, const Theme& theme);

/** What `save_settings_if_changed` did. An enum: "nothing needed writing" and "the write FAILED" must
 *  not look alike, or a full SD card reports nothing. */
enum class SettingsWrite { UNCHANGED, SAVED, FAILED };

/**
 * Write `settings.json` only if its bytes would differ — the verb the shell calls on exit.
 * ⚠️⚠️ This exists INSTEAD OF A DIRTY FLAG. Not every change goes through one path (the THEME EDITOR and
 * LOAD THEME mutate `theme` directly), and a flag lost a session's palette. Comparing the bytes on disk
 * with memory covers every current and future field with nothing to remember, and still writes only
 * on EXIT — never per key-repeat.
 */
SettingsWrite save_settings_if_changed(FileSystem& fs, const SettingsValues& values, const Theme& theme);

}  // namespace pt::ui
