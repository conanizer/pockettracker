#ifndef POCKETTRACKER_SONGCORE_SAMPLE_EDIT_H
#define POCKETTRACKER_SONGCORE_SAMPLE_EDIT_H

// ─── The sample editor, below the seam (Phase 3 S6b) ─────────────────────────────────────────────
//
// **Almost none of the sample editor is here, and that is the headline.** Every one of its twelve
// operations — crop, copy, cut, dupl, paste, del, normalise, fade in, fade out, silence, reverse, undo
// — plus the FX chain, the pitch shift, the time stretch and the transient detector, already live in
// `native/sample-editor.cpp` and `native/transient-detector.cpp`, in C++, and always did: Android's JNI
// layer is a thin forward and nothing more. `SongcoreHost` exposes them as verbs (host.h) and the whole
// of that is a one-line call each.
//
// What lands in THIS file is only what the engine cannot do on its own, because it needs something the
// engine does not have — the ROUTING (a sample's rate ratio, which songcore owns because songcore is
// what opened the file) or the PROJECT (the instrument being auditioned). There are four such things:
//
//   • **the RATE mode's ratio cache** — Kotlin's `originalSampleRateRatios`, a map that exists so LOFI
//     → HIGH can restore the ratio the file was loaded with rather than compounding factors;
//   • **the SOURCE preview** — a stereo sample auditioned as LEFT / RIGHT / MONO plays out of a scratch
//     slot, not its own, so the pitch it plays at must be borrowed from the instrument's ratio;
//   • **the DRY audition** — the raw waveform with the instrument's EQ, sends and modulation switched
//     off, which is the entire reason to have an audition inside an editor;
//   • **SAVE and CHOP** — pulling the edited PCM back out and writing it, with its slice boundaries, as
//     a WAV (`wav_writer.h`).

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <string>
#include <utility>
#include <vector>

#include "engine_setup.h"   // SOURCE_PREVIEW_SLOT — the scratch slot a channel-selected audition plays from
#include "model.h"
#include "voice_derive.h"   // Routing, detune/root frequency math
#include "wav_writer.h"

