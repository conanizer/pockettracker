/*
Copyright (c) 2023 Electrosmith, Corp, Sean Costello, Istvan Varga, Paul Batchelor

Use of this source code is governed by the LGPL V2.1
license that can be found in the LICENSE file or at
https://opensource.org/license/lgpl-2-1/
*/

#pragma once
#ifndef DSYSP_REVERBSC_H
#define DSYSP_REVERBSC_H

/* ⚠️ PT: `aux_` IS SIZED IN FLOATS, AND USED TO BE COUNTED IN BYTES.
 *
 * Upstream's 98936 is a byte budget, but `aux_` is `float[DSY_REVERBSC_MAX_SIZE]` and `Init` placed
 * each delay line at a float offset it had accumulated in bytes — so every line sat four times
 * further into the array than it needed and three quarters of a 396 KB member was padding between
 * them. `Init` now advances in floats, which is the unit `buf` is indexed in, and the array is the
 * count the eight lines really need.
 *
 * The three constants below are ONE fact: the size is what `kReverbParams` asks for at the highest
 * rate the lines are allowed to be built at AND at the deepest modulation they are allowed to be
 * driven at, and reverbsc.cpp static_asserts them against that table, so raising either without
 * regrowing the array — or the reverse — will not compile. `ReverbModule::MAX_SUPPORTED_RATE` reads
 * the rate from here rather than repeating it, and a device running faster than this gets its
 * reverb built at this rate instead (reverb-module.h). */
#define DSY_REVERBSC_MAX_RATE 48000.0f

/* ⚠️ PT: THE LINES ARE SIZED FOR THE DEEPEST MODULATION, NOT FOR THE ONE CURRENTLY SET.
 *
 * `i_pitch_mod_` scales how far each line's read head wanders from its nominal delay, and upstream
 * pinned it at 1 with no way to reach it. `SetPitchMod` opens it up to this ceiling — the MOD cell on
 * the EFFECTS screen — which means a line may be asked for a longer delay than it was built for the
 * moment the cell moves. Sizing every line at the ceiling once, at Init, is what makes that safe:
 * changing the modulation afterwards can never need a byte the buffer does not already hold.
 *
 * ⚠️ Growing the lines is SONICALLY TRANSPARENT and must stay so — a line's effective delay is
 * `buffer_size − read_pos`, which `InitDelayLine` sets from the delay TIME, and every index wraps
 * modulo the size. The extra room is dead air above the read head, not a longer reverb. */
#define DSY_REVERBSC_MAX_PITCHMOD 4.0f
#define DSY_REVERBSC_MAX_SIZE 26160

namespace daisysp
{
/**Delay line for internal reverb use
*/
typedef struct
{
    int    write_pos;         /**< write position */
    int    buffer_size;       /**< buffer size */
    int    read_pos;          /**< read position */
    int    read_pos_frac;     /**< fractional component of read pos */
    int    read_pos_frac_inc; /**< increment for fractional */
    int    dummy;             /**<  dummy var */
    int    seed_val;          /**< randseed */
    int    rand_line_cnt;     /**< number of random lines */
    float  filter_state;      /**< state of filter */
    float *buf;               /**< buffer ptr */
} ReverbScDl;

/** Stereo Reverb */
class ReverbSc
{
  public:
    ReverbSc() {}
    ~ReverbSc() {}
    /** Initializes the reverb module, and sets the sample_rate at which the Process function will be called.
        Returns 0 if all good, or 1 if it runs out of delay times exceed maximum allowed.
    */
    int Init(float sample_rate);

    /** Process the input through the reverb, and updates values of out1, and out2 with the new processed signal.
    */
    int Process(const float &in1, const float &in2, float *out1, float *out2);

    /** controls the reverb time. reverb tail becomes infinite when set to 1.0
        \param fb - sets reverb time. range: 0.0 to 1.0
    */
    inline void SetFeedback(const float &fb) { feedback_ = fb; }
    /** controls the internal dampening filter's cutoff frequency.
        \param freq - low pass frequency. range: 0.0 to sample_rate / 2
    */
    inline void SetLpFreq(const float &freq) { lpfreq_ = freq; }

    /** ⚠️ PT: how far the eight read heads wander, 0 to DSY_REVERBSC_MAX_PITCHMOD. Upstream fixed
        this at 1 inside Init and exposed nothing; 1 is still what Init leaves it at, so a caller
        that never touches it gets exactly the reverb that shipped.

        0 stops the wander entirely and the tail turns static and metallic; above 1 the heads swing
        wider and the tail takes on a chorus. ⚠️ It is read once per random line SEGMENT, not per
        sample, so a change lands over the following segment — up to about a second — rather than
        immediately. Clamped here because the delay lines are only sized to the ceiling. */
    inline void SetPitchMod(const float &mod)
    {
        i_pitch_mod_ = mod < 0.0f ? 0.0f
                                  : (mod > DSY_REVERBSC_MAX_PITCHMOD ? DSY_REVERBSC_MAX_PITCHMOD
                                                                     : mod);
    }

  private:
    void       NextRandomLineseg(ReverbScDl *lp, int n);
    int        InitDelayLine(ReverbScDl *lp, int n);
    float      feedback_, lpfreq_;
    float      i_sample_rate_, i_pitch_mod_, i_skip_init_;
    float      sample_rate_;
    float      damp_fact_;
    float      prv_lpfreq_;
    int        init_done_;
    ReverbScDl delay_lines_[8];
    float      aux_[DSY_REVERBSC_MAX_SIZE];
};


} // namespace daisysp
#endif
