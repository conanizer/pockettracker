#ifndef POCKETTRACKER_SONGCORE_SAMPLE_EDIT_H
#define POCKETTRACKER_SONGCORE_SAMPLE_EDIT_H

// ─── The sample editor, below the seam ───────────────────────────────────────────────────────────
//
// The editor's operations and DSP live in the engine (`native/sample-editor.cpp`,
// `transient-detector.cpp`); `SongcoreHost` forwards to them. This file holds only what needs the
// ROUTING (a sample's rate ratio) or the PROJECT (the instrument auditioned):
//
//   • the RATE mode's ratio cache, so LOFI → HIGH restores the loaded ratio instead of compounding;
//   • the SOURCE preview: LEFT / RIGHT / MONO plays from a scratch slot, pitched by the instrument's
//     ratio;
//   • the DRY audition: the raw waveform with EQ, sends and modulation off;
//   • SAVE and CHOP: the edited PCM written back, with slice boundaries, as WAV (`wav_writer.h`).

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <string>
#include <utility>
#include <vector>

#include "engine_setup.h"   // SOURCE_PREVIEW_SLOT
#include "model.h"
#include "voice_derive.h"   // Routing, detune/root frequency math
#include "wav_writer.h"

namespace songcore {

/**
 * The ratio each slot was LOADED with, remembered only while the RATE row has moved it off HIGH —
 * otherwise LOFI → NORM would decimate an already-decimated ratio. 0 = absent (a ratio is never 0).
 */
struct RateCache {
    float orig[POOL_INSTRUMENTS];

    RateCache() { reset(); }
    void reset() {
        for (int i = 0; i < POOL_INSTRUMENTS; ++i) orig[i] = 0.0f;
    }
    bool  has(int id) const { return in_range(id) && orig[id] > 0.0f; }
    void  clear(int id) { if (in_range(id)) orig[id] = 0.0f; }

