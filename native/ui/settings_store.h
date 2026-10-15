#pragma once

// settings.json. Missing file, key or parse: the default stays.
// ⚠️ Every row a platform has must be persisted here, or it resets every launch — only a save → load
// round trip catches the omission.

#include "ui/filesystem.h"
#include "ui/modules/settings_editor.h"
#include "ui/theme.h"

namespace pt::ui {

// `theme` gets its full palette (an older file's theme name still loads by name).
// ⚠️ False both with no file and with one that will not parse — the next save then discards it. A
// caller that cares asks the filesystem itself (`app.cpp` does).
bool load_settings(FileSystem& fs, SettingsValues& values, Theme& theme);

bool save_settings(FileSystem& fs, const SettingsValues& values, const Theme& theme);

enum class SettingsWrite { UNCHANGED, SAVED, FAILED };

// Write only if the bytes would differ — the shell's call on exit, instead of a dirty flag (the theme
// editor writes `theme` directly, and a flag lost its palette).
SettingsWrite save_settings_if_changed(FileSystem& fs, const SettingsValues& values, const Theme& theme);

}  // namespace pt::ui
