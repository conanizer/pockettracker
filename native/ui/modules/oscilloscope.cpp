#include "ui/modules/oscilloscope.h"

#include <algorithm>  // std::max — MSVC pulls it in transitively, libstdc++ does not

#include "ui/helpers.h"

namespace pt::ui {

namespace {
inline float clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }
}  // namespace

void OscilloscopeModule::draw(Canvas& c, int x, int y, const OscilloscopeState& s) {
    const Theme& t = s.theme;

    // The ProTracker look: OCTA fills the strip with `background` so the gaps BETWEEN lanes read as
    // background, then paints each lane's own `vizBackground` panel. Every other mode fills the whole
    // strip with `vizBackground`.
    const bool isOcta =
        (t.visualizerType == VisualizerType::OCTA || t.visualizerType == VisualizerType::OCTA_FULL);
    c.fill_rect(x, y, WIDTH, HEIGHT, isOcta ? t.background : t.vizBackground);

    switch (t.visualizerType) {
        case VisualizerType::SCOPE:
            draw_scope(c, x, y, s.waveform, t);
            break;
        case VisualizerType::FLAT:
            draw_flat(c, x, y, t);
            break;
        case VisualizerType::OCTA:
        case VisualizerType::OCTA_FULL:
            draw_octa(c, x, y, s.trackWaveforms, s.activeTrackMask, t);
            break;
        case VisualizerType::SPECTRUM:
            draw_bar_amps(c, x, y, s.spectrum, t, /*peak_mode=*/false);
            break;
        case VisualizerType::SPECTRUM_PEAKS:
            draw_bar_amps(c, x, y, s.spectrum, t, /*peak_mode=*/true);
            break;
    }
}

void OscilloscopeModule::draw_scope(Canvas& c, int x, int y, const float* wave,
                                    const Theme& t) const {
    const int centerY      = y + HEIGHT / 2;
    const int maxAmplitude = (HEIGHT / 2) - 4;

    c.fill_rect(x, centerY, WIDTH, 1, t.vizCenterLine);
    draw_wave_dots(c, x, WIDTH, wave, centerY, maxAmplitude, t.vizWave);
}

void OscilloscopeModule::draw_wave_dots(Canvas& c, int scope_x, int scope_w, const float* wave,
                                        int center_y, int max_amplitude, Argb color) const {
    for (int i = 0; i < scope_w; ++i) {
        // A null buffer is silence — every dot lands on the centre line, which is exactly what the
        // Android app draws when nothing is playing.
        const float sample =
            wave ? clampf(wave[(static_cast<long long>(i) * WAVEFORM_SIZE) / scope_w] * WAVEFORM_GAIN,
                          -1.0f, 1.0f)
                 : 0.0f;
        const int dotY = center_y + static_cast<int>(sample * static_cast<float>(max_amplitude));
        c.fill_rect(scope_x + i, dotY, 1, 1, color);
    }
}

void OscilloscopeModule::draw_flat(Canvas& c, int x, int y, const Theme& t) const {
    c.fill_rect(x, y + HEIGHT - 1, WIDTH, 1, t.vizCenterLine);
}

void OscilloscopeModule::draw_octa(Canvas& c, int x, int y, const float* tracks, int mask,
                                   const Theme& t) const {
    const int centerY      = y + HEIGHT / 2;
    const int maxAmplitude = (HEIGHT / 2) - 4;

    if (!tracks || mask == 0) {
        // Nothing scheduled: one full-width panel and a centre line (the strip itself is background).
        c.fill_rect(x, y, WIDTH, HEIGHT, t.vizBackground);
        c.fill_rect(x, centerY, WIDTH, 1, t.vizCenterLine);
        return;
    }

    // Lanes 0-7 are the song tracks; lane 8 is the preview (set only when stopped). OCTA_FULL forces
    // all eight on regardless of what is scheduled, so the layout never reflows mid-song.
    int activeTracks[TRACK_WAVEFORM_COUNT];
    int count = 0;
    for (int i = 0; i < TRACK_WAVEFORM_COUNT; ++i) {
        if ((mask >> i) & 1) activeTracks[count++] = i;
    }
    if (count == 0) return;

    const int totalGap = OCTA_TRACK_GAP * (count - 1);
    const int trackW   = (WIDTH - totalGap) / count;

    for (int idx = 0; idx < count; ++idx) {
        const int    scopeX = x + idx * (trackW + OCTA_TRACK_GAP);
        const float* wave   = tracks + static_cast<size_t>(activeTracks[idx]) * WAVEFORM_SIZE;

        c.fill_rect(scopeX, y, trackW, HEIGHT, t.vizBackground);
        c.fill_rect(scopeX, centerY, trackW, 1, t.vizCenterLine);
        draw_wave_dots(c, scopeX, trackW, wave, centerY, maxAmplitude, t.vizWave);
    }
}

void OscilloscopeModule::draw_bar_amps(Canvas& c, int x, int y, const float* amps, const Theme& t,
                                       bool peak_mode) {
    const int   barBottom = y + HEIGHT - 2;                     // 2px bottom margin
    const float maxAmp    = static_cast<float>(HEIGHT - 4);     // 2px top + 2px bottom

    for (int i = 0; i < NUM_BARS; ++i) {
        const int   barX = x + BAR_START_OFFSET + i * (BAR_W + BAR_GAP);
        const float raw  = amps ? amps[i] : 0.0f;

        // Instant attack, exponential decay — a bar that fell as fast as the audio would strobe.
        if (raw > barSmoothed_[i]) barSmoothed_[i] = raw;
        else                       barSmoothed_[i] = std::max(0.0f, barSmoothed_[i] * BAR_DECAY);

        const int barH = static_cast<int>(barSmoothed_[i] * maxAmp);

        for (int dy = 0; dy + SEGMENT_H <= barH; dy += SEG_STEP) {
            c.fill_rect(barX, barBottom - dy - SEGMENT_H + 1, BAR_W, SEGMENT_H, t.vizWave);
        }

        if (!peak_mode) continue;

        if (barSmoothed_[i] > peakValues_[i]) {
            peakValues_[i]        = barSmoothed_[i];
            peakDecayCounters_[i] = 0;
        } else {
            peakDecayCounters_[i]++;
            if (peakDecayCounters_[i] > PEAK_HOLD_FRAMES) {
                peakValues_[i] =
                    std::max(0.0f, peakValues_[i] - static_cast<float>(SEG_STEP) / maxAmp);
            }
        }

        // Snap the dot to a segment boundary, and only draw it once it has cleared the bar itself.
        const int peakH  = static_cast<int>(peakValues_[i] * maxAmp);
        const int peakDy = (peakH / SEG_STEP) * SEG_STEP;
        if (peakDy > barH) {
            c.fill_rect(barX, barBottom - peakDy - SEGMENT_H + 1, BAR_W, SEGMENT_H,
                        darken(t.vizWave, 0.55f));
        }
    }
}

}  // namespace pt::ui
