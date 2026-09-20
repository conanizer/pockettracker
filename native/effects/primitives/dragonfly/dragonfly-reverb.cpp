// Dragonfly Reverb's four plugins as the reverb send's algorithms. See the header.
//
// The set-up calls and the signal flow inside each engine are Dragonfly's (`plugins/*/DSP.cpp`,
// Copyright (c) 2018-2019 Michael Willis, Rob van den Berg, GPL-3.0-or-later — LICENSE-dragonfly),
// and the fixed values are the plugin preset named beside each engine. What is NOT Dragonfly's is
// `apply`: which cell drives which setter, and over what range.
//
// ⚠️ PRE, WIDE and INP EQ never reach this file. They are applied around it by ReverbModule, the same
// way for every algorithm, so each engine's own pre-delay is pinned at the 0.1 ms floor the plugins
// use (freeverb mishandles zero) and its width at the preset's.

#include "dragonfly-reverb.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "../../modules/reverb-presets.h"
#include "freeverb/earlyref.hpp"
#include "freeverb/efilter.hpp"
#include "freeverb/nrev.hpp"
#include "freeverb/nrevb.hpp"
#include "freeverb/progenitor2.hpp"
#include "freeverb/strev.hpp"
#include "freeverb/zrev2.hpp"

namespace {

// Freeverb is driven in blocks no longer than this, as the plugins drive it.
constexpr int kBlock = 256;

struct Cells {
    int decay = -1, size = -1, damp = -1, mod = -1;
};

// ─── The cell → Dragonfly-unit mappings every engine shares ─────────────────────────────────────

/** DCAY → the tail's RT60 in seconds, on the SAME curve the shipping reverb uses, so a DCAY typed on
 *  one algorithm rings for about as long on another. ⚠️ FF is the freeze there and has no time; it
 *  is read as FE here, the longest finite one. */
double decay_seconds(int decayHex) { return reverb_decay_seconds(std::min(decayHex, 0xFE)); }

/** DAMP → a corner frequency on the shipping reverb's curve, clamped to the 1-16 kHz the plugins'
 *  own damping knobs span. */
double damp_hz(int dampHex) { return std::clamp(double(reverb_damp_freq(dampHex)), 1000.0, 16000.0); }

/** SIZE → metres, linear over a plugin's own Size range. */
double size_m(int sizeHex, double lo, double hi) { return lo + (hi - lo) * sizeHex / 255.0; }

// ─── Engines ─────────────────────────────────────────────────────────────────────────────────────

struct Engine {
    float rate = 0.0f;
    Cells applied;   // what `apply` last saw; -1 forces every setter
    virtual ~Engine() = default;
    virtual void setRate(double sr) = 0;
    virtual void mute() = 0;
    /** Call the setters for whichever cells differ from `applied`. May allocate. */
    virtual void apply(const Cells& c) = 0;
    /** `n` <= kBlock. The inputs may be overwritten. */
    virtual void run(float* inL, float* inR, float* outL, float* outR, long n) = 0;
};

/**
 * freeverb's early-reflection unit, with a `mute` that empties all of it. ⚠️ Its own leaves the four
 * output filters holding their last state, and the 4-50 Hz high-pass among them rings for tens of
 * milliseconds — an engine switched back to would start with a trace of the tail it stopped on.
 */
struct EarlyRef : fv3::earlyref_f {
    void mute() override {
        fv3::earlyref_f::mute();
        out1_lpf.mute();
        out2_lpf.mute();
        out1_hpf.mute();
        out2_hpf.mute();
    }
};

void early_setup(fv3::earlyref_f& e) {
    e.setMuteOnChange(false);
    e.setdryr(0);
    e.setwet(0);
    e.setwidth(0.8);
    e.setLRDelay(0.3);
    e.setLRCrossApFreq(750, 4);
    e.setDiffusionApFreq(150, 4);
}

// HALL — earlyref into zrev2. Fixed values: "Small Clear Hall".
//   SIZE 10-60 m · DCAY RT60 · DAMP high cut, and how much faster the highs die · MOD modulation %
struct HallEngine final : Engine {
    EarlyRef        early;
    fv3::zrev2_f    late;
    float           earlyOut[2][kBlock], lateIn[2][kBlock], lateOut[2][kBlock];

