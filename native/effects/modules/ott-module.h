#pragma once
#include "../primitives/lr-crossover.h"
#include "../primitives/upward-compressor.h"
#include "../primitives/daisysp/compressor.h"

// ===========================================================================
// BandCompressor — linked stereo bidirectional compressor for one band.
// Key signal = max(|L|, |R|) so both channels get identical gain.
//
// ⚠️ ONE THRESHOLD DRIVES BOTH HALVES, and the halves run in series: the
// upward detector is keyed on the DOWNWARD HALF'S OUTPUT, not on the band
// input. That ordering is what keeps the upward half quiet — downward has
// already pinned anything loud to the threshold, so upward sees a stable level
// and only lifts what is genuinely below it after compression.
//
// ⚠️ Keying both halves off the band input instead makes the upward half swing
// its full range between notes and dump the accumulated boost, times the
// makeup below, onto the front of the next note: measured +5 dB of peak on a
// render whose RMS went DOWN, because the limiter then spends a second
// recovering from each spike.
//
// makeup runs after both halves, before the band is summed back.
//
// ⚠️ DaisySP's own makeup stays at 0. It starts from gain_rec_=0.1 after
// Init(), so a non-zero SetMakeup fires a first-note +makeup-dB pop before
// compression settles. makeupLin below does that job instead.
// ===========================================================================
struct BandCompressor {
    // The preset's below-threshold ratio, the same on all three bands.
    static constexpr float UP_RATIO = 4.f;

    daisysp::Compressor downward;
    UpwardCompressor    upward;
    float sampleRate = 44100.f;
    float attackSec  = 0.001f;
    float releaseSec = 0.030f;
    float threshDb   = -30.f;
    float downRatio  = 66.7f;
    float makeupLin  = 1.0f;

    void applySettings() {
        downward.SetRatio(downRatio);
        downward.SetThreshold(threshDb);
        downward.SetAttack(attackSec);
        downward.SetRelease(releaseSec);
        downward.AutoMakeup(false);
        downward.SetMakeup(0.f);
        upward.setParams(threshDb, UP_RATIO, attackSec, releaseSec, sampleRate);
    }

    void init(float sr, float atk, float rel,
              float thresh, float ratio, float makeup) {
        sampleRate = sr;
        attackSec  = atk;
        releaseSec = rel;
        threshDb   = thresh;
        downRatio  = ratio;
        makeupLin  = makeup;
        downward.Init(sr);
        upward.init(sr);
        applySettings();
    }

    void reset() {
        downward.Init(sampleRate);
        upward.reset();
        applySettings();
    }

    inline void process(float& L, float& R) {
        downward.Process(fmaxf(fabsf(L), fabsf(R)));
        L = downward.Apply(L);
        R = downward.Apply(R);
        upward.Process(fmaxf(fabsf(L), fabsf(R)));
        L = upward.Apply(L) * makeupLin;
        R = upward.Apply(R) * makeupLin;
    }
};

// ===========================================================================
// OttModule — 3-band bidirectional compressor for the master bus.
//
// Signal flow:
//   input → [LRCrossover @88.3 Hz / 2500 Hz]
//        → 3× BandCompressor (downward → upward → makeup)
//        → sum × OUTPUT_GAIN → linear wet/dry mix → output
//
// One control: depth (0=bypass, 1=full OTT).
// enabled flag gates the DSP so MasterChain can skip it at depth=0.
//
// The band table is the Ableton Multiband Dynamics "OTT" preset, which the
// Xfer plugin replicates:
//
//   band   thresh   above      below   band gain   attack / release
//   low    -30 dB   66.7:1     4:1     +10.3 dB    2.8 ms / 40 ms
//   mid    -30 dB   66.7:1     4:1      +5.7 dB    1.4 ms / 28 ms
//   high   -30 dB   limiter    4:1     +10.3 dB    0.7 ms / 15 ms
//
// ⚠️ The preset also drives +5.2 dB into every band ahead of its detectors,
// which is NOT carried here. That drive exists to push a quiet mix over the
// threshold, and this master bus already sits ~27 dB above it, so all it would
// add is 5 dB of naked gain through the attack window on top of the makeup —
// measured as 5 dB of extra peak into the limiter for 0.1 dB of steady state.
//
// ⚠️ The outer bands are
// made up ~4.6 dB harder than the middle one, and that asymmetry is what gives
// OTT its scooped, hyped tone. It is a tilt, not a level trim — the three
// numbers only mean anything relative to each other.
//
// ⚠️ The attack/release column is NOT the preset's, which specifies no per-band
// times. It is Vital's multiband compressor, whose three bands run at exactly
// these values. The high band responds 4× faster than the low → transient
// sparkle, no bass pump.
//
// OUTPUT_GAIN trims the summed wet path so that turning OTT up does not cost
// volume. The band gains above are stated relative to Ableton's gain staging
// rather than ours, and the preset carries an output control for exactly this.
//
// OUTPUT_GAIN is applied to the wet sum rather than through DaisySP SetMakeup,
// which avoids the first-note pop; the warmup crossfade masks its onset ramp.
//
// Warmup fade: wet signal ramps from 0 to depth over WARMUP_SAMPLES.
// Triggered on: disabled→enabled, resetForRender, and auto-reset.
//
// Auto-reset: after SILENCE_RESET_FRAMES of silence the module resets DSP and
// starts a warmup. This ensures every playback-start-after-silence gets the
// warmup rather than the LR4 filter-transient + per-band compression artifact
// that sounds like a fade-in. Depth changes while enabled do NOT reset (avoids
// compressors losing their gain state during key-repeat parameter sweeps).
// ===========================================================================
struct OttModule {
    LRCrossover    xover;
    BandCompressor bandLow, bandMid, bandHigh;

