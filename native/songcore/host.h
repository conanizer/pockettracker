#ifndef POCKETTRACKER_SONGCORE_HOST_H
#define POCKETTRACKER_SONGCORE_HOST_H

// ─── The songcore runtime ────────────────────────────────────────────────────────────────────────
//
// The object an application owns to make songcore play: the Project, the bus (MidiRouter), the
// Sequencer and the trace sink, with the verbs — load, play/stop, poll, render, read playheads.
// Platform-free: the only outside dependency is the portable AudioEngine core.
//
// Single-threaded by contract: every verb is called from the app's UI/transport thread, never from
// the audio callback. The engine calls it makes land in the engine's lock-free queues.

#include <algorithm>
#include <cstdint>
#include <fstream>
#include <set>
#include <string>

#include <functional>

#include "../audio-engine.h"
#include "../common/byte_source.h"   // pt_read_file
#include "engine_consumer.h"
#include "engine_setup.h"
#include "midi_in.h"
#include "midi_out.h"
#include "model.h"
#include "project_io.h"
#include "project_ops.h"
#include "render.h"
#include "router.h"
#include "sample_edit.h"
#include "scheduler.h"
#include "sha1.h"
#include "trace_writer.h"

namespace songcore {

class SongcoreHost {
  public:
    // `engine` may be null — a trace-only host, for testing the scheduler without audio. The sample
    // rate is re-read from the engine on every verb: a device change can alter it mid-session.
    SongcoreHost(AudioEngine* engine, int sampleRate)
        : engine_(engine),
          sampleRate_(sampleRate),
          seq_(router_, project_, sampleRate),
          consumer_(engine, &project_, &routing_),
          external_(&project_) {
        // The engine consumer is the bus's permanent subscriber; the trace writer joins only while
        // tracing (set_trace) and sees the identical records.
        if (engine_) router_.add_consumer(&consumer_);
        // ⚠️ The EXTERNAL consumer is attached always, port or not: half its job is bookkeeping (which
        // track owns which note), and attaching mid-song would leave it blind to what is sounding.
        router_.add_consumer(&external_);

        // MIDI in: the drain runs on the ENGINE's thread (see below). Nothing here touches a port.
        midiLive_.pipeline = &midiIn_;
        midiLive_.engine   = engine_;
        if (engine_) engine_->setLiveInput(&midiLive_);
    }

    // ⚠️ The engine's live-input pointer names a member of this object, so it is withdrawn here. The
    // caller must close the audio stream before destroying the host.
    ~SongcoreHost() {
        if (engine_) engine_->setLiveInput(nullptr);
        set_trace(false, "");
    }

    MidiRouter& router() { return router_; }
    Sequencer&  sequencer() { return seq_; }
    const Project& project() const { return project_; }

    // ── ↕ EXTERNAL MIDI out ──────────────────────────────────────────────────────────────────────
    // The port is the platform's; everything above it is midi_out.h. The shell hands the open port in.
    ExternalConsumer& midi_out() { return external_; }
    void set_midi_out(IMidiOut* out) { external_.set_out(out); }
    void set_midi_offset_ms(int ms) { external_.set_offset_ms(ms); }
    /** The 24 PPQN clock + transport out. Takes effect at the next transport start. */
    void set_midi_sync_out(bool on) { external_.set_sync_out(on); }
    bool midi_sync_out() const { return external_.sync_out(); }

    /**
     * Hand the queue's release to a sender thread (`shell/midi-sender.h`).
     * ⚠️ One owner only: with `poll()` still pumping beside the thread, a broken sender would look
     * like a working one with worse jitter. Hosts that never call this keep pumping in `poll()`.
     */
    void set_midi_pump_external(bool external) { midiPumpExternal_ = external; }
    bool midi_pump_external() const { return midiPumpExternal_; }

    // ── ↕ MIDI in ────────────────────────────────────────────────────────────────────────────────
    //
    // The PORT is the platform's (`IMidiIn`); from the first byte on it is songcore's —
    // `MidiInPipeline` (midi_in.h).
    // ⚠️ The drain runs on the AUDIO thread, at the top of each live block, so a key sounds in the
    // block it arrives in. This thread, from `poll()`, publishes the routing facts the drain routes
    // against (`arm_midi_in`) and takes back what it handled (`drain_midi_in`): mapped knobs, thru,
    // bookkeeping, observer. With no engine, `poll()` runs the drain itself — same code, same order.

    /**
     * Where a backend delivers its bytes — `IMidiIn::set_sink(&host.midi_in_sink())`.
     * Called from an unknown thread, so it is a lock-free ring and nothing else.
     */
    MidiInQueue& midi_in_sink() { return midiIn_.sink(); }

    /** The routing policy's counters and the parser's, for the screens and the exit report. */
    const MidiInputRouter& midi_in_router() const { return midiIn_.router(); }
    const MidiParser&      midi_in_parser() const { return midiIn_.parser(); }
    const MidiInPipeline&  midi_in_pipeline() const { return midiIn_; }

    /**
     * Told about every message the drain handled, with its records, one poll later. Nullable; the
     * drain counts regardless — the counters tell "no cable" from "no track listening".
     */
    void set_midi_in_observer(IMidiInObserver* obs) { midiInObserver_ = obs; }

    /** What a live key plays: `instrument` on `track` (the SONG cursor's), over `voices` tracks
     *  (1 = MONO), and whether velocity counts. Pushed every frame by the shell; published to the
     *  drain by the next poll, only when it changed. Without it a configured keyboard is silent on a
     *  stopped song. */
    void set_midi_in_play(int instrument, int track, int voices, bool velocity = true) {
        midiInInstrument_ = instrument;
        midiInTrack_      = track;
        midiInVoices_     = voices;
        midiInVelocity_   = velocity;
    }

    /**
     * MIDI THRU — whether a live key on an EXTERNAL track reaches the cable. ON by default: playing
     * gear through the tracker is the point of an input port.
     * ⚠️ When input and output are the SAME device, thru is a feedback loop. The shell compares the
     * port names and turns it off when they match.
     */
    void set_midi_in_thru(bool on) { midiInThru_ = on; }
    bool midi_in_thru() const { return midiInThru_; }

    /** Bytes that reached the ring, and complete messages the parser made of them. */
    uint64_t midi_in_bytes() const { return midiIn_.bytes(); }
    uint64_t midi_in_messages() const { return midiIn_.messages(); }

    /** Records the drain handed to the engine, to the cable, and withheld from the cable by thru. */
    uint64_t midi_in_injected() const { return midiIn_.injected(); }
    uint64_t midi_in_thru_sent() const { return midiInThruSent_; }
    uint64_t midi_in_thru_suppressed() const { return midiInThruSuppressed_; }

    /**
     * Forget everything mid-flight — parked bytes and a half-assembled message.
     * ⚠️ Called when a port closes: running status from a pulled cable would complete a phantom note
     * from the next port's first data byte.
     * ⚠️ A REQUEST, honoured at the drain's next block (the parser belongs to the audio thread);
     * at once when there is no engine.
     */
    void reset_midi_in() {
        midiIn_.request_reset();
        if (!engine_) midiIn_.service_requests();
    }

    // ── ↓ data ───────────────────────────────────────────────────────────────────────────────────
    // The blob is the .ptp JSON. Pushing REPLACES the project in place: `project_` never moves, so the
    // Sequencer's pointer stays valid mid-playback. Costs a full parse; the app edits the live
    // document in place instead (edit_project below).
    bool push_project(const std::string& blob) {
        // allow_exceptions=false: a malformed blob leaves the previous project intact.
        json j = json::parse(blob, /*cb=*/nullptr, /*allow_exceptions=*/false);
        if (j.is_discarded() || !j.is_object()) return false;

        Project parsed = parse_project(j);
        normalize_and_migrate(parsed);   // pool repair + v0→1 table-volume migration
        project_ = std::move(parsed);
        projectSha_ = sha1_hex(blob);
        // Table data may have changed, so the consumer's "already sent" cache must go.
        consumer_.invalidate_tables();
        return true;
    }