    static bool in_range(int id) { return id >= 0 && id < POOL_INSTRUMENTS; }
};

/**
 * The FILE's sample rate, `deviceRate / ratio` — for the editor header, `applySampleFx`, SYNC, and SAVE
 * (a 22 kHz file edited on a 48 kHz device is saved as 22 kHz).
 * ⚠️ 44100 for an EMPTY slot: `Routing` initialises every ratio to 1.0, which would report the device
 * rate in an empty editor's header.
 */
template <typename Engine>
int original_sample_rate(Engine* engine, const Routing& routing, int id) {
    if (!engine || !RateCache::in_range(id)) return 44100;
    if (engine->getSampleLength(id) <= 0) return 44100;   // no sample loaded

    const float ratio = routing.sampleRateRatio[id];
    if (ratio <= 0.0f) return 44100;
    return std::max(static_cast<int>(static_cast<float>(engine->getSampleRate()) / ratio), 8000);
}

/**
 * RATE: HIGH (1×) / NORM (2×) / LOFI (4×) — a DESTRUCTIVE decimation of the buffer.
 * The ratio is always set RELATIVE TO THE ORIGINAL, so NORM → LOFI → NORM lands back on NORM.
 * `bits` (the BIT cell: 32 / 24 / 16 / 8) travels along because the engine rebuilds both from one
 * cached original; a RATE change unaware of it would restore full depth.
 */
template <typename Engine>
void apply_rate_and_bits(Engine* engine, Routing& routing, RateCache& cache, int id, int factor,
                         int bits) {
    if (!engine || !RateCache::in_range(id)) return;

    if (factor <= 1) {
        // Back to HIGH: restore the ratio the file was loaded with, and forget it.
        if (cache.has(id)) {
            routing.sampleRateRatio[id] = cache.orig[id];
            cache.clear(id);
        }
    } else {
        if (!cache.has(id)) cache.orig[id] = routing.sampleRateRatio[id];   // first departure from HIGH
        routing.sampleRateRatio[id] = cache.orig[id] * static_cast<float>(factor);
    }

    // The base frequency is derived from the ratio at schedule time, so this write IS the pitch fix.
    engine->applyRateAndBits(id, factor, bits);
}

/**
 * Both destructive resamplers. The rewritten buffer becomes the new "original", so the cache entry is
 * dropped — a stale one would make the next HIGH restore a ratio that no longer fits the audio.
 */
template <typename Engine>
void pitch_shift_sample(Engine* engine, RateCache& cache, int id, float semitones) {
    if (!engine) return;
    cache.clear(id);
    engine->pitchShiftSample(id, semitones);
}

template <typename Engine>
void time_stretch_sample(Engine* engine, RateCache& cache, int id, float ratio) {
    if (!engine) return;
    cache.clear(id);
    engine->timeStretchSample(id, ratio);
}

/**
 * Which SLOT the audition comes out of for the SOURCE mode. A mono sample or STEREO mode plays from
 * the instrument's own slot; LEFT, RIGHT or MONO on a stereo sample is extracted slot → slot into the
 * scratch slot 254 (never through big intermediate arrays).
 */
template <typename Engine>
int prepare_source_preview(Engine& engine, int id, int sourceMode) {
    if (!engine.hasStereoData(id) || sourceMode == 2 /*STEREO*/) return id;
    engine.prepareSourcePreview(SOURCE_PREVIEW_SLOT, id, sourceMode);
    return SOURCE_PREVIEW_SLOT;
}

/**
 * The editor's audition: the sample at its ROOT, DRY.
 * ⚠️ Deliberately not `plan_note_on`, which brings the instrument's EQ, sends and modulation — you
 * cannot find a zero crossing under a reverb tail. Drive, crush, filter and the window still apply
 * (the caller pushed playback params with the SELECTION as the window).
 * `sampleRateRatio` is the INSTRUMENT's even from scratch slot 254, which holds a copy of its audio.
 */
template <typename Engine>
void preview_instrument_dry(Engine& engine, const Instrument& ins, int slotId, float sampleRateRatio) {
    if (ins.instrumentType == InstrumentType::SOUNDFONT) return;   // no waveform to edit

    // ROOT × detune is the playing pitch; C-4 × the rate ratio is this file's "unity". Their quotient
    // is the resampling rate — so a 22 kHz file plays at its own pitch and ROOT transposes the audition.
    const float targetFreq = note_hz(note_to_midi(ins.root)) * detune_multiplier(ins.detune);
    const float baseFreq   = C4_HZ * sampleRateRatio;

    engine.scheduleKill(engine.getCurrentFrame(), Engine::PREVIEW_LANE);   // the previous audition
    engine.requestResume();

    // DRY: the three the audition must not wear.
    engine.clearInstrumentModulation(slotId);
    engine.setInstrumentEqSlot(slotId, -1);          // −1 = the engine's own bypass
    engine.setInstrumentSendLevels(slotId, 0, 0);

    engine.scheduleNote(engine.getCurrentFrame() + 100, slotId, Engine::PREVIEW_LANE,
                        /*frequency=*/targetFreq, /*baseFrequency=*/baseFreq, /*volume=*/1.0f,
                        /*phraseVolume=*/1.0f, /*pan=*/0.5f);
}

// ─── SAVE and CHOP ───────────────────────────────────────────────────────────────────────────────

/**
 * The channel buffers a save will write, and how many the WAV gets.
 * ⚠️ `right` is EMPTY unless `channels == 2` — `write_wav` reads it only for a stereo file, and a copy
 * would double a sample-length buffer for nothing.
 */
struct SaveChannels {
    std::vector<float> left;
    std::vector<float> right;
    int                channels = 1;
};

/**
 * Pull the edited PCM out of the engine per SOURCE mode: STEREO on a stereo sample writes two
 * channels; LEFT and RIGHT write one channel as mono; MONO downmixes. A mono sample ignores the mode.
 */
template <typename Engine>
SaveChannels resolve_save_channels(Engine& engine, int id, int sourceMode, bool hasStereo) {
    SaveChannels out;
    const int len = engine.getSampleLength(id);
    if (len <= 0) return out;

    auto pull_left = [&] {
        std::vector<float> v(static_cast<size_t>(len));
        engine.getSampleData(id, v.data());
        return v;
    };
    auto pull_right = [&] {
        std::vector<float> v(static_cast<size_t>(len));
        engine.getSampleDataRight(id, v.data());
        return v;
    };

    if (!hasStereo) {
        out.left     = pull_left();
        out.channels = 1;
        return out;
    }

    switch (sourceMode) {
        case 0:   // LEFT → mono
            out.left = pull_left();
            break;
        case 1:   // RIGHT → mono
            out.left = pull_right();
            break;
        case 2:   // STEREO → the only two-channel save
            out.left  = pull_left();
            out.right = pull_right();
            break;
        default: {   // MONO → downmix
            std::vector<float> l = pull_left();
            const std::vector<float> r = pull_right();
            // In place, into the buffer holding L: moving rather than assigning keeps the downmix at
            // two full-length copies instead of five.
            for (size_t i = 0; i < l.size() && i < r.size(); ++i) l[i] = (l[i] + r[i]) / 2.0f;
            out.left = std::move(l);
            break;
        }
    }
    out.channels = (sourceMode == 2) ? 2 : 1;
    return out;
}

/**
 * Write the edited sample to `path`, slices in the `cue ` chunk, at the file's OWN rate (an edit is
 * not a resample) and at `bits` — or, for 0, the loaded depth. 32 stays float only if the source was.
 */
template <typename Engine>
int resolve_save_bits(Engine& engine, int id, int bits) {
    return (bits > 0) ? bits : engine.getSampleBitDepth(id);
}

template <typename Engine>
bool save_sample_wav(Engine& engine, const Routing& routing, int id, const std::string& path,
                     const std::vector<int>& cuePoints, int sourceMode, bool hasStereo, int bits = 0) {
    const SaveChannels ch = resolve_save_channels(engine, id, sourceMode, hasStereo);
    if (ch.left.empty()) return false;
    const int depth = resolve_save_bits(engine, id, bits);
    return write_wav(path, ch.left, ch.right, original_sample_rate(&engine, routing, id), cuePoints,
                     ch.channels, depth, depth == 32 && engine.isSampleFloat(id));
}

/**
 * CHOP: every slice to its own WAV in `dir`, `<base>_00.wav`, `<base>_01.wav`, … Returns how many.
 * The PCM is pulled once and sliced in memory.
 * ⚠️ Writes the LEFT channel only, through `write_wav_mono` — a two-channel file with that channel in
 * both (wav_writer.h; a shipped format).
 */
template <typename Engine>
int chop_sample(Engine& engine, const Routing& routing, int id, const std::string& dir,
                const std::string& base_name, const std::vector<std::pair<int64_t, int64_t>>& slices,
                int bits = 0) {
    const int len = engine.getSampleLength(id);
    if (len <= 0 || slices.empty()) return 0;
    const int  depth   = resolve_save_bits(engine, id, bits);   // the slices come out at SAVE's depth
    const bool isFloat = depth == 32 && engine.isSampleFloat(id);

    std::vector<float> pcm(static_cast<size_t>(len));
    engine.getSampleData(id, pcm.data());
    const int rate = original_sample_rate(&engine, routing, id);

    int written = 0;
    for (size_t i = 0; i < slices.size(); ++i) {
        const int64_t start = std::clamp<int64_t>(slices[i].first, 0, len);
        const int64_t end   = std::clamp<int64_t>(slices[i].second, start, len);
        if (end <= start) continue;   // an empty slice is not a file

        const std::vector<float> slice(pcm.begin() + static_cast<ptrdiff_t>(start),
                                       pcm.begin() + static_cast<ptrdiff_t>(end));

        char suffix[8];
        std::snprintf(suffix, sizeof(suffix), "%02d", static_cast<int>(i));
        if (write_wav_mono(dir + "/" + base_name + "_" + suffix + ".wav", slice, rate, {}, depth, isFloat))
            written++;
    }
    return written;
}

}  // namespace songcore

#endif  // POCKETTRACKER_SONGCORE_SAMPLE_EDIT_H
