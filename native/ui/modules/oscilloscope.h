#pragma once

// ─── OSCILLOSCOPE / VISUALIZER ───────────────────────────────────────────────────────────────────
//
// The C++ twin of ui/modules/OscilloscopeModule.kt: the 620×70 strip across the top, in six modes
// (SCOPE / FLAT / OCTA / OCTA_FULL / SPECTRUM / SPECTRUM_PEAKS) chosen by the theme.
//
// THE ONLY STATEFUL MODULE IN THE UI, and the reason `TrackerLayout::draw` is not const. The peak-hold
// dots and the spectrum bars' decay are functions of the PREVIOUS frame, not of the audio — so the
// module instance lives in the layout and persists across frames, exactly as Kotlin's does (it holds
// three FloatArrays for the same reason). A module recreated per frame would show bars that never fall.
//
// It reads no engine. The samples arrive as plain pointers that the caller has already captured
// (ui/engine_feed.h does that on the SDL side; Kotlin's PixelPerfectRenderer does it in its 60 Hz
// loop). A null pointer is silence, not a crash — which is precisely how `tools/ptshot` draws a screen
// with no audio device in the process.

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
     * TRACK_WAVEFORM_COUNT × WAVEFORM_SIZE, flat — lane N starts at `trackWaveforms[N * 620]`. Flat
     * because that is the shape `AudioEngine::getTrackWaveforms` fills; splitting it into 9 arrays
     * would be a copy that buys nothing.
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

    /** Instant attack, exponential decay (~333 ms fall at 60 fps). */
    static constexpr float BAR_DECAY = 0.90f;

    static constexpr int OCTA_TRACK_GAP = 10;

    /** Non-const: the peak-hold and bar-decay state advance one frame per call. */
    void draw(Canvas& c, int x, int y, const OscilloscopeState& s);

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