    float depth      = 0.0f;
    float sampleRate = 44100.f;
    bool  enabled    = false;

    // 512 samples (~11.6ms): LR4 pole at 88.3Hz has τ≈113 samples; 4.5τ=509 ≈ -39 dBFS.
    int   warmupRemaining = 0;
    static constexpr int   WARMUP_SAMPLES      = 512;
    // Auto-reset after 500 ms of silence so each START-after-stop gets a warmup.
    int   silenceCounter  = 0;
    static constexpr int   SILENCE_RESET_FRAMES = 22050;   // 500 ms @ 44100 Hz

    static constexpr float XOVER_LOW     = 88.3f;
    static constexpr float XOVER_HIGH    = 2500.f;
    static constexpr float THRESH_DB     = -30.f;
    static constexpr float DOWN_RATIO    = 66.7f;
    // The preset's infinity:1 on the high band. DaisySP takes a finite ratio and
    // works from 1/ratio, so 1000:1 lands within a thousandth of a true limiter.
    static constexpr float DOWN_RATIO_HI = 1000.f;
    static constexpr float MAKEUP_LOW    = 3.2734f;   // +10.3 dB
    static constexpr float MAKEUP_MID    = 1.9276f;   //  +5.7 dB
    static constexpr float MAKEUP_HIGH   = 3.2734f;   // +10.3 dB
    static constexpr float OUTPUT_GAIN   = 2.0f;   // +6 dB

    void reset(float sr) {
        sampleRate = sr;
        xover.init(sr, XOVER_LOW, XOVER_HIGH);
        // High band 4× faster than low — see the band table above the struct.
        bandLow.init (sr, 0.0028f, 0.040f, THRESH_DB, DOWN_RATIO,    MAKEUP_LOW);
        bandMid.init (sr, 0.0014f, 0.028f, THRESH_DB, DOWN_RATIO,    MAKEUP_MID);
        bandHigh.init(sr, 0.0007f, 0.015f, THRESH_DB, DOWN_RATIO_HI, MAKEUP_HIGH);
        warmupRemaining = 0;
        silenceCounter  = 0;
    }

    void setDepth(float d) {
        bool wasEnabled = enabled;
        depth   = d;
        enabled = (d > 0.f);
        // Reset DSP state only on the disabled→enabled transition.
        // Resetting on every depth change (e.g. key-repeat while sweeping depth) would
        // prevent the compressors from ever building up gain, making OTT inaudible.
        if (!wasEnabled && enabled) {
            xover.init(sampleRate, XOVER_LOW, XOVER_HIGH);
            bandLow.reset();
            bandMid.reset();
            bandHigh.reset();
            warmupRemaining = WARMUP_SAMPLES;
        }
    }

    // Called by RenderController before offline render. Resets all DSP state and
    // enables warmup — the LR4 filters start from zero state so their output is
    // near-zero for the first ~500 samples; warmup hides this as a dry→wet fade
    // over 11.6ms rather than a pop at the start of the export.
    void resetForRender(float d) {
        depth   = d;
        enabled = (d > 0.f);
        if (enabled) {
            xover.init(sampleRate, XOVER_LOW, XOVER_HIGH);
            bandLow.reset();
            bandMid.reset();
            bandHigh.reset();
            warmupRemaining = WARMUP_SAMPLES;
            silenceCounter  = 0;
        }
    }

    void process(float* buf, int numFrames, int channelCount) {
        // Auto-reset: if signal arrives after SILENCE_RESET_FRAMES of silence,
        // reset DSP and start warmup to hide the LR4 zero-state filter transient.
        // Scan BOTH channels: an L-only check counts a hard-right-panned passage as
        // silence, and the next left-channel signal would trigger a reset+warmup dip.
        bool hasSignal = false;
        for (int i = 0; i < numFrames && !hasSignal; i++) {
            if (fabsf(buf[i * channelCount])     > 1e-4f ||
                fabsf(buf[i * channelCount + 1]) > 1e-4f) hasSignal = true;
        }
        if (!hasSignal) {
            if (silenceCounter < SILENCE_RESET_FRAMES) silenceCounter += numFrames;
        } else {
            if (silenceCounter >= SILENCE_RESET_FRAMES) {
                xover.init(sampleRate, XOVER_LOW, XOVER_HIGH);
                bandLow.reset(); bandMid.reset(); bandHigh.reset();
                warmupRemaining = WARMUP_SAMPLES;
            }
            silenceCounter = 0;
        }

        for (int i = 0; i < numFrames; i++) {
            float dryL = buf[i * channelCount];
            float dryR = buf[i * channelCount + 1];

            float lowL, lowR, midL, midR, highL, highR;
            xover.split(dryL, dryR, lowL, lowR, midL, midR, highL, highR);

            bandLow.process(lowL, lowR);
            bandMid.process(midL, midR);
            bandHigh.process(highL, highR);

            // Low + Mid + High reconstructs the original (flat LR4 response).
            float wetL = (lowL + midL + highL) * OUTPUT_GAIN;
            float wetR = (lowR + midR + highR) * OUTPUT_GAIN;

            // Linear wet/dry crossfade with warmup fade
            float pos = depth;
            if (warmupRemaining > 0) {
                pos *= (float)(WARMUP_SAMPLES - warmupRemaining) / WARMUP_SAMPLES;
                warmupRemaining--;
            }
            float dry = 1.0f - pos;
            buf[i * channelCount]     = dryL * dry + wetL * pos;
            buf[i * channelCount + 1] = dryR * dry + wetR * pos;
        }
    }
};
