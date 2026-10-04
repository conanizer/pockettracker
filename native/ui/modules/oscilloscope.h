#pragma once

// ─── OSCILLOSCOPE / VISUALIZER ───────────────────────────────────────────────────────────────────
//
// The 620×70 strip across the top, in six modes (SCOPE / FLAT / OCTA / OCTA_FULL / SPECTRUM /
// SPECTRUM_PEAKS) chosen by the theme.
//
// STATEFUL, which is why `TrackerLayout::draw` is not const: the peak-hold dots and the bars' decay
// depend on the previous frame, so the instance lives in the layout across frames.
//
// It reads no engine: the caller hands in captured samples (engine_feed.h). A null pointer is silence.

#include "ui/canvas.h"
#include "ui/theme.h"

namespace pt::ui {

/** Frames the engine hands over per lane — AudioEngine::WAVEFORM_SIZE. */
inline constexpr int WAVEFORM_SIZE = 620;
/** 8 song tracks + the preview lane — AudioEngine::TRACK_WAVEFORM_COUNT. */
inline constexpr int TRACK_WAVEFORM_COUNT = 9;
/** The lane every preview plays on, so it never lands on a song track's scope. */
inline constexpr int PREVIEW_LANE = 8;

struct OscilloscopeState {
    /** WAVEFORM_SIZE master samples, or null for silence. */
    const float* waveform = nullptr;

    /**
     * TRACK_WAVEFORM_COUNT × WAVEFORM_SIZE, flat, as `AudioEngine::getTrackWaveforms` fills it — lane
     * N starts at `trackWaveforms[N * 620]`.
     */
    const float* trackWaveforms = nullptr;

    /**
     * Which OCTA lanes to draw. Bits 0-7 are the song tracks that have had a note scheduled this
     * phrase (`SongcoreHost::track_mask`); bit 8 is the preview lane, and it is only ever set while
     * STOPPED — during playback a preview scope would crowd the eight that matter.
     */
    int activeTrackMask = 0;

    /** NUM_BARS log-spaced magnitudes (0..1), or null. */
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
    /** 40×14 + 39 = 599px of bars, 10px of margin each side within 620. */
    static constexpr int BAR_START_OFFSET = 10;

    static constexpr int PEAK_HOLD_FRAMES = 30;

    // SPECTRUM: LED-style segments.
    static constexpr int SEGMENT_H = 2;
    static constexpr int SEG_GAP   = 1;
    static constexpr int SEG_STEP  = SEGMENT_H + SEG_GAP;  // 3px per LED cell

    /** A full-scale bar, in pixels — 2px of top margin and 2px of bottom. */
    static constexpr float BAR_AMP_MAX = static_cast<float>(HEIGHT - 4);

    /** Instant attack, exponential decay (~333 ms fall at 60 fps). */
    static constexpr float BAR_DECAY = 0.90f;

    static constexpr int OCTA_TRACK_GAP = 10;

    // SCOPE / OCTA / OCTA_FULL "chunky" look, two knobs in draw_wave_dots:
    //   N — one N×N block per N horizontal pixels, snapped to an N-px grid.
    //   Z — time zoom: only the centred WAVEFORM_SIZE/Z samples, stretched across the strip.
    // The SPECTRUM modes use draw_bar_amps and ignore both.
    static constexpr int SCOPE_PIXEL_BLOCK = 2;  // N
    static constexpr int SCOPE_TIME_ZOOM   = 2;  // Z

    /** Non-const: the peak-hold and bar-decay state advance one frame per call. */
    void draw(Canvas& c, int x, int y, const OscilloscopeState& s);

    /**
     * Are the SPECTRUM bars and their peak dots all the way down?
     *
     * ⚠️ The shell's idle gate must ask this, because the bars fall INSIDE `draw`: once the gate
     * stops drawing (about a second after a stop) they freeze until an input buys one frame. A bar
     * under one LED cell snaps to zero, so the frame that brings the last one down also shows it gone.
     *
     * ⚠️ Answers only for the two SPECTRUM modes; the caller gates on the visualizer type
     * (layout.cpp), as it gates the mixer's markers on MIXER being up.
     */
    bool bars_at_rest() const;

private:
    void draw_scope(Canvas& c, int x, int y, const float* wave, const Theme& t) const;
    void draw_flat(Canvas& c, int x, int y, const Theme& t) const;
    void draw_octa(Canvas& c, int x, int y, const float* tracks, int mask, const Theme& t) const;
    void draw_bar_amps(Canvas& c, int x, int y, const float* amps, const Theme& t, bool peak_mode);

    /**
     * One waveform as integer-quantised pixel dots — the ProTracker look, and one fill per column.
     * Shared by SCOPE (full width) and each OCTA lane, so they cannot drift apart.
     */
    void draw_wave_dots(Canvas& c, int scope_x, int scope_w, const float* wave, int center_y,
                        int max_amplitude, Argb color) const;

    float peakValues_[NUM_BARS]        = {};
    int   peakDecayCounters_[NUM_BARS] = {};
    float barSmoothed_[NUM_BARS]       = {};
};

}  // namespace pt::ui