    // ── ↓ transport ──────────────────────────────────────────────────────────────────────────────
    // Each returns the frame the transport latched (the trace's session base).
    // CHAIN and PHRASE take the MIXER TRACK they play on — its fader, mute, voice slot and FX —
    // defaulting to 0.
    int64_t play_song(int startRow)   { before_play(); seq_.playSong(startRow);     return after_play(); }
    /** LIVE mode from a standing start: `mask` bit N launches track N at `songRow`, the rest begin silent. */
    int64_t play_song_live(int songRow, int mask) {
        before_play(); seq_.playSongLive(songRow, mask); return after_play();
    }
    int64_t play_chain(int chainId, int trackId = 0) {
        before_play(); seq_.playChain(chainId, trackId);   return after_play();
    }
    int64_t play_phrase(int phraseId, int trackId = 0) {
        before_play(); seq_.playPhrase(phraseId, trackId); return after_play();
    }

    /**
     * Stop the transport — the scheduler AND the engine.
     *
     * ⚠️ Stopping only the scheduler is not enough: notes already handed to the engine sit in its
     * queue ahead of the clock, and would keep playing — and a START would then layer a second
     * stream on top.
     * ⚠️ Order matters: the master EQ is restored BEFORE the queues are cleared, because an EQM
     * override may be waiting in the param queue.
     * The play_* verbs do not call this; the dispatcher stops before it starts, and an extra stop
     * would add a `t_stop` to the goldened traces.
     */
    void stop() {
        // ⚠️ Two sources arm this restore: a phrase EQM (the scheduler knows) and a TABLE row's EQM
        // (applied inside the engine). Read the engine's latch UNCONDITIONALLY, or it leaks into the
        // next take.
        const bool table_eqm = engine_ && engine_->takeTableMasterEqTouched();
        if (engine_ && (seq_.eqm_active() || table_eqm) && seq_.has_live_project()) {
            engine_->setMasterEqSlot(project_.masterEqSlot);
        }
        // VTR/VMV replaced the faders; put the authored values back, also before the queues clear so
        // a queued fader move cannot land after the restore.
        if (engine_ && seq_.mixer_vol_active() && seq_.has_live_project()) {
            const int tracks = static_cast<int>(project_.tracks.size());
            for (int i = 0; i < 8 && i < tracks; ++i)
                engine_->setTrackVolume(i, hex_to_float(project_.tracks[static_cast<size_t>(i)].volume));
            engine_->setMasterVolume(hex_to_float(project_.masterVolume));
        }
        // The delay time a TIM took over — two sources again; read the engine's latch unconditionally.
        const bool table_tim = engine_ && engine_->takeTableDelayTimeTouched();
        if (engine_ && (seq_.delay_time_active() || table_tim) && seq_.has_live_project()) {
            engine_->setDelayTime(project_.delayTime, project_.delaySync,
                                  static_cast<float>(project_.tempo));
        }
        sync_clock();
        seq_.stop();
        // Every note the cable holds, ended now; `seq_.stop()` does not reach the router.
        external_.panic();
        // …and the incoming keys, so the next chord does not steal from a note that is over. A
        // request: the allocator is the drain's.
        midiIn_.request_release_keys();
        if (!engine_) midiIn_.service_requests();
        if (engine_) {
            engine_->clearScheduledNotes();   // the lookahead: notes, kills and param updates
            // The sounding voices, RAMPED rather than cut (a cut is a full-scale step). The audio
            // thread finishes the ramp after this returns.
            engine_->stopAllRamped();
            engine_->stopMetronome();         // the click is neither queued nor a voice
        }
        consumer_.clear_track_mask();
        flush_trace();
    }

    // Bit N set once track N has had a note scheduled this session (the OCTA visualizer's lanes).
    int track_mask() const { return consumer_.track_mask(); }

    // The lookahead poll, once per UI frame.
    void poll() {
        sync_clock();
        // The audio thread's drain needs the routing facts and every table a key could land on; with
        // no engine, this thread IS the drain.
        arm_midi_in();
        if (!engine_) midiIn_.run(seq_.clock(), nullptr);
        // What the drain handled since the last poll: mapped knobs, thru, counters, observer.
        drain_midi_in();
        keep_walking();
        // TEMPO is editable while playing, so the beat length is pushed every time.
        if (engine_) engine_->setMetronomeBeat(frames_per_quarter());
    }

    /**
     * The sequencer's half of `poll`: tops the lookahead up to `aheadMs` and releases the MIDI that has
     * come due. Nothing input-side runs, so a long operation may call it mid-load with a deeper ask.
     */
    void keep_walking(int64_t aheadMs = Sequencer::HORIZON_MS) {
        sync_clock();
        seq_.updatePlaybackBuffer(aheadMs);
        // ⚠️ The MIDI queue is released here (unless a sender thread owns it), even when stopped: a
        // LEN gate and a panic's note-offs are owed after the last note.
        if (!midiPumpExternal_) external_.pump(seq_.clock());
        flush_trace();
    }

    // ── ↓ the render path ────────────────────────────────────────────────────────────────────────
    // Returns the total frame span scheduled. trackFilter == nullptr renders every track.
    int64_t schedule_song_range(int startRow, int endRow, const std::set<int>* trackFilter,
                                int repeat = 1) {
        sync_clock();
        int64_t frames = seq_.scheduleSongRowRange(startRow, endRow, trackFilter, repeat);
        flush_trace();
        return frames;
    }

    // ── ↓ the render itself (render.h) ───────────────────────────────────────────────────────────
    // prepare → schedule → render → finish; render_song_range_to_wav does all four.
    void prepare_render(int startRow, int endRow) {
        if (!engine_) return;
        // ⚠️ The cable is detached for a render: the whole song is scheduled at once and never
        // polled, so an attached ExternalConsumer would fire it all at the hardware afterwards.
        external_.panic();
        router_.remove_consumer(&external_);
        songcore::prepare_render(*engine_, project_, routing_, startRow, endRow);
        consumer_.clear_track_mask();
        sync_clock();                   // the frame counter is back at 0 — re-read it
    }

    RenderStats render_to_wav(const std::string& path, int64_t songFrames,
                              int stemsMode, bool applyMasterBus,
                              const std::function<void(float)>& progress = nullptr) {
        if (!engine_) return RenderStats();
        RenderOptions opts;
        opts.stemsMode      = stemsMode;
        opts.applyMasterBus = applyMasterBus;
        return songcore::render_to_wav(*engine_, project_, songFrames, path, opts, progress);
    }

    void finish_render() {
        if (!engine_) return;
        songcore::finish_render(*engine_, project_);
        consumer_.clear_track_mask();
        router_.add_consumer(&external_);   // the cable is live again (add_consumer is idempotent)
    }

    // prepare → schedule → render → finish, with songcore's own sequencer.
    // `repeat` plays the range that many times in ONE pass — files concatenated afterwards would cut
    // the reverb and delay at every join.
    RenderStats render_song_range_to_wav(int startRow, int endRow, const std::string& path,
                                         const RenderOptions& opts = RenderOptions(),
                                         const std::function<void(float)>& progress = nullptr,
                                         int repeat = 1) {
        if (!engine_) return RenderStats();
        prepare_render(startRow, endRow);
        const int64_t songFrames = schedule_song_range(startRow, endRow, nullptr, repeat);
        RenderStats stats = render_to_wav(path, songFrames, opts.stemsMode, opts.applyMasterBus, progress);
        finish_render();
        return stats;
    }

    // The whole song, bounds and all.
    RenderStats render_song_to_wav(const std::string& path,
                                   const RenderOptions& opts = RenderOptions(),
                                   const std::function<void(float)>& progress = nullptr) {
        const SongBounds b = find_song_bounds(project_);
        if (b.empty()) return RenderStats();
        return render_song_range_to_wav(b.startRow, b.endRow, path, opts, progress);
    }

