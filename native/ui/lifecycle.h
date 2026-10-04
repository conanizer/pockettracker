#pragma once

// ─── THE CRASH-RECOVERY AUTOSAVE — the file verbs ────────────────────────────────────────────────
//
// ⚠️ Its PRESENCE is the whole signal: written while there is unsaved work, DELETED on every clean
// transition (save, load, new, confirmed EXIT) — so a file surviving to the next launch means an
// unclean exit, and RECOVER WORK? keys on it. No timestamp, flag or header needed.
// The DELETIONS matter as much as the writes: a file left after a clean save asks the user to recover
// work already stored, and trains them to dismiss the prompt.
//
// ⚠️ Written ONLY through `FileSystem::write_file` (temp, checked close, rename). A truncating write
// interrupted halfway — and this file is written exactly when the machine is dying — destroys the
// previous good autosave in order to fail.
//
// One thread holds the document, so serialize and write are one call with no tearing possible. The
// debounce is a deadline in `InputDispatcher::set_now()`.

#include <string>

#include "songcore/host.h"
#include "ui/filesystem.h"

namespace pt::ui {

/** True if an autosave survived to this launch — i.e. the last session did not end cleanly. */
bool autosave_exists(FileSystem& fs);

/**
 * Serialize the LIVE project into the autosave file (atomic). A failure returns false and says
 * nothing: the user did not ask for this write, and a modal about a full card is worse than the miss.
 */
bool autosave_write(const songcore::SongcoreHost& host, FileSystem& fs);

/** Delete it. Deleting one that is not there SUCCEEDS — every caller is a clean transition asserting
 *  "nothing to recover", which already holds. */
bool autosave_clear(FileSystem& fs);

/**
 * Read the autosave into the live document: parse → push → load its media → push its params.
 * ⚠️ `mediaBaseDir` is the SESSION's, never the autosave's own folder (the app root): relative sample
 * paths resolved there come back silent while the project looks correct. The shell passes what it
 * gave `load_media`; a test its temp Samples folder.
 * The DIRTY flag is the caller's business (`InputDispatcher::recover_from_autosave`).
 */
bool autosave_load(songcore::SongcoreHost& host, FileSystem& fs, const std::string& mediaBaseDir);

}  // namespace pt::ui