    HallEngine() {
        early.loadPresetReflection(FV3_EARLYREF_PRESET_1);
        early_setup(early);
        late.setMuteOnChange(false);
        late.setwet(0);
        late.setdryr(0);
        early.setwidth(1.0);
        late.setwidth(1.0);
        late.setPreDelay(0.1);
        late.setidiffusion1(90.0 / 140.0);
        late.setapfeedback(90.0 / 140.0);
        early.setoutputhpf(4.0);
        late.setoutputhpf(4.0);
        late.setxover_low(500.0);
        late.setrt60_factor_low(1.3);
        late.setxover_high(5500.0);
        late.setspin(3.3);
        late.setwander(15.0);
    }
    void setRate(double sr) override {
        early.setSampleRate(sr);
        late.setSampleRate(sr);
    }
    void mute() override {
        early.mute();
        late.mute();
    }
    void apply(const Cells& c) override {
        if (c.size != applied.size) {
            const double m = size_m(c.size, 10.0, 60.0);
            early.setRSFactor(m / 10.0);
            late.setRSFactor(m / 80.0);
        }
        if (c.decay != applied.decay) late.setrt60(decay_seconds(c.decay));
        if (c.damp != applied.damp) {
            early.setoutputlpf(damp_hz(c.damp));
            late.setoutputlpf(damp_hz(c.damp));
            late.setrt60_factor_high(0.2 + 0.6 * c.damp / 255.0);
        }
        if (c.mod != applied.mod) {
            // 00 → the plugin's own 0.1 % floor; 40 and above → 100 %.
            const double pct = std::clamp(c.mod * 100.0 / 64.0, 0.1, 100.0);
            late.setspinfactor(pct / 100.0);
            late.setlfofactor(pct / 100.0);
        }
    }
    void run(float* inL, float* inR, float* outL, float* outR, long n) override {
        early.processreplace(inL, inR, earlyOut[0], earlyOut[1], n);
        for (long i = 0; i < n; i++) {
            lateIn[0][i] = 0.2f * earlyOut[0][i] + inL[i];
            lateIn[1][i] = 0.2f * earlyOut[1][i] + inR[i];
        }
        late.processreplace(lateIn[0], lateIn[1], lateOut[0], lateOut[1], n);
        for (long i = 0; i < n; i++) {
            outL[i] = 0.1f * earlyOut[0][i] + 0.2f * lateOut[0][i];
            outR[i] = 0.1f * earlyOut[1][i] + 0.2f * lateOut[1][i];
        }
    }
};

// ROOM — input filters, earlyref into progenitor2. Fixed values: "Medium Clear Room".
//   SIZE 8-32 m · DCAY RT60 · DAMP early and late damping · MOD spin 0-5 Hz
struct RoomEngine final : Engine {
    fv3::iir_1st_f       lpf[2], hpf[2];
    EarlyRef             early;
    fv3::progenitor2_f   late;
    double               sampleRate = 48000.0;
    float                earlyOut[2][kBlock], lateIn[2][kBlock], lateOut[2][kBlock];

