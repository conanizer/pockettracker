#pragma once
// The MACRO instrument's oscillators: the vendored macro-oscillator (vendor/braids), four to a sampler
// voice — a note and the rest of its chord. The oscillator is built for 96 kHz, so it runs at twice the
// engine's rate and a halfband filter halves that; at 48 kHz it is the module's own rate.
// ⚠️ Audio thread only, after the constructor, and nothing allocates after it. The vendored headers
// stay out of this one: they need defines no other file should have to carry.
#include <cstdint>
#include <memory>

class MacroBank {
public:
    static constexpr int NOTES  = 4;    // a voice's own note and Voice::CHORD_EXTRA more
    // The models, in the oscillator's own order; songcore's MACRO_MODEL_NAMES names each
    // (pinned in songcore/engine_consumer.h).
    static constexpr int MODELS = 47;

    explicit MacroBank(int voices);
    ~MacroBank();

    // A fresh note on `voice`: its oscillators cleared and struck, so every note starts alike and
    // nothing is left of the note or the model the voice had before.
    void start(int voice, int model);

    // The next `frames` samples of one of the voice's notes into `out`, within about ±1.
    // `midiNote` is the pitch, 60 = C-4, fractions and all; TIMBRE and COLOR are 0-255, and a change
    // to either glides. The oscillator takes a pitch from note 0 to just under 128, so it is held there.
    void render(int voice, int note, float* out, int frames, float midiNote, float timbre, float color,
                float sampleRate);

    // The noise every model draws on, back to its first value: what makes a render repeat.
    static void reseed();

private:
    struct Osc;
    std::unique_ptr<Osc[]> oscs_;
    int voices_;
};
