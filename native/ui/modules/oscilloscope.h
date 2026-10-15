#pragma once

// The 620×70 visualizer strip, six modes chosen by the theme. Stateful (peak dots, bar decay), so
// it lives in the layout across frames. Reads no engine: samples are handed in; null is silence.

#include "ui/canvas.h"
#include "ui/theme.h"

namespace pt::ui {

// AudioEngine::WAVEFORM_SIZE.
inline constexpr int WAVEFORM_SIZE = 620;
// 8 song tracks + the preview lane.
inline constexpr int TRACK_WAVEFORM_COUNT = 9;
// Previews play here, never on a song track's scope.
inline constexpr int PREVIEW_LANE = 8;

struct OscilloscopeState {
    const float* waveform = nullptr;

    // TRACK_WAVEFORM_COUNT × WAVEFORM_SIZE, flat (`AudioEngine::getTrackWaveforms`).
    const float* trackWaveforms = nullptr;

    // Which OCTA lanes to draw: bits 0-7 the tracks with a note this phrase; bit 8 the preview lane,
    // set only while stopped.
    int activeTrackMask = 0;

    // NUM_BARS log-spaced magnitudes (0..1), or null.
    const float* spectrum = nullptr;

    Theme theme = theme_classic();
};

class OscilloscopeModule {
public:
    static constexpr int WIDTH  = 620;
    static constexpr int HEIGHT = 70;

    static constexpr float WAVEFORM_GAIN = 3.0f;

    static constexpr int NUM_BARS = 40;
    static constexpr int BAR_W    = 14;
    static constexpr int BAR_GAP  = 1;
    // 10px margin each side within 620.
    static constexpr int BAR_START_OFFSET = 10;

    static constexpr int PEAK_HOLD_FRAMES = 30;

    // SPECTRUM: LED-style segments.
    static constexpr int SEGMENT_H = 2;
    static constexpr int SEG_GAP   = 1;
    static constexpr int SEG_STEP  = SEGMENT_H + SEG_GAP;  // 3px per LED cell

    // 2px margin top and bottom.
    static constexpr float BAR_AMP_MAX = static_cast<float>(HEIGHT - 4);

    // Instant attack, exponential decay.
    static constexpr float BAR_DECAY = 0.90f;

    static constexpr int OCTA_TRACK_GAP = 10;

    // The chunky SCOPE/OCTA look: N×N blocks on an N-px grid, and a time zoom of Z (the centred
    // WAVEFORM_SIZE/Z samples). The SPECTRUM modes ignore both.
    static constexpr int SCOPE_PIXEL_BLOCK = 2;  // N
    static constexpr int SCOPE_TIME_ZOOM   = 2;  // Z

    // Non-const: peak-hold and decay advance one frame per call.
    void draw(Canvas& c, int x, int y, const OscilloscopeState& s);

    // ⚠️ The idle gate must ask: the bars fall inside `draw`, so once it stops drawing they would
    // freeze. SPECTRUM modes only — the caller gates on the type.
    bool bars_at_rest() const;

private:
    void draw_scope(Canvas& c, int x, int y, const float* wave, const Theme& t) const;
    void draw_flat(Canvas& c, int x, int y, const Theme& t) const;
    void draw_octa(Canvas& c, int x, int y, const float* tracks, int mask, const Theme& t) const;
    void draw_bar_amps(Canvas& c, int x, int y, const float* amps, const Theme& t, bool peak_mode);

    // One waveform as pixel dots, one fill per column; SCOPE and each OCTA lane share it.
    void draw_wave_dots(Canvas& c, int scope_x, int scope_w, const float* wave, int center_y,
                        int max_amplitude, Argb color) const;

    float peakValues_[NUM_BARS]        = {};
    int   peakDecayCounters_[NUM_BARS] = {};
    float barSmoothed_[NUM_BARS]       = {};
};

}  // namespace pt::ui
