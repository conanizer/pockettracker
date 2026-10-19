#ifndef POCKETTRACKER_SONGCORE_HOST_H
#define POCKETTRACKER_SONGCORE_HOST_H

// What the app owns to make songcore play: the Project, the bus, the Sequencer and the trace sink,
// with the verbs — load, play/stop, poll, render, playheads. Platform-free.
// Every verb runs on the UI thread, never the audio callback.

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
    // `engine` may be null (a trace-only host for tests). The rate is re-read from it on every verb.
    SongcoreHost(AudioEngine* engine, int sampleRate);

    // ⚠️ Close the audio stream first: the engine's live-input pointer names a member of this object.
    ~SongcoreHost();

    MidiRouter& router() { return router_; }
    Sequencer&  sequencer() { return seq_; }
    const Project& project() const { return project_; }

    // ── MIDI out: the shell hands the open port in; everything above it is midi_out.h ──
    ExternalConsumer& midi_out() { return external_; }
    void set_midi_out(IMidiOut* out) { external_.set_out(out); }
    void set_midi_offset_ms(int ms) { external_.set_offset_ms(ms); }
    // The 24 PPQN clock + transport out, from the next transport start.
    void set_midi_sync_out(bool on) { external_.set_sync_out(on); }
    bool midi_sync_out() const { return external_.sync_out(); }

    // Hand the queue's release to a sender thread (`shell/midi/midi-sender.h`); otherwise `poll()` pumps.
    // ⚠️ One owner only: with both pumping, a broken sender looks like a working one.
    void set_midi_pump_external(bool external) { midiPumpExternal_ = external; }


    // ── MIDI in: the port is the platform's, every byte after it songcore's (midi_in.h) ──
    // ⚠️ The drain runs on the AUDIO thread at the top of each block, so a key sounds in the block it
    // arrives in. `poll()` publishes what it routes against (`arm_midi_in`) and takes back what it
    // handled (`drain_midi_in`). With no engine, `poll()` runs the drain itself.

    // Where a backend delivers its bytes; called from any thread, so a lock-free ring only.
    MidiInQueue& midi_in_sink() { return midiIn_.sink(); }

    // The counters, for the screens and the exit report.
    const MidiInputRouter& midi_in_router() const { return midiIn_.router(); }
    const MidiParser&      midi_in_parser() const { return midiIn_.parser(); }
    const MidiInPipeline&  midi_in_pipeline() const { return midiIn_; }

    // Told about every message the drain handled, one poll later. Nullable; the counters run anyway.
    void set_midi_in_observer(IMidiInObserver* obs) { midiInObserver_ = obs; }

    // What a live key plays: `instrument` on `track`, over `voices` tracks (1 = MONO). Pushed every
    // frame by the shell, published to the drain only on a change.
    void set_midi_in_play(int instrument, int track, int voices, bool velocity = true);

    // MIDI THRU: a live key on an EXTERNAL track reaches the cable. On by default.
    // ⚠️ With input and output on one device it is a feedback loop; the shell turns it off then.
    void set_midi_in_thru(bool on) { midiInThru_ = on; }
    bool midi_in_thru() const { return midiInThru_; }

    // Bytes that reached the ring, and complete messages parsed from them.
    uint64_t midi_in_bytes() const { return midiIn_.bytes(); }
    uint64_t midi_in_messages() const { return midiIn_.messages(); }

    // Records handed to the engine, to the cable, and withheld from the cable by thru.
    uint64_t midi_in_injected() const { return midiIn_.injected(); }
    uint64_t midi_in_thru_sent() const { return midiInThruSent_; }
    uint64_t midi_in_thru_suppressed() const { return midiInThruSuppressed_; }

    // Forget parked bytes and a half-assembled message, when a port closes — running status from a
    // pulled cable would complete a phantom note. A request, honoured at the drain's next block.
    void reset_midi_in();

    // ── SYNC IN: another device's clock leads (midi_sync_in.h) ──
    // `MidiSyncInMode`. A take that starts while a clock arrives puts its steps on the clock's ticks
    // and plays at its tempo; under FULL the clock's Start, Stop and position start and stop the song.
    // ⚠️ The song's TEMPO is never written: the tempo in force is `Project::clockTempo`, set here.
    void set_midi_sync_in(int mode) { syncIn_ = mode < SYNC_IN_OFF ? SYNC_IN_OFF : (mode > SYNC_IN_FULL ? SYNC_IN_FULL : mode); }
    int  midi_sync_in() const { return syncIn_; }
    // The incoming clock's tempo, 0 while none arrives.
    double midi_clock_bpm() const;
    // The clock as the last poll read it, and whether the take's steps sit on it.
    const MidiClockLine& midi_clock_line() const { return clockLine_; }
    bool following_clock_now() const { return followTake_ && seq_.is_playing() && sync_in_active(); }
    // How far the step about to be walked is from where the clock puts it, in frames (+ = early).
    double follow_error() const { return followError_; }

    // ── data ──
    // Replace the project from .ptp JSON, in place: `project_` never moves, so the Sequencer's pointer
    // stays valid. A full parse; the app edits through edit_project() instead.
    bool push_project(const std::string& blob);

    // ── transport: each returns the frame it latched (the trace's session base) ──
    // CHAIN and PHRASE take the mixer track they play on.
    int64_t play_song(int startRow)   { before_play(); seq_.playSong(startRow);     return after_play(); }
    // LIVE from stopped: `mask` bit N launches track N at `songRow`, the rest begin silent.
    int64_t play_song_live(int songRow, int mask);
    int64_t play_chain(int chainId, int trackId = 0);
    int64_t play_phrase(int phraseId, int trackId = 0);

    // Stop the scheduler AND the engine — notes already queued would otherwise keep playing.
    // ⚠️ The master EQ is restored before the queues are cleared (an EQM may be in the param queue).
    // The play_* verbs do not call it: an extra stop would add a `t_stop` to the traces.
    void stop();

    // Bit N: track N has had a note scheduled this session (the visualizer's lanes).
    int track_mask() const { return consumer_.track_mask(); }

    // The lookahead poll, once per UI frame.
    void poll();

    // The sequencer's half of `poll` — no input side, so a long operation may call it with a deeper ask.
    void keep_walking(int64_t aheadMs = Sequencer::HORIZON_MS);

    // ── render (render.h): prepare → schedule → render → finish ──
    // Returns the frame span scheduled; a null `trackFilter` = every track.
    int64_t schedule_song_range(int startRow, int endRow, const std::set<int>* trackFilter,
                                int repeat = 1);

    void prepare_render(int startRow, int endRow);

    RenderStats render_to_wav(const std::string& path, int64_t songFrames,
                              int stemsMode, bool applyMasterBus,
                              const std::function<void(float)>& progress = nullptr);

    void finish_render();

    // All four steps. `repeat` plays the range that many times in one pass, so tails cross each join.
    RenderStats render_song_range_to_wav(int startRow, int endRow, const std::string& path,
                                         const RenderOptions& opts = RenderOptions(),
                                         const std::function<void(float)>& progress = nullptr,
                                         int repeat = 1);

    // The whole song, bounds and all.
    RenderStats render_song_to_wav(const std::string& path,
                                   const RenderOptions& opts = RenderOptions(),
                                   const std::function<void(float)>& progress = nullptr);

    // Load the project's samples and SoundFonts and learn the Routing (engine_setup.h).
    // ⚠️ It also WRITES to the project: a WAV's cue points become its slice markers.
    MediaLoadResult load_media(const std::string& baseDir);

    // What the last `load_media` found. ⚠️ Without it a failed load (out of memory included) is
    // invisible where there is no console — the instrument just plays silence.
    const MediaLoadResult& last_media_load() const { return lastMediaLoad_; }

    // This install's app root, set once at boot; another install's dead absolute media paths are
    // re-rooted onto it at load. Unset (the tools) leaves paths as they are.
    void set_app_root(std::string root) { mediaRoots_.appRoot = std::move(root); }

    // ── the param push (engine_setup.h): what the engine holds across project swaps ──
    // ⚠️ Call push_params() after load_media(), or you hear the previous project's settings.
    void push_params();

    // One instrument's params.
    // ⚠️ Also refreshes the notes sounding on it — a voice copies filter, drive, crush and sends at
    // trigger, so an edit would otherwise wait for the next note.
    void push_instrument(int id, bool refreshSounding = true);

    // Mixer, master bus, sends, EQ bank, master EQ — what MIXER and EFFECTS edits push.
    // ⚠️ Not push_params(): that also sweeps 128 instruments on every key-repeat.
    void push_globals();

    // ── a mapped knob (midi_map.h) ──

    // The channel that may carry mapping knobs: 0-15, `MIDI_CTL_CH_ALL`, or −1 for none.
    void set_midi_control_channel(int ch);
    int  midi_control_channel() const { return controlChannel_; }

    // Mapped destinations moved, ever. The UI reads it once a frame and marks the song dirty.
    uint64_t mapped_cc_writes() const { return mappedCcWrites_; }

    // MIDI LEARN armed (`R` held): the next knob names a destination. A bool pushed down, a counter
    // watched from above — the drain never calls into the UI.
    void set_midi_learn_armed(bool on) { learnArmed_ = on; }


    // Moves once per knob seen while armed, with what it saw.
    uint64_t midi_learn_events() const { return learnEvents_; }
    int      midi_learn_controller() const { return learnController_; }
    int      midi_learn_channel() const { return learnChannel_; }

    // The channel the last CC arrived on, or −1 — shown beside the `CTL CH` row.
    int last_cc_channel() const { return lastCcChannel_; }

    // The audio callback's cost over the last second; zero without an engine.
    AudioEngine::BlockTiming block_timing() const;

    // A mapped knob moved: write the value into the project at once and make it heard. Returns how
    // many mappings it drove. A mapping whose destination is gone is skipped, not deleted.
    int apply_mapped_cc(int controller, int value);

    // What the running take owns, so `push_globals()` does not wipe it.
    // ⚠️ PEEK, never take, the table latches: they arm the restore in `stop()`.
    MixerHeld held_by_song() const;

    // ── the EQ editor ──
    // ⚠️ A band edit needs BOTH calls: `set_eq_band` writes only the bank, and each consumer compiles
    // its coefficients when the slot is ASSIGNED — so the editor re-assigns its slot after every nudge.

    void set_eq_band(int slot, int band, int type, int freqHex, int gainHex, int qHex);

    void set_master_eq_slot(int slot);

    void set_instrument_eq_slot(int id, int slot);

    void set_reverb_input_eq(int slot);

    void set_delay_input_eq(int slot);

    // The spectrum under the EQ editor: master (0), delay input (1), reverb input (2), instrument (3).
    // False with no engine.
    bool spectrum_for_source(int source, int instrId, int numBins, float* out) const;

    // ── the instrument operations (engine_setup.h) ──
    // ⚠️ Not guarded on `engine_`: they edit the document; the engine calls are guarded inside.

    void set_instrument_type(int id, InstrumentType type);

    void clear_instrument(int id);

    // The PRESET row's list, read from the file's index (so banks too large to load work too).
    // No SoundFont answers 0 / 0 / "---".
    int sf_preset_count(int id) const;
    int sf_preset_index(int id) const;
    std::string sf_preset_name(int id) const;
    void set_sf_preset_by_index(int id, int index);

    // Bring the loaded sound in line with the preset the instrument names; free when nothing moved.
    void sync_sf_preset(int id);

    // The PATCH row's load, started rather than done. False = engine busy, ask again.
    bool request_sf_preset(int id);

    // Install a finished background preset load, once a frame. Notes already scheduled keep the old
    // slot; the next row the walk reaches takes the new one.
    void poll_sf_load();

    // ── files: paths, opened through byte_source.h ──

    // Replace the project from a .ptp: stop → parse → push → load its media → push its params.
    bool load_project_file(const std::string& path, const std::string& baseDir);

    // ⚠️ songcore writes no user file: every save goes through `ui::FileSystem::write_file` (temp +
    // rename + checked close). A truncating `ofstream` here would destroy the old file at open.

    // ── PROJECT: NEW and the two COMPACTs (the surgery itself is project_ops.h) ──

    // A blank document, and an engine that has forgotten the last one.
    void new_project();

    // Unused chains and phrases back to factory; no engine call.
    void clean_seq() { songcore::clean_unused_seq(project_); }

    // Unused instruments, tables and grooves back to factory.
    // ⚠️ Media is reloaded and tables invalidated, or a compacted table would play its old rows.
    void clean_inst(const std::string& baseDir);

    // A sample file → instrument `id`.
    bool load_sample(int id, const std::string& path);

    // An .sf2/.sf3 → instrument `id`, which becomes a SOUNDFONT slot.
    bool load_soundfont(int id, const std::string& path);

    // The last media load failed for lack of memory rather than a bad file.
    bool last_load_ran_out_of_memory() const;

    // The user cancelled the last media load: not a failure.
    bool last_load_cancelled() const;

    // A .pti into instrument `id`. False if it will not parse or its source is gone (the params still land).
    bool load_instrument_preset(int id, const std::string& path);

    // Audition the file under the browser's cursor, on the preview lane.
    bool preview_file(const std::string& path);

    void clear_previews();

    // ── the sample editor: forwards to the engine, each guarded so tests run with no device ──

    int  sample_length(int id) const { return engine_ ? engine_->getSampleLength(id) : 0; }
    bool has_stereo_data(int id) const { return engine_ && engine_->hasStereoData(id); }
    // The depth the sample came in at — the ceiling of the BIT cell.
    int  sample_bit_depth(int id) const { return engine_ ? engine_->getSampleBitDepth(id) : 16; }

    // The file's rate; 44100 for an empty slot (sample_edit.h).
    int sample_rate_of(int id) const { return original_sample_rate(engine_, routing_, id); }

    // 0..1 while the sample sounds, else −1 — the waveform's playhead.
    float sample_playback_position(int id) const;

    // `bins` (min, max) pairs. `channel`: 0 left, 1 right, 2 averaged. (0, 0) = the whole sample.
    std::vector<float> sample_waveform(int id, int bins, int startFrame = 0, int endFrame = 0,
                                       int channel = 2) const;

    // Slice boundaries at `sensitivity`, at most 128.
    std::vector<int> detect_transients(int id, int sensitivity) const;

    // The nearest zero crossing (`dir` −1 back, +1 forward, 0 either) in the signal SOURCE will cut.
    int find_zero_crossing(int id, int frame, int dir, int sourceMode = 0) const;

    int clipboard_length() const { return engine_ ? engine_->getClipboardLength() : 0; }

    void backup_sample(int id) { if (engine_) engine_->backupSample(id); }
    void undo_sample(int id) { if (engine_) engine_->undoSample(id); }
    // The editor is closing: drop its undo copy.
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

    // The FX row: START applies for real and plays, the next gesture restores this backup; only APPLY
    // keeps it. Separate from the undo slot.
    void save_fx_preview_backup(int id) { if (engine_) engine_->saveFxPreviewBackup(id); }
    void restore_fx_preview_backup() { if (engine_) engine_->restoreFxPreviewBackup(); }

    void apply_sample_fx(int id, int fxType, int fxValue);

    // The three that change the rate ratio, so live in songcore (sample_edit.h).
    void apply_rate_and_bits(int id, int factor, int bits);
    void pitch_shift_sample(int id, float semitones);
    void time_stretch_sample(int id, float ratio, float grainMs = sola::DEFAULT_GRAIN_MS);
    // The FX row's SYNC, for good.
    void sync_sample(int id, const SampleSync& sync);

    // The editor's START: the sample dry at its root, through the SOURCE channel, windowed to the
    // selection. `sync` previews the FX row's SYNC from a copy.
    // ⚠️ The window goes in as FRAMES, not the 0-255 start/end grid — the audition must be exactly
    // what CROP keeps. It is read when the note fires, so `finish_sample_preview()` ends it later.
    void preview_sample_editor(int id, int sourceMode, int64_t selStart, int64_t selEnd,
                               int totalFrames, int pitchSemitones, const SampleSync& sync = {});

    // Put the instrument back (window, EQ, sends, modulation) — on the dispatcher's 100 ms deadline,
    // or at once on a second START.
    void finish_sample_preview(int id);

    // The edited PCM → a WAV, slices in the `cue ` chunk. `bits` 0 = the loaded depth.
    bool save_sample_wav(int id, const std::string& path, const std::vector<int>& cuePoints,
                         int sourceMode, bool hasStereo, int bits = 0);

    // After a SAVE that kept the buffer: it is the file now — take its depth, drop the RATE/BIT
    // original, or the next RATE/BIT touch would restore pre-save audio.
    void adopt_saved_sample(int id, int bits);

    // Every slice → its own WAV in `dir`. Returns how many were written.
    int chop_sample(int id, const std::string& dir, const std::string& baseName,
                    const std::vector<std::pair<int64_t, int64_t>>& slices, int bits = 0);

    // ── live editing: the Sequencer reads this same object, so an edit is heard from the next row ──
    // ⚠️ A TABLE edit must call invalidate_tables(): the consumer caches what it pushed.
    Project& edit_project() { return project_; }
    void     invalidate_tables() { consumer_.invalidate_tables(); }

    // ── LIVE mode: each verb arms a slot, rewinds its track, and drops what was queued past it ──

    bool               live_mode() const              { return seq_.live_mode(); }
    songcore::LiveSlot live_queue(int track) const    { return seq_.live_queue(track); }


    void set_live_mode(bool on)                       { sync_clock(); apply_rollback(seq_.set_live_mode(on, seq_.clock())); }
    void queue_live(int track, int songRow, bool now) { sync_clock(); apply_rollback(seq_.queue_live(track, songRow, now, seq_.clock())); }
    void queue_live_stop(int track, bool now)         { sync_clock(); apply_rollback(seq_.queue_live_stop(track, now, seq_.clock())); }
    void queue_live_row(int songRow, bool now)        { sync_clock(); apply_rollback(seq_.queue_live_row(songRow, now, seq_.clock())); }

    // ── the note preview, on the preview lane (track 8, a ninth voice) so it steals nothing ──
    // ⚠️ Through `plan_note_on`, the sequencer's own derivation — a hand-rolled copy would drift.
    // ⚠️ It reaches the CABLE through `external_` directly: via `router_` it would land in the trace.
    // `durationFrames <= 0` rings until stop_preview(). `tableIdOverride`: the TABLE screen's table.
    void preview_note(int instrumentId, const Note& note, int64_t durationFrames,
                      bool rootAudition = false, int tableIdOverride = -1);

    // Audition an instrument at its own ROOT until the next button press.
    // ⚠️ A root audition: the SoundFont path must know, or its 60 − root transpose plays a flat C-4.
    void preview_instrument(int instrumentId, int tableIdOverride = -1);

    // Point the preview lane at mixer channel `trackId` (0..7), or −1 for unity gain.
    void set_preview_track(int trackId);

    // Silence the preview lane. `cut` ends it in ~6 ms with no release tail.
    // ⚠️ Both halves: an EXTERNAL audition with no length would hold the gear's note for ever.
    void stop_preview(bool cut = false);

    // ── feedback ──

    // One track's playhead; in SONG mode the eight run independently.
    PlaybackPosition playheads(int trackId);

    bool is_playing() const { return seq_.is_playing(); }

    int sample_rate() const { return sampleRate_; }

    // ── the conformance trace (debug) ──
    // Enable AFTER the project is pushed: the header's project= is the sha of the pushed blob.
    void set_trace(bool enabled, const std::string& path);


  private:
    // A mapped knob IS the press: the take lets go of that destination, or `MixerHeld` would skip the
    // fader the knob is moving. ⚠️ A TABLE's TIM latch is left alone — it arms the restore in `stop()`.
    void release_song_hold(MapDestId id, int scopeIndex);

    int      controlChannel_  = -1;      // -1 = no channel is reserved for mapping knobs
    uint64_t mappedCcWrites_  = 0;
    bool     learnArmed_      = false;   // `R` is down: the next knob NAMES rather than drives
    uint64_t learnEvents_     = 0;
    int      learnController_ = -1;
    int      learnChannel_    = -1;
    int      lastCcChannel_   = -1;      // whatever channel the cable last carried a CC on

    // Drop what the rewound tracks had queued.
    // ⚠️ One frame PER TRACK: one earliest frame would drop notes a track further ahead will not redo.
    void apply_rollback(const songcore::RollbackPlan& plan);

    // The engine's frame counter IS the transport clock; with no engine it stays where a test put it.
    void sync_clock();

    void before_play();

    int64_t after_play();

    // Frames per quarter note. ⚠️ Same expression as `ExternalConsumer::frames_per_quarter`, built
    // from the truncated `frames_per_step` — or the clock's step while one leads — so the beat stays
    // on the scheduler's grid.
    int64_t frames_per_quarter() const;

    // ── SYNC IN, the UI thread's half ──
    // Is SYNC IN on and not listening to this device's own clock coming back?
    bool    sync_in_active() const;
    bool    clock_present(const MidiClockLine& line) const;
    int64_t offset_frames() const;
    // The poll's part: the transport events (FULL) and the tempo in force.
    void    follow_midi_clock();
    // A received Start or Continue: the song from its position.
    void    start_from_position(const MidiTransportEvent& ev);
    // Before a take: on the clock — where it starts, and the step its first fill walks.
    void    plan_follow_start();
    // Before each fill: the step that puts the grid on the clock's ticks.
    void    apply_follow_step(int64_t aheadMs);
    // The step to walk before the clock's tempo is known: the last one walked, or the tempo in force.
    int64_t guess_step() const;
    // The tempo in force (0 = the song's), and what follows from it at once.
    void    set_clock_tempo(int bpm);

    // Every SoundFont instrument made to hold the sound it names, as the transport starts.
    // ⚠️ Browsing PATCH while stopped can evict a slot a song instrument points at, and the track
    // would play the wrong sound.
    void resync_soundfont_slots();

    // The audio thread's side of MIDI in, at the top of every block: a live key goes straight into
    // the engine's queues, so the same block plays it. Not via `router_` — that reads the project.
    // ⚠️ Nothing may be derived here; the engine resolves the note from copies `arm_midi_in` keeps.
    struct LiveInput : AudioEngine::LiveInputSource, MidiInPipeline::Apply {
        MidiInPipeline* pipeline = nullptr;
        AudioEngine*    engine   = nullptr;

        void drainLiveInput(int64_t blockStartFrame, const AudioEngine::LiveClock& clock) override {
            pipeline->run(blockStartFrame, this, MidiArrivalClock{clock.frame, clock.us, clock.sampleRate});
        }
        void discardLiveInput() override { pipeline->discard(); }

        void apply(const Event& ev, bool external) override;
        void wheel(int64_t frame, uint8_t track, float semitones) override {
            engine->scheduleVoiceWheel(frame, track, semitones);
        }
    };

    // Give the drain its route and everything it can play; published only when it changed.
    // ⚠️ Tables are pushed EAGERLY: the note path's lazy push never runs for a live key.
    void arm_midi_in();

    // What the drain handled since the last poll: mapped knobs applied, LEARN and the CC channel
    // recorded, routed records sent thru and taught to `TrackInstruments`; the observer last.
    void drain_midi_in();

    // Would this record have gone to the cable? Only to count a suppression; the owner comes from the
    // cable consumer's own `TrackInstruments`.
    bool record_is_external(const Event& ev) const;

    // The preview lane's note-off on the cable, through `consume` — the same gate as the note-on.
    void preview_note_off(int64_t frame);

    // Flushed after each verb: a long session stays bounded in RAM and a crash keeps the trace so far.
    void flush_trace();

    AudioEngine* engine_ = nullptr;
    int sampleRate_ = 44100;
    MediaLoadResult lastMediaLoad_{};   // see last_media_load()

    Project project_ = make_default_project();
    std::string projectSha_ = "-";
    // set_app_root() plus the last load_media(); both empty ⇒ no resolving (the tools' default)
    MediaRoots mediaRoots_;
    Routing routing_;

    // The RATE row's ratio cache (sample_edit.h), so LOFI → HIGH restores rather than compounds.
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

    // SYNC IN. `followLine_` is the take's last usable line: a refit mid-take does not stall it.
    // How far past the next audio portion a take walks while it or the clock's line is young: a
    // poll's gap and a margin, short of a step.
    static constexpr int64_t YOUNG_AHEAD_MS = 20;
    int                syncIn_ = SYNC_IN_OFF;
    MidiClockLine      clockLine_{};
    uint32_t           clockLineSeen_  = 0;
    MidiClockLine      followLine_{};
    bool               followTake_     = false;   // the take's step k sits on tick followDownbeat_ + 6k
    int64_t            followDownbeat_ = 0;
    int64_t            followLate_     = 0;       // how long after its downbeat a received take is heard
    int64_t            followStep_     = 0;       // the step last handed to the walk
    double             followError_    = 0.0;
    bool               startPending_   = false;   // start_from_position → the take it starts
    MidiTransportEvent startEvent_{};

    TraceWriter   writer_;
    std::string   traceBuf_;
    std::ofstream traceFile_;
    bool          traceEnabled_ = false;
};

}  // namespace songcore

#endif  // POCKETTRACKER_SONGCORE_HOST_H
