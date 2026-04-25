#pragma once
#include "../primitives/lr-crossover.h"
#include "../primitives/upward-compressor.h"
#include "../primitives/daisysp/compressor.h"

// ===========================================================================
// BandCompressor — linked stereo bidirectional compressor for one band.
// Key signal = max(|L|, |R|) so both channels get identical gain.
//
// Downward (DaisySP): attenuates loud peaks above threshold (-27 dBFS).
//   AutoMakeup OFF, SetMakeup(0) — no unconditional boost inside the compressor.
//   DaisySP starts with gain_rec_=0.1 after Init(); any non-zero makeup causes
//   a first-note +makeup-dB pop before compression settles. Keep makeup at 0.
//
// Upward (custom): boosts signals below -35 dBFS toward the threshold.
//   Separate lower threshold (vitOTT-matched) creates a ~8 dB neutral zone
//   between -35 and -27 dBFS where neither compressor fires. This prevents
//   the upward expander from lifting note tails and inter-note noise floor.
//
// Band-specific time constants (vitOTT-matched, ms attack / ms release):
//   Low:  2.8 / 40   Mid:  1.4 / 28   High: 0.7 / 15
//   High band responds 4× faster than low → transient sparkle, no bass pump.
// ===========================================================================
struct BandCompressor {
    daisysp::Compressor downward;
    UpwardCompressor    upward;
    float sampleRate = 44100.f;
    float attackSec  = 0.001f;
    float releaseSec = 0.030f;

    void applySettings() {
        downward.SetRatio(8.f);
        // vitOTT upper threshold average: Low -28 / Mid -25 / High -30 → ~-27 dBFS
        downward.SetThreshold(-27.f);
        downward.SetAttack(attackSec);
        downward.SetRelease(releaseSec);
        downward.AutoMakeup(false);
        // Makeup stays at 0 — avoids first-note pop from DaisySP's initial gain_rec_=0.1.
        downward.SetMakeup(0.f);
        // vitOTT lower threshold average: Low -35 / Mid -36 / High -35 → ~-35 dBFS.
        // ~8 dB gap above downward threshold prevents upward from lifting note tails.
        upward.setParams(-35.f, 4.f, attackSec, releaseSec, sampleRate);
    }

    void init(float sr, float atk, float rel) {
        sampleRate = sr;
        attackSec  = atk;
        releaseSec = rel;
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
        float key = fmaxf(fabsf(L), fabsf(R));
        downward.Process(key);
        L = downward.Apply(L);
        R = downward.Apply(R);
        upward.Process(key);
        L = upward.Apply(L);
        R = upward.Apply(R);
    }
};

// ===========================================================================
// OttModule — 3-band bidirectional compressor for the master bus.
//
// Signal flow:
//   input → [LRCrossover @120 Hz / 2500 Hz]
//        → 3× BandCompressor (downward + upward per band, band-specific τ)
//        → sum × OUTPUT_GAIN → linear wet/dry mix → output
//
// One control: depth (0=bypass, 1=full OTT).
// enabled flag gates the DSP so MasterChain can skip it at depth=0.
//
// OUTPUT_GAIN (+6 dB) is applied to the wet sum after compression, not inside
// DaisySP SetMakeup. This avoids the DaisySP first-note pop (gain_rec_=0.1 at
// Init causes +makeup boost before compression settles). The warmup crossfade
// masks the OUTPUT_GAIN onset ramp.
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

    // 512 samples (~11.6ms): LR4 pole at 120Hz has τ≈83 samples; 6τ=498 ≈ -72 dBFS.
    int   warmupRemaining = 0;
    static constexpr int   WARMUP_SAMPLES      = 512;
    // Auto-reset after 500 ms of silence so each START-after-stop gets a warmup.
    int   silenceCounter  = 0;
    static constexpr int   SILENCE_RESET_FRAMES = 22050;   // 500 ms @ 44100 Hz
    // +6 dB post-band output gain (vitOTT-style): compensates for SetMakeup(0).
    static constexpr float OUTPUT_GAIN          = 2.0f;

    void reset(float sr) {
        sampleRate = sr;
        xover.init(sr);
        // vitOTT-matched band time constants: high band 4× faster than low.
        bandLow.init(sr,  0.0028f, 0.040f);
        bandMid.init(sr,  0.0014f, 0.028f);
        bandHigh.init(sr, 0.0007f, 0.015f);
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
            xover.init(sampleRate);
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
            xover.init(sampleRate);
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
        bool hasSignal = false;
        for (int i = 0; i < numFrames && !hasSignal; i++) {
            if (fabsf(buf[i * channelCount]) > 1e-4f) hasSignal = true;
        }
        if (!hasSignal) {
            if (silenceCounter < SILENCE_RESET_FRAMES) silenceCounter += numFrames;
        } else {
            if (silenceCounter >= SILENCE_RESET_FRAMES) {
                xover.init(sampleRate);
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
            // OUTPUT_GAIN (+6 dB) applied to wet sum; onset boost is masked by warmup.
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
