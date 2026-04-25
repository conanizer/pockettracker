#pragma once
#include "../primitives/lr-crossover.h"
#include "../primitives/upward-compressor.h"
#include "../primitives/daisysp/compressor.h"

// ===========================================================================
// BandCompressor — linked stereo bidirectional compressor for one band.
// Key signal = max(|L|, |R|) so both channels get identical gain.
//
// Downward (DaisySP): attenuates loud peaks above threshold.
//   AutoMakeup OFF — the DaisySP formula adds ~7.9 dB flat to all signals,
//   masking compression character. SetMakeup(+6 dB) instead: unconditional
//   +6 dB on every sample. With 8:1 ratio at -24 dBFS threshold, the makeup
//   overcomes compression only above −16 dBFS, so loud peaks get NET reduction
//   while moderate signals get NET boost → wide-range squash.
//
// Upward (custom): boosts quiet signals below threshold toward the threshold.
//   Threshold aligned to downward to eliminate the "dead zone" where neither
//   compressor fires.
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
        downward.SetThreshold(-24.f);
        downward.SetAttack(attackSec);
        downward.SetRelease(releaseSec);
        downward.AutoMakeup(false);
        // +6 dB unconditional makeup per band — OTT character loudness.
        // Net effect at -24 dBFS threshold with 8:1 ratio:
        //   signal at -24 dBFS → +6 dB net → -18 dBFS
        //   signal at -12 dBFS → 10.5 dB compression, 6 dB makeup → -4.5 dB net → -16.5 dBFS
        //   18 dB input range (-24 to -6 dBFS) compressed to 2.3 dB output range
        downward.SetMakeup(6.f);
        // Aligned threshold — both compressors active at every level, no dead zone.
        upward.setParams(-24.f, 4.f, attackSec, releaseSec, sampleRate);
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
//        → sum → linear wet/dry mix → output
//
// One control: depth (0=bypass, 1=full OTT).
// enabled flag gates the DSP so MasterChain can skip it at depth=0.
//
// Startup fade: on the disabled→enabled transition (and on resetForRender),
// filter/compressor states are reset and the wet signal fades in over
// WARMUP_SAMPLES to hide the LR4 zero-state startup transient. Depth changes
// while enabled do NOT reset state, so compressors can maintain gain across
// key-repeat sweeps.
// ===========================================================================
struct OttModule {
    LRCrossover    xover;
    BandCompressor bandLow, bandMid, bandHigh;

    float depth      = 0.0f;
    float sampleRate = 44100.f;
    bool  enabled    = false;

    // Wet fades from 0→depth over WARMUP_SAMPLES on first enable and on render reset.
    // 512 samples (~11.6ms) is enough: LR4 pole at 120Hz has τ≈83 samples; 6τ=498 ≈ -72 dBFS.
    int   warmupRemaining = 0;
    static constexpr int WARMUP_SAMPLES = 512;

    void reset(float sr) {
        sampleRate = sr;
        xover.init(sr);
        // vitOTT-matched band time constants: high band 4× faster than low.
        bandLow.init(sr,  0.0028f, 0.040f);
        bandMid.init(sr,  0.0014f, 0.028f);
        bandHigh.init(sr, 0.0007f, 0.015f);
        warmupRemaining = 0;
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
        }
    }

    void process(float* buf, int numFrames, int channelCount) {
        for (int i = 0; i < numFrames; i++) {
            float dryL = buf[i * channelCount];
            float dryR = buf[i * channelCount + 1];

            float lowL, lowR, midL, midR, highL, highR;
            xover.split(dryL, dryR, lowL, lowR, midL, midR, highL, highR);

            bandLow.process(lowL, lowR);
            bandMid.process(midL, midR);
            bandHigh.process(highL, highR);

            // Low + Mid + High = original (flat LR4 response). Band makeup gains
            // add the OTT character loudness (+6 dB per band, applied inside process()).
            float wetL = lowL + midL + highL;
            float wetR = lowR + midR + highR;

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
