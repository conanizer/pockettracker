#pragma once
#include "modules/limiter-module.h"
#include "modules/ott-module.h"
#include "modules/dust-chain.h"
#include "modules/eq-module.h"

// The master bus: EQ → OTT or DUST (`masterFx` 0 / 1, skipped at depth 0) → limiter.
// The effect heard crossfades with the dry whenever it comes in, goes out or changes, and comes in
// fresh: DUST from a reset, OTT from a restart.
struct MasterChain {
    OttModule         ott;
    skdust::DustChain dust;
    LimiterModule     limiter;
    EqModule          masterEq;

    int   masterFx  = 0;
    float dustDepth = 0.f;

    // No default rate: every module bakes it in.
    void reset(float sampleRate) {
        ott.reset(sampleRate);
        dust.prepare(sampleRate, 512, 2);
        dust.reset();
        limiter.reset();
        masterEq.reset(sampleRate);
        heard = Path::none;
        share.set(0.f, false);
        primeLeft = 0;
    }

    // ⚠️ Idempotent: it is re-sent on every mixer gesture. A switch is taken up by `process`.
    void setMasterFx(int fx) { masterFx = fx; }

    void setDustDepth(float d) {
        dustDepth = d;
        dust.setDustAmount(d);
    }

    // An export starts on the effect already in, with no fade.
    void setDustDepthForRender(float d) {
        dustDepth = d;
        dust.reset();
        dust.setDustAmount(d);
        snapPending = true;
    }
    void setOttDepthForRender(float d) {
        ott.resetForRender(d);
        snapPending = true;
    }

    void setLimiterPreGain(float g) { limiter.setPreGain(g); }

    // Interleaved stereo, in place.
    void process(float* buf, int numFrames, int channelCount) {
        masterEq.fresh = false;   // the master always sounds
        if (masterEq.active()) {
            for (int i = 0; i < numFrames; i++) {
                float L = buf[i * channelCount];
                float R = buf[i * channelCount + 1];
                masterEq.processStereo(L, R);
                buf[i * channelCount]     = L;
                buf[i * channelCount + 1] = R;
            }
        }
        for (int at = 0; at < numFrames; at += CHUNK) {
            const int n = numFrames - at < CHUNK ? numFrames - at : CHUNK;
            processFx(buf + at * channelCount, n, channelCount);
        }
        limiter.process(buf, numFrames, channelCount);
    }

  private:
    static constexpr int CHUNK = 256;

    enum class Path { none, ott, dust };

    Path heard       = Path::none;
    Ramp share;                // the heard path against the dry, 0..1
    int  primeLeft   = 0;      // DUST's delay line fills unheard before it fades in
    bool snapPending = false;

    Path wanted() const {
        if (masterFx == 0) return ott.audible() ? Path::ott : Path::none;
        return dustDepth > 0.f ? Path::dust : Path::none;
    }

    // Once a chunk: a path going out fades to the dry first, then the next one comes in.
    void follow() {
        const Path want = wanted();
        if (snapPending) {
            snapPending = false;
            heard = want;
            share.set(want == Path::none ? 0.f : 1.f, false);
            primeLeft = 0;
            return;
        }
        if (want == heard) {
            if (heard != Path::none && share.goal < 1.f) share.start(share.value(), 1.f, MASTER_FX_SAMPLES);
            return;
        }
        if (heard != Path::none && share.value() > 0.f) {
            if (share.goal > 0.f) share.start(share.value(), 0.f, MASTER_FX_SAMPLES);
            return;
        }
        heard = want;
        if (heard == Path::dust) {
            dust.reset();
            dust.setDustAmount(dustDepth);
            primeLeft = skdust::DustChain::latencySamples();
        } else if (heard == Path::ott) {
            ott.restart();
        }
        share.start(0.f, 1.f, MASTER_FX_SAMPLES);
    }

    void run(float* buf, int n, int channelCount) {
        if (heard == Path::ott) ott.process(buf, n, channelCount);
        else              dust.process(buf, n, channelCount);
    }

    void processFx(float* buf, int n, int channelCount) {
        follow();
        if (heard == Path::none) return;
        if (!share.moving() && share.goal >= 1.f && primeLeft == 0) {
            run(buf, n, channelCount);
            return;
        }
        float dry[CHUNK * 2];
        for (int i = 0; i < n; i++) {
            dry[i * 2]     = buf[i * channelCount];
            dry[i * 2 + 1] = buf[i * channelCount + 1];
        }
        run(buf, n, channelCount);
        for (int i = 0; i < n; i++) {
            float w = 0.f;
            if (primeLeft > 0) --primeLeft;
            else               w = share.next();
            float& L = buf[i * channelCount];
            float& R = buf[i * channelCount + 1];
            L = dry[i * 2]     + (L - dry[i * 2])     * w;
            R = dry[i * 2 + 1] + (R - dry[i * 2 + 1]) * w;
        }
    }
};
