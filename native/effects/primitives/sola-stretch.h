// =============================================================================
// sola-stretch.h
//
// SOLA (Synchronized Overlap-Add) time-stretching, Akai-cyclic flavor.
//
// Given an input buffer and a stretch ratio (outputLength / inputLength),
// produces audio of a different duration at the ORIGINAL pitch:
//   ratio = 1.0 -> no change; 2.0 -> twice as long (half tempo); 0.5 -> half as long.
//
// WHY SOLA AND NOT FFT
// --------------------
// 1. No FFT library needed -> works anywhere, fits the Miyoo Flip CPU budget.
// 2. The "cyclic" SOLA mode is literally what Akai S950/S1000 used. The
//    characteristic gritty texture that defines jungle breakbeats IS the
//    by-product of this algorithm. Modern, cleaner algorithms can't fake it.
//
// Core idea: cut the input into ~40 ms chunks; repeat material to slow down,
// skip material to speed up; equal-power-crossfade every splice.
//
// Offline only: called from timeStretchSample (destructive APPLY FX), once per
// channel for stereo — deterministic, so channels stay phase-aligned.
// =============================================================================

#pragma once

#include <vector>
#include <cstddef>
#include <cmath>
#include <algorithm>

namespace sola {

// ----------------------------------------------------------------------------
// Tuning constants. These are the "Akai feel" knobs.
//
// SEQUENCE_MS:  chunk length.
//   - Smaller = grittier, choppier, more "1990s sampler" character.
//   - Larger  = smoother but can introduce slap-back / phasing artifacts.
//   - Akai S1000 hovered around 30-50 ms. 40 ms is a great default.
//
// OVERLAP_MS:   crossfade length between chunks.
//   - Roughly 25-50% of SEQUENCE_MS is the sweet spot.
//   - Too small -> audible clicks at every splice (the dreaded "tic-tic-tic").
//   - Too large -> mushy, comb-filtered sound.
//
// SEEK_MS:      how far around the "ideal" splice point to search for a better
//               one. 0 = pure cyclic Akai sound; ~10-15 ms = smoother
//               "intelligent" mode.
// ----------------------------------------------------------------------------
static constexpr float SEQUENCE_MS = 40.0f;  // chunk length
static constexpr float OVERLAP_MS  = 15.0f;  // crossfade length
static constexpr float SEEK_MS     = 0.0f;   // 0 = pure cyclic / Akai mode


// Equal-power (cosine) crossfade of prevTail (fading out) into newHead (fading
// in) over len samples. Equal-power rather than linear: perceived loudness
// stays constant instead of dipping mid-fade.
inline void crossfade(const float* prevTail,
                      const float* newHead,
                      float*       out,
                      int          len)
{
    for (int i = 0; i < len; ++i)
    {
        float t = (float)i / (float)(len);
        float fadeOut = cosf(t * 0.5f * (float)M_PI);
        float fadeIn  = sinf(t * 0.5f * (float)M_PI);
        out[i] = prevTail[i] * fadeOut + newHead[i] * fadeIn;
    }
}


// "Intelligent" SOLA: search ±seekRange around targetPos for the splice point
// whose samples best correlate with the tail of the previous output chunk.
// Not called in cyclic mode (SEEK_MS == 0) — kept for the smoother mode.
inline int findBestOverlap(const float* prevTail,    // end of previous output chunk
                           const float* input,       // entire input buffer
                           int          inputLen,
                           int          targetPos,   // ideal splice position
                           int          seekRange,   // ± samples to search
                           int          overlapLen)  // crossfade length
{
    float bestCorr   = -1e30f;
    int   bestOffset = 0;

    // Limit the search so we never read past the end of the buffer.
    int seekMin = -seekRange;
    int seekMax =  seekRange;
    if (targetPos + seekMin                 < 0)        seekMin = -targetPos;
    if (targetPos + seekMax + overlapLen >= inputLen)   seekMax =  inputLen - targetPos - overlapLen - 1;

    for (int offset = seekMin; offset <= seekMax; ++offset)
    {
        // Dot-product correlation: higher = more similar waveforms = smoother splice.
        float corr = 0.0f;
        for (int i = 0; i < overlapLen; ++i)
        {
            corr += prevTail[i] * input[targetPos + offset + i];
        }
        if (corr > bestCorr)
        {
            bestCorr   = corr;
            bestOffset = offset;
        }
    }
    return bestOffset;
}


// Time-stretch a mono buffer by `ratio` (outputLen / inputLen; >1 = slower/longer).
// Offline: whole sample in, freshly allocated stretched sample out.
inline std::vector<float> stretch(const float* input,
                                  int          inputLen,
                                  float        ratio,
                                  float        sampleRate)
{
    // ratio ≈ 1: plain copy, no DSP, no artifacts.
    if (ratio > 0.999f && ratio < 1.001f) {
        return std::vector<float>(input, input + inputLen);
    }
    // Beyond ~4x stretch / 0.25x compression SOLA artifacts get severe; clamp
    // to a safe envelope against bad input.
    if (ratio < 0.1f)  ratio = 0.1f;
    if (ratio > 10.0f) ratio = 10.0f;

    // ms -> samples.
    int sequenceLen = (int)(SEQUENCE_MS * 0.001f * sampleRate); // ~1764 at 44.1k
    int overlapLen  = (int)(OVERLAP_MS  * 0.001f * sampleRate); // ~661 at 44.1k
    int seekRange   = (int)(SEEK_MS     * 0.001f * sampleRate); // 0 in cyclic mode

    // Safety: overlap must not be larger than the chunk itself.
    if (overlapLen >= sequenceLen) overlapLen = sequenceLen / 2;

    // Hop sizes — the heart of the stretch:
    //   outputHop: constant advance in the OUTPUT per chunk (the non-overlapped
    //              part of each chunk).
    //   inputHop = outputHop / ratio: advance in the INPUT per chunk.
    // ratio > 1 (slowdown): inputHop < outputHop -> material is re-read
    // (repetition = longer output); ratio < 1: input is skipped = shorter.
    int   outputHop = sequenceLen - overlapLen;
    float inputHop  = (float)outputHop / ratio;

    // Output is ~ratio × inputLen; one sequenceLen of headroom so the final
    // chunk has room to write (trimmed at the end).
    int outputLen = (int)((float)inputLen * ratio) + sequenceLen;
    std::vector<float> output(outputLen, 0.0f);

    // inputPos is fractional (inputHop is a float): accumulating fractions and
    // flooring keeps the average tempo accurate for non-integer hops.
    float inputPosFloat  = 0.0f;
    int   outputPos      = 0;

    // Scratch for the crossfade result — reused every iteration, no allocation
    // in the loop.
    std::vector<float> fadeBuffer(overlapLen, 0.0f);

    // First chunk: straight copy — there is nothing to crossfade with yet.
    {
        int firstCopyLen = std::min(sequenceLen, inputLen);
        for (int i = 0; i < firstCopyLen; ++i)
        {
            output[i] = input[i];
        }
        outputPos     = outputHop;
        inputPosFloat = inputHop;
    }

    // Every subsequent chunk is crossfaded onto the tail of the previous one.
    while (true)
    {
        int inputPos = (int)inputPosFloat;

        // Intelligent mode only: nudge inputPos to the best-correlating splice.
        if (seekRange > 0)
        {
            const float* prevTail = &output[outputPos - overlapLen];
            int offset = findBestOverlap(prevTail, input, inputLen,
                                         inputPos, seekRange, overlapLen);
            inputPos += offset;
        }

        if (inputPos < 0)                              break;
        if (inputPos + sequenceLen >= inputLen)        break;
        if (outputPos + sequenceLen >= (int)output.size()) break;

        // Crossfade the new chunk's head over the previous chunk's tail,
        // then write the blend back over that tail.
        const float* prevTail = &output[outputPos - overlapLen];
        const float* newHead  = &input[inputPos];

        crossfade(prevTail, newHead, fadeBuffer.data(), overlapLen);

        for (int i = 0; i < overlapLen; ++i)
        {
            output[outputPos - overlapLen + i] = fadeBuffer[i];
        }

        // Rest of the new chunk copies straight through (no fading).
        for (int i = 0; i < sequenceLen - overlapLen; ++i)
        {
            output[outputPos + i] = input[inputPos + overlapLen + i];
        }

        outputPos     += outputHop;     // constant step in the output buffer
        inputPosFloat += inputHop;      // tempo-dependent step in the input
    }

    // Trim the headroom padding: usable length = wherever the last write ended.
    output.resize(std::min((int)output.size(), outputPos));
    return output;
}

} // namespace sola
