#pragma once

// ─── The PROJECT screen's actions, where they meet the FILESYSTEM ────────────────────────────────
//
// SAVE, EXPORT (mix and stems), and the song TEMPLATE. Split out of the dispatcher because all three
// need two things at once — the host (to serialize, to render) and a FileSystem (to name a file, to
// make a folder) — and because ptdispatch can then drive them over a real temp directory and check
// what actually landed on disk.
//
// ⚠️ WHY THE STEMS POLICY IS SPLIT IN TWO. `songcore::stems_plan` decides WHICH stems a project has
// (pure, no filesystem — songcore has to keep compiling for the Android NDK, where "where do files
// live" is scoped storage and Kotlin's answer). This file decides WHERE they go. Kotlin's
// RenderController holds both halves; it is zone C and dies with the Kotlin sequencer, so the two
// copies are temporary — and the C++ one is the measured one (ptdispatch renders a real stems set
// and counts the files).

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

/**
 * `<Renders>/<name>_0001.wav`, counting up until the name is free.
 *
 * ⚠️ An EMPTY project name yields `_0001.wav`, and that is Kotlin's (`generateFilename` sanitizes and
 * takes 32, but unlike the stems path it has no `ifEmpty { "project" }` fallback). Ported as-is: the
 * filename is still valid and still unique, and a divergence here would be a divergence in what a
 * user's Renders folder looks like on the two platforms.
 */
std::string unique_render_path(FileSystem& fs, const std::string& dir, const std::string& safeName);

/** PROJECT → SAVE. Writes `<Projects>/<name>.ptp` and marks the document clean. */
ActionResult save_project(songcore::SongcoreHost& host, FileSystem& fs, AppState& s);

/**
 * PROJECT → EXPORT → MIX. One WAV of the whole song, with the master bus, into Renders/.
 *
 * SYNCHRONOUS, and the caller must have silenced the audio device first — see the shell. `progress`
 * is called from inside the render; the shell repaints the frame from it, which is what a
 * single-threaded app does instead of a coroutine.
 */
ActionResult render_mix(songcore::SongcoreHost& host, FileSystem& fs, AppState& s,
                        const std::function<void(float)>& progress);

/**
 * PROJECT → EXPORT → STEMS. One WAV per active track, plus the reverb and delay returns, into
 * `Renders/<name>/`. Stems bypass the master bus by design.
 */
ActionResult render_stems(songcore::SongcoreHost& host, FileSystem& fs, AppState& s,
                          const std::function<void(float)>& progress);

// ─── SONG selection → RESAMPLE ─────────────────────────────────────────────────────────────────
//
// The twin of Kotlin's RenderController.renderSelectionToWav + generateResampledBaseName +
// InstrumentController.createResampledInstrument. A SONG selection is rendered — the chosen row range,
// only the chosen tracks, WITH the master bus (like the mix, not like a stem) — to a WAV in Resampled/,
// and that WAV is loaded into the first free instrument slot as a clean SAMPLER. Zone C on Android; the
// C++ copy is the measured one (ptdispatch renders a real selection and checks what lands).

/**
 * The suggested base name the RESAMPLE keyboard opens on — `Resample_NNNN`, the first free in
 * Resampled/ (no directory, no extension). Kotlin's `generateResampledBaseName`.
 */
std::string resample_base_name(FileSystem& fs);

/**
 * Render rows [startRow,endRow] of `trackFilter` (0-indexed track ids) to a WAV in Resampled/, WITH the
 * master bus, and hand the path back in `outPath`. SYNCHRONOUS — the caller silences the audio device
 * first, exactly as render_mix does. `customBaseName` empty ⇒ auto `Resample_NNNN` (de-duplicated); a
 * non-empty name is sanitized and used verbatim, OVERWRITING — Kotlin's `generateResampledFilename`
 * only de-duplicates the auto path. On any failure returns {false,"RESAMPLE FAILED"} and leaves
 * `outPath` untouched. Mirrors renderSelectionToWav: prepare → schedule(range,filter) → render →
 * finish, with finish ALWAYS run.
 */
ActionResult render_resample(songcore::SongcoreHost& host, FileSystem& fs,
                             int startRow, int endRow, const std::set<int>& trackFilter,
                             const std::string& customBaseName, std::string& outPath,
                             const std::function<void(float)>& progress);

/**
 * A resampled WAV → the first genuinely FREE instrument slot, as a clean SAMPLER rooted at C-4. Returns
 * the slot id, or -1 if there is no free slot or the load failed. `instrument_is_free`, NOT
 * `sampleFilePath == null`: a configured SoundFont slot also has a null sample path, and claiming one
 * would leave a broken SOUNDFONT instrument with a sample behind it. Kotlin's createResampledInstrument.
 */
int create_resampled_instrument(songcore::SongcoreHost& host, const std::string& wavPath);

/** SETTINGS → TEMPLATE → SAVE. The current project becomes what the app boots into. */
ActionResult save_template(songcore::SongcoreHost& host, FileSystem& fs);

/** SETTINGS → TEMPLATE → CLEAR. Deleting a template that is not there still succeeds — Kotlin's. */
ActionResult clear_template(FileSystem& fs);

}  // namespace pt::ui