    RoomEngine() {
        for (auto& f : lpf) f.mute();
        for (auto& f : hpf) f.mute();
        early.loadPresetReflection(FV3_EARLYREF_PRESET_1);
        early_setup(early);
        late.setMuteOnChange(false);
        late.setwet(0);
        late.setdryr(0);
        early.setwidth(100.0 / 120.0);
        late.setwidth(1.0);
        late.setPreDelay(0.1);
        late.setidiffusion1(70.0 / 120.0);
        late.setodiffusion1(70.0 / 120.0);
        late.setwander(40.0 / 200.0 + 0.1);
        late.setwander2(40.0 / 200.0 + 0.1);
        late.setdamp2(600.0);
    }
    void setRate(double sr) override {
        sampleRate = sr;
        early.setSampleRate(sr);
        late.setSampleRate(sr);
        for (auto& f : lpf) f.setLPF_BW(16000.0, sr);
        for (auto& f : hpf) f.setHPF_BW(4.0, sr);
    }
    void mute() override {
        early.mute();
        late.mute();
        for (auto& f : lpf) f.mute();
        for (auto& f : hpf) f.mute();
    }
    void apply(const Cells& c) override {
        const double m = size_m(c.size, 8.0, 32.0);
        const double t = decay_seconds(c.decay);
        if (c.size != applied.size) {
            early.setRSFactor(m / 10.0);
            late.setRSFactor(m / 10.0);
        }
        if (c.decay != applied.decay) late.setrt60(t);
        // The plugin derives its low boost from boost, decay and size together; boost is 50 %.
        if (c.size != applied.size || c.decay != applied.decay)
            late.setbassboost(50.0 / 20.0 / std::pow(t, 1.5) * (m / 10.0));
        if (c.damp != applied.damp) {
            early.setoutputlpf(damp_hz(c.damp));
            late.setdamp(damp_hz(c.damp));
            late.setoutputdamp(damp_hz(c.damp));
        }
        if (c.mod != applied.mod) {
            const double spin = 5.0 * c.mod / 255.0;
            late.setspin(spin);
            late.setspin2(std::sqrt(100.0 - (10.0 - spin) * (10.0 - spin)) / 2.0);
        }
    }
    void run(float* inL, float* inR, float* outL, float* outR, long n) override {
        for (long i = 0; i < n; i++) {
            inL[i] = lpf[0].process(hpf[0].process(inL[i]));
            inR[i] = lpf[1].process(hpf[1].process(inR[i]));
        }
        early.processreplace(inL, inR, earlyOut[0], earlyOut[1], n);
        for (long i = 0; i < n; i++) {
            lateIn[0][i] = 0.2f * earlyOut[0][i] + inL[i];
            lateIn[1][i] = 0.2f * earlyOut[1][i] + inR[i];
        }
        late.processreplace(lateIn[0], lateIn[1], lateOut[0], lateOut[1], n);
        for (long i = 0; i < n; i++) {
            outL[i] = 0.1f * earlyOut[0][i] + 0.2f * lateOut[0][i];
            outR[i] = 0.1f * earlyOut[1][i] + 0.2f * lateOut[1][i];
        }
    }
};

// PLATE / FOIL / TANK — input filters into one of the Plate plugin's three models. Fixed values:
// "Clear Plate" (low cut 100 Hz).
//   DCAY RT60 · DAMP the model's damping, or the input high cut where the model has none · SIZE and
//   MOD read nothing
//
// ⚠️⚠️ **DRAGONFLY'S OWN DAMPEN KNOB DOES NOTHING ON ITS "SIMPLE" AND "NESTED" MODELS.** The plugin
// overrides a `processloop2` that this copy of freeverb's `nrev` never calls — `processreplace` runs
// its own loop inline — so the damping filters it sets are never in the signal path, and "Nested"
// runs the "Simple" loop too. What those two DO respond to is the INPUT high cut, so that is what DAMP
// drives there. TANK has a real damping stage and DAMP drives it, as the plugin does.
template <class Model>
struct PlateEngine final : Engine {
    fv3::iir_1st_f lpf[2], hpf[2];
    Model          model;
    bool           dampIsInputCut;
    double         sampleRate = 48000.0;
    double         inputCutHz = 16000.0;