namespace songcore {

/**
 * Kotlin's `AudioEngine.originalSampleRateRatios` — the ratio each slot was LOADED with, remembered
 * only while the RATE row has moved it away from HIGH.
 *
 * Without it, LOFI → NORM would multiply the already-decimated ratio again and the sample would come
 * back at the wrong pitch. Kotlin uses a HashMap and keys presence on `containsKey`; an array with 0 as
 * "absent" says the same thing (a rate ratio is never 0), with no allocation on a handheld.
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
 * The FILE's sample rate, recovered from the ratio it was loaded with: `deviceRate / ratio`.
 *
 * The editor's header reads it, `applySampleFx` is given it, SYNC measures the sample's length in
 * seconds with it, and SAVE writes it into the WAV — so a 22 kHz file edited on a 48 kHz device is
 * saved back as 22 kHz rather than silently resampled.
 *
 * ⚠️ 44100 when the slot holds NO sample. Kotlin's ratio map simply has no entry for an unloaded slot
 * and its `?: return 44100` catches that; a C++ `Routing` initialises every ratio to 1.0f, which would
 * instead report the DEVICE rate and put "48000Hz" in the header of an empty editor.
 */
template <typename Engine>
int original_sample_rate(Engine* engine, const Routing& routing, int id) {
    if (!engine || !RateCache::in_range(id)) return 44100;
    if (engine->getSampleLength(id) <= 0) return 44100;   // no sample → Kotlin's map has no entry

    const float ratio = routing.sampleRateRatio[id];
    if (ratio <= 0.0f) return 44100;
    return std::max(static_cast<int>(static_cast<float>(engine->getSampleRate()) / ratio), 8000);
}

/**
 * RATE: HIGH (1×) / NORM (2×) / LOFI (4×) — a DESTRUCTIVE decimation of the buffer, and the only edit
 * on row 1 that changes the audio rather than the view.
 *
 * The ratio is always set RELATIVE TO THE ORIGINAL, never to the current value, so NORM → LOFI → NORM
 * lands back where NORM was instead of drifting an octave each time.
 */
template <typename Engine>
void apply_rate_mode(Engine* engine, Routing& routing, RateCache& cache, int id, int factor) {
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

    // The playback base frequency is derived from the ratio at schedule time, so the write above IS the
    // whole pitch correction — there is no second cache to keep in step.
    engine->applyRateMode(id, factor);
}

/**
 * Both destructive resamplers. They rewrite the buffer, so whatever RATE decimation was in flight is
 * baked into it — the shifted buffer becomes the new "original", and a stale cache entry would make the
 * next HIGH restore a ratio that no longer describes the audio.
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
 * Which SLOT the audition will actually come out of, given the editor's SOURCE mode.
 *
 * A mono sample, or STEREO mode, plays from the instrument's own slot. Anything else — LEFT, RIGHT,
 * MONO on a stereo sample — needs a channel selected or a downmix, which the engine does slot→slot into
 * the scratch slot 254. (Kotlin's old path pulled both channels into Java arrays to do it, which is the
 * OOM class the native load paths exist to avoid.)
 */
template <typename Engine>
int prepare_source_preview(Engine& engine, int id, int sourceMode) {
    if (!engine.hasStereoData(id) || sourceMode == 2 /*STEREO*/) return id;
    engine.prepareSourcePreview(SOURCE_PREVIEW_SLOT, id, sourceMode);
    return SOURCE_PREVIEW_SLOT;
}

/**
 * The editor's audition: the sample at its ROOT, DRY.
 *
 * ⚠️ This is the second note in the port that does not go through `plan_note_on`, and — like the file
 * browser's (engine_setup.h) — the exception is the point rather than a shortcut. `plan_note_on` derives
 * a note WITH the instrument's voice: its EQ, its sends, its modulation. An editor audition exists to
 * let you hear **the waveform you are cutting**, so those three are explicitly switched off first. A
 * reverb tail over a sample you are trying to find a zero crossing in is not a preview, it is a
 * disguise. (Drive, crush, filter and the sample window are NOT switched off — they come through
 * `push_instrument_playback_params`, which the caller has already pushed with the SELECTION as the
 * window. That is Kotlin's behaviour, comment notwithstanding: its own `previewInstrumentDry` says "no
 * filter" while `updateInstrumentPlaybackParams` pushes one.)
 *
 * `sampleRateRatio` is the INSTRUMENT's, even when `slotId` is the 254 scratch — that slot holds a copy
 * of the instrument's audio and must be pitched exactly as the instrument would be. Kotlin says the same
 * thing by assigning `sampleRateRatios[254] = sampleRateRatios[instId]` before it reads the map back.
 */
template <typename Engine>
void preview_instrument_dry(Engine& engine, const Instrument& ins, int slotId, float sampleRateRatio) {
    if (ins.instrumentType == InstrumentType::SOUNDFONT) return;   // no waveform to edit

    constexpr float C4_HZ = 261.63f;

    // ROOT × detune is the pitch it plays at; C-4 × the rate ratio is what "unity" means for this file.
    // Their quotient is the resampling rate, so a 22 kHz sample plays at its own pitch, and dialling
    // ROOT up transposes the audition — which is what makes ROOT audible at all.
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

/** The channel buffers a save will write, and how many of them the WAV gets. */
struct SaveChannels {
    std::vector<float> left;
    std::vector<float> right;
    int                channels = 1;
};

/**
 * Pull the edited PCM back out of the engine, honouring the editor's SOURCE mode.
 *
 * Only SOURCE=STEREO on a stereo sample writes a two-channel file. LEFT and RIGHT each pick one channel
 * and write it as mono; MONO downmixes. A mono sample ignores the mode entirely — there is nothing to
 * choose between.
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
        out.right    = out.left;
        out.channels = 1;
        return out;
    }

    switch (sourceMode) {
        case 0:   // LEFT → mono
            out.left  = pull_left();
            out.right = out.left;
            break;
        case 1:   // RIGHT → mono
            out.left  = pull_right();
            out.right = out.left;
            break;
        case 2:   // STEREO → the only two-channel save
            out.left  = pull_left();
            out.right = pull_right();
            break;
        default: {   // MONO → downmix
            const std::vector<float> l = pull_left();
            const std::vector<float> r = pull_right();
            std::vector<float> m(l.size());
            for (size_t i = 0; i < l.size(); ++i) m[i] = (l[i] + r[i]) / 2.0f;
            out.left  = m;
            out.right = m;
            break;
        }
    }
    out.channels = (sourceMode == 2) ? 2 : 1;
    return out;
}

/**
 * Write the edited sample to `path`, with its slice boundaries in the WAV's `cue ` chunk.
 *
 * At the file's OWN rate, not the device's: an edit is not a resample, and a 22 kHz sample that came
 * back as 48 kHz would double in size for no audible gain.
 */
template <typename Engine>
bool save_sample_wav(Engine& engine, const Routing& routing, int id, const std::string& path,
                     const std::vector<int>& cuePoints, int sourceMode, bool hasStereo) {
    const SaveChannels ch = resolve_save_channels(engine, id, sourceMode, hasStereo);
    if (ch.left.empty()) return false;
    return write_wav(path, ch.left, ch.right, original_sample_rate(&engine, routing, id), cuePoints,
                     ch.channels);
}

/**
 * CHOP: every slice out to its own WAV in `dir`, named `<base>_00.wav`, `<base>_01.wav`, …
 *
 * Returns how many were written. The PCM is pulled ONCE and sliced in memory — the alternative (a pull
 * per slice) is the same bytes copied N times for a sample that may be minutes long.
 *
 * ⚠️ It writes the LEFT channel only, through `write_wav_mono` — which, being Kotlin's, produces a
 * two-channel file with that one channel in both. See wav_writer.h; the format is a shipped one.
 */
template <typename Engine>
int chop_sample(Engine& engine, const Routing& routing, int id, const std::string& dir,
                const std::string& base_name, const std::vector<std::pair<int64_t, int64_t>>& slices) {
    const int len = engine.getSampleLength(id);
    if (len <= 0 || slices.empty()) return 0;

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
        if (write_wav_mono(dir + "/" + base_name + "_" + suffix + ".wav", slice, rate)) written++;
    }
    return written;
}

}  // namespace songcore

#endif  // POCKETTRACKER_SONGCORE_SAMPLE_EDIT_H
