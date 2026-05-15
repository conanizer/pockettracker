/* transient-detector.cpp
 * AudioEngine::detectTransients — spectral flux onset detection via KissFFT.
 * Called from JNI for the TRANSIENT slice mode in the sample editor.
 */
#include "audio-engine.h"
#include "kissfft/kiss_fftr.h"
#include <vector>
#include <cmath>
#include <algorithm>

static const int FFT_SIZE = 1024;
static const int HOP_SIZE = 256;

int AudioEngine::detectTransients(int id, int sensitivity, int* outMarkers, int maxMarkers) {
    if (id < 0 || id >= 256 || !samples[id] || maxMarkers <= 0) return 0;

    const int length = sampleLengths[id];
    if (length < FFT_SIZE) return 0;

    const int numHops = (length - FFT_SIZE) / HOP_SIZE;
    if (numHops < 2) return 0;

    /* Hann window */
    std::vector<float> window(FFT_SIZE);
    for (int i = 0; i < FFT_SIZE; i++) {
        window[i] = 0.5f * (1.0f - cosf(2.0f * (float)M_PI * i / FFT_SIZE));
    }

    kiss_fftr_cfg cfg = kiss_fftr_alloc(FFT_SIZE, 0, nullptr, nullptr);
    if (!cfg) return 0;

    const int numBins = FFT_SIZE / 2 + 1;
    std::vector<float>        timeInput(FFT_SIZE);
    std::vector<kiss_fft_cpx> spectrum(numBins);
    std::vector<float>        prevMags(numBins, 0.0f);
    std::vector<float>        flux(numHops, 0.0f);

    for (int hop = 0; hop < numHops; hop++) {
        int offset = hop * HOP_SIZE;

        /* Apply Hann window */
        for (int i = 0; i < FFT_SIZE; i++) {
            timeInput[i] = samples[id][offset + i] * window[i];
        }

        /* Forward real FFT */
        kiss_fftr(cfg, timeInput.data(), spectrum.data());

        /* Half-wave rectified spectral flux */
        float fluxVal = 0.0f;
        for (int b = 0; b < numBins; b++) {
            float mag = sqrtf(spectrum[b].r * spectrum[b].r + spectrum[b].i * spectrum[b].i);
            float diff = mag - prevMags[b];
            if (diff > 0.0f) fluxVal += diff;
            prevMags[b] = mag;
        }
        flux[hop] = fluxVal;
    }

    kiss_fftr_free(cfg);

    /* Compute mean flux for adaptive threshold. */
    float meanFlux = 0.0f;
    for (float f : flux) meanFlux += f;
    meanFlux /= (float)numHops;

    if (meanFlux <= 0.0f) return 0;

    /* Threshold factor: sensitivity 0x00 → 3.0× mean (few markers),
                         sensitivity 0xFF → 0.3× mean (many markers). */
    float factor    = 3.0f - (sensitivity / 255.0f) * 2.7f;
    float threshold = meanFlux * factor;

    /* Minimum distance between peaks: ~58 ms at 44100 Hz = ~10 hops of 256 frames. */
    const int MIN_HOP_DIST = 10;

    int markerCount = 0;
    int lastPeakHop = -MIN_HOP_DIST - 1;

    for (int hop = 1; hop < numHops - 1; hop++) {
        if (flux[hop] > threshold &&
            flux[hop] > flux[hop - 1] &&
            flux[hop] > flux[hop + 1] &&
            (hop - lastPeakHop) >= MIN_HOP_DIST) {

            outMarkers[markerCount++] = hop * HOP_SIZE + HOP_SIZE / 2;
            lastPeakHop = hop;

            if (markerCount >= maxMarkers) break;
        }
    }

    return markerCount;
}
