#ifndef POCKETTRACKER_SONGCORE_RENDER_H
#define POCKETTRACKER_SONGCORE_RENDER_H

// ─── The offline render ──────────────────────────────────────────────────────────────────────────
//
// RenderController's orchestration, in C++: ready the engine, render the scheduled span in chunks,
// let the tail ring out, stream it to a WAV, put the engine back. Ported in S6b because
// `renderToWavFile` was a Kotlin *private fun* — so tools/ptrender and the SDL shell had nothing to
// drive the engine with, and S7 would have had to write it a second time anyway.
//
// The scheduler is NOT part of this. prepare → (someone schedules) → render is three steps precisely
// so that Android can still schedule with the KOTLIN sequencer while everything around it is shared
// C++ — which is what keeps the ENG=KT vs ENG=C++ byte-identical WAV comparison honest: it isolates
// the sequencer as the only difference between the two renders.
//
// TWO USER-FACING BUGS ARE FIXED HERE (both found by the S5 device test, both older than songcore).
// The rule, from the user: "DSPs SHOULD have the tails at the END of a render — that's the whole
// point of reverb and delay. But their tails SHOULDN'T be picked up at the START of a new render."
// It used to be backwards on both counts:
//
//   (a) THE START INHERITED HISTORY. stopAll() stops voices but never touched the effect chains, so a
//       render began inside the previous one's reverb tail, delay buffer and OTT/limiter envelopes —
//       and because ReverbSc's random-lineseg LCG kept walking, the same song rendered DIFFERENTLY
//       every time. prepare_render now calls AudioEngine::resetEffectState() and re-pushes the whole
//       project (engine_setup.h). A render is now a pure function of the project, which is also what
//       makes ptrender's determinism check possible at all.
//
//   (b) THE END TRUNCATED ITS OWN TAIL. The render stopped dead at the scheduler's span — the last
//       step's boundary — so every WAV was cut mid-waveform: no note release, no reverb tail, no
//       delay repeats, plus a click. (Measured on a device render: the file ended at 100% of the
//       whole render's peak.) render_to_wav now keeps going until the output has decayed below
//       −90 dBFS, capped so a runaway feedback delay cannot render forever.
//
// Live playback cannot be affected by any of this: resetEffectState()'s only callers are here.

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

// Frames per renderOffline() call: ~5 s of stereo float ≈ 1.7 MB per chunk. Rendering a whole song in
// ONE call held ~4 full-song copies in RAM at peak — an OOM kill on 1 GB devices for songs of a few
// minutes. Chunking keeps peak memory flat and makes real progress reporting possible. The engine
// carries its state across chunks, so chunked output is bit-identical to one big call.
constexpr int RENDER_CHUNK_FRAMES = 220500;

// ── the tail (bug (b)) ──
// −90 dBFS: below the noise floor of the 16-bit file being written, so the cut is provably inaudible.
constexpr float TAIL_SILENCE_PEAK = 3.1623e-5f;
// ~93 ms at 44.1 kHz — the granularity at which decay is checked, and the most silence that can be
// appended before the writer stops. Small enough not to pad the file, big enough to be cheap.
constexpr int   TAIL_CHUNK_FRAMES = 4096;
// A delay at maximum feedback never decays. Without a cap it would render forever.
constexpr int   TAIL_MAX_SECONDS  = 30;

struct RenderStats {
    bool    ok          = false;
    int64_t songFrames  = 0;   // the scheduler's span — where the last step ends
    int64_t tailFrames  = 0;   // what the decay tail added past it
    int64_t totalFrames = 0;   // what actually landed in the file
};

struct RenderOptions {
    // AudioEngine::setStemsMode — 0 = full mix, 1-8 = track N stem, 9 = reverb return, 10 = delay return.
    int  stemsMode = 0;
    // Stems deliberately bypass the master bus (OTT/DUST/master-EQ), so they don't apply it.
    bool applyMasterBus = true;
};

