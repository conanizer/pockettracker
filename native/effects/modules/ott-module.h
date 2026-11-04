#pragma once
#include <algorithm>
#include "../primitives/lr-crossover.h"
#include "../primitives/daisysp/dsp.h"

// One band of OTT: downward then upward compression, stereo-linked on the louder channel's RMS, then
// makeup. Gains are in dB; the attack/release smoothing runs on the gain, not on the level.
// ⚠️ The upward half is keyed on the downward half's OUTPUT. Keyed on the band input, it swings
// between notes and throws its boost onto the next note's front.
struct BandCompressor {
    static constexpr float THRESH_DB = -30.f;
    static constexpr float KNEE_DB   = 6.f;
    static constexpr float UP_SLOPE  = 0.75f;   // 4:1 below the threshold
    // Signal further under the threshold than this is left alone: no lifting of fades and noise floors.
    static constexpr float UP_RANGE_DB = 30.f;

    float slopeDown = 0.985f;
    float makeupLin = 1.f;
    float aAtk = 0.f, aRel = 0.f, aRms = 0.f;
    float msDown[2] = {}, msUp[2] = {};            // running mean square, L / R
    float downAtk = 0, downRel = 0, upAtk = 0, upRel = 0;

    void init(float sr, float ratio, float makeup) {
        slopeDown = 1.f - 1.f / ratio;
        makeupLin = makeup;
        aAtk = expf(-1.f / (0.005f * sr));
        aRel = expf(-1.f / (0.050f * sr));
        aRms = expf(-1.f / (0.0025f * sr));
        reset();
    }

    // The detectors start AT the threshold: from zero, the first note would meet a full upward boost.
    void reset() {
        const float t = daisysp::pow10f(THRESH_DB / 10.f);
        msDown[0] = msDown[1] = msUp[0] = msUp[1] = t;
        downAtk = downRel = upAtk = upRel = 0.f;
    }

    static float downGr(float lvl, float slope) {
        const float o = lvl - THRESH_DB;
        if (o <= -KNEE_DB / 2) return 0.f;
        if (o < KNEE_DB / 2) { const float t = o + KNEE_DB / 2; return 0.5f * slope * t * t / KNEE_DB; }
        return slope * o;
    }
    static float upGr(float lvl) {
        const float u = THRESH_DB - lvl;
        if (u <= 0.f || u > UP_RANGE_DB) return 0.f;
        if (u < KNEE_DB / 2) { const float t = u + KNEE_DB / 2; return -0.5f * UP_SLOPE * t * t / KNEE_DB; }
        return -UP_SLOPE * u;
    }
    // Release follows the target down; a rising target is taken at once, then the attack smooths it.
    inline float smooth(float target, float& atk, float& rel) const {
        const float r = aRel * rel + (1.f - aRel) * target;
        rel = target > r ? target : r;
        atk = aAtk * atk + (1.f - aAtk) * rel;
        return std::clamp(atk, -24.f, 60.f);
    }
    // 10·log10 of the mean square = the RMS in dB.
    static inline float levelDb(float a, float b) {
        return 10.f * daisysp::fastlog10f(daisysp::fmax(a, b) + 1e-20f);
    }

    inline void process(float& L, float& R) {
        msDown[0] = aRms * msDown[0] + (1.f - aRms) * L * L;
        msDown[1] = aRms * msDown[1] + (1.f - aRms) * R * R;
        float g = daisysp::pow10f(-0.05f * smooth(downGr(levelDb(msDown[0], msDown[1]), slopeDown), downAtk, downRel));
        L *= g; R *= g;
        msUp[0] = aRms * msUp[0] + (1.f - aRms) * L * L;
        msUp[1] = aRms * msUp[1] + (1.f - aRms) * R * R;
        g = daisysp::pow10f(-0.05f * smooth(upGr(levelDb(msUp[0], msUp[1])), upAtk, upRel)) * makeupLin;
        L *= g; R *= g;
    }
};

