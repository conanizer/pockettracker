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

// When it is written: 3 s after the last edit, and as the app leaves.
class Autosave {
  public:
    Autosave(songcore::SongcoreHost& host, FileSystem& fs) : host_(host), fs_(fs) {}

    // An edit happened. ⚠️ Re-armed on every edit, so it writes 3 s after the last change; a held
    // A+UP edits every 100 ms.
    void arm(long long now_ms) {
        pending_ = true;
        dueAtMs_ = now_ms + DEBOUNCE_MS;
    }

    bool pending() const { return pending_; }

    // The deadline passed: write, if the work is still unsaved.
    // ⚠️ A save inside the window does not cancel the deadline; writing anyway would bring the file
    // back and a false RECOVER WORK? with it.
    void run_due(long long now_ms, bool dirty) {
        if (!pending_ || now_ms < dueAtMs_) return;
        flush(dirty);
    }

    // Write now if there is unsaved work.
    void flush(bool dirty) {
        pending_ = false;
        if (!dirty) return;
        host_.keep_walking();   // only the normal horizon: a deep fill would make the next edit late
        autosave_write(host_, fs_);
    }

    // The document is clean and new (NEW, LOAD): the file and the deadline go, or the deadline would
    // write the file back 3 s later.
    void discard() {
        pending_ = false;
        autosave_clear(fs_);
    }

  private:
    static constexpr long long DEBOUNCE_MS = 3000;

    songcore::SongcoreHost& host_;
    FileSystem&             fs_;
    bool                    pending_ = false;
    long long               dueAtMs_ = 0;
};

}  // namespace pt::ui
