// =============================================================================
// sola-stretch.h
//
// SOLA (Synchronized Overlap-Add) time-stretching, Akai-cyclic flavor.
//
// WHAT THIS DOES
// --------------
// Given an input sample buffer and a "stretch ratio", produces a new buffer
// of audio that PLAYS BACK FOR A DIFFERENT DURATION but at the ORIGINAL PITCH.
// Stretch ratio is "outputLength / inputLength":
//   - ratio = 1.0  -> no change
//   - ratio = 2.0  -> output is twice as long (slowed to half tempo)
//   - ratio = 0.5  -> output is half as long  (sped up to 2x tempo)
//
// WHY SOLA AND NOT FFT
// --------------------
// 1. No FFT library needed -> works anywhere, fits the Miyoo Flip CPU budget.
// 2. The "cyclic" SOLA mode is literally what Akai S950/S1000 used. The
//    characteristic gritty texture that defines jungle breakbeats IS the
//    by-product of this algorithm. Modern, cleaner algorithms can't fake it.
//
// THE CORE IDEA IN PLAIN ENGLISH
// ------------------------------
// Cut the input audio into small chunks (about 40 ms each). To slow audio
// down, repeat each chunk a bit. To speed it up, skip a bit between chunks.
// Where the chunks join, fade one out while fading the next in (this is the
// "overlap-add" part). That's it. That's the algorithm.
//
// HOW TO USE IT
// -------------
//   std::vector<float> stretched = sola::stretch(
//       inputBuffer,            // const float* (mono input)
//       inputLength,            // int (frames)
//       2.0f,                   // float ratio  -> 2x longer / half tempo
//       44100.0f);              // float sample rate
//
//   // For stereo: call stretch() twice, once per channel, with the SAME
//   // random seed inside (we don't use randomness here, so they stay in sync
//   // automatically).
// =============================================================================

#pragma once

#include <vector>     // std::vector for the output buffer
#include <cstddef>    // size_t
#include <cmath>      // fmaxf, fminf, cosf, M_PI
#include <algorithm>  // std::min, std::max