    // Load the project's samples and SoundFonts and learn the Routing (engine_setup.h).
    // ⚠️ It also WRITES to the project: a WAV's cue points become its slice markers.
    MediaLoadResult load_media(const std::string& baseDir) {
        lastMediaLoad_ = MediaLoadResult();
        if (!engine_) return lastMediaLoad_;
        mediaRoots_.baseDir = baseDir;
        lastMediaLoad_ =
            load_project_media(*engine_, project_, baseDir, mediaRoots_.appRoot, routing_);
        return lastMediaLoad_;
    }

    /**
     * What the most recent `load_media` found — recorded here so every loading path reports it.
     * ⚠️ Without it a failed sample load (including out-of-memory on a small device) is invisible
     * where there is no console: the instrument just plays silence.
     */
    const MediaLoadResult& last_media_load() const { return lastMediaLoad_; }

    /**
     * THIS install's app root (Samples/, Soundfonts/… live under it), set once at boot. A project from
     * another install has its dead absolute media paths re-rooted onto it at load
     * (resolve_media_path). Unset — as in the host tools — leaves paths as they are.
     */
    void set_app_root(std::string root) { mediaRoots_.appRoot = std::move(root); }

    // ── ↓ the LIVE param push (engine_setup.h) ───────────────────────────────────────────────────
    //
    // What the engine holds on its own and keeps across project swaps: mixer, master bus, sends, EQ
    // bank, instrument params. No event pushes it.
    // ⚠️ Call push_params() after load_media(), or what you hear is the previous project's settings.
    void push_params() {
        if (!engine_) return;
        push_live_params(*engine_, project_, routing_);
    }

    /**
     * One instrument's params — what an INSTRUMENT / MODS / pool edit pushes.
     * ⚠️ Also refreshes the notes already sounding on it: a voice COPIES filter, drive, crush and sends
     * at trigger, so without this an edit is unheard until the next note.
     */
    void push_instrument(int id, bool refreshSounding = true) {
        if (!engine_) return;
        if (id < 0 || id >= static_cast<int>(project_.instruments.size())) return;
        push_instrument_params(*engine_, project_.instruments[id], routing_, project_.tempo, sampleRate_);
        // `sampleId` is the index the params were written at and the one a voice remembers.
        // ⚠️ Only for an EDIT: the refresh wipes what a table row or phrase command set on the voice.
        if (refreshSounding) engine_->refreshSoundingInstrument(project_.instruments[id].sampleId);
    }

    /**
     * The GLOBALS — mixer, master bus, sends, EQ bank, master EQ. What MIXER and EFFECTS edits push.
     * ⚠️ Not push_params(): that also sweeps 128 instruments (~2,500 engine calls) on every key-repeat.
     */
    void push_globals() {
        if (!engine_) return;
        push_mixer(*engine_, project_, held_by_song());
    }

    // ── ↕ a mapped knob (midi_map.h) ─────────────────────────────────────────────────────────────

    /**
     * Which incoming channel may carry MAPPING knobs: 0-15, `MIDI_CTL_CH_ALL`, or −1 for none (the
     * start state; the shell pushes the setting at boot).
     */
    void set_midi_control_channel(int ch) {
        controlChannel_ = (ch < 0 || ch > MIDI_CTL_CH_ALL) ? -1 : ch;
    }
    int  midi_control_channel() const { return controlChannel_; }

    /**
     * How many mapped destinations the cable has moved, ever. A counter, not a callback: the UI reads
     * it once a frame and marks the song dirty, so a knob sweep costs one bump per frame.
     */
    uint64_t mapped_cc_writes() const { return mappedCcWrites_; }

    /**
     * MIDI LEARN is armed (`R` held), so the next knob on the control channel names a destination.
     * The arm is a bool pushed down; the result is a counter watched from above — the drain can never
     * call into the UI.
     */
    void set_midi_learn_armed(bool on) { learnArmed_ = on; }
    bool midi_learn_armed() const { return learnArmed_; }

    /** Moves once per knob seen while learn was armed, with the controller and channel it saw. */
    uint64_t midi_learn_events() const { return learnEvents_; }
    int      midi_learn_controller() const { return learnController_; }
    int      midi_learn_channel() const { return learnChannel_; }

    /** The channel the last incoming CC arrived on, or −1 — how the user finds their controller's
     *  channel for the `CTL CH` row. */
    int last_cc_channel() const { return lastCcChannel_; }

    /** What the live audio callback costs over the last second; all zero without an engine. */
    AudioEngine::BlockTiming block_timing() const {
        return engine_ ? engine_->getBlockTiming() : AudioEngine::BlockTiming{};
    }

    /**
     * A mapped knob moved: write the value into the project and make it heard. Returns how many
     * mappings that controller drove (0 = none). The value is written at once — a knob has no
     * button-up; only the autosave is debounced (the dispatcher's job).
     * One controller may drive several destinations; a mapping whose destination is gone is skipped,
     * not deleted (the list screen greys it).
     */
    int apply_mapped_cc(int controller, int value) {
        int applied = 0;
        for (const MidiMapping& m : project_.midiMappings) {
            if (m.controller != controller) continue;
            const MapDest* d = map_dest(m.dest);
            if (!d) continue;   // an id from a newer version

            if (!write_mapped(project_, m, scale_cc(value, m.rangeMin, m.rangeMax))) continue;
            ++applied;
            if (!engine_) continue;

            if (d->scope == MapScope::INSTRUMENT) {
                // VOL and PAN are baked into a note when it is scheduled, so they reach the next row
                // the walk reaches; every other mapped parameter is engine state a voice reads live.
                push_instrument(m.scopeIndex);
            } else {
                push_mapped_dest(*engine_, project_, d->id, m.scopeIndex);
                release_song_hold(d->id, m.scopeIndex);
            }
        }
        return applied;
    }

    /**
     * What the running take owns now, so `push_globals()` can push the authored mixer without wiping
     * it (any MIXER/EFFECTS edit mid-song pushes). The same questions `stop()`'s restore asks.
     * ⚠️ PEEK, never take, the table latches: they arm the restore in `stop()`.
     */
    MixerHeld held_by_song() const {
        MixerHeld held;
        if (!engine_ || !seq_.has_live_project()) return held;
        held.faderTracks = seq_.mixer_vol_tracks();
        held.masterFader = seq_.master_vol_active();
        held.masterEq    = seq_.eqm_active() || engine_->tableMasterEqTouchedPeek();
        held.delayTime   = seq_.delay_time_active() || engine_->tableDelayTimeTouchedPeek();
        return held;
    }

    // ── ↕ the EQ editor ──────────────────────────────────────────────────────────────────────────
    //
    // ⚠️ A band edit needs BOTH calls. `setEqBand` writes only the 128-slot bank; the master bus and
    // each instrument compile their own coefficients when a slot is ASSIGNED. So the editor re-assigns
    // the same slot to the consumer that opened it after every band nudge — and must remember which
    // one that was. Cheaper than `push_globals()` on every key-repeat.

    void set_eq_band(int slot, int band, int type, int freqHex, int gainHex, int qHex) {
        if (!engine_) return;
        engine_->setEqBand(slot, band, type, freqHex, gainHex, qHex);
    }

    void set_master_eq_slot(int slot) {
        if (!engine_) return;
        engine_->setMasterEqSlot(slot);
    }

    void set_instrument_eq_slot(int id, int slot) {
        if (!engine_) return;
        engine_->setInstrumentEqSlot(id, slot);
    }

    void set_reverb_input_eq(int slot) {
        if (!engine_) return;
        engine_->setReverbInputEq(slot);
    }

    void set_delay_input_eq(int slot) {
        if (!engine_) return;
        engine_->setDelayInputEq(slot);
    }