    explicit PlateEngine(bool tank) : dampIsInputCut(!tank) {
        for (auto& f : lpf) f.mute();
        for (auto& f : hpf) f.mute();
        model.setdryr(0);
        model.setwetr(1);
        model.setMuteOnChange(false);
        model.setwidth(100.0 / 120.0);
        model.setPreDelay(0.1);
    }
    void setRate(double sr) override {
        sampleRate = sr;
        model.setSampleRate(sr);
        for (auto& f : hpf) f.setHPF_BW(100.0, sr);
        for (auto& f : lpf) f.setLPF_BW(inputCutHz, sr);
    }
    void mute() override {
        model.mute();
        for (auto& f : lpf) f.mute();
        for (auto& f : hpf) f.mute();
    }
    void apply(const Cells& c) override {
        if (c.decay != applied.decay) model.setrt60(decay_seconds(c.decay));
        if (c.damp != applied.damp) {
            if (dampIsInputCut) {
                inputCutHz = damp_hz(c.damp);
                for (auto& f : lpf) f.setLPF_BW(inputCutHz, sampleRate);
            } else {
                setModelDamp(damp_hz(c.damp));
            }
        }
    }
    void setModelDamp(double) {}
    void run(float* inL, float* inR, float* outL, float* outR, long n) override {
        for (long i = 0; i < n; i++) {
            inL[i] = lpf[0].process(hpf[0].process(inL[i]));
            inR[i] = lpf[1].process(hpf[1].process(inR[i]));
        }
        model.processreplace(inL, inR, outL, outR, n);
        for (long i = 0; i < n; i++) {
            outL[i] *= 0.2f;
            outR[i] *= 0.2f;
        }
    }
};

template <>
void PlateEngine<fv3::strev_f>::setModelDamp(double hz) {
    model.setdamp(hz);
    model.setoutputdamp(std::max(hz * 2.0, 16000.0));
}

// ⚠️ TANK's modulation noise (freeverb's `noisegen_pink_frac`) calls `std::rand` per sample: a render
// on TANK is never byte-reproducible, and on glibc `rand` takes a lock on the audio thread.
std::unique_ptr<Engine> make_tank() {
    auto e = std::make_unique<PlateEngine<fv3::strev_f>>(true);
    e->model.setdccutfreq(6);
    e->model.setspinlimit(12);
    e->model.setspindiff(0.15);
    return e;
}

// EARLY — earlyref alone. Fixed values: the plugin's defaults (low cut 50 Hz).
//   SIZE 10-60 m · DAMP high cut · MOD which of the eight reflection programs · DCAY reads nothing
struct EarlyEngine final : Engine {
    // The plugin's eight programs, in its order, as freeverb's preset numbers.
    static constexpr int kPrograms[8] = {2, 18, 0, 19, 1, 13, 14, 21};

    EarlyRef        model;
    int             program = -1;

    EarlyEngine() {
        early_setup(model);
        model.setwidth(1.0);
        model.setoutputhpf(50.0);
    }
    void setRate(double sr) override { model.setSampleRate(sr); }
    void mute() override { model.mute(); }
    void apply(const Cells& c) override {
        const int prog = std::min(7, c.mod / 32);
        if (prog != program) {
            model.loadPresetReflection(kPrograms[prog]);
            program = prog;
        }
        if (c.size != applied.size) model.setRSFactor(size_m(c.size, 10.0, 60.0) / 10.0);
        if (c.damp != applied.damp) model.setoutputlpf(damp_hz(c.damp));
    }
    void run(float* inL, float* inR, float* outL, float* outR, long n) override {
        model.processreplace(inL, inR, outL, outR, n);
        for (long i = 0; i < n; i++) {
            outL[i] *= 0.2f;
            outR[i] *= 0.2f;
        }
    }
};

/**
 * The level each algorithm is trimmed by, so that choosing one changes the character and not the
 * loudness. Indexed by algorithm; 0 is unused.
 * ⚠️ Measured on noise at the default cells against the shipping reverb's wet level — a DCAY or SIZE
 * away from the defaults moves these engines' level, which nothing here compensates.
 */
//                                           OLD   HALL   ROOM   PLATE  FOIL   TANK   EARLY
constexpr float kTrim[kReverbAlgoCount] = {1.0f, 2.22f, 4.62f, 3.63f, 2.26f, 1.72f, 1.74f};

std::unique_ptr<Engine> make_engine(int algo) {
    switch (algo) {
        case kReverbAlgoHall:  return std::make_unique<HallEngine>();
        case kReverbAlgoRoom:  return std::make_unique<RoomEngine>();
        case kReverbAlgoPlate: return std::make_unique<PlateEngine<fv3::nrevb_f>>(false);
        case kReverbAlgoFoil:  return std::make_unique<PlateEngine<fv3::nrev_f>>(false);
        case kReverbAlgoTank:  return make_tank();
        case kReverbAlgoEarly: return std::make_unique<EarlyEngine>();
        default:               return nullptr;
    }
}

}  // namespace