namespace sola {

// ----------------------------------------------------------------------------
// Tuning constants. These are the "Akai feel" knobs.
//
// SEQUENCE_MS:  how long each chunk of audio is, in milliseconds.
//   - Smaller = grittier, choppier, more "1990s sampler" character.
//   - Larger  = smoother but can introduce slap-back / phasing artifacts.
//   - Akai S1000 hovered around 30-50 ms. 40 ms is a great default.
//
// OVERLAP_MS:   how long the crossfade between chunks is.
//   - Roughly 25-50% of SEQUENCE_MS is the sweet spot.
//   - Too small -> audible clicks at every splice (the dreaded "tic-tic-tic").
//   - Too large -> mushy, comb-filtered sound.
//
// SEEK_MS:      how far around the "ideal" splice point we look for a better
//               splice point. Set to 0 to get the pure cyclic Akai sound.
//               Set to ~10-15 ms to upgrade to "intelligent" mode (smoother).
// ----------------------------------------------------------------------------
static constexpr float SEQUENCE_MS = 40.0f;  // chunk length
static constexpr float OVERLAP_MS  = 15.0f;  // crossfade length
static constexpr float SEEK_MS     = 0.0f;   // 0 = pure cyclic / Akai mode


// ----------------------------------------------------------------------------
// crossfade()
//
// Mixes "prevTail" into "newHead" using an equal-power crossfade (cosine
// curve). After this function runs, `out[i]` will smoothly transition from
// "prevTail" to "newHead" across `len` samples.
//
// Equal-power (cosine) crossfades sound better than linear ones for audio
// because the perceived loudness stays constant. Linear crossfades dip in
// volume in the middle.
// ----------------------------------------------------------------------------
inline void crossfade(const float* prevTail,  // samples fading OUT (chunk ending)
                      const float* newHead,   // samples fading IN  (chunk starting)
                      float*       out,       // result of the fade
                      int          len)       // crossfade length in samples
{
    // Loop over every sample inside the crossfade region.
    for (int i = 0; i < len; ++i)
    {
        // `t` goes from 0.0 (start of fade) to 1.0 (end of fade) linearly.
        // We add 1 to len to avoid t=1.0 exactly on the last sample, which
        // makes the fade end *just* before fully cutting prevTail.
        float t = (float)i / (float)(len);

        // Equal-power crossfade gains.
        // At t=0:   fadeOut=1, fadeIn=0   -> all prevTail
        // At t=0.5: fadeOut≈0.707, fadeIn≈0.707  -> equal mix (constant RMS)
        // At t=1:   fadeOut=0, fadeIn=1   -> all newHead
        float fadeOut = cosf(t * 0.5f * (float)M_PI);
        float fadeIn  = sinf(t * 0.5f * (float)M_PI);

        // Combine the two faded signals into a single output sample.
        out[i] = prevTail[i] * fadeOut + newHead[i] * fadeIn;
    }
}


// ----------------------------------------------------------------------------
// findBestOverlap()
//
// "Intelligent" SOLA: looks within a small window around `targetPos` for the
// position whose samples best match the tail of the previous output chunk.
// Matching is done by correlation (a simple dot product of normalized
// signals). Returns the offset of the best splice point.
//
// If SEEK_MS == 0 (cyclic mode), this function isn't called -> pure Akai.
//
// You only need this when you want the smoother "intelligent" mode.
// I include it for completeness but the default (cyclic) skips it.
// ----------------------------------------------------------------------------
inline int findBestOverlap(const float* prevTail,    // end of previous output chunk
                           const float* input,       // entire input buffer
                           int          inputLen,    // length of input
                           int          targetPos,   // ideal splice position
                           int          seekRange,   // ± samples to search
                           int          overlapLen)  // crossfade length
{
    // Best correlation found so far and where it was found.
    float bestCorr   = -1e30f;
    int   bestOffset = 0;

    // Limit the search so we never read past the end of the buffer.
    int seekMin = -seekRange;
    int seekMax =  seekRange;
    if (targetPos + seekMin                 < 0)        seekMin = -targetPos;
    if (targetPos + seekMax + overlapLen >= inputLen)   seekMax =  inputLen - targetPos - overlapLen - 1;

    // Try every candidate offset within the seek window.
    for (int offset = seekMin; offset <= seekMax; ++offset)
    {
        // Compute the dot product (correlation) between prevTail and the
        // candidate region of the input. The higher this value, the more
        // similar the two waveforms are -> the smoother the splice will be.
        float corr = 0.0f;
        for (int i = 0; i < overlapLen; ++i)
        {
            corr += prevTail[i] * input[targetPos + offset + i];
        }

        // Keep track of the offset with the highest correlation.
        if (corr > bestCorr)
        {
            bestCorr   = corr;
            bestOffset = offset;
        }
    }
    return bestOffset;
}


// ----------------------------------------------------------------------------
// stretch()  -- THE MAIN ENTRY POINT
//
// Time-stretches a mono input buffer by `ratio`. Returns a freshly allocated
// std::vector<float>. This is an OFFLINE operation: feed it a whole sample,
// get back a whole stretched sample. Perfect for your existing destructive
// "APPLY FX" pipeline in SampleEditorModule.
//
// Parameters
//   input        : pointer to the source samples (mono, float, -1..+1).
//   inputLen     : how many samples are in `input`.
//   ratio        : outputLen / inputLen. >1 makes it longer (slower tempo),
//                  <1 makes it shorter (faster tempo). 1.0 returns a copy.
//   sampleRate   : the input's sample rate (use 44100.0f for your engine).
//
// Returns
//   A std::vector<float> containing the stretched audio.
// ----------------------------------------------------------------------------
inline std::vector<float> stretch(const float* input,
                                  int          inputLen,
                                  float        ratio,
                                  float        sampleRate)
{
    // ------------------------------------------------------------------------
    // 1) Handle the trivial / invalid cases first.
    // ------------------------------------------------------------------------
    // If the user asked for ratio == 1 (or close to it) just copy the input
    // and return — no DSP needed, no risk of artifacts.
    if (ratio > 0.999f && ratio < 1.001f) {
        return std::vector<float>(input, input + inputLen);
    }
    // Clamp to a safe-ish range. Beyond 4x stretching or 0.25x compression,
    // SOLA artifacts become very obvious. Clamping protects against bad input.
    if (ratio < 0.1f)  ratio = 0.1f;
    if (ratio > 10.0f) ratio = 10.0f;

    // ------------------------------------------------------------------------
    // 2) Convert our time-domain tuning constants (ms) into sample counts.
    // ------------------------------------------------------------------------
    // sampleRate is in samples-per-second. Multiplying by seconds gives us
    // samples. (ms / 1000) converts milliseconds into seconds.
    int sequenceLen = (int)(SEQUENCE_MS * 0.001f * sampleRate); // ~1764 at 44.1k
    int overlapLen  = (int)(OVERLAP_MS  * 0.001f * sampleRate); // ~661 at 44.1k
    int seekRange   = (int)(SEEK_MS     * 0.001f * sampleRate); // 0 in cyclic mode

    // Safety: overlap must not be larger than the chunk itself.
    if (overlapLen >= sequenceLen) overlapLen = sequenceLen / 2;

    // ------------------------------------------------------------------------
    // 3) Compute the hop sizes.
    //
    // Two "hops":
    //   * outputHop: how far we advance in the OUTPUT buffer after writing
    //                each chunk. This is constant.
    //   * inputHop : how far we advance in the INPUT buffer after each chunk.
    //                THIS is what creates the time-stretch: if we walk through
    //                the input slower than the output, the result is longer.
    //
    // Relation:  inputHop = outputHop / ratio.
    //   - ratio > 1 (slowdown): inputHop < outputHop -> we re-read material
    //                             we already used = repetition = longer output.
    //   - ratio < 1 (speedup) : inputHop > outputHop -> we skip ahead = shorter.
    //
    // The "non-overlapping" part of each chunk has length (sequenceLen - overlapLen).
    // That's what we treat as the hop on the output side.
    // ------------------------------------------------------------------------
    int   outputHop = sequenceLen - overlapLen;
    float inputHop  = (float)outputHop / ratio;

    // ------------------------------------------------------------------------
    // 4) Allocate the output buffer to the predicted final length.
    // ------------------------------------------------------------------------
    // The output is approximately ratio * inputLen samples long. We add a
    // sequenceLen of padding so the final chunk has room to write into.
    int outputLen = (int)((float)inputLen * ratio) + sequenceLen;
    std::vector<float> output(outputLen, 0.0f);

    // ------------------------------------------------------------------------
    // 5) The main loop. We walk through the input and output buffers in
    //    lockstep, copying chunks with crossfades between them.
    // ------------------------------------------------------------------------
    // `inputPos` is fractional because inputHop is a float — we accumulate
    // fractional positions and floor() to get a whole index. This keeps the
    // average tempo accurate even when the hop isn't a whole number of samples.
    float inputPosFloat  = 0.0f;
    int   outputPos      = 0;

    // We need a scratch buffer to hold the crossfade result. Reusing it
    // every iteration avoids allocations in the hot loop.
    std::vector<float> fadeBuffer(overlapLen, 0.0f);

    // First chunk: just copy it straight through, no crossfade yet (there's
    // nothing to fade with — output is empty).
    {
        int firstCopyLen = std::min(sequenceLen, inputLen);
        for (int i = 0; i < firstCopyLen; ++i)
        {
            output[i] = input[i];
        }
        outputPos     = outputHop;       // next chunk's write position
        inputPosFloat = inputHop;        // next chunk's read position
    }

    // From here on, every new chunk is crossfaded onto the tail of the
    // previously-written chunk.
    while (true)
    {
        // Round the fractional input position down to an integer index.
        int inputPos = (int)inputPosFloat;

        // If the (intelligent-mode) seek is enabled, nudge inputPos to the
        // splice point that correlates best with the tail of what we've
        // already written. Skipped entirely in cyclic mode (seekRange == 0).
        if (seekRange > 0)
        {
            // The "tail" of the existing output is the last `overlapLen`
            // samples we wrote. Compare those against candidate positions.
            const float* prevTail = &output[outputPos - overlapLen];
            int offset = findBestOverlap(prevTail, input, inputLen,
                                         inputPos, seekRange, overlapLen);
            inputPos += offset;
        }

        // Make sure we don't read past the end of the input.
        if (inputPos < 0)                              break;
        if (inputPos + sequenceLen >= inputLen)        break;
        if (outputPos + sequenceLen >= (int)output.size()) break;

        // --- Crossfade region (first overlapLen samples of the new chunk) ---
        // `prevTail` is the audio we already wrote that's about to fade out.
        // `newHead`  is the start of the new chunk that's about to fade in.
        // The result of the crossfade overwrites the existing tail in `output`.
        const float* prevTail = &output[outputPos - overlapLen];
        const float* newHead  = &input[inputPos];

        crossfade(prevTail, newHead, fadeBuffer.data(), overlapLen);

        // Copy the crossfaded samples back into the output at the splice point.
        // We're overwriting the previous chunk's tail with the smooth blend.
        for (int i = 0; i < overlapLen; ++i)
        {
            output[outputPos - overlapLen + i] = fadeBuffer[i];
        }

        // --- Post-crossfade region (the rest of the new chunk, no fading) ---
        // Copy the remaining (sequenceLen - overlapLen) samples of the new
        // chunk directly into the output, after where the crossfade ended.
        for (int i = 0; i < sequenceLen - overlapLen; ++i)
        {
            output[outputPos + i] = input[inputPos + overlapLen + i];
        }

        // --- Advance our cursors for the next iteration. ---
        outputPos     += outputHop;     // constant step in the output buffer
        inputPosFloat += inputHop;      // tempo-dependent step in the input
    }

    // ------------------------------------------------------------------------
    // 6) Trim trailing silence / padding before returning to the caller.
    //    Our `outputLen` estimate above included a chunk of headroom; the
    //    actual usable length is wherever the last successful write ended.
    // ------------------------------------------------------------------------
    output.resize(std::min((int)output.size(), outputPos));
    return output;
}


// ----------------------------------------------------------------------------
// stretchStereo()
//
// Convenience wrapper for stereo samples. Calls stretch() once per channel
// with the SAME parameters. Because we don't use randomness anywhere, both
// channels stay perfectly phase-aligned — no stereo image drift.
//
// outL/outR will be filled with the stretched left/right channels.
// ----------------------------------------------------------------------------
inline void stretchStereo(const float*           inL,
                          const float*           inR,
                          int                    inputLen,
                          float                  ratio,
                          float                  sampleRate,
                          std::vector<float>&    outL,
                          std::vector<float>&    outR)
{
    outL = stretch(inL, inputLen, ratio, sampleRate);
    outR = stretch(inR, inputLen, ratio, sampleRate);

    // Both channels should produce identical lengths (same algorithm, same
    // params, deterministic), but just in case rounding diverges by one
    // sample, snap them to the shorter length.
    size_t shared = std::min(outL.size(), outR.size());
    outL.resize(shared);
    outR.resize(shared);
}

} // namespace sola
