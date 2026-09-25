/*
Copyright (c) 2020 Electrosmith, Corp

Use of this source code is governed by an MIT-style
license that can be found in the LICENSE file or at
https://opensource.org/licenses/MIT.
*/

#pragma once
#ifndef DSY_DELAY_H
#define DSY_DELAY_H
#include <stdlib.h>
#include <stdint.h>
namespace daisysp
{
/** Simple Delay line.
November 2019

Converted to Template December 2019

declaration example: (1 second of floats)

DelayLine<float, SAMPLE_RATE> del;

By: shensley
*/
template <typename T, size_t max_size>
class DelayLine
{
  public:
    DelayLine() {}
    ~DelayLine() {}
    /** initializes the delay line by clearing the values within, and setting delay to 1 sample.
    */
    void Init() { Reset(); }
    /** clears buffer, sets write ptr to 0, and delay to 1 sample.
    */
    void Reset()
    {
        for(size_t i = 0; i < max_size; i++)
        {
            line_[i] = T(0);
        }
        write_ptr_ = 0;
        delay_     = 1;
    }

    /** sets the delay time in samples
        If a float is passed in, a fractional component will be calculated for interpolating the delay line.
    */
    inline void SetDelay(size_t delay)
    {
        frac_  = 0.0f;
        delay_ = delay < max_size ? delay : max_size - 1;
    }

    /** sets the delay time in samples
        If a float is passed in, a fractional component will be calculated for interpolating the delay line.
    */
    inline void SetDelay(float delay)
    {
        int32_t int_delay = static_cast<int32_t>(delay);
        frac_             = delay - static_cast<float>(int_delay);
        delay_ = static_cast<size_t>(int_delay) < max_size ? int_delay
                                                           : max_size - 1;
    }

    /** writes the sample of type T to the delay line, and advances the write ptr
    */
    inline void Write(const T sample)
    {
        line_[write_ptr_] = sample;
        write_ptr_        = write_ptr_ == 0 ? max_size - 1 : write_ptr_ - 1;
    }

    /** returns the next sample of type T in the delay line, interpolated if necessary.
    */
    inline const T Read() const
    {
        const size_t i = Wrap(write_ptr_ + delay_);
        T a = line_[i];
        T b = line_[Wrap(i + 1)];
        return a + (b - a) * frac_;
    }

    /** Read from a set location */
    inline const T Read(float delay) const
    {
        int32_t delay_integral   = static_cast<int32_t>(delay);
        float   delay_fractional = delay - static_cast<float>(delay_integral);
        const size_t i = Wrap(write_ptr_ + delay_integral);
        const T a = line_[i];
        const T b = line_[Wrap(i + 1)];
        return a + (b - a) * delay_fractional;
    }

    // ⚠️ `delay` must lie in [0, max_size): the index is wrapped once, not reduced.
    inline const T ReadHermite(float delay) const
    {
        int32_t delay_integral   = static_cast<int32_t>(delay);
        float   delay_fractional = delay - static_cast<float>(delay_integral);

        const size_t t    = Wrap(write_ptr_ + delay_integral);
        const T     xm1   = line_[t == 0 ? max_size - 1 : t - 1];
        const T     x0    = line_[t];
        const T     x1    = line_[Wrap(t + 1)];
        const T     x2    = line_[Wrap(t + 2)];
        const float c     = (x1 - xm1) * 0.5f;
        const float v     = x0 - x1;
        const float w     = c + v;
        const float a     = w + v + (x2 - x0) * 0.5f;
        const float b_neg = w + a;
        const float f     = delay_fractional;
        return (((a * f) - b_neg) * f + c) * f + x0;
    }

    inline const T Allpass(const T sample, size_t delay, const T coefficient)
    {
        T read  = line_[Wrap(write_ptr_ + delay)];
        T write = sample + coefficient * read;
        Write(write);
        return -write * coefficient + read;
    }

  private:
    // One compare instead of a `%` per tap: every index here is below 2 × max_size.
    static inline size_t Wrap(size_t i) { return i >= max_size ? i - max_size : i; }

    float  frac_;
    size_t write_ptr_;
    size_t delay_;
    T      line_[max_size];
};
} // namespace daisysp
#endif
