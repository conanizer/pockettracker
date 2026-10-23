#pragma once

// SAVE, EXPORT, RESAMPLE and the song template: where the host meets the FileSystem.

#include <functional>
#include <set>
#include <string>

#include "songcore/host.h"
#include "ui/app_state.h"
#include "ui/filesystem.h"

namespace pt::ui {

// The status line's text, and whether it is green or red.
struct ActionResult {
    bool        ok = false;
    std::string message;
};

// `<dir>/<name>_0001.wav`, counting up until free.
std::string unique_render_path(FileSystem& fs, const std::string& dir, const std::string& safeName);

// Writes `<Projects>/<name>.ptp` and marks the document clean.
ActionResult save_project(songcore::SongcoreHost& host, FileSystem& fs, AppState& s);

// ─── COLLECT: the song and every file it plays, copied into one folder under Songs/ ──────────────

// The folder COLLECT writes: the project's file name.
std::string collect_folder_name(const songcore::SongcoreHost& host);

// Is there already a Songs/<folder>?
bool collect_folder_exists(FileSystem& fs, const std::string& folder);

// The first free `<folder>_2`, `<folder>_3`… under Songs/.
std::string collect_free_folder_name(FileSystem& fs, const std::string& folder);

// Songs/<folder>/<folder>.ptp plus a copy of every sample and SoundFont the song plays, the .ptp
// naming the copies relative to itself. An existing folder is written into, its files replaced.
// The open document and its file are untouched.
ActionResult collect_project(const songcore::SongcoreHost& host, FileSystem& fs,
                             const std::string& folder, const std::function<void(float)>& progress);

// The rows a render covers and how many times. ⚠️ `startRow < 0` is the whole song.
struct RenderRange {
    int startRow = -1;
    int endRow   = -1;
    int repeat   = 1;
};

// One WAV of `range` with the master bus, into Renders/. Synchronous: the caller pauses the audio first.
ActionResult render_mix(songcore::SongcoreHost& host, FileSystem& fs, AppState& s,
                        const RenderRange& range, const std::function<void(float)>& progress);

// One WAV per track active in the range, plus the two returns, into `Renders/<name>/`; no master bus.
ActionResult render_stems(songcore::SongcoreHost& host, FileSystem& fs, AppState& s,
                          const RenderRange& range, const std::function<void(float)>& progress);

// ─── RESAMPLE: a SONG selection mixed to a WAV and loaded into a free instrument slot ────────────

// The name the keyboard opens on: the first free `Resample_NNNN`.
std::string resample_base_name(FileSystem& fs);

// Rows [startRow,endRow] of `trackFilter` to Resampled/, with the master bus; synchronous. An empty
// `customBaseName` is numbered, a typed one overwrites. `outPath` is set only on success.
ActionResult render_resample(songcore::SongcoreHost& host, FileSystem& fs,
                             int startRow, int endRow, const std::set<int>& trackFilter,
                             const std::string& customBaseName, std::string& outPath,
                             const std::function<void(float)>& progress);

// The WAV into the first free slot as a clean SAMPLER at C-4; the slot, or -1.
int create_resampled_instrument(songcore::SongcoreHost& host, const std::string& wavPath);

// Instrument `id`, and its table if it has content, as a `.pti`.
bool save_instrument_preset(const songcore::SongcoreHost& host, FileSystem& fs, int id,
                            const std::string& path);

// The current project becomes what the app boots into.
ActionResult save_template(songcore::SongcoreHost& host, FileSystem& fs);

// Clearing a template that is not there succeeds.
ActionResult clear_template(FileSystem& fs);

}  // namespace pt::ui
