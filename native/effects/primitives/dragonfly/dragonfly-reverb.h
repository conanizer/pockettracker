#pragma once

// The Dragonfly Reverb plugins as the reverb send's algorithms 1..4: HALL, ROOM, PLATE ("Nested")
// and FOIL ("Simple"), over the freeverb3 subset in `freeverb/`. The cell mappings are in the .cpp.
// ⚠️ Freeverb reallocates when SIZE or the rate changes, so the audio thread never does: `prepare`
// (control thread) builds a whole engine and hands it over; `process` takes it and parks the old one
// for the next `prepare` to free. Nothing is built until its algorithm is chosen.
// Behind an opaque pointer so the freeverb headers stay in one translation unit.

#include <atomic>
#include <memory>

class DragonflyReverb {
  public:
    DragonflyReverb();
    ~DragonflyReverb();
    DragonflyReverb(const DragonflyReverb&)            = delete;
    DragonflyReverb& operator=(const DragonflyReverb&) = delete;

    // The four cells the algorithms read, 00-FF each. Any thread.
    void setCells(int decayHex, int sizeHex, int dampHex, int modHex);

    // Builds the engine for `algo` at this SIZE and rate if it is not already. Allocates: control
    // thread only.
    void prepare(int algo, int decayHex, int sizeHex, int dampHex, int modHex, float sampleRate);

    // Empty every tail before the next block. Any thread.
    void requestClear();

    // Stereo in, wet out, `algo` 1..4; silent until `prepare` built it. Audio thread only.
    // ⚠️ `switched` on the first block after another algorithm, or an idle engine replays its old tail.
    void process(int algo, bool switched, const float* inL, const float* inR, float* outL,
                 float* outR, int numFrames);

  private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};
