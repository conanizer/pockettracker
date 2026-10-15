// SOLA time-stretch: a new length at the original pitch. Chunks are repeated to slow down and skipped
// to speed up, spliced with equal-power crossfades on a fixed grid — the gritty sampler sound.
// Offline only (the sample editor's SYNC), once per channel; deterministic, so stereo stays aligned.

#pragma once

#include <vector>
#include <cstddef>
#include <cmath>
#include <algorithm>

namespace sola {

// The chunk length, ms: shorter is grittier, longer smoother but can slap back.
static constexpr float DEFAULT_GRAIN_MS = 40.0f;

// The share of each chunk that is crossfade.
static constexpr float OVERLAP_OF_GRAIN = 0.375f;

// Stretches a mono buffer by `ratio` (output / input length; > 1 = longer).
// ⚠️ The result is exactly lround(inputLen * ratio) long: SYNC fits a loop to the grid by length.
inline std::vector<float> stretch(const float* input,
                                  int          inputLen,
                                  float        ratio,
                                  float        sampleRate,
                                  float        grainMs = DEFAULT_GRAIN_MS)
{
    if (inputLen <= 0) return std::vector<float>();

    // Against bad input.
    if (ratio < 0.1f)  ratio = 0.1f;
    if (ratio > 10.0f) ratio = 10.0f;

    const int targetLen = std::max(1, (int)std::lround((double)inputLen * (double)ratio));

    // Already the length, to within two frames (a ratio window would be ms on a long loop).
    if (std::abs(targetLen - inputLen) <= 2) {
        std::vector<float> out(input, input + inputLen);
        out.resize((size_t)targetLen, 0.0f);
        return out;
    }

    // ⚠️ At least 2 frames, or the hop is 0 and the loop never advances.
    int sequenceLen = std::max(2, (int)(grainMs * 0.001f * sampleRate));
    int overlapLen  = std::max(0, (int)(grainMs * OVERLAP_OF_GRAIN * 0.001f * sampleRate));

    // ⚠️ At most half a chunk, or the first crossfade reads before the buffer's start.
    if (overlapLen > sequenceLen / 2) overlapLen = sequenceLen / 2;

    // The output advances a fixed hop per chunk, the input that hop / ratio: re-read to slow down,
    // skipped to speed up.
    int   outputHop = sequenceLen - overlapLen;
    float inputHop  = (float)outputHop / ratio;

    // Equal-power, so the loudness does not dip mid-fade.
    std::vector<float> fadeOut((size_t)overlapLen), fadeIn((size_t)overlapLen);
    for (int i = 0; i < overlapLen; ++i)
    {
        float t = (float)i / (float)(overlapLen);
        fadeOut[(size_t)i] = cosf(t * 0.5f * (float)M_PI);
        fadeIn[(size_t)i]  = sinf(t * 0.5f * (float)M_PI);
    }

    // One chunk of headroom for the last write; trimmed at the end.
    std::vector<float> output((size_t)targetLen + sequenceLen, 0.0f);


    std::copy(input, input + std::min(sequenceLen, inputLen), output.begin());

    // Fractional, so a non-integer hop keeps the average rate exact.
    float inputPosFloat = inputHop;
    int   outputPos     = outputHop;

    // The output drives the loop, because its length is the contract.
    while (outputPos < targetLen)
    {
        int inputPos = (int)inputPosFloat;

        // ⚠️ Clamp to the last full chunk, never stop on it: when slowing down the input runs out
        // first, and stopping would leave the output short.
        if (inputPos > inputLen - sequenceLen) inputPos = inputLen - sequenceLen;

        if (inputPos < 0)                              break;   // input shorter than one chunk
        if (outputPos + sequenceLen >= (int)output.size()) break;

        float*       out     = &output[(size_t)outputPos];
        float*       tail    = out - overlapLen;
        const float* newHead = &input[inputPos];


        for (int i = 0; i < overlapLen; ++i)
            tail[i] = tail[i] * fadeOut[(size_t)i] + newHead[i] * fadeIn[(size_t)i];
        for (int i = 0; i < sequenceLen - overlapLen; ++i)
            out[i] = newHead[overlapLen + i];

        outputPos     += outputHop;
        inputPosFloat += inputHop;
    }

    output.resize((size_t)targetLen);
    return output;
}

} // namespace sola