    /**
     * The spectrum of ONE signal path for the EQ editor: master bus (0), delay input (1), reverb input
     * (2), or one instrument's voices (3) — an EQ on a send is drawn over that send's signal. False
     * with no engine; the editor then draws an empty grid.
     */
    bool spectrum_for_source(int source, int instrId, int numBins, float* out) const {
        if (!engine_ || numBins <= 0 || !out) return false;
        engine_->getSpectrumMagnitudesForSource(source, instrId, numBins, out);
        return true;
    }

    // ── ↕ the instrument operations ──────────────────────────────────────────────────────────────
    // The verbs that own a SOURCE (engine_setup.h).
    // ⚠️ Not guarded on `engine_`: they edit the DOCUMENT and only also free engine resources. The
    // null checks live around the engine calls, inside engine_setup.h.

    void set_instrument_type(int id, InstrumentType type) {
        songcore::set_instrument_type(engine_, project_, id, type, routing_);
        push_instrument(id);   // a no-op without an engine
    }

    void clear_instrument(int id) {
        songcore::clear_instrument(engine_, project_, id, routing_);
        push_instrument(id);
    }

    // The PRESET row's list. All three answer for an instrument with no SoundFont (0 / 0 / "---"), and
    // read the FILE's index, so they work for banks too large to load.
    int sf_preset_count(int id) const {
        if (!engine_ || id < 0 || id >= POOL_INSTRUMENTS) return 0;
        return soundfont_preset_count(*engine_, project_.instruments[static_cast<size_t>(id)],
                                      mediaRoots_);
    }
    int sf_preset_index(int id) const {
        if (!engine_ || id < 0 || id >= POOL_INSTRUMENTS) return 0;
        return soundfont_preset_index(*engine_, project_.instruments[static_cast<size_t>(id)],
                                      mediaRoots_);
    }
    std::string sf_preset_name(int id) const {
        if (!engine_ || id < 0 || id >= POOL_INSTRUMENTS) return "---";
        return soundfont_preset_name(*engine_, project_.instruments[static_cast<size_t>(id)],
                                     mediaRoots_);
    }
    void set_sf_preset_by_index(int id, int index) {
        if (!engine_ || id < 0 || id >= POOL_INSTRUMENTS) return;
        songcore::set_soundfont_preset_by_index(*engine_, project_.instruments[static_cast<size_t>(id)],
                                                index, mediaRoots_);
    }

    /** Bring instrument `id`'s loaded sound in line with the preset it names; free when nothing moved,
     *  so safe every frame. */
    void sync_sf_preset(int id) {
        if (!engine_ || id < 0 || id >= POOL_INSTRUMENTS) return;
        songcore::sync_instrument_soundfont(*engine_, project_.instruments[static_cast<size_t>(id)],
                                            routing_, mediaRoots_);
    }

    /** The PATCH row's load, started rather than done. False = engine busy, ask again. */
    bool request_sf_preset(int id) {
        if (!engine_ || id < 0 || id >= POOL_INSTRUMENTS) return true;
        return songcore::request_instrument_soundfont(
            *engine_, project_.instruments[static_cast<size_t>(id)], routing_, mediaRoots_);
    }

    /** Install a finished background preset load. Called once a frame by the feed. Notes already
     *  scheduled keep the old slot; the next row the walk reaches takes the new one. */
    void poll_sf_load() {
        if (!engine_) return;
        songcore::collect_instrument_soundfont(*engine_, project_, routing_, mediaRoots_);
    }

    // ── ↕ the FILE verbs ─────────────────────────────────────────────────────────────────────────
    //
    // They take paths, not a `ui::FileSystem` — that abstraction is for the browser (listing,
    // renaming). Opening funnels through `pt_read_file` / `pt_fopen` (byte_source.h), the one place a
    // path becomes a handle.

    /** Replace the project from a .ptp on disk: stop → parse → push → load its media → push its params. */
    bool load_project_file(const std::string& path, const std::string& baseDir) {
        std::string blob;
        if (!pt_read_file(path.c_str(), blob)) return false;

        // ⚠️ The transport stops BEFORE the document is replaced. Left running, the scheduler walks
        // the old song's position through the new data, falls behind the clock, and the new song
        // starts late and without its first row. A parse failure therefore leaves the transport
        // stopped with the previous project intact — there is no position to resume to.
        const bool wasPlaying = seq_.is_playing();
        stop();

        if (!push_project(blob)) return false;

        // ⚠️ Both, in this order, or the project you hear is not the one you loaded.
        load_media(baseDir);
        push_params();

        // Loading while playing SWITCHES SONGS: a hard cut (the media decode blocks), then the new song
        // from row 0 — an old CHAIN or PHRASE id means nothing in the new document. `play_song`
        // re-reads the clock after the disk work. Nothing starts that was not already playing.
        if (wasPlaying) play_song(0);
        return true;
    }

    // ⚠️ songcore writes no user file. Every save goes through `ui::FileSystem::write_file` (temp +
    // rename + checked close), one layer up; songcore cannot depend on it. A truncating `ofstream`
    // here would destroy the old file at open and miss a failure that surfaces at flush.

    // ── PROJECT screen: NEW, and the two COMPACTs ────────────────────────────────────────────────
    //
    // The document surgery is pure (project_ops.h). These are host verbs because each also
    // invalidates state the engine holds.

    /** PROJECT → NEW. A blank document, and an engine that has forgotten the last one. */
    void new_project() {
        // This stops the transport because NEW ends the session. It does NOT make freeing PCM safe:
        // `clearAllSamples()` does that itself, stopping voices under `sampleEditMutex`.
        stop();
        songcore::new_project(project_);

        if (engine_) {
            engine_->clearAllSamples();
            engine_->clearAllSoundfonts();
        }
        routing_.reset();     // the ratios and SF slots described the old project's media

        invalidate_tables();
        push_params();
    }

    /** PROJECT → COMPACT → SEQ. Unused chains and phrases back to factory. Pure arrangement, so no
     *  engine call. */
    void clean_seq() { songcore::clean_unused_seq(project_); }

    /**
     * PROJECT → COMPACT → INST. Unused instruments, tables and grooves back to factory.
     * ⚠️ All three engine steps matter: the old buffers stay loaded until media is reloaded, and
     * without `invalidate_tables` a compacted table would go on playing its old rows.
     */
    void clean_inst(const std::string& baseDir) {
        songcore::clean_unused_inst(project_);
        load_media(baseDir);   // clears samples, SoundFonts and routing, then reloads
        invalidate_tables();
        push_params();
    }

    /** A sample (wav/mp3/flac/ogg/opus) → instrument `id`. The browser's A on a sampler slot. */
    bool load_sample(int id, const std::string& path) {
        if (!load_instrument_sample(engine_, project_, id, path, routing_)) return false;
        push_instrument(id);   // the fresh source needs its slot's filter/window/loop
        return true;
    }

    /** An .sf2/.sf3 → instrument `id`, which becomes a SOUNDFONT slot. The browser's A on one. */
    bool load_soundfont(int id, const std::string& path) {
        if (!load_instrument_soundfont(engine_, project_, id, path, routing_)) return false;
        push_instrument(id);
        return true;
    }

    /**
     * True when the last media load failed for lack of memory rather than a bad file — the two want
     * different messages. One bool, so the UI never reaches the engine.
     */
    bool last_load_ran_out_of_memory() const {
        return engine_ && engine_->lastLoadFailure() == AudioEngine::LoadFailure::OUT_OF_MEMORY;
    }

    /** True when the user cancelled the last media load: not a failure, and the UI says nothing. */
    bool last_load_cancelled() const {
        return engine_ && engine_->lastLoadFailure() == AudioEngine::LoadFailure::CANCELLED;
    }