// ─── prepare ─────────────────────────────────────────────────────────────────────────────────────
// Everything between "the user pressed render" and "the notes get scheduled". Order matters: the
// chains are wiped to a clean state FIRST and the project is re-pushed SECOND, because
// resetEffectState() leaves every module at its factory defaults (reverb feedback 0x60, delay 500 ms,
// master EQ bypassed) — without the re-push, the render would silently come out with default reverb
// and delay instead of the project's.
template <typename Engine>
void prepare_render(Engine& engine, const Project& project, int startRow, int endRow) {
    engine.setOfflineRendering(true);   // the live stream goes silent so it can't eat the note queue
    engine.stopAll();
    engine.clearScheduledNotes();
    engine.resetFrameCounter();         // also re-seeds noteSeedEntropy — per-render RND/DRNK LFO
                                        // variation is deliberate, and stays
    engine.resetEffectState();          // (a): no inherited reverb tail, delay buffer or LCG position
    push_project_params(engine, project, startRow, endRow);
}

// ─── render ──────────────────────────────────────────────────────────────────────────────────────
// `songFrames` is what the scheduler returned — the span from frame 0 to the last step's boundary.
// The file is that plus however long the audio takes to die away.
//
// `progress` reports 0..1 across the song; the tail reports 1.0 once (its length is not knowable in
// advance, and in practice it is a second or two).
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

    // ── the tail (b) ──
    // Keep rendering past the last step until a whole chunk peaks below −90 dBFS. That chunk is
    // inaudible by construction, so it is NOT written: the file ends where the music does, with no
    // trailing silence and — the point — no cut through a ringing waveform.
    //
    // Note this drains the note QUEUE too, not just the effect tails: a DEL / arp / retrig on the
    // final step can schedule a note at or past `songFrames`, and those notes now sound instead of
    // being silently dropped. That is the right answer musically, but it means the "tail" is not
    // purely a decay — a project that keeps retriggering past the end will keep it alive, which is
    // what TAIL_MAX_SECONDS is ultimately guarding against.
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
        if (peak < TAIL_SILENCE_PEAK) break;   // decayed — stop, and don't append this chunk

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
// Put the engine back the way live playback expects it. Mirrors RenderController's `finally` blocks:
// the master EQ goes back to the project's slot (an EQM effect in the song mutates the GLOBAL master
// EQ — without this the live bus would stay on whatever preset the song's last EQM left behind).
template <typename Engine>
void finish_render(Engine& engine, const Project& project) {
    engine.setStemsMode(0);
    engine.stopAll();
    engine.clearScheduledNotes();
    engine.setMasterEqSlot(project.masterEqSlot);
    engine.setOfflineRendering(false);   // live playback back on — always, even on the error paths
}

// ─── find the song's bounds (RenderController.findSongBounds) ────────────────────────────────────
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

// ─── the STEMS plan (RenderController.renderStemsToWav, the pure half) ───────────────────────────
//
// WHICH stems a project has — deliberately NOT where they are written. Building the paths needs to
// ask a filesystem what already exists and to create a folder, and songcore has no filesystem: it
// must keep compiling for the Android NDK, where the answer to "where do files live" is scoped
// storage and Kotlin's. So the POLICY is here and PURE, and the file half sits in the UI beside the
// FileSystem interface that exists for exactly this question (ui/project_actions.h).

/** The project name, made safe for a filename. Kotlin: `[^a-zA-Z0-9_\-]` → `_`, then take(32). */
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
 * The passes a stems render will make.
 *
 * A track earns a stem when it is NOT MUTED and has at least one chain reference in the song. The
 * two send returns earn one when any instrument the song actually uses feeds them. ⚠️ The track
 * stems are numbered SEQUENTIALLY (_1.._N), not by track index — Kotlin's, and it means a song using
 * tracks 1, 4 and 7 yields _1, _2, _3.
 */
inline std::vector<StemPass> stems_plan(const Project& project) {
    std::vector<StemPass> passes;

    const SongBounds bounds = find_song_bounds(project);
    if (bounds.empty()) return passes;

    std::vector<int> activeTracks;
    for (int id = 0; id < static_cast<int>(project.tracks.size()) && id < 8; ++id) {
        const Track& track = project.tracks[static_cast<size_t>(id)];
        if (track.mute) continue;
        bool hasChain = false;
        for (int row = 0; row < 256 && row < static_cast<int>(track.chainRefs.size()); ++row) {
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
