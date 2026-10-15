#pragma once

// The crash-recovery autosave. Its presence is the signal: written while there is unsaved work, deleted
// on every clean save, load, new and exit — so a file at launch means an unclean exit.
// ⚠️ The deletions matter as much as the writes: a stale file asks to recover work already saved.
// Written only through `FileSystem::write_file`, so a write cut short keeps the last good one.

#include <string>

#include "songcore/host.h"
#include "ui/filesystem.h"

namespace pt::ui {

bool autosave_exists(FileSystem& fs);

// A failure returns false and says nothing: the user did not ask for this write.
bool autosave_write(const songcore::SongcoreHost& host, FileSystem& fs);

// Deleting one that is not there succeeds.
bool autosave_clear(FileSystem& fs);

// Into the live document, media and all. ⚠️ `mediaBaseDir` is the session's, never the autosave's own
// folder: relative sample paths resolved there come back silent.
bool autosave_load(songcore::SongcoreHost& host, FileSystem& fs, const std::string& mediaBaseDir);

}  // namespace pt::ui