    /**
     * Read a .pti into instrument `id`. False if it will not parse or its source file is gone — in the
     * latter case the parameters still land.
     */
    bool load_instrument_preset(int id, const std::string& path) {
        std::string blob;
        if (!pt_read_file(path.c_str(), blob)) return false;

        json j = json::parse(blob, /*cb=*/nullptr, /*allow_exceptions=*/false);
        if (j.is_discarded() || !j.is_object()) return false;

        const InstrumentPreset ip = parse_instrument_preset(j);
        const bool sourceOk = apply_instrument_preset(engine_, project_, id, ip, routing_, mediaRoots_);
        invalidate_tables();   // the preset may have brought a table with it
        push_instrument(id);
        return sourceOk;
    }

    /** Audition the file under the browser's cursor (slot 255, the preview lane). START, on a file. */
    bool preview_file(const std::string& path) {
        if (!engine_) return false;
        return preview_sample_file(*engine_, path) > 0;
    }

    /** Drop the browser's audition — what leaving the browser does. */
    void clear_previews() {
        if (!engine_) return;
        clear_preview_slots(*engine_);
    }

    // ── ↕ THE SAMPLE EDITOR ──────────────────────────────────────────────────────────────────────
    //
    // Thin forwards to the engine's DSP (sample-editor.cpp, transient-detector.cpp). Each is guarded on
    // `engine_`, so the tests can drive the whole editor with no audio device.

    // ── Reading the sample (the feed) ────────────────────────────────────────────────────────────
    int  sample_length(int id) const { return engine_ ? engine_->getSampleLength(id) : 0; }
    bool has_stereo_data(int id) const { return engine_ && engine_->hasStereoData(id); }
    /** The depth the slot's sample came in at — the ceiling of the editor's BIT cell. */
    int  sample_bit_depth(int id) const { return engine_ ? engine_->getSampleBitDepth(id) : 16; }

    /** The FILE's rate (deviceRate / ratio); 44100 when the slot is empty. See sample_edit.h. */
    int sample_rate_of(int id) const { return original_sample_rate(engine_, routing_, id); }

    /** 0..1 while the sample is sounding, −1 when it is not — the waveform's playhead. */
    float sample_playback_position(int id) const {
        return engine_ ? engine_->getSamplePlaybackPosition(id) : -1.0f;
    }

    /**
     * `bins` (min, max) pairs, so 2 × bins floats. `channel`: 0 left, 1 right, 2 averaged (stereo only).
     * A range covering the whole sample — (0, 0) or (0, length) — takes the whole-sample entry point;
     * both spellings must draw identically.
     */
    std::vector<float> sample_waveform(int id, int bins, int startFrame = 0, int endFrame = 0,
                                       int channel = 2) const {
        std::vector<float> out(static_cast<size_t>(std::max(bins, 0)) * 2, 0.0f);
        if (!engine_ || bins <= 0) return out;

        const int  len   = engine_->getSampleLength(id);
        const bool whole = (startFrame <= 0) && (endFrame <= 0 || endFrame >= len);

        if (engine_->hasStereoData(id)) {
            // Stereo always uses the channel-aware entry point: the plain one averages, and
            // SOURCE=LEFT must draw the left channel.
            engine_->getSampleWaveformRangeSource(id, whole ? 0 : startFrame, whole ? len : endFrame,
                                                  out.data(), bins, channel);
        } else if (whole) {
            engine_->getSampleWaveform(id, out.data(), bins);
        } else {
            engine_->getSampleWaveformRange(id, startFrame, endFrame, out.data(), bins);
        }
        return out;
    }

    /** The slice boundaries the detector finds at `sensitivity`, capped at 128. */
    std::vector<int> detect_transients(int id, int sensitivity) const {
        if (!engine_) return {};
        int       markers[128];
        const int n = engine_->detectTransients(id, sensitivity, markers, 128);
        return std::vector<int>(markers, markers + std::max(n, 0));
    }

    /** The nearest zero crossing to `frame` in direction `dir` (−1 back, +1 forward, 0 either), in the
     *  signal the SOURCE mode will cut — both channels under STEREO. */
    int find_zero_crossing(int id, int frame, int dir, int sourceMode = 0) const {
        return engine_ ? engine_->findZeroCrossing(id, frame, dir, 512, sourceMode) : frame;
    }

    int clipboard_length() const { return engine_ ? engine_->getClipboardLength() : 0; }

    // ── The destructive operations ───────────────────────────────────────────────────────────────
    void backup_sample(int id) { if (engine_) engine_->backupSample(id); }
    void undo_sample(int id) { if (engine_) engine_->undoSample(id); }
    /** The editor is closing: its single-level undo is now just held memory. */
    void free_sample_undo(int id) { if (engine_) engine_->freeSampleUndo(id); }

    void crop_sample(int id, int start, int end) { if (engine_) engine_->cropSample(id, start, end); }
    void delete_sample_region(int id, int start, int end) { if (engine_) engine_->deleteSampleRegion(id, start, end); }
    void copy_region(int id, int start, int end) { if (engine_) engine_->copyRegion(id, start, end); }
    void paste_region(int id, int insertAt) { if (engine_) engine_->pasteRegion(id, insertAt); }

    void normalize_sample(int id, int start, int end) { if (engine_) engine_->normalizeSample(id, start, end); }
    void fade_in_sample(int id, int start, int end) { if (engine_) engine_->fadeInSample(id, start, end); }
    void fade_out_sample(int id, int start, int end) { if (engine_) engine_->fadeOutSample(id, start, end); }
    void silence_region(int id, int start, int end) { if (engine_) engine_->silenceRegion(id, start, end); }
    void reverse_sample(int id, int start, int end) { if (engine_) engine_->reverseSample(id, start, end); }

    // ── The FX row: a non-destructive preview, and a destructive apply ───────────────────────────
    //
    // START applies the effect for real and plays it; the next gesture restores this backup. Only
    // APPLY keeps it. Separate from the undo slot: "before I previewed" vs "before I committed".
    void save_fx_preview_backup(int id) { if (engine_) engine_->saveFxPreviewBackup(id); }
    void restore_fx_preview_backup() { if (engine_) engine_->restoreFxPreviewBackup(); }

    void apply_sample_fx(int id, int fxType, int fxValue) {
        if (!engine_) return;
        engine_->applySampleFx(id, fxType, fxValue,
                               static_cast<float>(sample_rate_of(id)), project_.limiterPreGain);
    }

    // ── The three that change the rate ratio, so live in songcore (sample_edit.h) ────────────────
    void apply_rate_and_bits(int id, int factor, int bits) {
        songcore::apply_rate_and_bits(engine_, routing_, rateCache_, id, factor, bits);
    }
    void pitch_shift_sample(int id, float semitones) {
        songcore::pitch_shift_sample(engine_, rateCache_, id, semitones);
    }
    void time_stretch_sample(int id, float ratio) {
        songcore::time_stretch_sample(engine_, rateCache_, id, ratio);
    }

    // ── The audition ─────────────────────────────────────────────────────────────────────────────

    /**
     * The editor's START: the sample DRY at its root, through the SOURCE mode's channel, windowed to
     * the SELECTION.
     * ⚠️ The window goes in as FRAMES (`setInstrumentFrameWindow`), not the 0-255 sampleStart/End grid
     * (8 ms steps on a 2 s sample) — the audition must be exactly what CROP will keep.
     * ⚠️ The frame window (and a swapped `sampleId`) is read when the note FIRES, 100 frames later, so
     * it outlives this call; `finish_sample_preview()` ends it on the dispatcher's deadline.
     */
    void preview_sample_editor(int id, int sourceMode, int64_t selStart, int64_t selEnd,
                               int totalFrames, int pitchSemitones) {
        if (!engine_ || id < 0 || id >= POOL_INSTRUMENTS) return;
        Instrument& ins = project_.instruments[static_cast<size_t>(id)];

        const Note savedRoot = ins.root;
        // A pending pitch shift is heard by transposing the ROOT; nothing is resampled until SAVE.
        if (pitchSemitones != 0)
            ins.root = note_from_midi(std::clamp(note_to_midi(ins.root) + pitchSemitones, 0, 127));

        const int slot = prepare_source_preview(*engine_, id, sourceMode);

        // A voice's window is keyed on the SLOT it plays from, so the params go to the scratch slot
        // when there is one.
        const int savedSampleId = ins.sampleId;
        if (slot != id) ins.sampleId = slot;
        push_instrument_playback_params(*engine_, ins);
        // After the push, which clears any previous window. The scratch slot is a frame-for-frame
        // copy, so the selection indexes both.
        if (totalFrames > 0 && selEnd > selStart)
            engine_->setInstrumentFrameWindow(slot, static_cast<int>(selStart), static_cast<int>(selEnd));
        preview_instrument_dry(*engine_, ins, slot, routing_.sampleRateRatio[id]);
        if (slot != id) ins.sampleId = savedSampleId;

        ins.root = savedRoot;   // read at SCHEDULE time, so it can go back at once
    }

