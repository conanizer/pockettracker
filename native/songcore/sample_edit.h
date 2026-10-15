#ifndef POCKETTRACKER_SONGCORE_SAMPLE_EDIT_H
#define POCKETTRACKER_SONGCORE_SAMPLE_EDIT_H

// The sample editor's parts that need the Routing (a sample's rate ratio) or the Project: RATE's ratio
// cache, the SOURCE and SYNC previews, the dry audition, SAVE and CHOP. The DSP itself is the engine's
// (sample-editor.cpp).

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <string>
#include <utility>
#include <vector>

#include "../effects/primitives/sola-stretch.h"   // DEFAULT_GRAIN_MS
#include "engine_setup.h"   // SOURCE_PREVIEW_SLOT
#include "model.h"
#include "voice_derive.h"   // Routing, detune/root frequency math
#include "wav_writer.h"

namespace songcore {

// The ratio each slot was LOADED with, kept while RATE is off HIGH so LOFI → NORM does not compound.
// 0 = absent.
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

// The FILE's sample rate, `deviceRate / ratio`. 44100 for an empty slot (whose ratio is 1.0).
template <typename Engine>
int original_sample_rate(Engine* engine, const Routing& routing, int id) {
    if (!engine || !RateCache::in_range(id)) return 44100;
    if (engine->getSampleLength(id) <= 0) return 44100;   // no sample loaded

    const float ratio = routing.sampleRateRatio[id];
    if (ratio <= 0.0f) return 44100;
    return std::max(static_cast<int>(static_cast<float>(engine->getSampleRate()) / ratio), 8000);
}

// RATE: HIGH (1×) / NORM (2×) / LOFI (4×), a destructive decimation relative to the ORIGINAL. `bits`
// travels along: the engine rebuilds both from one cached original.
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

// The destructive resamplers. The result is the new original, so the cache entry is dropped.
template <typename Engine>
void pitch_shift_sample(Engine* engine, RateCache& cache, int id, float semitones) {
    if (!engine) return;
    cache.clear(id);
    engine->pitchShiftSample(id, semitones);
}

template <typename Engine>
void time_stretch_sample(Engine* engine, RateCache& cache, int id, float ratio, float grainMs,
                         float sampleRate) {
    if (!engine) return;
    cache.clear(id);
    engine->timeStretchSample(id, ratio, grainMs, sampleRate);
}

// What SYNC does: nothing, a resample (RPITCH) or a stretch (TSTRETCH) — one value for APPLY and the
// audition alike.
struct SampleSync {
    enum class Kind { NONE, REPITCH, STRETCH };
    Kind  kind      = Kind::NONE;
    float semitones = 0.0f;   // REPITCH: the whole fit. STRETCH: a pitch shift baked in on the way.
    int   frames    = 0;      // STRETCH: the length to land on
    float grainMs   = sola::DEFAULT_GRAIN_MS;   // STRETCH: the chunk length
};

// ⚠️ The pitch goes first (it changes the length); the stretch after it lands on `frames`.
template <typename Engine>
void run_sample_sync(Engine& engine, int id, const SampleSync& sync, float sampleRate) {
    if (sync.kind == SampleSync::Kind::NONE) return;
    if (sync.semitones != 0.0f) engine.pitchShiftSample(id, sync.semitones);
    if (sync.kind != SampleSync::Kind::STRETCH) return;

    const int len = engine.getSampleLength(id);
    if (len <= 0) return;
    engine.timeStretchSample(id, static_cast<float>(static_cast<double>(sync.frames) / len),
                             sync.grainMs, sampleRate);
}

// The slot the audition plays from: the instrument's own, or for LEFT/RIGHT/MONO on a stereo sample,
// the scratch slot 254.
template <typename Engine>
int prepare_source_preview(Engine& engine, int id, int sourceMode) {
    if (!engine.hasStereoData(id) || sourceMode == 2 /*STEREO*/) return id;
    engine.prepareSourcePreview(SOURCE_PREVIEW_SLOT, id, sourceMode);
    return SOURCE_PREVIEW_SLOT;
}

// SYNC's audition, built from a copy in the scratch slot; returns the slot to play.
// ⚠️ A different LENGTH: a frame window on it is the selection scaled by new / old.
template <typename Engine>
int prepare_sync_preview(Engine& engine, int id, int sourceMode, const SampleSync& sync,
                         float sampleRate) {
    if (sync.kind == SampleSync::Kind::NONE) return prepare_source_preview(engine, id, sourceMode);
    engine.prepareSourcePreview(SOURCE_PREVIEW_SLOT, id, sourceMode);
    run_sample_sync(engine, SOURCE_PREVIEW_SLOT, sync, sampleRate);
    return SOURCE_PREVIEW_SLOT;
}

// The editor's audition: the sample at its root, DRY.
// ⚠️ Not `plan_note_on`: its EQ, sends and modulation would hide a zero crossing under a tail.
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

// What a save writes. `right` is empty unless `channels == 2`.
struct SaveChannels {
    std::vector<float> left;
    std::vector<float> right;
    int                channels = 1;
};

// The edited PCM per SOURCE mode: STEREO two channels, LEFT/RIGHT one, MONO a downmix.
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

template <typename Engine>
int resolve_save_bits(Engine& engine, int id, int bits) {
    return (bits > 0) ? bits : engine.getSampleBitDepth(id);
}

// The edited sample as a WAV at the file's own rate, slices in the `cue ` chunk. `bits` 0 = the
// loaded depth; 32 is float only if the source was.
template <typename Engine>
bool save_sample_wav(Engine& engine, const Routing& routing, int id, const std::string& path,
                     const std::vector<int>& cuePoints, int sourceMode, bool hasStereo, int bits = 0) {
    const SaveChannels ch = resolve_save_channels(engine, id, sourceMode, hasStereo);
    if (ch.left.empty()) return false;
    const int depth = resolve_save_bits(engine, id, bits);
    return write_wav(path, ch.left, ch.right, original_sample_rate(&engine, routing, id), cuePoints,
                     ch.channels, depth, depth == 32 && engine.isSampleFloat(id));
}

// CHOP: every slice to `<base>_NN.wav` in `dir`; returns how many.
// ⚠️ The LEFT channel only, through `write_wav_mono` (both channels hold it — a shipped format).
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
