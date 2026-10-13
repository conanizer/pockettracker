// SOLA (Synchronized Overlap-Add) time-stretch: a different duration at the ORIGINAL pitch.
//
// The input is cut into chunks ("grains"); material is repeated to slow down and skipped to speed up,
// and every splice is an equal-power crossfade. The splices fall on a fixed grid ("cyclic"), which is
// the gritty sound of an early hardware sampler and the reason this is not an FFT stretch.
//
// Offline only: the sample editor's SYNC and its audition, once per channel — deterministic, so the
// channels of a stereo sample stay phase-aligned.

#pragma once

#include <vector>
#include <cstddef>
#include <cmath>
#include <algorithm>

namespace sola {

/** The chunk length, in milliseconds: shorter is grittier, longer is smoother but can slap back. */
static constexpr float DEFAULT_GRAIN_MS = 40.0f;

/**
 * How much of each chunk is crossfade. It scales with the chunk, so the chunk length is the one
 * setting: a fixed fade would click under a long grain and swallow a short one.
 */
static constexpr float OVERLAP_OF_GRAIN = 0.375f;

/**
 * Time-stretch a mono buffer by `ratio` (outputLen / inputLen; >1 = slower/longer).
 * Offline: whole sample in, freshly allocated stretched sample out.
 *
 * ⚠️ INVARIANT: the returned length is EXACTLY lround(inputLen * ratio). The sample editor's SYNC
 * fits a loop to the project's grid by asking for a length, so an output quantized to the chunk
 * grid — or short by the tail chunk — puts the loop point in the wrong place, and a drum loop that
 * wraps a few ms early is the whole symptom. Length is a contract here, not an outcome.
 */
inline std::vector<float> stretch(const float* input,
                                  int          inputLen,
                                  float        ratio,
                                  float        sampleRate,
                                  float        grainMs = DEFAULT_GRAIN_MS)
{
    if (inputLen <= 0) return std::vector<float>();

    // Beyond ~4x stretch / 0.25x compression SOLA artifacts get severe; clamp
    // to a safe envelope against bad input.
    if (ratio < 0.1f)  ratio = 0.1f;
    if (ratio > 10.0f) ratio = 10.0f;

    const int targetLen = std::max(1, (int)std::lround((double)inputLen * (double)ratio));

    // Already the requested length (to within the rounding): splicing cannot improve on the samples
    // that are already there. Measured in FRAMES, not as a ratio window — a "0.1% is inaudible"
    // window is 8 ms on an 8-second loop, which is most of a tick.
    if (std::abs(targetLen - inputLen) <= 2) {
        std::vector<float> out(input, input + inputLen);
        out.resize((size_t)targetLen, 0.0f);
        return out;
    }

    // ms -> frames. ⚠️ At least 2 frames of chunk, or the hop below is 0 and the loop never advances.
    int sequenceLen = std::max(2, (int)(grainMs * 0.001f * sampleRate));
    int overlapLen  = std::max(0, (int)(grainMs * OVERLAP_OF_GRAIN * 0.001f * sampleRate));

    // ⚠️ Half the chunk, not the whole chunk: the crossfade reads back `overlapLen` from `outputPos`,
    // whose smallest value is one outputHop, so an overlap past the halfway point indexes before the
    // start of the buffer.
    if (overlapLen > sequenceLen / 2) overlapLen = sequenceLen / 2;

    // Hop sizes — the heart of the stretch:
    //   outputHop: constant advance in the OUTPUT per chunk (the non-overlapped
    //              part of each chunk).
    //   inputHop = outputHop / ratio: advance in the INPUT per chunk.
    // ratio > 1 (slowdown): inputHop < outputHop -> material is re-read
    // (repetition = longer output); ratio < 1: input is skipped = shorter.
    int   outputHop = sequenceLen - overlapLen;
    float inputHop  = (float)outputHop / ratio;

    // Equal-power (cosine) rather than linear: perceived loudness stays constant instead of dipping
    // mid-fade.
    std::vector<float> fadeOut((size_t)overlapLen), fadeIn((size_t)overlapLen);
    for (int i = 0; i < overlapLen; ++i)
    {
        float t = (float)i / (float)(overlapLen);
        fadeOut[(size_t)i] = cosf(t * 0.5f * (float)M_PI);
        fadeIn[(size_t)i]  = sinf(t * 0.5f * (float)M_PI);
    }

    // One sequenceLen of headroom so the final chunk has room to write; trimmed to targetLen at the end.
    std::vector<float> output((size_t)targetLen + sequenceLen, 0.0f);

    // First chunk: straight copy — there is nothing to crossfade with yet.
    std::copy(input, input + std::min(sequenceLen, inputLen), output.begin());

    // inputPos is fractional (inputHop is a float): accumulating fractions and
    // flooring keeps the average tempo accurate for non-integer hops.
    float inputPosFloat = inputHop;
    int   outputPos     = outputHop;

    // Every subsequent chunk is crossfaded onto the tail of the previous one, until the output is
    // filled to targetLen — the OUTPUT drives the loop, because the output length is the contract.
    while (outputPos < targetLen)
    {
        int inputPos = (int)inputPosFloat;

        // ⚠️ CLAMP to the last full chunk, never break on it. Stopping here instead leaves the final
        // sequenceLen of input unread and the output that much short — and for ratio > 1 the input
        // runs out first by design, so that is the common case, not the edge case. Re-reading the last
        // chunk is what "repeat material to slow down" means; the tail still lands in the output.
        if (inputPos > inputLen - sequenceLen) inputPos = inputLen - sequenceLen;

        if (inputPos < 0)                              break;   // input shorter than one chunk
        if (outputPos + sequenceLen >= (int)output.size()) break;

        float*       out     = &output[(size_t)outputPos];
        float*       tail    = out - overlapLen;
        const float* newHead = &input[inputPos];

        // The new chunk's head over the previous chunk's tail, then the rest straight through.
        for (int i = 0; i < overlapLen; ++i)
            tail[i] = tail[i] * fadeOut[(size_t)i] + newHead[i] * fadeIn[(size_t)i];
        for (int i = 0; i < sequenceLen - overlapLen; ++i)
            out[i] = newHead[overlapLen + i];

        outputPos     += outputHop;     // constant step in the output buffer
        inputPosFloat += inputHop;      // tempo-dependent step in the input
    }

    // Trim the headroom to the length ASKED for. The loop always fills at least that far (it exits
    // early only when the input is shorter than one chunk), so this is a trim, not a zero-pad.
    output.resize((size_t)targetLen);
    return output;
}

} // namespace sola
