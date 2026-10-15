#pragma once
#include "../primitives/daisysp/reverbsc.h"
#include "../primitives/dragonfly/dragonfly-reverb.h"
#include "../primitives/bus-sleep.h"
#include "eq-module.h"
#include "reverb-presets.h"
#include <cmath>
#include <cstring>

// The pre-delay line per channel: 0.15 s at 48 kHz.
// ⚠️ A sample count, so at higher rates PRE clamps silently (75 ms at 96 kHz).
static constexpr size_t REVERB_PREDELAY_MAX_SAMPLES = 7200;
static constexpr float  kReverbPreDelayMaxSeconds   = 0.15f;

// The reverb send: input EQ, pre-delay, the algorithm, then WIDE — the same around every algorithm.
// Each cell's mapping is a function in reverb-presets.h, shared with the UI.
struct ReverbModule {
    daisysp::ReverbSc reverb;
    DragonflyReverb   dragonfly;   // algorithms 1..4; allocates nothing until one of them sounds
    EqModule          inputEq;
    float             sampleRate = 44100.0f; // the rate the delay lines were actually built at

    // ⚠️ ReverbSc refuses a rate its fixed array cannot hold and leaves pointers dangling, so `Init`'s
    // result must be checked; above the ceiling it is built at the ceiling.
    static constexpr float MAX_SUPPORTED_RATE = DSY_REVERBSC_MAX_RATE;

    // ⚠️ PRE and WIDE are skipped at their defaults, not computed as an identity, so an older project
    // is bit-exact. MOD's default is a real setting. PRE is kept as the cell: its frame count
    // depends on the rate.
    int preHex          = 0x00;     // 00 = the send goes straight in
    int preDelaySamples = 0;        // derived from preHex and the rate — never written directly
    int widthHex        = 0x80;     // 0x80 = untouched; 00 = mono, FF = twice the sides
    int modHex          = 0x10;     // 00 = none; 0x40 is the wander the algorithm is built around

    // The pre-delay ring, one write head for both. Cleared on reset so a load does not replay the last
    // project into the tail.
    float preBufL[REVERB_PREDELAY_MAX_SAMPLES] = {};
    float preBufR[REVERB_PREDELAY_MAX_SAMPLES] = {};
    int   preWrite = 0;

    // Ramped across the block: DCAY and SIZE move it ~20 dB while the tail rings.
    float wetGain       = 1.0f;   // where the last block left it
    float wetGainTarget = 1.0f;   // what DCAY and SIZE last asked of algorithm 0

    // The algorithm asked for, and the one the last block ran. They differ for one block after a
    // switch, and that block empties the incoming algorithm's stale tail.
    int algo         = 0;
    int soundingAlgo = 0;
    // The cells, kept so algorithm 0 can be rebuilt when it comes back.
    int decayHex = 0x60, dampHex = 0x80, sizeHex = 0x60;

    // Algorithms 1..4 take a block at a time, so the send is pre-delayed into these first.
    static constexpr int kDragonflyBlock = 256;
    float dfInL[kDragonflyBlock], dfInR[kDragonflyBlock];

    void reset(float sr) {
        sampleRate = sr;
        sleep.reset();
        if (reverb.Init(sr) != 0) {
            sampleRate = MAX_SUPPORTED_RATE;
            reverb.Init(MAX_SUPPORTED_RATE);
        }
        dragonfly.requestClear();
        setParams(0x60, 0x80, 0x60);   // the defaults, until the project pushes its own
        wetGain = wetGainTarget;
        // ⚠️ `Init` resets the wander to 1, and the pre-delay's frame count depends on the rate.
        reverb.SetPitchMod(reverb_mod_scale(modHex));
        updatePreDelaySamples();
        inputEq.reset(sr);
        clearPreDelay();
    }

    // DCAY, DAMP and SIZE. The feedback takes DCAY and SIZE together, so SIZE leaves the length alone.
    // Re-pushing the same SIZE moves nothing: `SetRoom` sets glide targets.
    void setParams(int decayHexIn, int dampHexIn, int sizeHexIn = 0x60) {
        decayHex = decayHexIn;
        dampHex  = dampHexIn;
        sizeHex  = sizeHexIn;
        reverb.SetRoom(reverb_room_scale(sizeHex), reverb_mod_rate(sizeHex));
        reverb.SetFeedback(reverb_decay_feedback(decayHex, sizeHex));
        reverb.SetLpFreq(reverb_damp_freq(dampHex));
        wetGainTarget = reverb_decay_gain(decayHex, sizeHex);
        dragonfly.setCells(decayHex, sizeHex, dampHex, modHex);
    }

    // ALGO, 0..kReverbAlgoCount-1. Takes effect at the next block.
    void setAlgo(int a) { algo = (a >= 0 && a < kReverbAlgoCount) ? a : 0; }

    // PRE, WIDE and MOD. PRE is not ramped: moving it under a loud send steps the reverb's input.
    void setCharacter(int preHexIn, int widthHexIn, int modHexIn) {
        const bool wasOff = (preDelaySamples == 0);
        preHex            = preHexIn;
        updatePreDelaySamples();
        // ⚠️ The ring is written only while PRE is up; clear it when PRE comes back on, or it replays
        // old audio. Only on that transition — every project edit re-pushes this.
        if (wasOff && preDelaySamples > 0) clearPreDelay();
        widthHex = widthHexIn;
        modHex   = modHexIn;
        reverb.SetPitchMod(reverb_mod_scale(modHex));
        dragonfly.setCells(decayHex, sizeHex, dampHex, modHex);
    }

