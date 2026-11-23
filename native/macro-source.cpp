#include "macro-source.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "effects/glide.h"

// The vendored headers: MSVC needs `__attribute__` gone before them, and their warnings are not ours
// to fix.
#include "vendor/braids/pt-msvc.h"
#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wshadow"
#pragma GCC diagnostic ignored "-Wunused-parameter"
#elif defined(_MSC_VER)
#pragma warning(push, 0)
#endif
#include "braids/macro_oscillator.h"
#include "stmlib/utils/random.h"
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#elif defined(_MSC_VER)
#pragma warning(pop)
#endif

namespace {

// The oscillator renders this many samples a call, always: several models write them in pairs.
constexpr int BLOCK = 24;
constexpr int OUT   = BLOCK / 2;

// The halfband filter: 35 taps, every second one zero, 0.5 at the centre and these at 1, 3, 5 …
// samples either side. Flat to 18 kHz at a 48 kHz engine, and 70 dB down from 30 kHz, which is what
// would fold under 18 kHz. A Kaiser-windowed sinc.
constexpr int   PAIRS = 9;
constexpr int   REACH = 2 * PAIRS - 1;   // the furthest tap from the centre
constexpr float HALFBAND[PAIRS] = {3.152128235e-01f,  -9.723553121e-02f, 4.982790795e-02f,
                                   -2.788624976e-02f, 1.542583197e-02f,  -8.002492929e-03f,
                                   3.705718895e-03f,  -1.419501316e-03f, 3.714928456e-04f};
// The input is kept as two lines, its even samples and its odd ones: an output is half an even sample
// plus the odd ones around it, so each tap then reads straight along a line.
constexpr int KEPT = REACH;              // samples each line keeps from one block for the next

// The oscillator's pitch is in 128ths of a semitone, and it reads tables by it: outside 0..16383 it
// reads past them.
constexpr int PITCH_MAX = (128 << 7) - 1;

// A full swing of TIMBRE or COLOR takes GLIDE_SAMPLES; the oscillator takes a new value per block.
constexpr float GLIDE_STEP = 255.0f * (float)OUT / (float)GLIDE_SAMPLES;

const uint8_t NO_SYNC[BLOCK] = {};

static_assert(MacroBank::MODELS == braids::MACRO_OSC_SHAPE_QUESTION_MARK,
              "a name per model the oscillator has, less its hidden last one");

int16_t braids_param(float v) {   // 0-255 onto the oscillator's 0..32767
    return (int16_t)(std::max(0.0f, std::min(255.0f, v)) * (32767.0f / 255.0f) + 0.5f);
}

float glide_toward(float from, float to) {
    if (from < 0.0f) return to;   // the note's first block starts on its values
    return from + std::max(-GLIDE_STEP, std::min(GLIDE_STEP, to - from));
}

}  // namespace

struct MacroBank::Osc {
    braids::MacroOscillator osc;
    // The filter's input: what the last block left, then this block.
    float even[KEPT + OUT], odd[KEPT + OUT];
    float ready[OUT];              // filtered samples not yet handed out
    int   readyFrom;               // the next of them; OUT = none left
    float timbre, color;           // what the last block was rendered with; -1 before the first
};

MacroBank::MacroBank(int voices) : oscs_(new Osc[(size_t)voices * NOTES]), voices_(voices) {
    for (int v = 0; v < voices; ++v) start(v, 0);
}

MacroBank::~MacroBank() = default;

void MacroBank::start(int voice, int model) {
    if (voice < 0 || voice >= voices_) return;
    const auto shape = (braids::MacroOscillatorShape)std::max(0, std::min(MODELS - 1, model));
    for (int n = 0; n < NOTES; ++n) {
        Osc& o = oscs_[(size_t)voice * NOTES + n];
        // ⚠️ All of it, not just Init(): that leaves the delay lines a plucked or comb model reads.
        std::memset(static_cast<void*>(&o), 0, sizeof(o));
        o.osc.Init();
        o.osc.set_shape(shape);
        o.osc.Strike();
        o.readyFrom = OUT;
        o.timbre = o.color = -1.0f;
    }
}

void MacroBank::carry(int voice, int from) {
    if (voice < 0 || voice >= voices_ || from < 0 || from >= voices_) return;
    for (int n = 0; n < NOTES; ++n) {
        Osc& o = oscs_[(size_t)voice * NOTES + n];
        if (from != voice) std::memcpy(static_cast<void*>(&o), &oscs_[(size_t)from * NOTES + n], sizeof(o));
        o.osc.Strike();
    }
}

void MacroBank::render(int voice, int note, float* out, int frames, float midiNote, float timbre,
                       float color, float sampleRate) {
    if (voice < 0 || voice >= voices_ || note < 0 || note >= NOTES) {
        std::fill(out, out + frames, 0.0f);
        return;
    }
    Osc& o = oscs_[(size_t)voice * NOTES + note];

    // The oscillator believes it runs at 96 kHz. At twice any other engine rate its pitch is off by
    // the ratio, so the note is moved the other way.
    const float retune = 12.0f * log2f(96000.0f / (2.0f * sampleRate));
    const int   pitch  = std::max(0, std::min(PITCH_MAX, (int)lrintf((midiNote + retune) * 128.0f)));

    int done = 0;
    while (done < frames) {
        if (o.readyFrom >= OUT) {
            o.timbre = glide_toward(o.timbre, timbre);
            o.color  = glide_toward(o.color, color);
            o.osc.set_pitch((int16_t)pitch);
            o.osc.set_parameters(braids_param(o.timbre), braids_param(o.color));

            int16_t block[BLOCK];
            o.osc.Render(NO_SYNC, block, BLOCK);
            for (int i = 0; i < OUT; ++i) {
                o.even[KEPT + i] = (float)block[2 * i]     * (1.0f / 32768.0f);
                o.odd[KEPT + i]  = (float)block[2 * i + 1] * (1.0f / 32768.0f);
            }

            // Output k is centred on even sample PAIRS + k. An odd sample follows the even one of its
            // number, so the pair nearest the centre is the odd one before it and its own, and each
            // further pair is one more out on both sides.
            for (int k = 0; k < OUT; ++k) o.ready[k] = 0.5f * o.even[PAIRS + k];
            for (int t = 0; t < PAIRS; ++t) {
                const float* const before = o.odd + (PAIRS - 1 - t);
                const float* const after  = o.odd + (PAIRS + t);
                for (int k = 0; k < OUT; ++k) o.ready[k] += HALFBAND[t] * (before[k] + after[k]);
            }
            std::memmove(o.even, o.even + OUT, sizeof(float) * KEPT);
            std::memmove(o.odd,  o.odd  + OUT, sizeof(float) * KEPT);
            o.readyFrom = 0;
        }
        const int take = std::min(frames - done, OUT - o.readyFrom);
        std::memcpy(out + done, o.ready + o.readyFrom, sizeof(float) * (size_t)take);
        o.readyFrom += take;
        done        += take;
    }
}

void MacroBank::reseed() { stmlib::Random::Seed(0x21); }
