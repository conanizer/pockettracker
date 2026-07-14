#pragma once

// ─── THE CRASH-RECOVERY AUTOSAVE — the file verbs (Phase 3 S10) ───────────────────────────────────
//
// The C++ twin of `FileController`'s autosave block (`hasAutosave` / `writeAutosave` / `loadAutosave`
// / `clearAutosave`) — four functions over a path that nothing else in the app may look at.
//
// ── WHAT THE FILE MEANS ──────────────────────────────────────────────────────────────────────────
//
// ⚠️ **Its PRESENCE is the whole signal.** It is written while there is unsaved work and DELETED on
// every clean transition — save, load, new, and a confirmed EXIT — so an autosave that survives to
// the next launch means the last session did not end cleanly. That is what the RECOVER WORK? prompt
// keys on, and it is why nothing here needs a timestamp, a flag or a header: the file either is there
// or it is not, and each answer means exactly one thing. Kotlin puts it in one line (AutosaveManager):
// "its presence at next launch signals an unclean exit".
//
// Which makes the DELETIONS as load-bearing as the writes, and easier to forget. A clean save that
// leaves the file behind asks the user to recover work they have already safely stored — and trains
// them to dismiss the one prompt in the app that exists to save their song.
//
// ── ⚠️ IT IS WRITTEN THROUGH `FileSystem::write_file`, NOT `SongcoreHost::save_project_file` ──────
//
// And that is not a stylistic preference — it is the reason the temp-file dance exists at all.
// `write_file` writes `<path>.tmp` and renames it over the target (StdFileSystem::write_bytes,
// AndroidFileSystem.writeFile); `save_project_file` is a plain `ofstream` with `trunc`.
//
// **The autosave is the file where that difference bites hardest, because it is written precisely
// when the machine is about to die.** The SIGTERM flush runs with a launcher's SIGKILL possibly
// seconds behind it; a hardware power slider does not negotiate. A non-atomic write interrupted
// halfway does not merely fail to save the new work — it has already TRUNCATED the previous, good
// autosave in order to fail. That is strictly worse than never having written at all, and it turns
// the one file whose job is to survive a bad death into the one file guaranteed not to.
//
// (⚠️ S10 found the same hole in PROJECT → SAVE and in the TEMPLATE, which had been going through
// `save_project_file` since S7 — so the interface promised atomicity, Android delivered it, and the
// port's own two save paths quietly opted out. Fixed with this; see ui/project_actions.cpp.)
//
// ── WHAT HAS NO TWIN ─────────────────────────────────────────────────────────────────────────────
//
// `AutosaveManager` itself. Its entire content is a THREADING split — serialize on the main thread
// (the project's sole mutator, so the walk cannot observe a half-applied edit), write the bytes on
// `Dispatchers.IO`. There is one thread here and it is the one holding the document, so the split has
// nothing to split: the serialize and the write are the same call, on the same thread, and the
// tear-freedom Kotlin has to arrange is a property of the program rather than a discipline.
//
// The DEBOUNCE has no twin here either, for the opposite reason — it is not missing, it MOVED. Kotlin
// debounces in a `LaunchedEffect(projectVersion)` that a new edit re-keys and therefore cancels; the
// shell has no coroutines, so it is a deadline in `InputDispatcher::set_now()`, beside the sample
// editor's preview restore. Same behaviour, said out loud instead of arranged by a framework.

#include <string>

#include "songcore/host.h"
#include "ui/filesystem.h"

namespace pt::ui {

/** True if an autosave survived to this launch — i.e. the last session did not end cleanly. */
bool autosave_exists(FileSystem& fs);

/**
 * Serialize the LIVE project into the autosave file. Atomic (see the note above).
 *
 * Never a reason to disturb the app: a failure returns false and says nothing. Kotlin is explicit
 * about that too ("an autosave failure must not disturb the app") — the user is mid-edit, they did
 * not ask for this write, and a modal about a full SD card is worse than the missed backup.
 */
bool autosave_write(const songcore::SongcoreHost& host, FileSystem& fs);

/**
 * Delete it. **Deleting one that is not there SUCCEEDS** — Kotlin's `clearAutosave` returns true for
 * an absent file, and every caller of this is a CLEAN transition asserting "there is nothing to
 * recover". That post-condition already held; failing it would be a lie about a job already done.
 */
bool autosave_clear(FileSystem& fs);

/**
 * Read the autosave back into the live document: parse → push → load its media → push its params.
 *
 * ⚠️ **`mediaBaseDir` is the SESSION's, never the autosave's own directory.** A `.ptp` stores sample
 * PATHS, not PCM, and the autosave lives in the app ROOT while the samples it names do not. Resolve
 * its relative paths against the folder it happens to sit in and every instrument comes back silent —
 * with the project itself looking perfectly correct on screen, which is the worst way for it to fail.
 * The shell hands in what it hands `load_media` at boot; ptdispatch hands in its temp Samples folder.
 *
 * The DIRTY flag is NOT this function's business — it is the caller's, and it is the one thing about
 * recovery that is easy to get backwards. See `InputDispatcher::recover_from_autosave`.
 */
bool autosave_load(songcore::SongcoreHost& host, FileSystem& fs, const std::string& mediaBaseDir);

}  // namespace pt::ui
