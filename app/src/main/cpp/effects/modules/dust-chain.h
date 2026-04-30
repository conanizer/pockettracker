/*
    skoomaDust - DustChain
    Fixed signal chain:
        low-shelf → LP → tube (Airwindows Tube2) → FET compressor (APComp)
                       → wow/drift → bitcrush → soft-clipper.

    The FET compressor is adapted from APComp by Alain Paul / AP Mastering
    (BSD-3-Clause, copyright preserved in DustChain.cpp). Threshold sweeps
    0 → −25 dB as the knob goes 0 → 1; all other parameters are baked in.

    License: GPL-3.0-or-later
*/

#pragma once

#include <array>
#include <cstdint>
#include <vector>

namespace skdust {

class DustChain
{
public:
    DustChain() = default;

    // Wow/drift delay-line latency — report to host via setLatencySamples().
    static constexpr int kWowCenterDelay = 100;   // samples (≈ 2 ms at 48 kHz)
    static constexpr int kWowHfBufSize   = 256;   // > centerDelay + max modulation depth

    void prepare(double sampleRate, int blockSize, int numChannels);
    void reset();

    void setDustAmount(float v) noexcept { dustAmount = v; }

    // Stage on/off — debug aid, surfaced via right-click menu during testing.
    struct Enables {
        bool lp = true, lowShelf = true, bitcrush = true,
             tube = true, fet = true, wow = true, clipper = true,
             lp2 = true;  // second LP at chain output — combined with lp gives -12 dB/oct
    };
    void setEnables(const Enables& e) noexcept { enables = e; }

    void process(float* buf, int numFrames, int channelCount);

private:
    // ---- Stage constants ----
    // LP cutoff tracks the knob: 10 kHz at 0%, 5 kHz at 100% (log interpolation).
    static constexpr float kLPCutoffMax     = 10000.0f;  // Hz at knob = 0%
    static constexpr float kLPCutoffMin     =  5000.0f;  // Hz at knob = 100%
    static constexpr float kLowShelfHz      =    80.0f;
    static constexpr float kLowShelfDb      =    -4.0f;  // tames sub-bass before tube generates harmonics from it
    static constexpr float kLowShelfQ       =    0.7f;
    static constexpr int   kBitcrushBits    =      12;   // after comp so noise floor is level-locked; 10-bit was too harsh
    static constexpr float kClipKnee        =   0.95f;   // soft-clip knee (≈ −0.45 dBFS); transparent below this
    static constexpr float kTubeInputPad    =    0.5f;
    static constexpr float kTubePostGainDb  =   -4.0f;   // without this the comp sees a too-hot signal even at knob 0

    // Wow & drift — rate and depth both track the knob (5× slower/shallower at 0%).
    static constexpr float kWowSplitHz      =   200.0f;  // LF/HF crossover — fixed
    static constexpr float kWowFreqMin      =    0.05f;  // Hz at knob = 0%
    static constexpr float kWowFreqMax      =    0.25f;  // Hz at knob = 100%
    static constexpr float kWowAmpMin       =     2.0f;  // samples at knob = 0%
    static constexpr float kWowAmpMax       =    12.0f;  // samples at knob = 100%
    static constexpr float kDriftLpMin      =    0.01f;  // Hz at knob = 0%
    static constexpr float kDriftLpMax      =    0.07f;  // Hz at knob = 100%
    static constexpr float kDriftAmpMin     =     2.0f;  // samples at knob = 0%
    static constexpr float kDriftAmpMax     =    12.0f;  // samples at knob = 100%

    // ---- APComp parameters (baked in — only threshold varies with the knob) ----
    // Tuned by ear against Arturia FET-76 + Vulf comp references.
    static constexpr float kCompThresholdMin  =  -25.0f; // dB at knob = 1.0
    static constexpr float kCompRatio         =    10.0f;
    static constexpr float kCompAttackSec     = 0.0005f;
    static constexpr float kCompReleaseSec    =  0.150f;
    static constexpr float kCompConvexity     =    1.1f; // exponent on GR-in-dB; >1 softens knee engagement
    static constexpr float kCompInertia       =    0.1f; // velocity-overshoot mass (FET-76 character)
    static constexpr float kCompInertiaDecay  =   0.94f; // mapped internally to 0.99 + x·0.01
    // Extra makeup that scales with the knob: +0% at knob=0, +30% at knob=100%.
    // Gives the comp more apparent loudness/drive at high settings.
    static constexpr float kCompMakeupDrive   =    0.1f;

    // RMS auto-makeup: peak-followed input/output energies, makeup = sqrt(inEnv/outEnv).
    // Asymmetric (fast attack, slow release). Fast output attack means a knob-slam
    // collapses the makeup ratio quickly — no volume spike.
    static constexpr float kMakeupEnvAttackMs  =   5.0f;
    static constexpr float kMakeupEnvReleaseMs = 300.0f;

    // ---- Per-channel state ----
    struct ChannelState {
        // Low-shelf biquad DF-I state
        float lsX1 = 0.0f, lsX2 = 0.0f, lsY1 = 0.0f, lsY2 = 0.0f;
        // Tube2 (Airwindows) state
        float tubePrevA = 0.0f, tubePrevC = 0.0f, tubePrevE = 0.0f;
        // Wow/drift: separate HF (modulated) and LF (fixed-delay) buffers for phase alignment.
        // wowSplitLpZ is the 1-pole crossover state.
        std::array<float, kWowHfBufSize>   wowHfBuf {};
        std::array<float, kWowCenterDelay> wowLfBuf {};
        int   wowHfWriteIdx = 0;
        int   wowLfWriteIdx = 0;
        float wowSplitLpZ   = 0.0f;
        // APComp per-channel state (dB unless noted)
        double slewedSignal    = -200.0;
        double gainReductionDb =    0.0;
        double inertiaVelocity =    0.0;
    };

    Enables enables;
    double sampleRate      = 48000.0;
    float  dustAmount      = 0.0f;
    bool   tubeHighSR      = false;
    float  tubePostGainLin = 1.0f;
    float  compInEnvSq     = 0.0f;
    float  compOutEnvSq    = 0.0f;
    float  compEnvAtkCoef  = 0.0f;
    float  compEnvRelCoef  = 0.0f;

    // Wow/drift LFO state (shared across channels for stereo coherence).
    // wowPhaseInc, driftLpCoef, driftScale are derived per-block from dustAmount.
    float    wowPhase       = 0.0f;
    float    wowSplitLpCoef = 0.0f;
    float    driftLpZ       = 0.0f;
    uint32_t driftRng       = 0x9E3779B9u;

    // Low-shelf biquad coefficients (computed once in prepare, same formula as JUCE makeLowShelf)
    float lsB0 = 1.0f, lsB1 = 0.0f, lsB2 = 0.0f, lsA1 = 0.0f, lsA2 = 0.0f;

    // 1-pole LP state (per channel)
    std::vector<float> lpState;   // stage 2, early in chain
    std::vector<float> lpState2;  // stage 8, output (adds -6 dB/oct → combined -12 dB/oct)

    std::vector<ChannelState> chans;
};

} // namespace skdust
