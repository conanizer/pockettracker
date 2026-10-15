#pragma once

// What the UI reads back from the engine once a frame: scopes, the note monitor, TABLE's playing rows,
// meters, spectra, RAM.
// ⚠️ The one place in `pt-ui` that includes the engine; modules must not (the screenshots have none).

#include <algorithm>
#include <cmath>

#include "audio-engine.h"
#include "common/platform_memory.h"
#include "songcore/host.h"
#include "ui/app_state.h"
#include "ui/modules/oscilloscope.h"

namespace pt::ui {

class EngineFeed {
public:
    // ⚠️ Call after `isPlaying` and the playheads are set — the waveform decay depends on them.
    void poll(AudioEngine& engine, songcore::SongcoreHost& host, AppState& state, long long now_ms) {
        poll_engine(engine, state);
        poll_soundfont_presets(host, state);
        poll_soundfont_reload(host, state, now_ms);
        poll_peaks(engine, state, now_ms);
        poll_eq_spectrum(host, state, now_ms);
        poll_sample_editor(host, state);
        poll_sample_ram(engine, state);
    }

private:
    // The EQ editor's spectrum, while it is up — of the signal the EQ filters: a send's input, an
    // instrument's voices, or the master bus. Every frame, so it matches the scope drawn beside it.
    void poll_eq_spectrum(songcore::SongcoreHost& host, AppState& state, long long now_ms) {
        if (!state.eq.isOpen) {
            state.eqSpectrum      = nullptr;
            state.eqSpectrumCount = 0;
            eqSmoothValid_        = false;
            return;
        }
        state.eqSampleRate = host.sample_rate();

        // 0 master bus · 1 the delay's input · 2 the reverb's input · 3 one instrument's voices.
        int source  = 0;
        int instrId = -1;
        switch (state.eq.caller.kind) {
            case EqCallerContext::Kind::DELAY_IN:  source = 1; break;
            case EqCallerContext::Kind::REVERB_IN: source = 2; break;
            case EqCallerContext::Kind::INSTRUMENT:
                source  = 3;
                instrId = state.eq.caller.instrId;
                break;
            default: break;   // MASTER, SAMPLE_EDITOR_FX → the master bus
        }

        if (host.spectrum_for_source(source, instrId, EQ_SPECTRUM_BINS, eqSpectrumRaw_)) {
            smooth_eq_spectrum(source, instrId, now_ms);
            state.eqSpectrum      = eqSpectrum_;
            state.eqSpectrumCount = EQ_SPECTRUM_BINS;
        }
    }

    // Analyser ballistics: fast up, slow down, one pole per bin, by elapsed time (a stalled frame is
    // clamped). A new source snaps instead of gliding.
    void smooth_eq_spectrum(int source, int instrId, long long now_ms) {
        if (!eqSmoothValid_ || source != eqSmoothSource_ || instrId != eqSmoothInstr_) {
            std::copy(eqSpectrumRaw_, eqSpectrumRaw_ + EQ_SPECTRUM_BINS, eqSpectrum_);
            eqSmoothValid_  = true;
            eqSmoothSource_ = source;
            eqSmoothInstr_  = instrId;
            eqSmoothMs_     = now_ms;
            return;
        }

        long long dtMs = now_ms - eqSmoothMs_;
        if (dtMs < 1)                dtMs = 1;
        if (dtMs > EQ_SMOOTH_GAP_MS) dtMs = EQ_SMOOTH_GAP_MS;
        eqSmoothMs_ = now_ms;

        const float dt   = (float)dtMs;
        const float rise = 1.0f - expf(-dt / EQ_RISE_MS);
        const float fall = 1.0f - expf(-dt / EQ_FALL_MS);

        for (int i = 0; i < EQ_SPECTRUM_BINS; i++) {
            const float target = eqSpectrumRaw_[i];
            float&      v      = eqSpectrum_[i];
            v += (target - v) * (target > v ? rise : fall);
        }
    }

