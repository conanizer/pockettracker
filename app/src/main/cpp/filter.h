#pragma once
#include <cmath>

// ===================================
// BIQUAD FILTER COEFFICIENT CALCULATION
// ===================================
// Calculates resonant low-pass, high-pass, and band-pass filter coefficients
// Using Robert Bristow-Johnson's Audio EQ Cookbook formulas

inline void calculateBiquadCoeffs(
        int filterType,     // 0=off, 1=lp, 2=hp, 3=bp
        int cutParam,       // 0-255 (cutoff frequency parameter)
        int resParam,       // 0-255 (resonance parameter)
        float sampleRate,   // Audio sample rate (e.g., 44100 Hz)
        float& b0, float& b1, float& b2,  // Output: feedforward coefficients
        float& a1, float& a2              // Output: feedback coefficients
) {
    if (filterType == 0) {
        // Filter off: pass-through (unity gain)
        b0 = 1.0f; b1 = 0.0f; b2 = 0.0f;
        a1 = 0.0f; a2 = 0.0f;
        return;
    }

    // Map cutoff parameter (0-255) to frequency (20 Hz - 20 kHz)
    // Use exponential curve for musical feel
    float cutoff = 20.0f * powf(1000.0f, cutParam / 255.0f);  // 20 Hz to 20 kHz
    cutoff = fminf(cutoff, sampleRate * 0.45f);  // Limit to below Nyquist

    // Map resonance parameter (0-255) to Q factor (0.5 - 20.0)
    // Higher Q = sharper resonance peak
    float Q = 0.5f + (resParam / 255.0f) * 19.5f;  // 0.5 to 20.0

    // Calculate intermediate values
    float w0 = 2.0f * M_PI * cutoff / sampleRate;  // Angular frequency
    float cosw0 = cosf(w0);
    float sinw0 = sinf(w0);
    float alpha = sinw0 / (2.0f * Q);  // Bandwidth parameter

    // Calculate coefficients based on filter type
    float a0;  // Normalization coefficient

    if (filterType == 1) {
        // LOW-PASS filter
        b0 = (1.0f - cosw0) / 2.0f;
        b1 = 1.0f - cosw0;
        b2 = (1.0f - cosw0) / 2.0f;
        a0 = 1.0f + alpha;
        a1 = -2.0f * cosw0;
        a2 = 1.0f - alpha;
    } else if (filterType == 2) {
        // HIGH-PASS filter
        b0 = (1.0f + cosw0) / 2.0f;
        b1 = -(1.0f + cosw0);
        b2 = (1.0f + cosw0) / 2.0f;
        a0 = 1.0f + alpha;
        a1 = -2.0f * cosw0;
        a2 = 1.0f - alpha;
    } else if (filterType == 3) {
        // BAND-PASS filter (constant skirt gain)
        b0 = alpha;
        b1 = 0.0f;
        b2 = -alpha;
        a0 = 1.0f + alpha;
        a1 = -2.0f * cosw0;
        a2 = 1.0f - alpha;
    } else {
        // Unknown type, pass-through
        b0 = 1.0f; b1 = 0.0f; b2 = 0.0f;
        a1 = 0.0f; a2 = 0.0f;
        return;
    }

    // Normalize coefficients by a0
    b0 /= a0;
    b1 /= a0;
    b2 /= a0;
    a1 /= a0;
    a2 /= a0;
}
