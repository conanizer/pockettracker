#ifndef POCKETTRACKER_SONGCORE_RENDER_H
#define POCKETTRACKER_SONGCORE_RENDER_H

// ─── The offline render ──────────────────────────────────────────────────────────────────────────
//
// Ready the engine, render the scheduled span in chunks, let the tail ring out, stream it to a WAV,
// put the engine back. The scheduler is not part of this: prepare → (caller schedules) → render.
//
// A render is a pure function of the project:
//   • The START inherits nothing: prepare_render resets every effect chain (reverb tail, delay buffer,
//     OTT/limiter envelopes, the reverb's LCG) and re-pushes the project (engine_setup.h).
//   • The END keeps its tail: rendering continues until the output decays below −90 dBFS, capped so
//     runaway feedback cannot render forever.
//   • The range running out KILs every track at the last row's end frame, so looping samples and
//     held SoundFont notes release instead of ringing until the cap.
//
// Live playback is unaffected; the only other caller of resetEffectState() is the engine itself on a
// device rate change, which restores the bus settings (AudioEngine::setDeviceSampleRate).

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <functional>
#include <set>
#include <string>
#include <vector>

#include "engine_setup.h"
#include "model.h"
#include "traversal.h"
#include "wav_writer.h"

namespace songcore {

// Frames per renderOffline() call, ~5 s of stereo float ≈ 1.7 MB. Keeps peak memory flat (one whole-song
// call can OOM a 1 GB device) and allows progress reporting; the output is bit-identical either way.
constexpr int RENDER_CHUNK_FRAMES = 220500;

// ── the tail ──
// −90 dBFS: below the noise floor of the 16-bit file, so the cut is inaudible.
constexpr float TAIL_SILENCE_PEAK = 3.1623e-5f;
// ~93 ms at 44.1 kHz: how often decay is checked, and the most silence that can be appended.
constexpr int   TAIL_CHUNK_FRAMES = 4096;
// A delay at maximum feedback never decays; this caps the tail.
constexpr int   TAIL_MAX_SECONDS  = 30;

struct RenderStats {
    bool    ok          = false;
    int64_t songFrames  = 0;   // the scheduler's span — where the last step ends
    int64_t tailFrames  = 0;   // what the decay tail added past it
    int64_t totalFrames = 0;   // what actually landed in the file
};

struct RenderOptions {
    // AudioEngine::setStemsMode — 0 full mix, 1-8 track N, 9 reverb return, 10 delay return.
    int  stemsMode = 0;
    // Stems bypass the master bus (OTT/DUST/master EQ), so they skip it.
    bool applyMasterBus = true;
};

// ─── prepare ─────────────────────────────────────────────────────────────────────────────────────
// ⚠️ Wipe the chains FIRST, re-push the project SECOND: resetEffectState() leaves every module at
// factory defaults, and without the re-push the render uses default reverb and delay.
template <typename Engine>
void prepare_render(Engine& engine, const Project& project, const Routing& routing,
                    int startRow, int endRow) {
    engine.setOfflineRendering(true);   // the live stream goes silent so it cannot eat the note queue
    engine.stopAll();
    engine.clearScheduledNotes();
    engine.resetFrameCounter();         // also re-seeds noteSeedEntropy: per-render RND/DRNK LFO
                                        // variation is deliberate
    engine.resetEffectState();          // no inherited reverb tail, delay buffer or LCG position
    push_project_params(engine, project, routing, startRow, endRow);
}

// ─── render ──────────────────────────────────────────────────────────────────────────────────────
// `songFrames` is the scheduler's span (0 to the last step's boundary); the file is that plus however
// long the audio takes to die away. `progress` reports 0..1 over the song, then 1.0 once for the tail.
template <typename Engine>
RenderStats render_to_wav(Engine& engine, const Project& project, int64_t songFrames,
                          const std::string& path,
                          const RenderOptions& opts = RenderOptions(),
                          const std::function<void(float)>& progress = nullptr) {
    RenderStats stats;
    if (songFrames <= 0) return stats;

    const int sampleRate = engine.getSampleRate();
    if (opts.applyMasterBus) apply_master_bus_for_render(engine, project);
    engine.setStemsMode(opts.stemsMode);

    WavStreamWriter writer(path, sampleRate);
    if (!writer.is_open()) {
        engine.setStemsMode(0);
        return stats;
    }

    std::vector<float> buf(static_cast<size_t>(RENDER_CHUNK_FRAMES) * 2);

    // ── the range ends, so the notes do ──
    // A KIL at the frame the last row ends on, queued on the sample-accurate timeline. What follows in
    // the file is the TAILS alone — releases, reverb, delay — not the rest of each track's bar.
    engine.scheduleNoteOffAll(songFrames);

    // ── the song ──
    int64_t rendered = 0;
    while (rendered < songFrames) {
        const int chunk = static_cast<int>(std::min<int64_t>(RENDER_CHUNK_FRAMES, songFrames - rendered));
        engine.renderOffline(chunk, buf.data(), sampleRate);
        writer.append_interleaved(buf.data(), chunk);
        rendered += chunk;
        if (progress) progress(static_cast<float>(rendered) / static_cast<float>(songFrames));
    }
    stats.songFrames = songFrames;

    // ── the tail ──
    // Render past the end until a whole chunk peaks below −90 dBFS; that chunk is not written, so the
    // file ends where the music does with no cut through a ringing waveform.
    // This drains the note queue too: a DEL / arp / retrig on the final step can sound past
    // `songFrames`. ⚠️ Such a note starts after the release above; if it loops, it rings until the cap
    // (releasing again would cut those notes at birth).
    if (progress) progress(1.0f);
    const int64_t maxTail = static_cast<int64_t>(TAIL_MAX_SECONDS) * sampleRate;
    int64_t tail = 0;
    while (tail < maxTail) {
        engine.renderOffline(TAIL_CHUNK_FRAMES, buf.data(), sampleRate);

        float peak = 0.0f;
        for (int i = 0; i < TAIL_CHUNK_FRAMES * 2; ++i) {
            const float a = std::fabs(buf[static_cast<size_t>(i)]);
            if (a > peak) peak = a;
        }
        if (peak < TAIL_SILENCE_PEAK) break;   // decayed — stop, and do not append this chunk

        writer.append_interleaved(buf.data(), TAIL_CHUNK_FRAMES);
        tail += TAIL_CHUNK_FRAMES;
    }
    stats.tailFrames = tail;

    engine.setStemsMode(0);
    stats.ok = writer.finish();
    stats.totalFrames = stats.ok ? (songFrames + tail) : 0;
    return stats;
}

// ─── finish ──────────────────────────────────────────────────────────────────────────────────────
// Back to live playback. The master EQ returns to the project's slot — a song's EQM changes the
// global master EQ.
template <typename Engine>
void finish_render(Engine& engine, const Project& project) {
    engine.setStemsMode(0);
    engine.stopAll();
    engine.clearScheduledNotes();
    engine.setMasterEqSlot(project.masterEqSlot);
    engine.setOfflineRendering(false);   // always, even on error paths
}

// ─── the song's bounds ───────────────────────────────────────────────────────────────────────────
// First and last song row with any chain reference on any track. {-1, -1} = the song is empty.
struct SongBounds {
    int startRow = -1;
    int endRow   = -1;
    bool empty() const { return startRow < 0; }
};

inline SongBounds find_song_bounds(const Project& project) {
    SongBounds b;
    for (int row = 0; row < 256; ++row) {
        bool hasContent = false;
        for (const Track& track : project.tracks) {
            if (row < static_cast<int>(track.chainRefs.size()) &&
                track.chainRefs[static_cast<size_t>(row)] >= 0 &&
                track.chainRefs[static_cast<size_t>(row)] <= 255) {
                hasContent = true;
                break;
            }
        }
        if (hasContent) {
            if (b.startRow < 0) b.startRow = row;
            b.endRow = row;
        }
    }
    return b;
}

// ─── SECTIONS: the run of rows a render range defaults to ────────────────────────────────────────
//
// A run of consecutive song rows with content, bounded by rows blank on EVERY track — the default for
// SONG START / SONG END. ⚠️ Not the scheduler's block (one track's run, `song_cell_plays`): a render
// picks rows for all eight tracks, and a section contains every track's block.

/** Does any track have a chain on this row? A row where none does is a section boundary. */
inline bool song_row_filled(const Project& project, int row) {
    if (row < 0 || row >= 256) return false;
    for (const Track& track : project.tracks) {
        if (row < static_cast<int>(track.chainRefs.size()) &&
            track.chainRefs[static_cast<size_t>(row)] >= 0 &&
            track.chainRefs[static_cast<size_t>(row)] <= 255)
            return true;
    }
    return false;
}

/**
 * The first row of the section `row` sits in.
 * ⚠️ A blank row belongs to the section BELOW it: a cursor in a gap is usually heading to the next
 * part. With nothing below, the nearest part above; on an empty song, row 0.
 */
inline int song_section_start(const Project& project, int row) {
    if (row < 0) row = 0;
    int at = -1;
    for (int r = row; r < 256; ++r) if (song_row_filled(project, r)) { at = r; break; }
    if (at < 0) for (int r = row; r >= 0; --r) if (song_row_filled(project, r)) { at = r; break; }
    if (at < 0) return 0;
    while (at > 0 && song_row_filled(project, at - 1)) --at;
    return at;
}

/** The last row of the section starting at `startRow` — what SONG END's AUTO resolves to. */
inline int song_section_end(const Project& project, int startRow) {
    if (startRow < 0) startRow = 0;
    int at = startRow;
    while (at < 255 && song_row_filled(project, at + 1)) ++at;
    return at;
}

/**
 * The start of the section before or after the one `row` is in — R+UP/DOWN in the render dialog.
 * Returns `row`'s own section when there is none that way, so the gesture clamps rather than wrapping.
 */
inline int adjacent_section_start(const Project& project, int row, int delta) {
    const int here = song_section_start(project, row);
    if (delta > 0) {
        for (int r = song_section_end(project, here) + 1; r < 256; ++r)
            if (song_row_filled(project, r)) return r;
        return here;
    }
    for (int r = here - 1; r >= 0; --r)
        if (song_row_filled(project, r)) return song_section_start(project, r);
    return here;
}

// ─── the STEMS plan ──────────────────────────────────────────────────────────────────────────────
//
// WHICH stems a project has, not where they are written: paths need a filesystem, which songcore
// lacks. The file half is ui/project_actions.h.

/** The project name made safe for a filename: `[^a-zA-Z0-9_\-]` → `_`, at most 32 chars. */
inline std::string safe_project_name(const std::string& name) {
    std::string out;
    for (char ch : name) {
        const bool ok = (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
                        (ch >= '0' && ch <= '9') || ch == '_' || ch == '-';
        out += ok ? ch : '_';
        if (out.size() >= 32) break;
    }
    return out;
}

/** One pass of a stems render. `stemsMode`: 1-8 = track N, 9 = reverb return, 10 = delay return. */
struct StemPass {
    int         stemsMode = 0;
    std::string suffix;   // appended to the safe project name: "_1", "_reverb", …
    std::string label;    // the progress line
};

/**
 * The passes a stems render makes over `startRow..endRow` (−1, −1 = the whole song).
 * A track earns a stem when NOT MUTED with a chain reference IN THE RANGE; a send return when an
 * instrument the range uses feeds it. Track stems are numbered SEQUENTIALLY (_1.._N), not by index.
 * ⚠️ The RANGE decides: exporting one part must not write silent files for tracks playing elsewhere.
 */
inline std::vector<StemPass> stems_plan(const Project& project, int startRow = -1, int endRow = -1) {
    std::vector<StemPass> passes;

    SongBounds bounds;
    if (startRow >= 0 && endRow >= startRow) { bounds.startRow = startRow; bounds.endRow = endRow; }
    else                                       bounds = find_song_bounds(project);
    if (bounds.empty()) return passes;

    std::vector<int> activeTracks;
    for (int id = 0; id < static_cast<int>(project.tracks.size()) && id < 8; ++id) {
        const Track& track = project.tracks[static_cast<size_t>(id)];
        if (!track_audible(project, track)) continue;
        bool hasChain = false;
        for (int row = bounds.startRow; row <= bounds.endRow && row < 256 &&
                                        row < static_cast<int>(track.chainRefs.size()); ++row) {
            const int ref = track.chainRefs[static_cast<size_t>(row)];
            if (ref >= 0 && ref <= 255) { hasChain = true; break; }
        }
        if (hasChain) activeTracks.push_back(id);
    }
    if (activeTracks.empty()) return passes;

    const std::set<int> used = collect_used_instruments(project, bounds.startRow, bounds.endRow);
    bool hasReverbSend = false, hasDelaySend = false;
    for (int id : used) {
        if (id < 0 || id >= static_cast<int>(project.instruments.size())) continue;
        const Instrument& ins = project.instruments[static_cast<size_t>(id)];
        if (ins.reverbSend > 0) hasReverbSend = true;
        if (ins.delaySend  > 0) hasDelaySend  = true;
    }

    for (size_t i = 0; i < activeTracks.size(); ++i) {
        StemPass pass;
        pass.stemsMode = activeTracks[i] + 1;
        pass.suffix    = "_" + std::to_string(i + 1);
        pass.label     = "Rendering track " + std::to_string(i + 1) + "/" +
                         std::to_string(activeTracks.size()) + "...";
        passes.push_back(pass);
    }
    if (hasReverbSend) passes.push_back(StemPass{9,  "_reverb", "Rendering reverb stem..."});
    if (hasDelaySend)  passes.push_back(StemPass{10, "_delay",  "Rendering delay stem..."});

    return passes;
}

}  // namespace songcore

#endif  // POCKETTRACKER_SONGCORE_RENDER_H