    // USED and FREE RAM on PROJECT and INST.POOL. ⚠️ Debug builds only: both stay 0 in release.
    void poll_sample_ram(AudioEngine& engine, AppState& state) {
        if (!state.caps.debug) return;
        if (state.currentScreen != ScreenType::PROJECT &&
            state.currentScreen != ScreenType::INST_POOL) return;
        state.sampleRamBytes = engine.audio_memory_bytes();

        // The kernel's number, not the engine's walk — the two do not sum. Not `load_budget_bytes()`.
        state.freeRamBytes = pt::available_memory_bytes();
    }

    void poll_engine(AudioEngine& engine, AppState& state) {
        // Stopped, nothing refills the capture ring; without the decay the scope freezes mid-wave.
        if (!state.isPlaying) engine.decayWaveform();
        engine.getWaveform(waveform_, WAVEFORM_SIZE);
        state.waveform = waveform_;

        const VisualizerType vt = state.theme.visualizerType;
        const bool octa     = (vt == VisualizerType::OCTA || vt == VisualizerType::OCTA_FULL);
        const bool spectrum = (vt == VisualizerType::SPECTRUM || vt == VisualizerType::SPECTRUM_PEAKS);

        // Ask only for what the mode draws: the engine fills these only while they are read.
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

        // The note monitor, from the voices: a long sample still shows past its chain.
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

        // TABLE's playing rows, one per FX column. The first track in this table answers for all three.
        for (int l = 0; l < TABLE_LANES; ++l) state.tablePlaybackRows[l] = -1;
        if (state.currentScreen == ScreenType::TABLE && state.isPlaying) {
            for (int trackId = 0; trackId < 8; ++trackId) {
                int rows[TABLE_LANES];
                if (!engine.getTableRowsFor(trackId, state.currentTable, rows)) continue;
                bool any = false;
                for (int l = 0; l < TABLE_LANES; ++l) any |= (rows[l] >= 0);
                if (!any) continue;
                for (int l = 0; l < TABLE_LANES; ++l) state.tablePlaybackRows[l] = rows[l];
                break;
            }
        }
    }

    // The MIXER's meters, only on MIXER (the read takes a mutex the audio callback takes).
    // ⚠️ Every 60 ms is a contract: the module's peak hold counts polls via `peaksVersion`.
    // Stopped, this decays the peaks by hand — one step per 60 ms slot passed, so a fall interrupted by
    // leaving the screen is finished on return.
    void poll_peaks(AudioEngine& engine, AppState& state, long long now_ms) {
        // On every screen: playback elsewhere wrote the peaks itself and must not replay as decay.
        if (state.isPlaying) peaksLiveMs_ = now_ms;

        if (state.currentScreen != ScreenType::MIXER) return;

        const bool first = (peaksPolledMs_ == 0);
        if (!first && now_ms - peaksPolledMs_ < PEAK_POLL_MS) return;

        // How long the meters have stood still.
        const long long stale = now_ms - std::max(peaksPolledMs_, peaksLiveMs_);
        peaksPolledMs_        = now_ms;

        long long steps = (first || stale < PEAK_POLL_MS) ? 1 : stale / PEAK_POLL_MS;
        if (steps > PEAK_CATCHUP_SLOTS) steps = PEAK_CATCHUP_SLOTS;

        if (!state.isPlaying) {
            for (long long i = 0; i < steps; ++i) engine.decayPeaks();
            engine.decayWaveform();
        }
        engine.getTrackPeaks(state.trackPeaks);
        engine.getMasterPeaks(state.masterPeaks);
        engine.getSendPeaks(state.sendPeaks);
        state.peaksVersion += static_cast<unsigned>(steps);
    }