    /**
     * Put the instrument back — window, EQ, sends, modulation. Runs on the dispatcher's 100 ms
     * deadline, or at once if a second START arrives first. The push itself disarms the frame window.
     */
    void finish_sample_preview(int id) {
        if (!engine_ || id < 0 || id >= POOL_INSTRUMENTS) return;
        Instrument& ins = project_.instruments[static_cast<size_t>(id)];
        push_instrument_playback_params(*engine_, ins);
        // The push only reaches the instrument's own slot; a channel audition used the scratch slot.
        engine_->setInstrumentFrameWindow(SOURCE_PREVIEW_SLOT, -1, -1);
        // The three the DRY preview switched off.
        push_instrument_mod_eq_sends(*engine_, ins, project_.tempo, engine_->getSampleRate());
    }

    // ── SAVE and CHOP ────────────────────────────────────────────────────────────────────────────

    /** The edited PCM → a WAV at `path`, slices in the `cue ` chunk. `bits` 0 = the loaded depth. */
    bool save_sample_wav(int id, const std::string& path, const std::vector<int>& cuePoints,
                         int sourceMode, bool hasStereo, int bits = 0) {
        if (!engine_) return false;
        return songcore::save_sample_wav(*engine_, routing_, id, path, cuePoints, sourceMode, hasStereo,
                                         bits);
    }

    /**
     * After a SAVE that kept the slot's buffer: that buffer is now the file, so its depth becomes the
     * saved one and the RATE/BIT original is dropped — else the next RATE/BIT touch would restore
     * pre-save audio.
     */
    void adopt_saved_sample(int id, int bits) {
        if (!engine_) return;
        const int depth = songcore::resolve_save_bits(*engine_, id, bits);
        engine_->adoptSavedSampleFormat(id, depth, depth == 32 && engine_->isSampleFloat(id));
        rateCache_.clear(id);
    }

    /** Every slice → its own WAV in `dir`. Returns how many were written. */
    int chop_sample(int id, const std::string& dir, const std::string& baseName,
                    const std::vector<std::pair<int64_t, int64_t>>& slices, int bits = 0) {
        if (!engine_) return 0;
        return songcore::chop_sample(*engine_, routing_, id, dir, baseName, slices, bits);
    }

    // ── ↕ live editing ───────────────────────────────────────────────────────────────────────────
    //
    // The UI edits THIS project in place; the Sequencer reads the same object, so an edit while
    // playing is heard from the next row the walk reaches (Sequencer::HORIZON_MS ahead).
    // ⚠️ A TABLE edit must call invalidate_tables(): the consumer caches what it already pushed.
    Project& edit_project() { return project_; }
    void     invalidate_tables() { consumer_.invalidate_tables(); }

    // ── ↕ LIVE mode ──────────────────────────────────────────────────────────────────────────────
    //
    // Queue-and-launch. Each verb arms a slot and rewinds its track so the launch lands on the
    // boundary it was aimed at (the scheduler runs ahead of the clock). Dropping the queued notes past
    // that frame is the host's half.

    bool               live_mode() const              { return seq_.live_mode(); }
    songcore::LiveSlot live_queue(int track) const    { return seq_.live_queue(track); }
    bool               live_silent(int track) const   { return seq_.live_silent(track); }

    void set_live_mode(bool on)                       { sync_clock(); apply_rollback(seq_.set_live_mode(on, seq_.clock())); }
    void queue_live(int track, int songRow, bool now) { sync_clock(); apply_rollback(seq_.queue_live(track, songRow, now, seq_.clock())); }
    void queue_live_stop(int track, bool now)         { sync_clock(); apply_rollback(seq_.queue_live_stop(track, now, seq_.clock())); }
    void queue_live_row(int songRow, bool now)        { sync_clock(); apply_rollback(seq_.queue_live_row(songRow, now, seq_.clock())); }

    // ── ↕ the note preview ───────────────────────────────────────────────────────────────────────
    //
    // Plays on the dedicated PREVIEW LANE (track 8, a ninth voice), so it steals nothing from a song.
    // ⚠️ Through `plan_note_on`, the sequencer's own derivation — a hand-rolled copy would drift. The
    // payload is a note with no phrase: no FX, no transpose, velocity −1, the instrument's volume and
    // pan, `tableId = -1` (its own table).
    // ⚠️ It also goes to the CABLE, handed to `external_` directly rather than via `router_`: the
    // router feeds the trace writer (no preview belongs in a trace) and the engine consumer (which
    // cannot carry `rootAudition`). The routing verdict is the shared model predicate either way.
    //
    // `durationFrames <= 0` = no timed kill: the voice rings until stop_preview() — an instrument
    // audition. `tableIdOverride` lets the TABLE screen audition the table it is showing.
    void preview_note(int instrumentId, const Note& note, int64_t durationFrames,
                      bool rootAudition = false, int tableIdOverride = -1) {
        if (!engine_) return;
        if (note == Note::EMPTY()) return;   // A on an empty cell must not thump the lane
        if (instrumentId < 0 || instrumentId >= static_cast<int>(project_.instruments.size())) return;
        const Instrument& ins = project_.instruments[instrumentId];
        const bool external = instrument_routes_external(ins);

        engine_->requestResume();
        const int64_t frame = engine_->getCurrentFrame() + 100;  // a short lead-in

        Event ev{};
        ev.type       = EV_NOTE_ON;
        ev.frame      = frame;
        ev.track      = AudioEngine::PREVIEW_LANE;
        ev.instrument = static_cast<int16_t>(instrumentId);

        NoteOnPayload& n = ev.noteOn;
        n.note        = static_cast<uint8_t>(note_to_midi(note));
        // ⚠️ The velocity fields are wired differently per destination. The sampler takes VOL as
        // `velGain` (the crossed wiring, see event.h). The cable and the SoundFont read velocity −1
        // as "derive from velGain = (V/127)²" and take a square root, which would boost a raw VOL. So
        // they get full velocity, with VOL in the field that scales by VOL — as a played note has it.
        const bool fullVelocity = external || ins.instrumentType == InstrumentType::SOUNDFONT;
        n.velocity    = fullVelocity ? 127 : -1;
        n.velGainBits = f32_bits(fullVelocity ? 1.0f : hex_to_float(ins.volume));   // seam arg `volume`
        n.volGainBits = f32_bits(fullVelocity ? hex_to_float(ins.volume) : 1.0f);   // seam arg `phraseVol`
        n.panBits     = f32_bits(hex_to_float(ins.pan));
        n.start = -1; n.slice = -1; n.tableId = tableIdOverride; n.tableRow = -1;
        n.transpose = 0; n.pit = 0; n.arp = 0;
        n.pslOffBits = n.pslDurBits = n.pbnRateBits = n.vibSpdBits = n.vibDepBits = f32_bits(0.0f);

        // ⚠️ Unconditionally, whichever way this instrument routes: a note-on for an internal
        // instrument on a lane last used by an EXTERNAL one ends the note the cable still holds.
        // START is exempt from `on_stop_preview`, so nothing else would.
        external_.consume(ev);

        if (external) {
            // A timed audition owes the cable a note-off (the engine's half is `scheduleKill` below).
            // `end_note` takes the earlier of this and the LEN gate.
            if (durationFrames > 0) preview_note_off(frame + durationFrames);
            return;   // no voice, cable or not: EXTERNAL means "not this engine"
        }

        // A fresh cache every preview, so a table edit is heard at once.
        bool tableLoaded[POOL_TABLES] = {false};
        plan_note_on(*engine_, ev, project_, routing_, tableLoaded, rootAudition);

        if (durationFrames > 0) engine_->scheduleKill(frame + durationFrames, AudioEngine::PREVIEW_LANE);
    }

