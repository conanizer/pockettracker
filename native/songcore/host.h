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
    SongcoreHost(AudioEngine* engine, int sampleRate);

    // ⚠️ The engine's live-input pointer names a member of this object, so it is withdrawn here. The
    // caller must close the audio stream before destroying the host.
    ~SongcoreHost();

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
     * Hand the queue's release to a sender thread (`shell/midi/midi-sender.h`).
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
    void set_midi_in_play(int instrument, int track, int voices, bool velocity = true);

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
    void reset_midi_in();

    // ── ↓ data ───────────────────────────────────────────────────────────────────────────────────
    // The blob is the .ptp JSON. Pushing REPLACES the project in place: `project_` never moves, so the
    // Sequencer's pointer stays valid mid-playback. Costs a full parse; the app edits the live
    // document in place instead (edit_project below).
    bool push_project(const std::string& blob);

    // ── ↓ transport ──────────────────────────────────────────────────────────────────────────────
    // Each returns the frame the transport latched (the trace's session base).
    // CHAIN and PHRASE take the MIXER TRACK they play on — its fader, mute, voice slot and FX —
    // defaulting to 0.
    int64_t play_song(int startRow)   { before_play(); seq_.playSong(startRow);     return after_play(); }
    /** LIVE mode from a standing start: `mask` bit N launches track N at `songRow`, the rest begin silent. */
    int64_t play_song_live(int songRow, int mask);
    int64_t play_chain(int chainId, int trackId = 0);
    int64_t play_phrase(int phraseId, int trackId = 0);

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
    void stop();

    // Bit N set once track N has had a note scheduled this session (the OCTA visualizer's lanes).
    int track_mask() const { return consumer_.track_mask(); }

    // The lookahead poll, once per UI frame.
    void poll();

    /**
     * The sequencer's half of `poll`: tops the lookahead up to `aheadMs` and releases the MIDI that has
     * come due. Nothing input-side runs, so a long operation may call it mid-load with a deeper ask.
     */
    void keep_walking(int64_t aheadMs = Sequencer::HORIZON_MS);

    // ── ↓ the render path ────────────────────────────────────────────────────────────────────────
    // Returns the total frame span scheduled. trackFilter == nullptr renders every track.
    int64_t schedule_song_range(int startRow, int endRow, const std::set<int>* trackFilter,
                                int repeat = 1);

    // ── ↓ the render itself (render.h) ───────────────────────────────────────────────────────────
    // prepare → schedule → render → finish; render_song_range_to_wav does all four.
    void prepare_render(int startRow, int endRow);

    RenderStats render_to_wav(const std::string& path, int64_t songFrames,
                              int stemsMode, bool applyMasterBus,
                              const std::function<void(float)>& progress = nullptr);

    void finish_render();

    // prepare → schedule → render → finish, with songcore's own sequencer.
    // `repeat` plays the range that many times in ONE pass — files concatenated afterwards would cut
    // the reverb and delay at every join.
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
    void push_params();

    /**
     * One instrument's params — what an INSTRUMENT / MODS / pool edit pushes.
     * ⚠️ Also refreshes the notes already sounding on it: a voice COPIES filter, drive, crush and sends
     * at trigger, so without this an edit is unheard until the next note.
     */
    void push_instrument(int id, bool refreshSounding = true);

    /**
     * The GLOBALS — mixer, master bus, sends, EQ bank, master EQ. What MIXER and EFFECTS edits push.
     * ⚠️ Not push_params(): that also sweeps 128 instruments (~2,500 engine calls) on every key-repeat.
     */
    void push_globals();

    // ── ↕ a mapped knob (midi_map.h) ─────────────────────────────────────────────────────────────

    /**
     * Which incoming channel may carry MAPPING knobs: 0-15, `MIDI_CTL_CH_ALL`, or −1 for none (the
     * start state; the shell pushes the setting at boot).
     */
    void set_midi_control_channel(int ch);
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
    AudioEngine::BlockTiming block_timing() const;

    /**
     * A mapped knob moved: write the value into the project and make it heard. Returns how many
     * mappings that controller drove (0 = none). The value is written at once — a knob has no
     * button-up; only the autosave is debounced (the dispatcher's job).
     * One controller may drive several destinations; a mapping whose destination is gone is skipped,
     * not deleted (the list screen greys it).
     */
    int apply_mapped_cc(int controller, int value);

    /**
     * What the running take owns now, so `push_globals()` can push the authored mixer without wiping
     * it (any MIXER/EFFECTS edit mid-song pushes). The same questions `stop()`'s restore asks.
     * ⚠️ PEEK, never take, the table latches: they arm the restore in `stop()`.
     */
    MixerHeld held_by_song() const;

    // ── ↕ the EQ editor ──────────────────────────────────────────────────────────────────────────
    //
    // ⚠️ A band edit needs BOTH calls. `setEqBand` writes only the 128-slot bank; the master bus and
    // each instrument compile their own coefficients when a slot is ASSIGNED. So the editor re-assigns
    // the same slot to the consumer that opened it after every band nudge — and must remember which
    // one that was. Cheaper than `push_globals()` on every key-repeat.

    void set_eq_band(int slot, int band, int type, int freqHex, int gainHex, int qHex);

    void set_master_eq_slot(int slot);

    void set_instrument_eq_slot(int id, int slot);

    void set_reverb_input_eq(int slot);

    void set_delay_input_eq(int slot);

    /**
     * The spectrum of ONE signal path for the EQ editor: master bus (0), delay input (1), reverb input
     * (2), or one instrument's voices (3) — an EQ on a send is drawn over that send's signal. False
     * with no engine; the editor then draws an empty grid.
     */
    bool spectrum_for_source(int source, int instrId, int numBins, float* out) const;

    // ── ↕ the instrument operations ──────────────────────────────────────────────────────────────
    // The verbs that own a SOURCE (engine_setup.h).
    // ⚠️ Not guarded on `engine_`: they edit the DOCUMENT and only also free engine resources. The
    // null checks live around the engine calls, inside engine_setup.h.

    void set_instrument_type(int id, InstrumentType type);

    void clear_instrument(int id);

    // The PRESET row's list. All three answer for an instrument with no SoundFont (0 / 0 / "---"), and
    // read the FILE's index, so they work for banks too large to load.
    int sf_preset_count(int id) const;
    int sf_preset_index(int id) const;
    std::string sf_preset_name(int id) const;
    void set_sf_preset_by_index(int id, int index);

    /** Bring instrument `id`'s loaded sound in line with the preset it names; free when nothing moved,
     *  so safe every frame. */
    void sync_sf_preset(int id);

    /** The PATCH row's load, started rather than done. False = engine busy, ask again. */
    bool request_sf_preset(int id);

    /** Install a finished background preset load. Called once a frame by the feed. Notes already
     *  scheduled keep the old slot; the next row the walk reaches takes the new one. */
    void poll_sf_load();

    // ── ↕ the FILE verbs ─────────────────────────────────────────────────────────────────────────
    //
    // They take paths, not a `ui::FileSystem` — that abstraction is for the browser (listing,
    // renaming). Opening funnels through `pt_read_file` / `pt_fopen` (byte_source.h), the one place a
    // path becomes a handle.

    /** Replace the project from a .ptp on disk: stop → parse → push → load its media → push its params. */
    bool load_project_file(const std::string& path, const std::string& baseDir);

    // ⚠️ songcore writes no user file. Every save goes through `ui::FileSystem::write_file` (temp +
    // rename + checked close), one layer up; songcore cannot depend on it. A truncating `ofstream`
    // here would destroy the old file at open and miss a failure that surfaces at flush.

    // ── PROJECT screen: NEW, and the two COMPACTs ────────────────────────────────────────────────
    //
    // The document surgery is pure (project_ops.h). These are host verbs because each also
    // invalidates state the engine holds.

    /** PROJECT → NEW. A blank document, and an engine that has forgotten the last one. */
    void new_project();

    /** PROJECT → COMPACT → SEQ. Unused chains and phrases back to factory. Pure arrangement, so no
     *  engine call. */
    void clean_seq() { songcore::clean_unused_seq(project_); }

    /**
     * PROJECT → COMPACT → INST. Unused instruments, tables and grooves back to factory.
     * ⚠️ All three engine steps matter: the old buffers stay loaded until media is reloaded, and
     * without `invalidate_tables` a compacted table would go on playing its old rows.
     */
    void clean_inst(const std::string& baseDir);

    /** A sample (wav/mp3/flac/ogg/opus) → instrument `id`. The browser's A on a sampler slot. */
    bool load_sample(int id, const std::string& path);

    /** An .sf2/.sf3 → instrument `id`, which becomes a SOUNDFONT slot. The browser's A on one. */
    bool load_soundfont(int id, const std::string& path);

    /**
     * True when the last media load failed for lack of memory rather than a bad file — the two want
     * different messages. One bool, so the UI never reaches the engine.
     */
    bool last_load_ran_out_of_memory() const;

    /** True when the user cancelled the last media load: not a failure, and the UI says nothing. */
    bool last_load_cancelled() const;

    /**
     * Read a .pti into instrument `id`. False if it will not parse or its source file is gone — in the
     * latter case the parameters still land.
     */
    bool load_instrument_preset(int id, const std::string& path);

    /** Audition the file under the browser's cursor (slot 255, the preview lane). START, on a file. */
    bool preview_file(const std::string& path);

    /** Drop the browser's audition — what leaving the browser does. */
    void clear_previews();

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
    float sample_playback_position(int id) const;

    /**
     * `bins` (min, max) pairs, so 2 × bins floats. `channel`: 0 left, 1 right, 2 averaged (stereo only).
     * A range covering the whole sample — (0, 0) or (0, length) — takes the whole-sample entry point;
     * both spellings must draw identically.
     */
    std::vector<float> sample_waveform(int id, int bins, int startFrame = 0, int endFrame = 0,
                                       int channel = 2) const;

    /** The slice boundaries the detector finds at `sensitivity`, capped at 128. */
    std::vector<int> detect_transients(int id, int sensitivity) const;

    /** The nearest zero crossing to `frame` in direction `dir` (−1 back, +1 forward, 0 either), in the
     *  signal the SOURCE mode will cut — both channels under STEREO. */
    int find_zero_crossing(int id, int frame, int dir, int sourceMode = 0) const;

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

    void apply_sample_fx(int id, int fxType, int fxValue);

    // ── The three that change the rate ratio, so live in songcore (sample_edit.h) ────────────────
    void apply_rate_and_bits(int id, int factor, int bits);
    void pitch_shift_sample(int id, float semitones);
    void time_stretch_sample(int id, float ratio);

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
                               int totalFrames, int pitchSemitones);

    /**
     * Put the instrument back — window, EQ, sends, modulation. Runs on the dispatcher's 100 ms
     * deadline, or at once if a second START arrives first. The push itself disarms the frame window.
     */
    void finish_sample_preview(int id);

    // ── SAVE and CHOP ────────────────────────────────────────────────────────────────────────────

    /** The edited PCM → a WAV at `path`, slices in the `cue ` chunk. `bits` 0 = the loaded depth. */
    bool save_sample_wav(int id, const std::string& path, const std::vector<int>& cuePoints,
                         int sourceMode, bool hasStereo, int bits = 0);

    /**
     * After a SAVE that kept the slot's buffer: that buffer is now the file, so its depth becomes the
     * saved one and the RATE/BIT original is dropped — else the next RATE/BIT touch would restore
     * pre-save audio.
     */
    void adopt_saved_sample(int id, int bits);

    /** Every slice → its own WAV in `dir`. Returns how many were written. */
    int chop_sample(int id, const std::string& dir, const std::string& baseName,
                    const std::vector<std::pair<int64_t, int64_t>>& slices, int bits = 0);

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
                      bool rootAudition = false, int tableIdOverride = -1);

    /**
     * Audition an instrument at its own ROOT — START on INSTRUMENT / INST_POOL / MODS, and on TABLE
     * with a table override. It rings out until the next plain button press, and it is a ROOT
     * AUDITION: the SoundFont path must know, or its 60 − root transpose would play a flat C-4.
     */
    void preview_instrument(int instrumentId, int tableIdOverride = -1);

    /**
     * Point the preview lane at mixer channel `trackId` (0..7), or −1 for unity gain. An index, not a
     * gain, so the live fader is re-read every block. The lane keeps its own voice either way.
     */
    void set_preview_track(int trackId);

    /**
     * Silence the audition lane ("press any button to stop the preview").
     * ⚠️ Both halves: an EXTERNAL audition with `midiLen == 0` has no next note to end it, and gear
     * holds an unanswered note-on until power-cycled.
     * `cut` ends the lane in ~6 ms with no release tail (letting go of a phrase note's A).
     */
    void stop_preview(bool cut = false);

    // ── ↑ feedback ───────────────────────────────────────────────────────────────────────────────

    // One track's playhead. In SONG mode the eight run independently, so the UI asks per track.
    PlaybackPosition playheads(int trackId);

    bool is_playing() const { return seq_.is_playing(); }

    /** The device rate the sequencer runs at. */
    int sample_rate() const { return sampleRate_; }

    // ── ↑ debug: the conformance trace ───────────────────────────────────────────────────────────
    // Enable AFTER the project is pushed: the header's project= is the sha of the pushed blob.
    void set_trace(bool enabled, const std::string& path);

    bool trace_enabled() const { return traceEnabled_; }

  private:
    /**
     * A mapped knob IS the press: whatever the take held on that destination is the hand's now, or
     * `MixerHeld` would skip the very fader the knob is moving.
     * ⚠️ A TABLE's TIM latch is left alone — it arms the restore in `stop()`.
     */
    void release_song_hold(MapDestId id, int scopeIndex);

    int      controlChannel_  = -1;      // -1 = no channel is reserved for mapping knobs
    uint64_t mappedCcWrites_  = 0;
    bool     learnArmed_      = false;   // `R` is down: the next knob NAMES rather than drives
    uint64_t learnEvents_     = 0;
    int      learnController_ = -1;
    int      learnChannel_    = -1;
    int      lastCcChannel_   = -1;      // whatever channel the cable last carried a CC on

    // Drop what the rewound tracks had queued. ⚠️ One frame PER TRACK: clearing every track from
    // the earliest boundary would drop notes a track further ahead will not schedule again.
    void apply_rollback(const songcore::RollbackPlan& plan);

    // The engine's frame counter IS the transport clock. With no engine it stays where a test put it.
    void sync_clock();

    void before_play();

    int64_t after_play();

    /** Frames per quarter note at the live tempo. ⚠️ Same expression as
     *  `ExternalConsumer::frames_per_quarter`: built from the truncated `frames_per_step`, so the
     *  beat stays on the scheduler's grid. */
    int64_t frames_per_quarter() const;

    /**
     * Every SoundFont instrument made to hold the sound it names, as the transport starts.
     * ⚠️ Browsing the PATCH row while stopped can make LRU eviction reclaim a slot a song instrument
     * points at — `routing.sfSlot` cannot tell, and the track plays the wrong sound. (While playing,
     * note triggers keep their own slots fresh.) Here rather than at each `play_*`; nearly free.
     * A project needing more distinct sounds than `MAX_SOUNDFONTS` reloads on every start.
     */
    void resync_soundfont_slots();

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

        void apply(const Event& ev, bool external) override;
    };

    /**
     * Give the audio thread's drain its route and everything it can play. The route is rebuilt each
     * poll and published only when it changed.
     * ⚠️ Tables are pushed EAGERLY: the note path's lazy push never runs for a live key.
     */
    void arm_midi_in();

    /**
     * What the drain handled since the last poll, on the song's thread:
     *   • MAPPED knobs are applied to the project (`apply_mapped_cc`);
     *   • LEARN records the controller, and every CC records its channel;
     *   • ROUTED records reach the cable when thru is on (suppressions are counted) and teach
     *     `TrackInstruments` which instrument each track plays.
     * The observer is told last.
     */
    void drain_midi_in();

    /**
     * Would this record have gone to the cable? Only to count a suppression. Track-scoped records
     * take their owner from the cable consumer's own `TrackInstruments` — never a second opinion.
     */
    bool record_is_external(const Event& ev) const;

    /**
     * The preview lane's note-off on the CABLE. A bus record through `consume`, so it passes the same
     * routing gate, `TrackInstruments` and LEN `min` as the note-on did.
     */
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
