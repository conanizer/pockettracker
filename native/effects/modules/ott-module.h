#pragma once
#include "../primitives/lr-crossover.h"
#include "../primitives/upward-compressor.h"
#include "../primitives/daisysp/compressor.h"

// One band of OTT: downward then upward compression, stereo-linked on max(|L|, |R|), then makeup.
// ⚠️ The upward half is keyed on the downward half's OUTPUT. Keyed on the band input, it swings
// between notes and throws its boost onto the next note's front.
// ⚠️ DaisySP's own makeup stays 0 — it pops on the first note; `makeupLin` does the job.
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

    // daisysp::fmax, not fmaxf: on gcc without -ffast-math fmaxf is a libm call, six per frame here.
    inline void process(float& L, float& R) {
        downward.Process(daisysp::fmax(fabsf(L), fabsf(R)));
        L = downward.Apply(L);
        R = downward.Apply(R);
        upward.Process(daisysp::fmax(fabsf(L), fabsf(R)));
        L = upward.Apply(L) * makeupLin;
        R = upward.Apply(R) * makeupLin;
    }
};

// OTT on the master bus: three bands (crossover 88.3 Hz / 2.5 kHz), each compressed both ways, summed,
// then a wet/dry mix by depth (0 = bypass).
//
//   band   thresh   above      below   band gain   attack / release
//   low    -30 dB   66.7:1     4:1     +10.3 dB    2.8 ms / 40 ms
//   mid    -30 dB   66.7:1     4:1      +5.7 dB    1.4 ms / 28 ms
//   high   -30 dB   limiter    4:1     +10.3 dB    0.7 ms / 15 ms
//
// The outer bands' extra makeup is the scooped tone — a tilt, not a level. The common preset's
// +5.2 dB input drive is left out: this bus already sits far above the threshold.
// The wet path fades in over WARMUP_SAMPLES when enabled, before a render, and when signal returns
// after SILENCE_RESET_FRAMES (the DSP restarts from zero then).
// ⚠️ Silence does not cover a restart: a START soon after STOP meets a tail the upward halves lifted
// by up to +22 dB, so the engine calls restart() for every take after a stop.
struct OttModule {
    LRCrossover    xover;
    BandCompressor bandLow, bandMid, bandHigh;

    float depth      = 0.0f;
    float sampleRate = 44100.f;
    bool  enabled    = false;

    // Long enough for the 88.3 Hz crossover to settle from zero (~−39 dB).
    int   warmupRemaining = 0;
    static constexpr int   WARMUP_SAMPLES      = 512;
    int   silenceCounter  = 0;
    static constexpr int   SILENCE_RESET_FRAMES = 22050;   // 500 ms @ 44100 Hz

    static constexpr float XOVER_LOW     = 88.3f;
    static constexpr float XOVER_HIGH    = 2500.f;
    static constexpr float THRESH_DB     = -30.f;
    static constexpr float DOWN_RATIO    = 66.7f;
    // The high band's ∞:1 — DaisySP needs a finite ratio.
    static constexpr float DOWN_RATIO_HI = 1000.f;
    static constexpr float MAKEUP_LOW    = 3.2734f;   // +10.3 dB
    static constexpr float MAKEUP_MID    = 1.9276f;   //  +5.7 dB
    static constexpr float MAKEUP_HIGH   = 3.2734f;   // +10.3 dB
    static constexpr float OUTPUT_GAIN   = 2.0f;   // +6 dB

    void reset(float sr) {
        sampleRate = sr;
        xover.init(sr, XOVER_LOW, XOVER_HIGH);
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
        // Only on switching on: a reset per depth change would keep the compressors from ever
        // building up gain during a sweep.
        if (!wasEnabled && enabled) restart();
    }

    // The DSP from zero, behind the dry→wet warmup fade.
    void restart() {
        xover.init(sampleRate, XOVER_LOW, XOVER_HIGH);
        bandLow.reset();
        bandMid.reset();
        bandHigh.reset();
        warmupRemaining = WARMUP_SAMPLES;
    }

    // Before an offline render: start from zero behind the warmup fade.
    void resetForRender(float d) {
        depth   = d;
        enabled = (d > 0.f);
        if (enabled) {
            restart();
            silenceCounter = 0;
        }
    }

    void process(float* buf, int numFrames, int channelCount) {
        // Signal after a long silence restarts the DSP. Both channels: a hard-right passage is not
        // silence.
        bool hasSignal = false;
        for (int i = 0; i < numFrames && !hasSignal; i++) {
            if (fabsf(buf[i * channelCount])     > 1e-4f ||
                fabsf(buf[i * channelCount + 1]) > 1e-4f) hasSignal = true;
        }
        if (!hasSignal) {
            if (silenceCounter < SILENCE_RESET_FRAMES) silenceCounter += numFrames;
        } else {
            if (silenceCounter >= SILENCE_RESET_FRAMES) restart();
            silenceCounter = 0;
        }

        // Idle: past the reset span, a block of exact zeros passes untouched.
        if (silenceCounter >= SILENCE_RESET_FRAMES) {
            bool zero = true;
            for (int i = 0; i < numFrames * channelCount && zero; i++) zero = buf[i] == 0.0f;
            if (zero) return;
        }

        for (int i = 0; i < numFrames; i++) {
            float dryL = buf[i * channelCount];
            float dryR = buf[i * channelCount + 1];

            float lowL, lowR, midL, midR, highL, highR;
            xover.split(dryL, dryR, lowL, lowR, midL, midR, highL, highR);

            bandLow.process(lowL, lowR);
            bandMid.process(midL, midR);
            bandHigh.process(highL, highR);

            float wetL = (lowL + midL + highL) * OUTPUT_GAIN;
            float wetR = (lowR + midR + highR) * OUTPUT_GAIN;

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