    /**
     * Audition an instrument at its own ROOT — START on INSTRUMENT / INST_POOL / MODS, and on TABLE
     * with a table override. It rings out until the next plain button press, and it is a ROOT
     * AUDITION: the SoundFont path must know, or its 60 − root transpose would play a flat C-4.
     */
    void preview_instrument(int instrumentId, int tableIdOverride = -1) {
        if (instrumentId < 0 || instrumentId >= static_cast<int>(project_.instruments.size())) return;
        preview_note(instrumentId, project_.instruments[instrumentId].root, /*durationFrames=*/0,
                     /*rootAudition=*/true, tableIdOverride);
    }

    /**
     * Point the preview lane at mixer channel `trackId` (0..7), or −1 for unity gain. An index, not a
     * gain, so the live fader is re-read every block. The lane keeps its own voice either way.
     */
    void set_preview_track(int trackId) {
        if (engine_) engine_->setPreviewTrack(trackId);
    }

    /**
     * Silence the audition lane ("press any button to stop the preview").
     * ⚠️ Both halves: an EXTERNAL audition with `midiLen == 0` has no next note to end it, and gear
     * holds an unanswered note-on until power-cycled.
     * `cut` ends the lane in ~6 ms with no release tail (letting go of a phrase note's A).
     */
    void stop_preview(bool cut = false) {
        if (!engine_) return;
        const int64_t now = engine_->getCurrentFrame();
        if (cut) engine_->scheduleCut(now, AudioEngine::PREVIEW_LANE);
        else     engine_->scheduleKill(now, AudioEngine::PREVIEW_LANE);
        preview_note_off(now);
        // ⚠️ An auditioned TABLE can carry an EQM, and no transport stop follows an audition — so the
        // master EQ is restored here. Only while IDLE: playing, the latch belongs to stop() and the
        // song's own table rows.
        if (!seq_.is_playing() && seq_.has_live_project() && engine_->takeTableMasterEqTouched())
            engine_->setMasterEqSlot(project_.masterEqSlot);
    }

    // ── ↑ feedback ───────────────────────────────────────────────────────────────────────────────

    // One track's playhead. In SONG mode the eight run independently, so the UI asks per track.
    PlaybackPosition playheads(int trackId) {
        sync_clock();
        return seq_.getPlaybackPosition(trackId);
    }

    bool is_playing() const { return seq_.is_playing(); }

    /** The device rate the sequencer runs at. */
    int sample_rate() const { return sampleRate_; }

    // ── ↑ debug: the conformance trace ───────────────────────────────────────────────────────────
    // Enable AFTER the project is pushed: the header's project= is the sha of the pushed blob.
    void set_trace(bool enabled, const std::string& path) {
        if (enabled == traceEnabled_) return;
        if (enabled) {
            traceFile_.open(path, std::ios::binary | std::ios::trunc);
            if (!traceFile_.is_open()) return;
            traceBuf_.clear();
            writer_.begin(&traceBuf_, projectSha_);
            router_.add_consumer(&writer_);
            traceEnabled_ = true;
        } else {
            flush_trace();
            router_.remove_consumer(&writer_);
            writer_.end();
            if (traceFile_.is_open()) traceFile_.close();
            traceEnabled_ = false;
        }
    }

    bool trace_enabled() const { return traceEnabled_; }

  private:
    /**
     * A mapped knob IS the press: whatever the take held on that destination is the hand's now, or
     * `MixerHeld` would skip the very fader the knob is moving.
     * ⚠️ A TABLE's TIM latch is left alone — it arms the restore in `stop()`.
     */
    void release_song_hold(MapDestId id, int scopeIndex) {
        switch (id) {
            case MapDestId::TRACK_VOL:  seq_.release_mixer_vol_track(scopeIndex); break;
            case MapDestId::MASTER_VOL: seq_.release_master_vol();                break;
            case MapDestId::DLY_TIME:   seq_.release_delay_time();                break;
            default: break;
        }
    }

    int      controlChannel_  = -1;      // -1 = no channel is reserved for mapping knobs
    uint64_t mappedCcWrites_  = 0;
    bool     learnArmed_      = false;   // `R` is down: the next knob NAMES rather than drives
    uint64_t learnEvents_     = 0;
    int      learnController_ = -1;
    int      learnChannel_    = -1;
    int      lastCcChannel_   = -1;      // whatever channel the cable last carried a CC on

    // Drop what the rewound tracks had queued. ⚠️ One frame PER TRACK: clearing every track from
    // the earliest boundary would drop notes a track further ahead will not schedule again.
    void apply_rollback(const songcore::RollbackPlan& plan) {
        if (!engine_) return;
        for (int t = 0; t < 8; ++t)
            if (plan.frames[t] >= 0) engine_->clearScheduledNotesFrom(plan.frames[t], t);
    }

    // The engine's frame counter IS the transport clock. With no engine it stays where a test put it.
    void sync_clock() {
        if (!engine_) return;
        seq_.set_clock(engine_->getCurrentFrame());
        int sr = engine_->getSampleRate();
        if (sr > 0) sampleRate_ = sr;
        seq_.set_sample_rate(sampleRate_);
    }

    void before_play() {
        sync_clock();
        if (engine_) engine_->startTake();
    }

    int64_t after_play() {
        resync_soundfont_slots();
        flush_trace();
        // Every play verb passes here, so the metronome grid is pinned to the take's start frame.
        if (engine_) engine_->startMetronome(seq_.playback_start_frame(), frames_per_quarter());
        return seq_.playback_start_frame();
    }

    /** Frames per quarter note at the live tempo. ⚠️ Same expression as
     *  `ExternalConsumer::frames_per_quarter`: built from the truncated `frames_per_step`, so the
     *  beat stays on the scheduler's grid. */
    int64_t frames_per_quarter() const {
        return frames_per_step(project_.tempo, sampleRate_) * 4;
    }

    /**
     * Every SoundFont instrument made to hold the sound it names, as the transport starts.
     * ⚠️ Browsing the PATCH row while stopped can make LRU eviction reclaim a slot a song instrument
     * points at — `routing.sfSlot` cannot tell, and the track plays the wrong sound. (While playing,
     * note triggers keep their own slots fresh.) Here rather than at each `play_*`; nearly free.
     * A project needing more distinct sounds than `MAX_SOUNDFONTS` reloads on every start.
     */
    void resync_soundfont_slots() {
        if (!engine_) return;
        for (const Instrument& ins : project_.instruments)
            songcore::sync_instrument_soundfont(*engine_, ins, routing_, mediaRoots_);
    }

    /**
     * The audio thread's side of MIDI in, called at the top of every live block. A live key's record
     * goes STRAIGHT into the engine's queues at the block's first frame, so the same block plays it.
     * Not via `router_`: a live key is not part of the song, and the bus consumers read the project,
     * which this thread may not.
     * ⚠️ The engine resolves the note from its own copies, so nothing may be derived here;
     * `arm_midi_in` keeps those copies current.
     */
    struct LiveInput : AudioEngine::LiveInputSource, MidiInPipeline::Apply {
        MidiInPipeline* pipeline = nullptr;
        AudioEngine*    engine   = nullptr;

        void drainLiveInput(int64_t blockStartFrame) override { pipeline->run(blockStartFrame, this); }
        void discardLiveInput() override { pipeline->discard(); }

