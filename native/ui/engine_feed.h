#pragma once

// ─── The engine feed ─────────────────────────────────────────────────────────────────────────────
//
// Everything the UI reads back OUT of the audio engine, once per frame: the oscilloscope, the note
// monitor, the TABLE screen's playing rows, meters, spectra, RAM.
// UI code, not shell code: it is the policy of which buffers each visualizer mode needs.
// ⚠️ The ONE place in `pt-ui` that includes the engine. Modules must not — the headless screenshots draw every screen
// with no engine; a shell constructs a feed, a tool does not.

#include <algorithm>
#include <cmath>

#include "audio-engine.h"
#include "platform_memory.h"
#include "songcore/host.h"
#include "ui/app_state.h"
#include "ui/modules/oscilloscope.h"

namespace pt::ui {

class EngineFeed {
public:
    /**
     * One frame's reads, into `state`.
     * ⚠️ Call AFTER the transport fields (`isPlaying`, playheads) are set from the host — the waveform
     * decay depends on `isPlaying`.
     * `now_ms` is handed in (the meters run on their own 60 ms cadence): time is an argument, never
     * read here.
     */
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
    /**
     * The EQ editor's spectrum, polled only while that screen is up.
     * ⚠️ The SOURCE is whoever opened the editor: a send's EQ filters the send's INPUT, an instrument's
     * its voices, the master EQ the bus. A curve over the wrong signal is worse than none — it looks right.
     * ⚠️ Every frame, because the oscilloscope keeps drawing beside it from the same ring; a slower poll
     * shows the same audio at two ages. (Over silence this still polls — the loop stops drawing, not
     * polling. Small, but the first suspect if this screen ever runs warm.)
     * The buffer is a member: `AppState::eqSpectrum` points at it during the draw.
     */
    void poll_eq_spectrum(songcore::SongcoreHost& host, AppState& state, long long now_ms) {
        if (!state.eq.isOpen) {
            state.eqSpectrum      = nullptr;
            state.eqSpectrumCount = 0;
            eqSmoothValid_        = false;
            return;
        }
        state.eqSampleRate = host.sample_rate();

        // 0 master bus · 1 the delay's input · 2 the reverb's input · 3 one instrument's voices.
        // The sample editor's FX EQ is applied destructively on APPLY, so it watches the master.
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

    /**
     * Analyser ballistics over the raw bins: fast up, slow down, one pole per bin. One FFT frame is a
     * noisy estimate (two windows of a steady tone differ by dB), so raw it twitches.
     * A function of ELAPSED TIME, not frames; the gap clamp keeps one stalled frame from collapsing the
     * smoothing into a jump. State is per SOURCE: switching signal snaps rather than gliding between
     * two signals.
     */
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

    /**
     * The USED RAM readout on PROJECT and INST.POOL — the engine is asked how much audio it holds.
     * ⚠️ DEVELOPER BUILDS ONLY, like the two draw sites (`project_editor.cpp`, `instrument_pool.cpp`):
     * `sampleRamBytes` and `freeRamBytes` stay 0 in release, so a new consumer must carry the same gate.
     * Only on those two screens.
     */
    void poll_sample_ram(AudioEngine& engine, AppState& state) {
        if (!state.caps.debug) return;
        if (state.currentScreen != ScreenType::PROJECT &&
            state.currentScreen != ScreenType::INST_POOL) return;
        state.sampleRamBytes = engine.audio_memory_bytes();

        // ⚠️ Not the same kind of number: USED is the engine's walk of its own buffers (no allocator
        // overhead, transient copies or leaks); FREE is the kernel's and is what decides whether the
        // next load survives. They are not meant to sum.
        // ⚠️ FREE shown is `available_memory_bytes()`, NOT the deliberately generous `load_budget_bytes()`.
        state.freeRamBytes = pt::available_memory_bytes();
    }

    void poll_engine(AudioEngine& engine, AppState& state) {
        // ── The visualizer ───────────────────────────────────────────────────────────────────────
        // Stopped, the capture ring is never refilled; without the decay the scope freezes mid-wave.
        if (!state.isPlaying) engine.decayWaveform();
        engine.getWaveform(waveform_, WAVEFORM_SIZE);
        state.waveform = waveform_;

        const VisualizerType vt = state.theme.visualizerType;
        const bool octa     = (vt == VisualizerType::OCTA || vt == VisualizerType::OCTA_FULL);
        const bool spectrum = (vt == VisualizerType::SPECTRUM || vt == VisualizerType::SPECTRUM_PEAKS);

        // Demand-driven: the engine does the per-track accumulation and spectrum writes only while
        // somebody reads them — asking for a buffer no mode draws costs the audio callback every block.
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
        // From the VOICE POOL, not the sequencer: a long sample sustains past its chain, and the monitor
        // shows what you can still hear.
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

        // ── The TABLE screen's playing rows, one per FX column ───────────────────────────────────
        // Resolved here at 60 Hz, not in the draw pass (which also runs on every cursor move).
        // ⚠️ The FIRST track standing in this table answers for all three columns; another track
        // playing the same table has cursors this screen has no room to show.
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

    /**
     * The MIXER's meters: eight stereo track pairs, the master pair, the two send returns.
     * ⚠️ Only on the MIXER: `getTrackPeaks` takes a mutex the AUDIO CALLBACK takes.
     * ⚠️ Every 60 ms, which is a CONTRACT: `MixerModule::PEAK_HOLD_FRAMES = 45` counts polls, so the hold
     * is 45 × 60 ms ≈ 2.7 s. `peaksVersion` tells the module a poll happened (its draw runs at 60 Hz).
     * Stopped, nothing decays the peaks, so this decays them by hand (as `decayWaveform`).
     * ⚠️ One step per poll SLOT that has passed, not per poll: otherwise a fall caught by leaving the
     * screen would resume, frozen, on return. Replaying missed slots costs nothing while away.
     */
    void poll_peaks(AudioEngine& engine, AppState& state, long long now_ms) {
        // Stamped on EVERY screen, before the gate: while playing, the callback writes peaks itself,
        // so a spell of playback elsewhere must not replay as decay.
        if (state.isPlaying) peaksLiveMs_ = now_ms;

        if (state.currentScreen != ScreenType::MIXER) return;

        const bool first = (peaksPolledMs_ == 0);
        if (!first && now_ms - peaksPolledMs_ < PEAK_POLL_MS) return;

        // How long the meters have stood still: since the last poll or the transport's last write,
        // whichever is later.
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

    /**
     * The PRESET row's count, index and name — only the engine has opened the .sf2. MEMOISED: finding
     * the index walks the preset list, hundreds long in a big bank; the key is everything the answer
     * depends on.
     */
    void poll_soundfont_presets(songcore::SongcoreHost& host, AppState& state) {
        if (state.currentScreen != ScreenType::INSTRUMENT || !state.project) return;

        const int id = state.currentInstrument;
        const songcore::Instrument& ins = state.project->instruments[static_cast<size_t>(id)];
        const bool sf = ins.instrumentType == songcore::InstrumentType::SOUNDFONT;

        // The PATH is in the key (another .sf2 at the same bank/preset changes every field), and so is
        // the TYPE: the "---" of a non-SoundFont slot is a cached answer too, and keyed without the type
        // a return to the SoundFont would HIT and keep the placeholder.
        const std::string& path = ins.soundfontPath.value_or(std::string());
        if (id == sfCachedId_ && sf == sfCachedIsSf_ && ins.sfBank == sfCachedBank_ &&
            ins.sfPreset == sfCachedPreset_ && path == sfCachedPath_) {
            return;   // nothing the answer depends on has moved
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

    /**
     * Load the sound the PATCH row names, once the row has stopped moving — each preset is a parse,
     * fine for a 2 MB sound, not for a 50 MB one.
     * ⚠️ The load runs on a worker; this only decides WHEN to ask. `poll_sf_load` installs results every
     * frame; a request refused (engine busy) leaves the flag up and asks again next frame.
     * ⚠️ Not gated on the INSTRUMENT screen: leaving within the settle window must not strand the
     * instrument on its old sound (and a `.pti` moves presets too). A pending load for an instrument the
     * cursor leaves is flushed, not dropped.
     */
    void poll_soundfont_reload(songcore::SongcoreHost& host, AppState& state, long long now_ms) {
        // ⚠️ Unconditional and above every early return: a load started on INSTRUMENT must still be
        // installed after the user leaves.
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
            // ⚠️ The flush stays SYNCHRONOUS: nothing will ask again for an instrument being left, and a
            // refused request would strand it. Costs a frame, only in that window.
            if (sfReloadPending_ && sfReloadId_ != id) host.sync_sf_preset(sfReloadId_);
            sfReloadId_      = id;
            sfReloadBank_    = ins.sfBank;
            sfReloadPreset_  = ins.sfPreset;
            sfReloadPath_    = path;
            sfReloadDueMs_   = now_ms + SF_RELOAD_SETTLE_MS;
            sfReloadPending_ = true;
            return;
        }
        // ⚠️ Cleared only when ACCEPTED; refused means the engine is still decoding.
        if (sfReloadPending_ && now_ms >= sfReloadDueMs_ && host.request_sf_preset(id)) {
            sfReloadPending_ = false;
        }
    }

    /**
     * The SAMPLE EDITOR's live reads. The waveform and the transient detector are EDGE-TRIGGERED on
     * remembered keys — each is too expensive for every frame on a handheld. The playhead has no key
     * but time, so it polls (one float off the voice).
     */
    void poll_sample_editor(songcore::SongcoreHost& host, AppState& state) {
        if (state.currentScreen != ScreenType::SAMPLE_EDITOR) {
            wfKeyValid_ = false;   // the next entry must rebuild
            return;
        }
        SampleEditorState& se = state.sampleEditor;

        // ── The playhead ─────────────────────────────────────────────────────────────────────────
        // 0..1 while sounding, −1 when not. Follows the slot ACTUALLY playing: LEFT/RIGHT/MONO
        // auditions come out of scratch slot 254.
        const int voiceSlot = (se.hasStereoData && se.sourceMode != 2)
                                  ? songcore::SOURCE_PREVIEW_SLOT
                                  : se.instrumentId;
        se.playbackPosition = host.sample_playback_position(voiceSlot);

        // ── The HAND-PLACED markers ──────────────────────────────────────────────────────────────
        // Changing the SLICE method or its setting resets hand-placed positions — derived from the data:
        // the list carries the (method, parameter) it was made under. ⚠️ One site, above every reader:
        // five writes to those fields would each have to remember.
        // ⚠️ CLEARED, not just ignored, or BY 08 → BY 09 → BY 08 would bring them back.
        // ⚠️ The STAMP says there is something to clear, not the list: a manual session with every
        // boundary deleted is an empty list that is still meant.
        if (se.manualKeyMethod >= 0 && !se.manual_markers_live()) {
            se.manualMarkers.clear();
            se.manualKeyMethod = -1;
            se.manualKeyParam  = -1;
        }

        // ── The TRANSIENTS ───────────────────────────────────────────────────────────────────────
        // Detect when the method is TRANSIENT and there are no markers — the state `handle_input` leaves
        // when entering transient mode or changing sensitivity. "Empty" is the trigger.
        if (se.sliceMethod == SampleEditorModule::SLICE_TRANSIENT && se.totalFrames > 0 &&
            se.transientMarkers.empty()) {
            se.transientMarkers = host.detect_transients(se.instrumentId, se.sliceSensitivity);
            // ⚠️ The INDEX resets (the new set may be shorter); the SELECTION is not touched — detecting
            // is not choosing, and a selection moved here would outlive turning slicing off.
            se.sliceIndex = 0;
        }

        // Any change of marker set can leave the index above the ceiling; clamped once here.
        se.sliceIndex = std::clamp(se.sliceIndex, 0, se.slice_index_ceiling());

        // ── The WAVEFORM ─────────────────────────────────────────────────────────────────────────
        // Re-binned when the window (`view_start`/`view_end`, which fold in zoom and scrolling) or the
        // drawn CHANNEL changes.
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

    // The sample editor's waveform key — what the bins on screen were computed FROM.
    bool    wfKeyValid_ = false;
    int64_t wfStart_ = 0, wfEnd_ = 0;
    int     wfSource_ = -1, wfTotal_ = -1, wfInst_ = -1;

    int         sfCachedId_ = -1, sfCachedBank_ = -1, sfCachedPreset_ = -1;
    bool        sfCachedIsSf_ = false;
    std::string sfCachedPath_{};

    /** How still the PATCH row must be before its sound loads: holding a direction loads nothing on
     *  the way past; a deliberate step is heard as soon as the finger lifts. */
    static constexpr long long SF_RELOAD_SETTLE_MS = 150;
    int         sfReloadId_ = -1, sfReloadBank_ = -1, sfReloadPreset_ = -1;
    std::string sfReloadPath_{};
    long long   sfReloadDueMs_   = 0;
    bool        sfReloadPending_ = false;

    /** Between peak reads — a contract with the peak-hold, not a throttle (poll_peaks). */
    static constexpr long long PEAK_POLL_MS = 60;
    long long                  peaksPolledMs_ = 0;

    /** The most slots one catch-up replays (6 s) — by then both halves of any fall have reached zero
     *  (peaks scale 0.92 a slot; a marker holds 45 slots, then falls 5 px a slot down 200 px). */
    static constexpr long long PEAK_CATCHUP_SLOTS = 100;

    /** When the transport last wrote the peaks itself. Stamped on every screen. */
    long long peaksLiveMs_ = 0;

    /**
     * The EQ spectrum's bins: 620, not the module's 495 pixels — the module rescales, taking the MAX of
     * each pixel's two straddling bins so a narrow peak cannot fall between columns.
     */
    static constexpr int        EQ_SPECTRUM_BINS = 620;
    float                       eqSpectrum_[EQ_SPECTRUM_BINS]    = {};   // what the module draws
    float                       eqSpectrumRaw_[EQ_SPECTRUM_BINS] = {};   // this frame's transform

    /** Analyser time constants, ms to 63% of a step: rise keeps a hit's edge, fall lets a steady
     *  sound hold a readable shape. */
    static constexpr float     EQ_RISE_MS       = 25.0f;
    static constexpr float     EQ_FALL_MS       = 250.0f;
    static constexpr long long EQ_SMOOTH_GAP_MS = 100;

    bool      eqSmoothValid_  = false;
    int       eqSmoothSource_ = -1;
    int       eqSmoothInstr_  = -1;
    long long eqSmoothMs_     = 0;
};

}  // namespace pt::ui
