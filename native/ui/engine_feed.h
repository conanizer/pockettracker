#pragma once

// ─── The engine feed ─────────────────────────────────────────────────────────────────────────────
//
// Everything the UI reads back OUT of the audio engine, once per frame: the oscilloscope's samples,
// the note monitor's eight notes, and the TABLE screen's playing row.
//
// On Android this is the body of PixelPerfectRenderer's two `LaunchedEffect` loops — it lives there
// because Compose needs the values in observable state before a recomposition can see them. There is
// no recomposition here, so the same reads collapse into one call the shell makes per frame, and the
// modules keep taking plain pointers.
//
// WHY IT IS UI CODE AND NOT SHELL CODE. Nothing in it is SDL, a window, or Linux — it is the *policy*
// of which buffers a given visualizer mode needs, and that policy belongs to the visualizer. Put it in
// the shell and the next shell (Android-on-SDL, per §12.8) reinvents it, differently.
//
// It is also the ONE place in `pt-ui` that includes the engine. The modules must not: a module that
// reached for `AudioEngine` could not be drawn by `tools/ptshot`, which has no engine at all — and
// ptshot's ability to draw every screen headlessly is the standing proof that the UI is portable. So
// the feed is what any *shell* constructs, and a tool simply does not construct one.

#include "audio-engine.h"
#include "ui/app_state.h"
#include "ui/modules/oscilloscope.h"

namespace pt::ui {

class EngineFeed {
public:
    /**
     * One frame's worth of reads, straight into `state`.
     *
     * ⚠️ Call it AFTER the transport fields (`isPlaying`, the playheads) are set from the host: the
     * waveform decay below is a function of `isPlaying`, and the table row is only resolved on the
     * TABLE screen. Reading the engine first would decay against the previous frame's transport.
     */
    void poll(AudioEngine& engine, AppState& state) {
        // ── The visualizer ───────────────────────────────────────────────────────────────────────
        // updateWaveformWithDecay(): with the transport stopped the engine's capture ring is never
        // refilled, so without an explicit decay the scope would freeze mid-wave at the moment of the
        // stop rather than settling to a line.
        if (!state.isPlaying) engine.decayWaveform();
        engine.getWaveform(waveform_, WAVEFORM_SIZE);
        state.waveform = waveform_;

        const VisualizerType vt = state.theme.visualizerType;
        const bool octa     = (vt == VisualizerType::OCTA || vt == VisualizerType::OCTA_FULL);
        const bool spectrum = (vt == VisualizerType::SPECTRUM || vt == VisualizerType::SPECTRUM_PEAKS);

        // Demand-driven capture: the engine only does the (expensive) per-track accumulation and the
        // spectrum ring writes while somebody is actually reading them. Asking for a buffer no mode
        // draws would make the audio callback do that work for nothing, on every block.
        if (octa) {
            engine.getTrackWaveforms(trackWaveforms_, activeFlags_);
            state.trackWaveforms    = trackWaveforms_;
            state.previewLaneActive = activeFlags_[PREVIEW_LANE];
        } else {
            state.trackWaveforms    = nullptr;
            state.previewLaneActive = false;
        }

        if (spectrum) {
            engine.getSpectrumMagnitudes(OscilloscopeModule::NUM_BARS, spectrum_);
            state.spectrum = spectrum_;
        } else {
            state.spectrum = nullptr;
        }

        // ── The note monitor ─────────────────────────────────────────────────────────────────────
        // Read from the VOICE POOL, not from the sequencer's track state: a long sample sustains past
        // the end of its chain, and the monitor should show what you can still hear rather than what
        // was last scheduled. (This is why the monitor needed nothing new from songcore — the C++
        // engine has always been able to answer it. Kotlin's `getCurrentPlayingNotes()` is three lines
        // over exactly this call.)
        if (state.isPlaying) {
            int encoded[8];
            engine.getTrackActiveNotes(encoded, 8);
            for (int i = 0; i < 8; ++i) {
                state.trackNotes[i] = (encoded[i] < 0)
                                          ? songcore::Note::EMPTY()
                                          : songcore::Note{encoded[i] % 12, encoded[i] / 12};
            }
        } else {
            for (int i = 0; i < 8; ++i) state.trackNotes[i] = songcore::Note::EMPTY();
        }

        // ── The TABLE screen's playing row ───────────────────────────────────────────────────────
        // Resolved HERE, at 60 Hz, and not in the draw pass — it costs up to 16 engine reads, and a
        // draw pass runs on every cursor move as well as every frame.
        state.tablePlaybackRow = -1;
        if (state.currentScreen == ScreenType::TABLE && state.isPlaying) {
            for (int trackId = 0; trackId < 8; ++trackId) {
                if (engine.getVoiceTableId(trackId) != state.currentTable) continue;
                const int row = engine.getVoiceTableRow(trackId);
                if (row >= 0) {
                    state.tablePlaybackRow = row;
                    break;
                }
            }
        }
    }

private:
    float waveform_[WAVEFORM_SIZE]                            = {};
    float trackWaveforms_[TRACK_WAVEFORM_COUNT * WAVEFORM_SIZE] = {};
    bool  activeFlags_[TRACK_WAVEFORM_COUNT]                  = {};
    float spectrum_[OscilloscopeModule::NUM_BARS]             = {};
};

}  // namespace pt::ui