        void apply(const Event& ev, bool external) override {
            switch (ev.type) {
                case EV_NOTE_ON:
                    // The routing gate's live form: EXTERNAL raises no voice, and a flip to one ends
                    // the sounding internal note.
                    if (external) { engine->scheduleKill(ev.frame, ev.track); return; }
                    engine->scheduleProgramNote(ev.frame, ev.track, ev.instrument, ev.noteOn, engine->tempo());
                    return;
                case EV_NOTE_OFF:
                    switch (ev.noteOff.mode) {
                        case NOTE_OFF_CUT: engine->scheduleKill(ev.frame, ev.track);       return;
                        case NOTE_OFF_KEY: engine->scheduleKeyRelease(ev.frame, ev.track); return;
                        default:           engine->scheduleNoteOff(ev.frame, ev.track);    return;
                    }
                case EV_CC:
                    // A cable sends literal controller numbers, never a CCA-CCD slot.
                    EngineConsumer::apply_cc(*engine, ev.frame, ev.track, ev.cc.param, f32_from_bits(ev.cc.valueBits));
                    return;
                default:
                    return;   // program change and pitch bend have no engine form (engine_consumer.h)
            }
        }
    };

    /**
     * Give the audio thread's drain its route and everything it can play. The route is rebuilt each
     * poll and published only when it changed.
     * ⚠️ Tables are pushed EAGERLY: the note path's lazy push never runs for a live key.
     */
    void arm_midi_in() {
        const MidiRoute route = build_midi_route(project_, midiInInstrument_, midiInTrack_, midiInVoices_,
                                                 controlChannel_, learnArmed_, midiInVelocity_);
        if (!midiRoutePublished_ || std::memcmp(&route, &midiRouteLast_, sizeof route) != 0) {
            midiIn_.publish_route(route);
            midiRouteLast_      = route;
            midiRoutePublished_ = true;
        }
        if (!engine_) return;
        consumer_.push_tables(project_);
        // ⚠️ A program carries its SF slot and rate ratio, which move (PATCH, load, eviction) without
        // passing `push_instrument`. A live key gets no per-note re-send, so re-send what moved.
        const int count = std::min(static_cast<int>(project_.instruments.size()), POOL_INSTRUMENTS);
        for (int id = 0; id < count; ++id) {
            const Instrument& ins = project_.instruments[static_cast<size_t>(id)];
            const int   sid   = ins.sampleId;
            const float ratio = (sid >= 0 && sid < POOL_INSTRUMENTS) ? routing_.sampleRateRatio[sid] : 1.0f;
            if (routing_.sfSlot[id] == programRouting_.sfSlot[id] && ratio == programRouting_.sampleRateRatio[id])
                continue;
            push_instrument_params(*engine_, ins, routing_, project_.tempo, sampleRate_);
            programRouting_.sfSlot[id]          = routing_.sfSlot[id];
            programRouting_.sampleRateRatio[id] = ratio;
        }
    }

    /**
     * What the drain handled since the last poll, on the song's thread:
     *   • MAPPED knobs are applied to the project (`apply_mapped_cc`);
     *   • LEARN records the controller, and every CC records its channel;
     *   • ROUTED records reach the cable when thru is on (suppressions are counted) and teach
     *     `TrackInstruments` which instrument each track plays.
     * The observer is told last.
     */
    void drain_midi_in() {
        MidiInSeen seen;
        while (midiIn_.pop_seen(seen)) {
            if (seen.msg.status == EV_CC) {
                lastCcChannel_ = static_cast<int>(seen.msg.channel);
                // Noticed on EVERY channel, not just the control one — "your knob is on channel 6"
                // is how the user finds the right `CTL CH`.
                if (learnArmed_) {
                    learnController_ = seen.msg.data1;
                    learnChannel_    = static_cast<int>(seen.msg.channel);
                    ++learnEvents_;
                }
            }
            if (seen.kind == MidiInSeen::MAPPED) {
                mappedCcWrites_ += static_cast<uint64_t>(apply_mapped_cc(seen.msg.data1, seen.msg.data2));
            }
            for (int j = 0; j < seen.count; ++j) {
                const Event& ev = seen.events[j];
                // Asked BEFORE either consumer learns from the record.
                const bool external = record_is_external(ev);
                consumer_.observe_live(ev);
                if (midiInThru_) {
                    external_.consume(ev);
                    if (external) ++midiInThruSent_;
                } else if (external) {
                    ++midiInThruSuppressed_;
                }
            }
            if (midiInObserver_) midiInObserver_->on_midi_in(seen.msg, seen.events, seen.count);
        }
    }

    /**
     * Would this record have gone to the cable? Only to count a suppression. Track-scoped records
     * take their owner from the cable consumer's own `TrackInstruments` — never a second opinion.
     */
    bool record_is_external(const Event& ev) const {
        int16_t instrument = ev.instrument;
        if (instrument == INSTRUMENT_NONE) instrument = external_.track_instruments().current(ev.track);
        if (instrument < 0 || static_cast<size_t>(instrument) >= project_.instruments.size()) return false;
        return instrument_routes_external(project_.instruments[static_cast<size_t>(instrument)]);
    }

    /**
     * The preview lane's note-off on the CABLE. A bus record through `consume`, so it passes the same
     * routing gate, `TrackInstruments` and LEN `min` as the note-on did.
     */
    void preview_note_off(int64_t frame) {
        Event off{};
        off.type         = EV_NOTE_OFF;
        off.frame        = frame;
        off.track        = AudioEngine::PREVIEW_LANE;
        off.instrument   = INSTRUMENT_NONE;   // track-scoped, like every note-off on the bus
        off.noteOff.mode = NOTE_OFF_CUT;
        external_.consume(off);
    }

    // Flushed after each verb: a long session stays bounded in RAM and a crash keeps the trace so far.
    void flush_trace() {
        if (!traceEnabled_ || traceBuf_.empty()) return;
        traceFile_.write(traceBuf_.data(), static_cast<std::streamsize>(traceBuf_.size()));
        traceFile_.flush();
        traceBuf_.clear();
    }

    AudioEngine* engine_ = nullptr;
    int sampleRate_ = 44100;
    MediaLoadResult lastMediaLoad_{};   // see last_media_load()

    Project project_ = make_default_project();
    std::string projectSha_ = "-";
    // set_app_root() plus the last load_media(); both empty ⇒ no resolving (the tools' default)
    MediaRoots mediaRoots_;
    Routing routing_;

    /** The RATE row's ratio cache (sample_edit.h). Editor-session state: lets LOFI → HIGH restore the
     *  file's ratio instead of compounding factors. */
    RateCache rateCache_;

    MidiRouter       router_;
    Sequencer        seq_;
    EngineConsumer   consumer_;
    ExternalConsumer external_;
    bool             midiPumpExternal_ = false;   // a sender thread owns the release, not poll()

    // MIDI in. The pipeline is shared with the audio thread (midi_in.h); the rest is this thread's.
    MidiInPipeline    midiIn_;
    LiveInput         midiLive_;                  // what the engine calls; points back at midiIn_
    MidiRoute         midiRouteLast_{};           // the route as last published, to publish only a change
    bool              midiRoutePublished_ = false;
    Routing           programRouting_;            // per INSTRUMENT id: the slot and ratio its program last carried
    int               midiInInstrument_   = -1;   // the instrument the UI is on
    int               midiInTrack_        = 0;    // the SONG cursor's track
    int               midiInVoices_       = 1;    // 1 = MONO
    bool              midiInVelocity_     = true;
    IMidiInObserver*  midiInObserver_     = nullptr;
    bool              midiInThru_ = true;
    uint64_t          midiInThruSent_ = 0, midiInThruSuppressed_ = 0;

    TraceWriter   writer_;
    std::string   traceBuf_;
    std::ofstream traceFile_;
    bool          traceEnabled_ = false;
};

}  // namespace songcore

#endif  // POCKETTRACKER_SONGCORE_HOST_H