    // Stereo send in, 100 % wet out. Sleeps after a second of silence in and out.
    void process(const float* inL, const float* inR, float* outL, float* outR, int numFrames) {
        if (sleep.skip(inL, inR, numFrames)) {
            std::memset(outL, 0, sizeof(float) * static_cast<size_t>(numFrames));
            std::memset(outR, 0, sizeof(float) * static_cast<size_t>(numFrames));
            return;
        }
        run(inL, inR, outL, outR, numFrames);
        sleep.observe(inL, inR, outL, outR, numFrames, static_cast<int>(sampleRate));
    }

  private:
    BusSleep sleep;

    void run(const float* inL, const float* inR, float* outL, float* outR, int numFrames) {
        const int  want     = algo;
        const bool switched = want != soundingAlgo;
        if (switched && want == kReverbAlgoOld) {
            // ⚠️ Rebuilt rather than resumed: it went silent mid-tail and would replay it.
            reverb.Init(sampleRate);
            setParams(decayHex, dampHex, sizeHex);
            reverb.SetPitchMod(reverb_mod_scale(modHex));
        }
        soundingAlgo = want;
        if (want != kReverbAlgoOld) {
            processDragonfly(want, switched, inL, inR, outL, outR, numFrames);
            return;
        }

        const Shape shape    = blockShape();
        const float gainStep = numFrames > 0 ? (wetGainTarget - wetGain) / numFrames : 0.0f;
        float       gain     = wetGain;
        int         preRead  = preReadStart();

        for (int i = 0; i < numFrames; i++) {
            float l = inL[i], r = inR[i];
            feed(l, r, preRead, shape);
            float wl, wr;
            reverb.Process(l, r, &wl, &wr);
            gain += gainStep;
            finish(wl, wr, gain, shape);
            outL[i] = wl;
            outR[i] = wr;
        }
        wetGain = wetGainTarget;
    }

    // Algorithms 1..4, a block at a time. No DCAY wet gain — it is fitted to algorithm 0; each engine
    // has its own trim. The ramp still glides to 1 after a switch.
    void processDragonfly(int want, bool switched, const float* inL, const float* inR, float* outL,
                          float* outR, int numFrames) {
        const Shape shape    = blockShape();
        const float gainStep = numFrames > 0 ? (1.0f - wetGain) / numFrames : 0.0f;
        float       gain     = wetGain;
        int         preRead  = preReadStart();

        for (int done = 0; done < numFrames;) {
            const int n = numFrames - done < kDragonflyBlock ? numFrames - done : kDragonflyBlock;
            for (int i = 0; i < n; i++) {
                dfInL[i] = inL[done + i];
                dfInR[i] = inR[done + i];
                feed(dfInL[i], dfInR[i], preRead, shape);
            }
            dragonfly.process(want, switched && done == 0, dfInL, dfInR, outL + done, outR + done, n);
            for (int i = 0; i < n; i++) {
                gain += gainStep;
                finish(outL[done + i], outR[done + i], gain, shape);
            }
            done += n;
        }
        wetGain = 1.0f;
    }

    // What every algorithm shares around it: INP EQ and PRE before, WIDE and the wet gain after.
    struct Shape {
        bool  delayed, widening;
        float side;
    };
    Shape blockShape() const { return {preDelaySamples > 0, widthHex != 0x80, widthHex / 128.0f}; }

    // Heads walked by hand: the ring is not a power of two, and `%` would divide per sample.
    int preReadStart() const {
        const int preRead = preWrite - preDelaySamples;
        return preRead < 0 ? preRead + static_cast<int>(REVERB_PREDELAY_MAX_SAMPLES) : preRead;
    }

    void feed(float& l, float& r, int& preRead, const Shape& shape) {
        constexpr int ring = static_cast<int>(REVERB_PREDELAY_MAX_SAMPLES);
        if (inputEq.active) inputEq.processStereo(l, r);
        if (shape.delayed) {
            preBufL[preWrite] = l;
            preBufR[preWrite] = r;
            l = preBufL[preRead];
            r = preBufR[preRead];
            if (++preWrite >= ring) preWrite = 0;
            if (++preRead >= ring) preRead = 0;
        }
    }

    static void finish(float& wl, float& wr, float gain, const Shape& shape) {
        if (shape.widening) {
            // Only the sides scale, so mono is not also quieter.
            const float mid = (wl + wr) * 0.5f;
            const float sd  = (wl - wr) * 0.5f * shape.side;
            wl = mid + sd;
            wr = mid - sd;
        }
        wl *= gain;
        wr *= gain;
    }

    // The one writer of `preDelaySamples`: the cell and the rate both move it. Clamps silently.
    void updatePreDelaySamples() {
        const float wanted = (preHex / 255.0f) * kReverbPreDelayMaxSeconds * sampleRate;
        preDelaySamples    = static_cast<int>(
            fminf(wanted, static_cast<float>(REVERB_PREDELAY_MAX_SAMPLES - 1)));
    }

    void clearPreDelay() {
        for (size_t i = 0; i < REVERB_PREDELAY_MAX_SAMPLES; i++) preBufL[i] = preBufR[i] = 0.0f;
        preWrite = 0;
    }
};
