#pragma once

// ───────────────────────────────────────────────────────────────────────────
// DragonflyReverb — the Dragonfly Reverb suite's four plugins, run as the reverb send's algorithms
// 1..6: HALL, ROOM, PLATE (Dragonfly's "Nested"), FOIL ("Simple"), TANK and EARLY.
//
// Each is Dragonfly's own `plugins/*/DSP.cpp` set-up and signal flow over the freeverb3 subset in
// `freeverb/`, with the plugin's parameter surface replaced by the reverb section's cells. The
// mapping from a cell to a freeverb setter lives in the .cpp, beside the engine that reads it.
//
// ⚠️⚠️ **FREEVERB REALLOCATES ITS DELAY LINES WHEN A ROOM'S SIZE OR THE RATE CHANGES** (`setRSFactor`,
// `setSampleRate`, `loadPresetReflection`), so the audio thread never does either. `prepare` — the
// control thread, whenever the algorithm, SIZE, EARLY's program or the rate changes — builds a whole
// engine at those values and hands it over; `process` takes it at its next block and parks the one it
// replaced for the next `prepare` to free. The other cells are applied to the running engine inside
// `process`, which allocates nothing. A new engine starts silent, as freeverb's own resize did.
//
// ⚠️ An engine is built when its algorithm is CHOSEN, not before: a project on algorithm 0 never
// allocates any of this.
//
// Kept behind an opaque pointer so the freeverb headers, their `LIBFV3_FLOAT` define and their
// include path stay inside one translation unit instead of reaching every file that includes the
// audio engine.
// ───────────────────────────────────────────────────────────────────────────

#include <atomic>
#include <memory>

class DragonflyReverb {
  public:
    DragonflyReverb();
    ~DragonflyReverb();
    DragonflyReverb(const DragonflyReverb&)            = delete;
    DragonflyReverb& operator=(const DragonflyReverb&) = delete;

    /** The four cells the algorithms read, 00-FF each. Any thread. */
    void setCells(int decayHex, int sizeHex, int dampHex, int modHex);

    /**
     * Make sure the engine for `algo` is built at this SIZE, program and rate, building it here if not.
     * The control thread only (the UI, or a render while the device is paused) — this allocates.
     */
    void prepare(int algo, int decayHex, int sizeHex, int dampHex, int modHex, float sampleRate);

    /** Empty every tail before the next block. Any thread. */
    void requestClear();

    /**
     * Stereo in, stereo wet out, `algo` 1..6. Audio thread only; silent until `prepare` has built
     * the algorithm.
     * ⚠️ `switched` must be true on the first block after the send was sounding a DIFFERENT algorithm:
     * an engine that sat idle still holds the tail it had when it stopped, and would replay it.
     */
    void process(int algo, bool switched, const float* inL, const float* inR, float* outL,
                 float* outR, int numFrames);

  private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};
