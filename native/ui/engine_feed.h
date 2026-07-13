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
#include "songcore/host.h"
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
    void poll(AudioEngine& engine, songcore::SongcoreHost& host, AppState& state) {
        poll_engine(engine, state);
        poll_soundfont_presets(host, state);
    }

private:
    void poll_engine(AudioEngine& engine, AppState& state) {
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

    /**
     * The INSTRUMENT screen's PRESET row: how many presets the loaded .sf2 has, which one this
     * instrument is on, and its name. Only the engine has opened the file — the Project stores a bank
     * and a preset NUMBER, not the list they index into.
     *
     * MEMOISED, because finding the index means walking the SF2's preset list and a big orchestral
     * bank has hundreds of them; recomputing that 60 times a second to redraw one unchanged row is
     * work a handheld's battery pays for. The key is everything the answer depends on.
     */
    void poll_soundfont_presets(songcore::SongcoreHost& host, AppState& state) {
        if (state.currentScreen != ScreenType::INSTRUMENT || !state.project) return;

        const int id = state.currentInstrument;
        const songcore::Instrument& ins = state.project->instruments[static_cast<size_t>(id)];
        if (ins.instrumentType != songcore::InstrumentType::SOUNDFONT) {
            state.sfPresetName  = "---";
            state.sfPresetCount = 0;
            state.sfPresetIndex = 0;
            return;
        }

        // The PATH is part of the key, not just the bank and preset: load a DIFFERENT .sf2 into this
        // slot that happens to sit at the same bank/preset and every displayed field changes while the
        // other three key fields do not.
        const std::string& path = ins.soundfontPath.value_or(std::string());
        if (id == sfCachedId_ && ins.sfBank == sfCachedBank_ && ins.sfPreset == sfCachedPreset_ &&
            path == sfCachedPath_) {
            return;   // nothing the answer depends on has moved
        }
        sfCachedId_     = id;
        sfCachedBank_   = ins.sfBank;
        sfCachedPreset_ = ins.sfPreset;
        sfCachedPath_   = path;

        state.sfPresetCount = host.sf_preset_count(id);
        state.sfPresetIndex = host.sf_preset_index(id);
        state.sfPresetName  = host.sf_preset_name(id);
    }

    float waveform_[WAVEFORM_SIZE]                            = {};
    float trackWaveforms_[TRACK_WAVEFORM_COUNT * WAVEFORM_SIZE] = {};
    bool  activeFlags_[TRACK_WAVEFORM_COUNT]                  = {};
    float spectrum_[OscilloscopeModule::NUM_BARS]             = {};

    int         sfCachedId_ = -1, sfCachedBank_ = -1, sfCachedPreset_ = -1;
    std::string sfCachedPath_{};
};

}  // namespace pt::ui
