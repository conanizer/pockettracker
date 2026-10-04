#pragma once

// ─── The PROJECT screen's actions, where they meet the FILESYSTEM ────────────────────────────────
//
// SAVE, EXPORT (mix and stems), RESAMPLE and the song TEMPLATE — each needs the host (to serialize,
// render) and a FileSystem (to name a file, make a folder), so the tests drive them over a temp
// directory and checks what lands.
// The stems POLICY is split: `songcore::stems_plan` decides WHICH stems (pure, no filesystem); this
// file decides WHERE they go.

#include <functional>
#include <set>
#include <string>

#include "songcore/host.h"
#include "ui/app_state.h"
#include "ui/filesystem.h"

namespace pt::ui {

/** What an action reports back — the status line's text, and whether it is green or red. */
struct ActionResult {
    bool        ok = false;
    std::string message;
};

/** `<Renders>/<name>_0001.wav`, counting up until free. An EMPTY name yields `_0001.wav` — still valid
 *  and unique (unlike stems, there is no "project" fallback). */
std::string unique_render_path(FileSystem& fs, const std::string& dir, const std::string& safeName);

/** PROJECT → SAVE. Writes `<Projects>/<name>.ptp` and marks the document clean. */
ActionResult save_project(songcore::SongcoreHost& host, FileSystem& fs, AppState& s);

/**
 * WHICH rows a render covers and how many times — the RENDER dialog's rows, resolved (no AUTO, no OFF).
 * ⚠️ `startRow < 0` means THE WHOLE SONG.
 */
struct RenderRange {
    int startRow = -1;
    int endRow   = -1;
    int repeat   = 1;
};

/**
 * PROJECT → EXPORT → MIX: one WAV of `range`, with the master bus, into Renders/. SYNCHRONOUS — the
 * caller silences the audio device first; `progress` is called from inside so the shell can repaint.
 */
ActionResult render_mix(songcore::SongcoreHost& host, FileSystem& fs, AppState& s,
                        const RenderRange& range, const std::function<void(float)>& progress);

/**
 * PROJECT → EXPORT → STEMS. One WAV per track ACTIVE IN THE RANGE, plus the reverb and delay returns,
 * into `Renders/<name>/`. Stems bypass the master bus by design.
 */
ActionResult render_stems(songcore::SongcoreHost& host, FileSystem& fs, AppState& s,
                          const RenderRange& range, const std::function<void(float)>& progress);

// ─── SONG selection → RESAMPLE ─────────────────────────────────────────────────────────────────
//
// A SONG selection — the chosen rows and tracks, WITH the master bus (a mix, not a stem) — is rendered
// to a WAV in Resampled/ and loaded into the first free instrument slot as a clean SAMPLER.

/** The name the RESAMPLE keyboard opens on: `Resample_NNNN`, the first free in Resampled/ (no
 *  directory, no extension). */
std::string resample_base_name(FileSystem& fs);

/**
 * Render rows [startRow,endRow] of `trackFilter` (0-indexed) to a WAV in Resampled/, WITH the master
 * bus; the path comes back in `outPath`. SYNCHRONOUS (silence the audio device first).
 * `customBaseName` empty ⇒ auto `Resample_NNNN` (de-duplicated); otherwise sanitized and used
 * verbatim, OVERWRITING. On failure {false, "RESAMPLE FAILED"} and `outPath` untouched. finish always
 * runs.
 */
ActionResult render_resample(songcore::SongcoreHost& host, FileSystem& fs,
                             int startRow, int endRow, const std::set<int>& trackFilter,
                             const std::string& customBaseName, std::string& outPath,
                             const std::function<void(float)>& progress);

/**
 * A resampled WAV → the first FREE slot as a clean SAMPLER rooted at C-4; the slot id, or -1.
 * `instrument_is_free`, NOT `sampleFilePath == null` — a SoundFont slot has a null sample path too.
 */
int create_resampled_instrument(songcore::SongcoreHost& host, const std::string& wavPath);

/** INSTRUMENT → SAVE PRESET: instrument `id`, and its table if it has content, as a `.pti` — through
 *  `FileSystem::write_file`, like a `.ptp`. */
bool save_instrument_preset(const songcore::SongcoreHost& host, FileSystem& fs, int id,
                            const std::string& path);

/** SETTINGS → TEMPLATE → SAVE. The current project becomes what the app boots into. */
ActionResult save_template(songcore::SongcoreHost& host, FileSystem& fs);

/** SETTINGS → TEMPLATE → CLEAR. Clearing a template that is not there succeeds. */
ActionResult clear_template(FileSystem& fs);

}  // namespace pt::ui