// OTT on the master bus: three bands (crossover 88.3 Hz / 2.5 kHz), each compressed both ways, summed,
// then a wet/dry mix by depth (0 = bypass).
//
//   band   thresh   above      below   band gain
//   low    -30 dB   66.7:1     4:1     +10.3 dB
//   mid    -30 dB   66.7:1     4:1      +5.7 dB
//   high   -30 dB   limiter    4:1     +10.3 dB
//   every band: RMS over 2.5 ms, 6 dB knee, attack 5 ms, release 50 ms
//
// The outer bands' extra makeup is the scooped tone — a tilt, not a level. The wet path is driven
// +5.2 dB into the bands. The dry is the raw input, not the summed bands: the crossover's phase
// against it thins the kick's body at middle depths, which is part of how the hits cut through.
// The +4 dB at full depth is applied after the mix, scaled by depth, so FF keeps the dry's loudness;
// inside the wet path it would double the wet's weight at every middle depth.
// The wet path fades in over WARMUP_SAMPLES when enabled, before a render, and when signal returns
// after SILENCE_RESET_FRAMES (the DSP restarts from zero then).
// ⚠️ Silence does not cover a restart: a START soon after STOP meets a tail the upward halves lifted
// by up to +22 dB, so the engine calls restart() for every take after a stop.
struct OttModule {
    LRCrossover    xover;
    BandCompressor bandLow, bandMid, bandHigh;

    float depth      = 0.0f;
    float postGain   = 1.0f;   // follows depth
    float sampleRate = 44100.f;
    bool  enabled    = false;

    // Long enough for the 88.3 Hz crossover to settle from zero (~−39 dB).
    int   warmupRemaining = 0;
    static constexpr int   WARMUP_SAMPLES      = 512;
    int   silenceCounter  = 0;
    static constexpr int   SILENCE_RESET_FRAMES = 22050;   // 500 ms @ 44100 Hz

    static constexpr float XOVER_LOW     = 88.3f;
    static constexpr float XOVER_HIGH    = 2500.f;
    static constexpr float DOWN_RATIO    = 66.7f;
    static constexpr float DOWN_RATIO_HI = 1e9f;      // the high band's ∞:1
    static constexpr float MAKEUP_LOW    = 3.2734f;   // +10.3 dB
    static constexpr float MAKEUP_MID    = 1.9276f;   //  +5.7 dB
    static constexpr float MAKEUP_HIGH   = 3.2734f;   // +10.3 dB
    static constexpr float DRIVE         = 1.8197f;   // +5.2 dB
    static constexpr float POST_DB_FULL  = 4.f;

    void reset(float sr) {
        sampleRate = sr;
        xover.init(sr, XOVER_LOW, XOVER_HIGH);
        bandLow.init (sr, DOWN_RATIO,    MAKEUP_LOW);
        bandMid.init (sr, DOWN_RATIO,    MAKEUP_MID);
        bandHigh.init(sr, DOWN_RATIO_HI, MAKEUP_HIGH);
        warmupRemaining = 0;
        silenceCounter  = 0;
    }

    void setDepth(float d) {
        bool wasEnabled = enabled;
        depth   = d;
        postGain = daisysp::pow10f(POST_DB_FULL / 20.f * d);
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
        postGain = daisysp::pow10f(POST_DB_FULL / 20.f * d);
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

            lowL *= DRIVE; lowR *= DRIVE; midL *= DRIVE; midR *= DRIVE; highL *= DRIVE; highR *= DRIVE;
            bandLow.process(lowL, lowR);
            bandMid.process(midL, midR);
            bandHigh.process(highL, highR);

            float wetL = lowL + midL + highL;
            float wetR = lowR + midR + highR;

            float pos = depth;
            float post = postGain;
            if (warmupRemaining > 0) {
                pos *= (float)(WARMUP_SAMPLES - warmupRemaining) / WARMUP_SAMPLES;
                post = daisysp::pow10f(POST_DB_FULL / 20.f * pos);
                warmupRemaining--;
            }
            float dry = 1.0f - pos;
            buf[i * channelCount]     = (dryL * dry + wetL * pos) * post;
            buf[i * channelCount + 1] = (dryR * dry + wetR * pos) * post;
        }
    }
};