struct DragonflyReverb::Impl {
    std::atomic<int>   decay{0x60}, size{0x60}, damp{0x80}, mod{0x10};
    std::atomic<float> rate{48000.0f};
    std::atomic<bool>  clearRequested{false};

    std::unique_ptr<Engine> engines[kReverbAlgoCount];
    float                   inL[kBlock], inR[kBlock];
};

DragonflyReverb::DragonflyReverb() : impl(std::make_unique<Impl>()) {}
DragonflyReverb::~DragonflyReverb() = default;

void DragonflyReverb::setCells(int decayHex, int sizeHex, int dampHex, int modHex) {
    impl->decay.store(std::clamp(decayHex, 0, 255), std::memory_order_relaxed);
    impl->size.store(std::clamp(sizeHex, 0, 255), std::memory_order_relaxed);
    impl->damp.store(std::clamp(dampHex, 0, 255), std::memory_order_relaxed);
    impl->mod.store(std::clamp(modHex, 0, 255), std::memory_order_relaxed);
}

void DragonflyReverb::setRate(float sampleRate) {
    impl->rate.store(sampleRate, std::memory_order_relaxed);
}

void DragonflyReverb::requestClear() { impl->clearRequested.store(true, std::memory_order_relaxed); }

void DragonflyReverb::process(int algo, bool switched, const float* inL, const float* inR,
                              float* outL, float* outR, int numFrames) {
    if (algo <= 0 || algo >= kReverbAlgoCount) {
        std::memset(outL, 0, sizeof(float) * numFrames);
        std::memset(outR, 0, sizeof(float) * numFrames);
        return;
    }
    Impl& m = *impl;

    std::unique_ptr<Engine>& slot = m.engines[algo];
    if (!slot) {
        slot     = make_engine(algo);
        switched = false;   // nothing in it to replay
    }
    Engine& e = *slot;

    const float rate = m.rate.load(std::memory_order_relaxed);
    if (e.rate != rate) {
        e.setRate(rate);
        e.rate    = rate;
        e.applied = Cells{};   // a rate change re-derives every coefficient
    }
    const bool clear = m.clearRequested.exchange(false, std::memory_order_relaxed);
    if (switched || clear) e.mute();

    const Cells want{m.decay.load(std::memory_order_relaxed), m.size.load(std::memory_order_relaxed),
                     m.damp.load(std::memory_order_relaxed), m.mod.load(std::memory_order_relaxed)};
    if (want.decay != e.applied.decay || want.size != e.applied.size ||
        want.damp != e.applied.damp || want.mod != e.applied.mod) {
        e.apply(want);
        e.applied = want;
    }

    const float trim = kTrim[algo];
    for (int done = 0; done < numFrames;) {
        const int n = std::min(kBlock, numFrames - done);
        std::memcpy(m.inL, inL + done, sizeof(float) * n);
        std::memcpy(m.inR, inR + done, sizeof(float) * n);
        e.run(m.inL, m.inR, outL + done, outR + done, n);
        for (int i = 0; i < n; i++) {
            outL[done + i] *= trim;
            outR[done + i] *= trim;
        }
        done += n;
    }
}
