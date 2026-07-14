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
     *
     * `now_ms` is the frame's clock reading — the MIXER's meters are polled on their own 60 ms cadence
     * rather than once a frame, and a class whose behaviour is a function of time must be handed the
     * time rather than reach for it (the same contract `SdlInput::handle_event` and `InputDispatcher`
     * are built on).
     */
    void poll(AudioEngine& engine, songcore::SongcoreHost& host, AppState& state, long long now_ms) {
        poll_engine(engine, state);
        poll_soundfont_presets(host, state);
        poll_peaks(engine, state, now_ms);
        poll_sample_editor(host, state);
        poll_sample_ram(engine, state);
    }

private:
    /**
     * PROJECT's debug-only USED RAM readout.
     *
     * ⚠️ It is a DIFFERENT number from Android's, and the divergence is deliberate. Kotlin reports
     * native-heap GROWTH since launch (`Debug.getNativeHeapAllocatedSize()` minus a baseline), which is
     * a proxy — it counts every native allocation the app has made, and merely happens to be dominated
     * by sample PCM. Here the engine is simply ASKED how much audio it is holding, which is the thing
     * the row is actually for. A proxy is what you use when you cannot reach the truth; in-process, we
     * can.
     *
     * Only on PROJECT, and only in a debug build — 128 integer reads is cheap, but not for a row that
     * is not on screen.
     */
    void poll_sample_ram(AudioEngine& engine, AppState& state) {
        if (!state.caps.debug || state.currentScreen != ScreenType::PROJECT) return;

        int64_t bytes = 0;
        for (int id = 0; id < songcore::POOL_INSTRUMENTS; ++id) {
            const int frames = engine.getSampleLength(id);
            if (frames <= 0) continue;
            const int channels = engine.hasStereoData(id) ? 2 : 1;
            bytes += static_cast<int64_t>(frames) * channels * 4;   // float32 PCM
        }
        state.sampleRamBytes = bytes;
    }

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
     * The MIXER's meters: eight stereo track pairs, the master pair, and the two send returns.
     *
     * TWO THINGS ARE DELIBERATE HERE, and both are Kotlin's — its whole peak loop is a
     * `LaunchedEffect(currentScreen)` gated on MIXER, ticking at `delay(60)`.
     *
     * ⚠️ **Only on the MIXER.** `getTrackPeaks` takes the engine's peak mutex, and the AUDIO CALLBACK
     * takes it too (that is where the peaks are written). Polling it from the UI thread on every screen
     * would be lock contention with the audio thread bought for a readout nobody is looking at.
     *
     * ⚠️ **Only every 60 ms, and that is not a saving — it is the CONTRACT the peak-hold is written
     * against.** `MixerModule::PEAK_HOLD_FRAMES = 45` counts *refreshes*: on Android a refresh is a
     * recomposition, and the only thing that triggers one on this screen is this poll. So the marker
     * hangs 45 × 60 ms ≈ 2.7 s. Poll (and therefore age it) at the shell's 60 Hz instead and the same
     * constant means 0.75 s — the meters would visibly fall off a cliff compared to Android's. The
     * cadence is what keeps one constant meaning one thing on both platforms; `peaksVersion` is how the
     * module knows a refresh happened, since its own draw runs at 60 Hz regardless.
     *
     * The manual decay is the mirror of `decayWaveform`: with the transport stopped the audio callback
     * is not running, so nothing is decaying the peaks and the meters would freeze mid-level at the
     * moment of the stop.
     */
    void poll_peaks(AudioEngine& engine, AppState& state, long long now_ms) {
        if (state.currentScreen != ScreenType::MIXER) return;
        if (peaksPolledMs_ != 0 && now_ms - peaksPolledMs_ < PEAK_POLL_MS) return;
        peaksPolledMs_ = now_ms;

        if (!state.isPlaying) {
            engine.decayPeaks();
            engine.decayWaveform();
        }
        engine.getTrackPeaks(state.trackPeaks);
        engine.getMasterPeaks(state.masterPeaks);
        engine.getSendPeaks(state.sendPeaks);
        state.peaksVersion++;
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

    /**
     * The SAMPLE EDITOR's three live reads — the C++ twin of MainActivity's three `LaunchedEffect`s.
     *
     * All three are EDGE-TRIGGERED, on the same keys Compose keys its effects on, and that is not an
     * optimisation. Each one is expensive enough that doing it every frame would be felt on a handheld:
     * the waveform re-bins the whole sample into 620 min/max pairs, and the detector walks it looking for
     * onsets. Compose reruns a `LaunchedEffect` when its keys change; here the keys are remembered and
     * compared, which is the same thing said explicitly.
     *
     * The PLAYHEAD is the exception — it has no key but time, so it polls. Kotlin polls it at ~30 fps;
     * this is once a frame, and the read is a single float off the voice.
     */
    void poll_sample_editor(songcore::SongcoreHost& host, AppState& state) {
        if (state.currentScreen != ScreenType::SAMPLE_EDITOR) {
            wfKeyValid_ = false;   // the next entry must rebuild, whatever it is looking at
            return;
        }
        SampleEditorState& se = state.sampleEditor;

        // ── The playhead ─────────────────────────────────────────────────────────────────────────
        // 0..1 while the sample sounds, −1 when it does not. It follows whichever slot is ACTUALLY
        // playing: a stereo sample auditioned as LEFT/RIGHT/MONO comes out of the 254 scratch, not out
        // of the instrument's own slot, and watching the wrong one would leave the playhead parked at
        // −1 through the entire preview.
        const int voiceSlot = (se.hasStereoData && se.sourceMode != 2)
                                  ? songcore::SOURCE_PREVIEW_SLOT
                                  : se.instrumentId;
        se.playbackPosition = host.sample_playback_position(voiceSlot);

        // ── The TRANSIENTS ───────────────────────────────────────────────────────────────────────
        // Detect when the method is TRANSIENT and there are no markers — which is exactly the state
        // `handle_input` leaves behind when the user switches INTO transient mode or changes the
        // sensitivity (both clear the list). So "empty" IS the trigger, and the module and the feed need
        // no other channel between them. Kotlin keys its effect the same way.
        if (se.sliceMethod == SampleEditorModule::SLICE_TRANSIENT && se.totalFrames > 0 &&
            se.transientMarkers.empty()) {
            se.transientMarkers = host.detect_transients(se.instrumentId, se.sliceSensitivity);
            se.sliceIndex       = 0;
            // Select the FIRST slice, so the detector's result is something you can immediately hear:
            // press START and you get slice 0, not the whole sample again.
            se.selectionStart = 0;
            se.selectionEnd   = se.transientMarkers.empty()
                                    ? se.totalFrames
                                    : se.transientMarkers.front();
        }

        // ── The WAVEFORM ─────────────────────────────────────────────────────────────────────────
        // Re-binned when the WINDOW moves (zoom, or the view scrolling to follow the cursor or the
        // playhead) or when the CHANNEL being drawn changes. `view_start`/`view_end` already fold in
        // everything the window depends on, so they are the whole key.
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

    // The sample editor's waveform key — what the 620 bins on screen were computed FROM.
    bool    wfKeyValid_ = false;
    int64_t wfStart_ = 0, wfEnd_ = 0;
    int     wfSource_ = -1, wfTotal_ = -1, wfInst_ = -1;

    int         sfCachedId_ = -1, sfCachedBank_ = -1, sfCachedPreset_ = -1;
    std::string sfCachedPath_{};

    /** Kotlin's `delay(60)` between peak reads. See poll_peaks — it is a contract, not a throttle. */
    static constexpr long long PEAK_POLL_MS = 60;
    long long                  peaksPolledMs_ = 0;
};

}  // namespace pt::ui