    // The PRESET row's count, index and name. Memoised — finding the index walks the preset list.
    void poll_soundfont_presets(songcore::SongcoreHost& host, AppState& state) {
        if (state.currentScreen != ScreenType::INSTRUMENT || !state.project) return;

        const int id = state.currentInstrument;
        const songcore::Instrument& ins = state.project->instruments[static_cast<size_t>(id)];
        const bool sf = ins.instrumentType == songcore::InstrumentType::SOUNDFONT;

        // The type is in the key: a non-SoundFont slot's "---" is a cached answer too.
        const std::string& path = ins.soundfontPath.value_or(std::string());
        if (id == sfCachedId_ && sf == sfCachedIsSf_ && ins.sfBank == sfCachedBank_ &&
            ins.sfPreset == sfCachedPreset_ && path == sfCachedPath_) {
            return;
        }
        sfCachedId_     = id;
        sfCachedIsSf_   = sf;
        sfCachedBank_   = ins.sfBank;
        sfCachedPreset_ = ins.sfPreset;
        sfCachedPath_   = path;

        if (!sf) {
            state.sfPresetName  = "---";
            state.sfPresetCount = 0;
            state.sfPresetIndex = 0;
            return;
        }

        state.sfPresetCount = host.sf_preset_count(id);
        state.sfPresetIndex = host.sf_preset_index(id);
        state.sfPresetName  = host.sf_preset_name(id);
    }

    // Load the sound the PATCH row names once the row stops moving. The load runs on a worker; this
    // decides when to ask, and asks again while refused.
    // ⚠️ Not gated on INSTRUMENT: leaving within the settle window must not strand the old sound. A
    // pending load for an instrument the cursor leaves is flushed, not dropped.
    void poll_soundfont_reload(songcore::SongcoreHost& host, AppState& state, long long now_ms) {
        // ⚠️ Above every early return: a load started on INSTRUMENT is installed after leaving it.
        host.poll_sf_load();

        if (!state.project) return;
        const int id = state.currentInstrument;
        if (id < 0 || id >= static_cast<int>(state.project->instruments.size())) return;

        const songcore::Instrument& ins = state.project->instruments[static_cast<size_t>(id)];
        if (ins.instrumentType != songcore::InstrumentType::SOUNDFONT || !ins.soundfontPath.has_value()) {
            sfReloadPending_ = false;
            return;
        }

        const std::string& path = *ins.soundfontPath;
        if (id != sfReloadId_ || ins.sfBank != sfReloadBank_ || ins.sfPreset != sfReloadPreset_ ||
            path != sfReloadPath_) {
            // ⚠️ Synchronous: nothing asks again for an instrument being left.
            if (sfReloadPending_ && sfReloadId_ != id) host.sync_sf_preset(sfReloadId_);
            sfReloadId_      = id;
            sfReloadBank_    = ins.sfBank;
            sfReloadPreset_  = ins.sfPreset;
            sfReloadPath_    = path;
            sfReloadDueMs_   = now_ms + SF_RELOAD_SETTLE_MS;
            sfReloadPending_ = true;
            return;
        }
        // Cleared only when accepted; refused means the engine is still decoding.
        if (sfReloadPending_ && now_ms >= sfReloadDueMs_ && host.request_sf_preset(id)) {
            sfReloadPending_ = false;
        }
    }

    // The sample editor's live reads. The waveform and the transients are recomputed only when their
    // inputs change; the playhead is read every frame.
    void poll_sample_editor(songcore::SongcoreHost& host, AppState& state) {
        if (state.currentScreen != ScreenType::SAMPLE_EDITOR) {
            wfKeyValid_ = false;   // the next entry must rebuild
            return;
        }
        SampleEditorState& se = state.sampleEditor;

        // 0..1 while sounding, −1 when not, from the slot actually playing (the preview slot for
        // LEFT/RIGHT/MONO and SYNC auditions).
        se.playbackPosition = host.sample_playback_position(songcore::SOURCE_PREVIEW_SLOT);
        if (se.playbackPosition < 0.0f && !(se.hasStereoData && se.sourceMode != 2))
            se.playbackPosition = host.sample_playback_position(se.instrumentId);

        // Hand-placed markers carry the (method, parameter) they were made under; a change clears them
        // here, once, above every reader. ⚠️ Cleared, not ignored, or BY 08 → 09 → 08 brings them back.
        // The stamp, not the list, says they are live: an empty manual list is still meant.
        if (se.manualKeyMethod >= 0 && !se.manual_markers_live()) {
            se.manualMarkers.clear();
            se.manualKeyMethod = -1;
            se.manualKeyParam  = -1;
        }

        // Transients: an empty list under TRANSIENT is the trigger (entering the mode or a new SENS empties it).
        if (se.sliceMethod == SampleEditorModule::SLICE_TRANSIENT && se.totalFrames > 0 &&
            se.transientMarkers.empty()) {
            se.transientMarkers = host.detect_transients(se.instrumentId, se.sliceSensitivity);
            se.sliceIndex = 0;   // the selection is not touched: detecting is not choosing
        }

        // A new marker set can leave the index above the ceiling.
        se.sliceIndex = std::clamp(se.sliceIndex, 0, se.slice_index_ceiling());

        // The waveform, re-binned when the window (zoom, scroll) or the channel changes.
        const int64_t vs = se.view_start();
        const int64_t ve = se.view_end();
        if (!wfKeyValid_ || vs != wfStart_ || ve != wfEnd_ || se.sourceMode != wfSource_ ||
            se.totalFrames != wfTotal_ || se.instrumentId != wfInst_) {
            wfKeyValid_ = true;
            wfStart_    = vs;
            wfEnd_      = ve;
            wfSource_   = se.sourceMode;
            wfTotal_    = se.totalFrames;
            wfInst_     = se.instrumentId;

            const int channel = (se.sourceMode == 0) ? 0 : (se.sourceMode == 1) ? 1 : 2;
            se.waveformData   = host.sample_waveform(se.instrumentId, SampleEditorModule::WAVEFORM_W,
                                                     static_cast<int>(vs), static_cast<int>(ve), channel);
        }
    }

    float waveform_[WAVEFORM_SIZE]                            = {};
    float trackWaveforms_[TRACK_WAVEFORM_COUNT * WAVEFORM_SIZE] = {};
    bool  activeFlags_[TRACK_WAVEFORM_COUNT]                  = {};
    float spectrum_[OscilloscopeModule::NUM_BARS]             = {};

    // What the sample editor's bins were computed from.
    bool    wfKeyValid_ = false;
    int64_t wfStart_ = 0, wfEnd_ = 0;
    int     wfSource_ = -1, wfTotal_ = -1, wfInst_ = -1;

    int         sfCachedId_ = -1, sfCachedBank_ = -1, sfCachedPreset_ = -1;
    bool        sfCachedIsSf_ = false;
    std::string sfCachedPath_{};

    // How still the PATCH row must be before its sound loads — a held direction loads nothing on the way.
    static constexpr long long SF_RELOAD_SETTLE_MS = 150;
    int         sfReloadId_ = -1, sfReloadBank_ = -1, sfReloadPreset_ = -1;
    std::string sfReloadPath_{};
    long long   sfReloadDueMs_   = 0;
    bool        sfReloadPending_ = false;

    static constexpr long long PEAK_POLL_MS = 60;   // a contract with the peak hold, not a throttle
    long long                  peaksPolledMs_ = 0;

    // The most slots one catch-up replays (6 s) — by then any fall has reached zero.
    static constexpr long long PEAK_CATCHUP_SLOTS = 100;

    long long peaksLiveMs_ = 0;   // when playback last wrote the peaks itself

    // More bins than the module's pixels: it takes the max of each pixel's two, so a narrow peak shows.
    static constexpr int        EQ_SPECTRUM_BINS = 620;
    float                       eqSpectrum_[EQ_SPECTRUM_BINS]    = {};   // what the module draws
    float                       eqSpectrumRaw_[EQ_SPECTRUM_BINS] = {};   // this frame's transform

    // Time constants, ms to 63 % of a step.
    static constexpr float     EQ_RISE_MS       = 25.0f;
    static constexpr float     EQ_FALL_MS       = 250.0f;
    static constexpr long long EQ_SMOOTH_GAP_MS = 100;

    bool      eqSmoothValid_  = false;
    int       eqSmoothSource_ = -1;
    int       eqSmoothInstr_  = -1;
    long long eqSmoothMs_     = 0;
};

}  // namespace pt::ui
