#ifndef POCKETTRACKER_SONGCORE_HOST_H
#define POCKETTRACKER_SONGCORE_HOST_H

// ─── The songcore runtime ────────────────────────────────────────────────────────────────────────
//
// The object an application owns to make songcore play: it holds the Project, the bus (MidiRouter),
// the Sequencer, and the trace sink, and it exposes exactly the verb set the event-schema §7 JNI
// vocabulary names — push the song, play/stop, poll, read back playheads. Phase 1 S5.
//
// PLATFORM-FREE ON PURPOSE. The only thing outside songcore it knows is AudioEngine (audio-engine.h),
// which is the *portable* engine core — no <jni.h>, no <oboe/*>, no <android/*> anywhere in this
// header. That is the whole point: the Android JNI shell (songcore-jni.cpp) is a thin marshalling
// layer over this class, and the SDL shell will construct the same class the same way. Nothing that
// decides how the song sounds lives above this line on either platform.
//
// Threading: single-threaded by contract. Every verb is called from the app's UI/transport thread
// (on Android: the Compose poll loop + input handlers), never from the audio callback. The engine
// calls it makes (scheduleNote…, clearScheduledNotesFrom) are the same ones the Kotlin sequencer made
// from that same thread — they land in the engine's lock-free queues, which is the existing contract.
//
// S6b closed the last gap: project→engine setup (engine_setup.h) and the offline render (render.h)
// are C++ too, so this class can now take a project from JSON all the way to a WAV with no app code
// at all — which is what tools/ptrender does, and what the SDL shell will do. The one thing still
// outside it is Android's *media* loading (samples/SF2 from disk): the Kotlin loader also drives
// MediaCodec for m4a and reads WAV cue points, so it stays for now and pushes its results down via
// push_routing(). load_media() is the C++ equivalent, used by every non-Android caller.

#include <algorithm>
#include <cstdint>
#include <fstream>
#include <set>
#include <string>

#include <functional>

#include "../audio-engine.h"
#include "../byte_source.h"   // pt_read_file — the .ptp and .pti reads
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
    // `engine` may be null (a trace-only host — useful for testing the scheduler with no audio). The
    // sample rate is re-read from the engine on every verb, mirroring PlaybackController, which calls
    // audioEngine.getDeviceSampleRate() on each poll — a headphone swap can change it mid-session.
    SongcoreHost(AudioEngine* engine, int sampleRate)
        : engine_(engine),
          sampleRate_(sampleRate),
          seq_(router_, project_, sampleRate),
          consumer_(engine, &project_, &routing_),
          external_(&project_) {
        // The engine consumer is the bus's permanent subscriber: events become audio. The trace
        // writer joins it only while tracing (set_trace), and sees the identical records.
        if (engine_) router_.add_consumer(&consumer_);
        // ⚠️ The EXTERNAL consumer is attached UNCONDITIONALLY, with or without a port — it is not an
        // optional feature bolted on when a cable appears. Half of its job is bookkeeping (which track
        // owns which note, what a channel was last told), and a consumer that only starts listening
        // when a device is picked would attach mid-song with no idea what is already sounding.
        router_.add_consumer(&external_);

        // MIDI in (phase E). Wired here and never again: the router reads the live Project (which is
        // replaced IN PLACE by push_project, so the pointer stays good) and the SAME `TrackInstruments`
        // the cable's own consumer keeps, so a live key and a sequenced note cannot disagree about
        // which instrument a track is playing. Nothing here opens or touches a port.
        midiInRouter_.set_project(&project_);
        midiInRouter_.set_track_instruments(&external_.track_instruments());
    }

    ~SongcoreHost() { set_trace(false, ""); }

    MidiRouter& router() { return router_; }
    Sequencer&  sequencer() { return seq_; }
    const Project& project() const { return project_; }

    // ── ↕ EXTERNAL MIDI out (MIDI plan §4.3) ─────────────────────────────────────────────────────
    // The port itself is the platform's (ALSA rawmidi / winmm / MidiManager); everything above it is
    // in midi_out.h and shared. The shell picks a device and hands the open port in here.
    ExternalConsumer& midi_out() { return external_; }
    void set_midi_out(IMidiOut* out) { external_.set_out(out); }
    void set_midi_offset_ms(int ms) { external_.set_offset_ms(ms); }
    /** Phase C: the 24 PPQN clock + transport out. Takes effect at the next transport start. */
    void set_midi_sync_out(bool on) { external_.set_sync_out(on); }
    bool midi_sync_out() const { return external_.sync_out(); }

    /**
     * Hand the queue's release over to somebody else — B3's sender thread (`shell/midi-sender.h`).
     *
     * ⚠️ **ONE OWNER OF "WHO RELEASES THE QUEUE", and it is the same rule B4.3 wrote for "who opens
     * the port".** Leaving `poll()`'s pump in place alongside a sender thread would not misbehave
     * visibly — `poll` reads the block-quantised counter, which is never AHEAD of the estimator, so it
     * could only ever release messages the thread was about to release anyway — and that is exactly
     * what makes it dangerous: the 60 Hz path would still be live, still be the thing timing some
     * fraction of the messages, and a broken sender thread would look like a working one with worse
     * jitter. A measurement can only tell the two cadences apart if only one of them is running.
     *
     * Nobody calls this in the tools or on the Android JNI path, so `poll()` keeps pumping there.
     */
    void set_midi_pump_external(bool external) { midiPumpExternal_ = external; }
    bool midi_pump_external() const { return midiPumpExternal_; }

    // ── ↕ MIDI in (MIDI plan phase E2) ───────────────────────────────────────────────────────────
    //
    // The mirror of the block above, and the same division of labour: the PORT is the platform's
    // (`IMidiIn`, opened by the shell), and everything from the first byte onwards is here — the queue
    // that crosses the backend's thread, the parser, the router, and the one place that drains them.
    //
    // ⚠️ **THE HOST OWNS THE DRAIN, NOT THE SHELL**, for the reason `poll()` owns the lookahead: three
    // platforms will each open a port their own way (winmm now, ALSA and `MidiManager` at E5) and the
    // thing that happens to the bytes afterwards must be the same code on all three. A shell that
    // drained for itself would be one drain per platform, diverging in one of them.

    /**
     * Where a backend delivers its bytes — `IMidiIn::set_sink(&host.midi_in_sink())`.
     *
     * ⚠️ Called from a thread this class knows nothing about, which is exactly what `MidiInQueue` is
     * for: the sink is a lock and a memcpy and NOTHING else, and everything with an opinion (parsing,
     * routing, the observer) runs on the frame loop in `poll()`.
     */
    MidiInQueue& midi_in_sink() { return midiInQueue_; }

    /** The routing policy's counters and the channel map's reader. See midi_in.h. */
    MidiInputRouter& midi_in_router() { return midiInRouter_; }
    const MidiParser& midi_in_parser() const { return midiInParser_; }

    /**
     * Told about every message drained, with the records it produced. E2's is the shell's console;
     * E4's is the injection into the engine. Nullable — with none, the drain still runs and still
     * counts, because the counters are how "no cable" is told from "no track listening".
     */
    void set_midi_in_observer(IMidiInObserver* obs) { midiInObserver_ = obs; }

    /**
     * The instrument a live key plays on a track the sequencer has not touched yet — the one the UI is
     * showing (midi_in.h says why this exists at all: without it a correctly configured keyboard is
     * SILENT on a stopped song, which is the "not configured looks like broken" failure verbatim).
     * Pushed every frame by the shell, because the user moves the cursor between frames.
     */
    void set_midi_in_instrument(int instrumentId) { midiInRouter_.set_fallback_instrument(instrumentId); }

    /**
     * MIDI THRU — whether a live key on a track whose instrument is EXTERNAL reaches the CABLE (E4).
     *
     * ⭐ **ON is the feature; OFF exists for exactly one configuration, and it is not a preference.**
     * Playing external gear from a keyboard through the tracker is the obvious use of an input port,
     * and a key on an EXTERNAL track that did nothing at all would be §B2's failure verbatim ("not
     * configured" indistinguishable from "broken").
     *
     * ⚠️ **BUT WHEN THE INPUT PORT AND THE OUTPUT PORT ARE THE SAME DEVICE, THRU IS A FEEDBACK LOOP** —
     * key → track → cable out → the same port → key again, amplifying, for ever. That is not a corner
     * case here: a loopback port is the only MIDI-in test rig this project has (§0.8's desk loopback,
     * now a manual regression test). So the SHELL compares the two open port names and turns thru off
     * when they match, saying so on the console; this flag is where that verdict lands.
     *
     * The default is ON, deliberately: a host nobody told (the tools, the JNI path) behaves like the
     * feature rather than like the exception, and the suppression has to be an explicit act.
     */
    void set_midi_in_thru(bool on) { midiInThru_ = on; }
    bool midi_in_thru() const { return midiInThru_; }

    /** Bytes that reached the queue, and complete messages the parser made of them. */
    uint64_t midi_in_bytes() const { return midiInBytes_; }
    uint64_t midi_in_messages() const { return midiInMessages_; }

    /** Records handed to the ENGINE consumer, to the cable, and withheld from the cable by thru (E4). */
    uint64_t midi_in_injected() const { return midiInInjected_; }
    uint64_t midi_in_thru_sent() const { return midiInThruSent_; }
    uint64_t midi_in_thru_suppressed() const { return midiInThruSuppressed_; }

    /**
     * Forget everything mid-flight — the queue's parked bytes and the parser's half-assembled message.
     *
     * ⚠️ Called when a port CLOSES, and it is not housekeeping: a cable pulled between a status byte
     * and its data leaves running status in force, so the next port's first data byte would complete a
     * note nobody played, on the previous device's channel.
     */
    void reset_midi_in() {
        midiInQueue_.clear();
        midiInParser_.reset();
    }

    /**
     * Drain → parse → route → **inject**, at `frame`. Returns the number of bus records produced.
     *
     * Called by `poll()` with the transport clock; a test calls it directly with a frame of its own.
     */
    int poll_midi_in(int64_t frame) {
        // The buffer is the ring's whole capacity, so ONE drain always empties it: a second pass could
        // only pick up bytes that arrived during this one, and those belong to the next frame anyway.
        uint8_t buf[MidiInQueue::CAPACITY];
        const int n = midiInQueue_.drain(buf, static_cast<int>(sizeof buf));
        if (n <= 0) return 0;
        midiInBytes_ += static_cast<uint64_t>(n);

        int total = 0;
        Event ev[MidiInputRouter::MAX_EVENTS];
        for (int i = 0; i < n; ++i) {
            if (!midiInParser_.feed(buf[i])) continue;
            ++midiInMessages_;
            const int k = midiInRouter_.route(midiInParser_.message(), frame, ev,
                                              MidiInputRouter::MAX_EVENTS);
            total += k;
            for (int j = 0; j < k; ++j) inject(ev[j]);
            // ⚠️ Called even when `k == 0`. A message that routed nowhere still ARRIVED, and an
            // observer told only about the routed ones cannot tell a dead cable from an unmapped
            // channel — the same argument as the router's four counters.
            // ⚠️ **AFTER the injection, not before** — an observer that prints is a debugging aid, and
            // a debugging aid that runs between the record and the sound it makes would be the one
            // thing changing the order of the two.
            if (midiInObserver_) midiInObserver_->on_midi_in(midiInParser_.message(), ev, k);
        }
        return total;
    }

    // ── ↓ data ───────────────────────────────────────────────────────────────────────────────────
    // The blob is the .ptp JSON verbatim — the bytes FileController.serializeProject() produces, which
    // S2 proved this parser reads byte-exactly. Pushing REPLACES the project in place: `project_` never
    // changes address, so the Sequencer's pointer into it stays valid even mid-playback (the Kotlin
    // sequencer likewise reads the one live Project object, seeing edits as they land).
    //
    // Whole-project push, not per-item diffs: the parser already exists and is proven, and a wrong
    // diff is a silent desync. ⚠️ The cost — ~440 KB of JSON for a full pool, measured at 2.2 MB peak
    // and 7 ms to parse — is paid only by a caller that pushes a serialized blob. The SDL shell does
    // not: it edits the live document in place (see edit_project below), so this is the JNI path's
    // price alone.
    bool push_project(const std::string& blob) {
        // allow_exceptions=false: a malformed blob must leave the previous project intact, not throw
        // across the JNI boundary (and it keeps songcore compilable with exceptions off).
        json j = json::parse(blob, /*cb=*/nullptr, /*allow_exceptions=*/false);
        if (j.is_discarded() || !j.is_object()) return false;

        Project parsed = parse_project(j);
        normalize_and_migrate(parsed);   // pool repair + v0→1 table-volume migration, as FileController does
        project_ = std::move(parsed);
        projectSha_ = sha1_hex(blob);
        // Table data may have changed with the push, so the consumer's "already sent to the engine"
        // cache must not survive it.
        consumer_.invalidate_tables();
        return true;
    }

    // The two per-instrument facts songcore cannot derive, because it never opens a file: the sample's
    // rate ratio (deviceRate / fileRate) and the SF2 slot the instrument's soundfontPath resolved to.
    // The app pushes them alongside the project — same call site, so they can never drift apart.
    void push_routing(const float* sampleRateRatios, const int* sfSlots, int count) {
        const int n = std::min(count, POOL_INSTRUMENTS);
        for (int i = 0; i < n; ++i) {
            routing_.sampleRateRatio[i] = sampleRateRatios[i];
            routing_.sfSlot[i] = sfSlots[i];
        }
    }

    // ── ↓ transport ──────────────────────────────────────────────────────────────────────────────
    // Each returns the frame the transport latched (the trace's session base), so a caller that also
    // keeps a Kotlin-side trace stamps the SAME base instead of re-reading a clock that has moved on.
    //
    // ⚠️ CHAIN and PHRASE take the MIXER TRACK they belong to, defaulted to 0. It is what gives them
    // the arrangement's fader, mute, voice slot and per-track FX instead of channel 1's; the default
    // is what keeps every tool caller — and its goldens — asking for track 0 unchanged.
    int64_t play_song(int startRow)   { sync_clock(); seq_.playSong(startRow);     return after_play(); }
    /** LIVE mode from a standing start: `mask` bit N launches track N at `songRow`, the rest begin silent. */
    int64_t play_song_live(int songRow, int mask) {
        sync_clock(); seq_.playSongLive(songRow, mask); return after_play();
    }
    int64_t play_chain(int chainId, int trackId = 0) {
        sync_clock(); seq_.playChain(chainId, trackId);   return after_play();
    }
    int64_t play_phrase(int phraseId, int trackId = 0) {
        sync_clock(); seq_.playPhrase(phraseId, trackId); return after_play();
    }

    /**
     * Stop the transport — the scheduler AND the engine.
     *
     * ⚠️ THIS USED TO STOP ONLY THE SCHEDULER, and it is the whole Phase-4 "START won't stop playback"
     * bug. `seq_.stop()` halts the thing that DECIDES what to play; the notes it has already handed to
     * the engine live in `noteQueue` with target frames in the future, and `processAudioBlock` goes on
     * draining them. The scheduler runs `BUFFER_PHRASES` (2) phrases ahead — ≈4 s at the default tempo
     * — so the button did nothing audible for four seconds, and the next START (correctly seeing
     * `isPlaying_ == false`) scheduled a SECOND stream on top of the stale one. That is what "layered"
     * playback was: two live schedules in one queue, not the voice allocator.
     *
     * ⚠️ THE ASSUMPTION WAS TRUE WHEN IT WAS MADE. On Android this host is only ever reached through
     * `PlaybackController.stop()`, which does the engine-side cleanup itself immediately afterwards —
     * and says so: *"songcore resets its own transport, per-track state and master EQ; the engine-side
     * cleanup below (queues, voices) is shared and runs for both engines"* (PlaybackController.kt:415).
     * It was shared right up until the SDL shell called `host_.stop()` with no PlaybackController under
     * it. The layer built on top invalidated the assumption, in a channel nothing was pointed at.
     * Android's calls are now redundant rather than wrong — both verbs are idempotent.
     *
     * ORDER IS LOAD-BEARING, and it is Kotlin's: the master EQ is restored BEFORE the queues are
     * cleared, because the EQM override lives in the param queue — drop those entries first and the bus
     * stays stuck on the last EQM preset forever. Guarded on a live project: the render path leaves
     * `currentProject_` null and restores its own master EQ in RenderController.
     *
     * `play_song`/`play_chain`/`play_phrase` deliberately do NOT call this. Kotlin's play* verbs each
     * open with `stop()`, but on both platforms the caller has already done it — the shell's `on_start`
     * stops before it starts (input_dispatcher.cpp), and the JNI's caller is that same
     * `PlaybackController.play*`. A second stop here would buy nothing and would put an extra `t_stop`
     * into the event trace the 36 goldens byte-compare.
     */
    void stop() {
        // ⚠️ TWO SOURCES ARM THIS RESTORE, ONE ON EACH SIDE OF THE SEAM. A phrase step's EQM is
        // emitted by the scheduler, which knows it happened; a TABLE row's EQM is applied by the
        // audio engine itself, per voice, and the scheduler never sees it. Read the engine's latch
        // UNCONDITIONALLY — short-circuiting past it would leave it set for the next take, which
        // would then restore the mixer EQ over a bus nothing had overridden.
        const bool table_eqm = engine_ && engine_->takeTableMasterEqTouched();
        if (engine_ && (seq_.eqm_active() || table_eqm) && seq_.has_live_project()) {
            engine_->setMasterEqSlot(project_.masterEqSlot);
        }
        // The mixer faders VTR/VMV moved, for the identical reason and in the identical window: they
        // REPLACE the authored value and nothing later puts it back, so without this the engine keeps
        // whatever the song faded to while the MIXER screen still draws what the user typed — and the
        // next PLAY starts from the wrong level. Also BEFORE the queues are cleared, so a queued fader
        // move due after the stop cannot land on top of the restore.
        if (engine_ && seq_.mixer_vol_active() && seq_.has_live_project()) {
            const int tracks = static_cast<int>(project_.tracks.size());
            for (int i = 0; i < 8 && i < tracks; ++i)
                engine_->setTrackVolume(i, hex_to_float(project_.tracks[static_cast<size_t>(i)].volume));
            engine_->setMasterVolume(hex_to_float(project_.masterVolume));
        }
        sync_clock();
        seq_.stop();
        // The transport ends: every note the cable is holding, ended NOW (not queued — the queue is
        // dropped). `seq_.stop()` does not reach the router, so this cannot ride an event.
        external_.panic();
        if (engine_) {
            engine_->clearScheduledNotes();   // the lookahead: notes, kills AND param updates
            engine_->stopAll();               // …and the voices already sounding (instant — no fade)
        }
        consumer_.clear_track_mask();   // Kotlin clears phraseTrackMask in clearScheduledNotes/stopAll
        flush_trace();
    }

    // AudioEngine.phraseTrackMask — bit N set once track N has had a note scheduled this session.
    // The OCTA visualizer lights one scope lane per bit.
    int track_mask() const { return consumer_.track_mask(); }

    // The lookahead poll — PixelPerfectRenderer's 60 Hz loop drives this, exactly as it drove
    // PlaybackController.updatePlaybackBuffer().
    void poll() {
        sync_clock();
        // ⚠️ **BEFORE the lookahead pass, and with a LEAD-IN.** A live key has no lookahead at all — the
        // byte is already late by the time it is drained — so it is stamped `now + 100` frames, which is
        // `preview_note`'s lead-in and for the identical reason: a frame in the immediate past is a
        // record the engine has already run past. Draining first also means a key pressed this frame is
        // routed against the clock this frame read, not the one the pass has moved on to.
        poll_midi_in(seq_.clock() + MIDI_IN_LEAD_FRAMES);
        seq_.updatePlaybackBuffer();
        // ⚠️ The MIDI queue is released HERE — unless a sender thread has taken the job (B3) — and it
        // must be released even when nothing is playing, because a LEN gate and a panic's note-offs are
        // things we OWE after the last note was scheduled. `pump` is idempotent on an empty queue.
        if (!midiPumpExternal_) external_.pump(seq_.clock());
        flush_trace();
    }

    // ── ↓ the render path (RenderController.scheduleSongForRender / scheduleSelectionForRender) ───
    // Returns the total frame span scheduled. trackFilter == nullptr renders every track.
    int64_t schedule_song_range(int startRow, int endRow, const std::set<int>* trackFilter) {
        sync_clock();
        int64_t frames = seq_.scheduleSongRowRange(startRow, endRow, trackFilter);
        flush_trace();
        return frames;
    }

    // ── ↓ the render itself (render.h) ───────────────────────────────────────────────────────────
    // Three verbs rather than one, because the caller may schedule with a DIFFERENT sequencer:
    // Android's RenderController must still be able to put the KOTLIN one between prepare and render
    // — that is exactly what the ENG=KT vs ENG=C++ byte-identical WAV check compares, and it stays
    // meaningful only while the sequencer is the sole difference between the two runs.
    // A caller with no other sequencer (tools/ptrender, the SDL shell) uses render_song_range_to_wav.
    void prepare_render(int startRow, int endRow) {
        if (!engine_) return;
        // ⚠️ **A RENDER IS NOT A PERFORMANCE — the cable is detached for its duration.** A render
        // schedules the entire song into the bus in one go and never polls, so an attached
        // ExternalConsumer would (a) accumulate the whole song's messages with nothing releasing them
        // and (b) fire them at the wall clock afterwards, playing the song at the hardware once the
        // render was over. §9 already says an EXTERNAL instrument cannot be rendered to a WAV; this is
        // what makes it render as silence rather than as chaos.
        external_.panic();
        router_.remove_consumer(&external_);
        songcore::prepare_render(*engine_, project_, startRow, endRow);
        consumer_.clear_track_mask();   // Kotlin clears phraseTrackMask in clearScheduledNotes()
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

    // prepare → schedule → render → finish, with songcore's own sequencer in the middle.
    RenderStats render_song_range_to_wav(int startRow, int endRow, const std::string& path,
                                         const RenderOptions& opts = RenderOptions(),
                                         const std::function<void(float)>& progress = nullptr) {
        if (!engine_) return RenderStats();
        prepare_render(startRow, endRow);
        const int64_t songFrames = schedule_song_range(startRow, endRow, nullptr);
        RenderStats stats = render_to_wav(path, songFrames, opts.stemsMode, opts.applyMasterBus, progress);
        finish_render();
        return stats;
    }

    // The whole song, bounds and all — what a host renderer actually wants to call.
    RenderStats render_song_to_wav(const std::string& path,
                                   const RenderOptions& opts = RenderOptions(),
                                   const std::function<void(float)>& progress = nullptr) {
        const SongBounds b = find_song_bounds(project_);
        if (b.empty()) return RenderStats();
        return render_song_range_to_wav(b.startRow, b.endRow, path, opts, progress);
    }

    // Load the project's samples and SoundFonts into the engine, and learn the Routing from them
    // (engine_setup.h). Android does this in Kotlin — its loader also drives MediaCodec for m4a — and
    // pushes the result in via push_routing() instead.
    //
    // ⚠️ It also WRITES to the project: a WAV's `cue ` chunk is where its slice boundaries live, and
    // loading the audio is what learns them (S6b). See load_project_media.
    MediaLoadResult load_media(const std::string& baseDir) {
        lastMediaLoad_ = MediaLoadResult();
        if (!engine_) return lastMediaLoad_;
        lastMediaLoad_ = load_project_media(*engine_, project_, baseDir, appRoot_, routing_);
        return lastMediaLoad_;
    }

    /**
     * What the most recent `load_media` found. Recorded HERE rather than returned by every path that
     * loads media, because two of them — `load_project_file` and `clean_inst` — do the load as one
     * step of a fixed sequence and have their own return values; a caller that had to remember to ask
     * for the count is a caller that will not.
     *
     * ⚠️ It matters because a failed sample load is otherwise INVISIBLE on the two platforms with no
     * console. The instrument simply plays silence. This is also where an out-of-memory sample lands
     * on a 512 MB device, and it is a handled, non-crashing path — the care is wasted if nobody says
     * so.
     */
    const MediaLoadResult& last_media_load() const { return lastMediaLoad_; }

    /**
     * THIS install's app root — the folder Samples/, Soundfonts/… live directly under. The SDL shell
     * hands in `$POCKETTRACKER_HOME` once at boot; a project copied off ANOTHER install (a phone, another
     * handheld) then has its absolute-but-dead media paths re-rooted onto it at load (resolve_media_path).
     *
     * ⚠️ Leave it UNSET and every load behaves exactly as before — which is precisely what the host tools
     * do, so their goldens do not move. Only a caller that sets it gets the relocation.
     */
    void set_app_root(std::string root) { appRoot_ = std::move(root); }

    // ── ↓ the LIVE param push (engine_setup.h) ───────────────────────────────────────────────────
    //
    // Everything the engine holds ON ITS OWN BEHALF and therefore survives a project swap: the mixer,
    // the master bus, reverb, delay, the 128-slot EQ bank, and every instrument's playback params.
    // None of it is a note, so nothing on the event path pushes it.
    //
    // ⚠️ **Call push_params() after load_media(), or the project you loaded is not the project you
    // hear.** Until Phase 3 S4 nothing did: `push_project_params` had exactly one caller in the tree,
    // `prepare_render`. A project rendered to a WAV therefore had its reverb, its master EQ, its track
    // volumes and its samplers' filters — and the same project PLAYED had the engine's defaults, or
    // whatever the last project left behind. See push_live_params for why seven conformance tools
    // could not see it.
    void push_params() {
        if (!engine_) return;
        push_live_params(*engine_, project_);
    }

    /** One instrument's params — what an INSTRUMENT / MODS / pool edit pushes. Cheap and idempotent. */
    void push_instrument(int id) {
        if (!engine_) return;
        if (id < 0 || id >= static_cast<int>(project_.instruments.size())) return;
        push_instrument_params(*engine_, project_.instruments[id], project_.tempo, sampleRate_);
    }

    /**
     * The GLOBALS — the mixer, the master bus, both send buses, the EQ bank, the master EQ. What a
     * MIXER or EFFECTS edit pushes, and the exact counterpart of push_instrument(id) above: the
     * right-sized verb for what those two screens can actually change.
     *
     * ⚠️ Deliberately not push_params(). That one additionally sweeps all 128 instruments — ~2,500
     * engine calls — and neither screen can touch an instrument. On a handheld, holding A+UP on a track
     * volume fires an edit every 100 ms (the key-repeat interval), and re-pushing the whole pool on each
     * one is work paid for nothing. push_params() stays what it is: the LOAD-time call.
     */
    void push_globals() {
        if (!engine_) return;
        push_mixer(*engine_, project_, held_by_song());
    }

    /**
     * What the RUNNING TAKE owns right now, so `push_globals()` can push the authored mixer WITHOUT
     * wiping it. Same two questions `stop()`'s restore asks, from the other end of the take.
     *
     * ⚠️ It is not only the mute/solo chords that need this. Every `mark_modified` on a globals screen
     * pushes too, so nudging a MIXER fader or an EFFECTS dial mid-song used to kill a running EQM the
     * same way a mute did.
     *
     * ⚠️ PEEK, NEVER TAKE, on the table latch: it is a one-way arm for the restore in `stop()`, and
     * consuming it here would leave a bus a TABLE row's EQM had overridden restored by nobody.
     */
    MixerHeld held_by_song() const {
        MixerHeld held;
        if (!engine_ || !seq_.has_live_project()) return held;
        held.faderTracks = seq_.mixer_vol_tracks();
        held.masterFader = seq_.master_vol_active();
        held.masterEq    = seq_.eqm_active() || engine_->tableMasterEqTouchedPeek();
        return held;
    }

    // ── ↕ the EQ editor (Phase 3 S8) ─────────────────────────────────────────────────────────────
    //
    // ⚠️ THE TWO CALLS ARE BOTH REQUIRED, and this is the "a push that is SKIPPED is the same bug
    // wearing a different hat" rule in its sharpest form. `setEqBand` writes ONLY into the engine's
    // 128-slot BANK. Nothing that USES a slot reads the bank while it runs: the master bus compiles its
    // own biquad coefficients (`masterChain.masterEq.bands[b].setParams(…)`), and each instrument keeps
    // its own copy (`instrumentParams[id].eqBands[i]`) — see sample-editor.cpp. So a band edit that only
    // calls `set_eq_band` updates the bank and CHANGES NOTHING YOU CAN HEAR: the EQ goes on filtering
    // with the coefficients it was last handed.
    //
    // Re-assigning the SAME slot to the same consumer is what forces the recompute, which is why Kotlin
    // re-calls the caller's own setter after every single band nudge — and why the editor has to
    // remember WHICH cell opened it. This is also why these are the right-sized verbs rather than
    // `push_globals()`: holding A+UP on a GAIN cell fires an edit every 100 ms, and re-pushing the whole
    // mixer, both send buses and all 128 EQ slots for one band's frequency is work paid for nothing.

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
     * The spectrum of ONE signal path, for the EQ editor's visualization — the master bus (0), the
     * delay's input (1), the reverb's input (2), or one instrument's own voices (3).
     *
     * Not the same question as the visualizer's `getSpectrumMagnitudes`, which is always the master bus.
     * An EQ sitting on the reverb send drawn over the master spectrum would be a curve over a signal the
     * band is not even in. Returns false with no engine, and the editor then draws its grid over nothing
     * — which is exactly what `ptshot` renders.
     */
    bool spectrum_for_source(int source, int instrId, int numBins, float* out) const {
        if (!engine_ || numBins <= 0 || !out) return false;
        engine_->getSpectrumMagnitudesForSource(source, instrId, numBins, out);
        return true;
    }

    // ── ↕ the instrument operations (InstrumentController) ────────────────────────────────────────
    // The verbs that own a SOURCE — the ones a plain field edit cannot express because freeing the old
    // sample or SoundFont is the engine's business. See engine_setup.h for the sharing guards.
    //
    // ⚠️ NOT guarded on `engine_`, and that distinction cost a harness failure to find: these EDIT THE
    // DOCUMENT and merely also free engine resources. An early `if (!engine_) return;` here would mean
    // the whole editing path silently did nothing without an audio device — so the null-check lives
    // around the engine CALLS, inside engine_setup.h, and the document is always written.

    void set_instrument_type(int id, InstrumentType type) {
        songcore::set_instrument_type(engine_, project_, id, type, routing_);
        push_instrument(id);   // itself a no-op without an engine
    }

    void clear_instrument(int id) {
        songcore::clear_instrument(engine_, project_, id, routing_);
        push_instrument(id);
    }

    // The SF2 preset list, for the INSTRUMENT screen's PRESET row. All three answer for an instrument
    // with no SoundFont (0 / 0 / "---"), which is what lets the screen draw before anything is loaded.
    int sf_preset_count(int id) const {
        if (!engine_ || id < 0 || id >= POOL_INSTRUMENTS) return 0;
        return soundfont_preset_count(*engine_, project_.instruments[static_cast<size_t>(id)], routing_);
    }
    int sf_preset_index(int id) const {
        if (!engine_ || id < 0 || id >= POOL_INSTRUMENTS) return 0;
        return soundfont_preset_index(*engine_, project_.instruments[static_cast<size_t>(id)], routing_);
    }
    std::string sf_preset_name(int id) const {
        if (!engine_ || id < 0 || id >= POOL_INSTRUMENTS) return "---";
        return soundfont_preset_name(*engine_, project_.instruments[static_cast<size_t>(id)], routing_);
    }
    void set_sf_preset_by_index(int id, int index) {
        if (!engine_ || id < 0 || id >= POOL_INSTRUMENTS) return;
        songcore::set_soundfont_preset_by_index(*engine_, project_.instruments[static_cast<size_t>(id)],
                                                routing_, index);
    }

    // ── ↕ the FILE verbs (Phase 3 S6a — what the browser's A button reaches) ─────────────────────
    //
    // Until now the host could only be handed a project blob by its caller. These five are what let the
    // app OPEN and SAVE things itself, and they are the reason `set_instrument_type` / `clear_instrument`
    // (S4) finally have company: those two could empty a slot, and nothing could fill one.
    //
    // All of them take a path and none of them take a FileSystem: songcore reads and writes files
    // directly (project_io.h parses a blob; the engine opens a .wav), and the `ui::FileSystem`
    // abstraction exists for the BROWSER — for listing, sorting, renaming — which is a UI concern.
    // Two layers, each doing its own job.
    //
    // The *opening* still funnels: `pt_read_file` and `pt_fopen` (native/byte_source.h) are the one
    // place a path string becomes a handle, for songcore and the engine alike. That is a different
    // seam from `ui::FileSystem` and deliberately narrower — it knows nothing about names, parents
    // or listings, which is what lets songcore keep depending on it and not on the UI.

    /** Replace the project from a .ptp on disk: stop → parse → push → load its media → push its params. */
    bool load_project_file(const std::string& path, const std::string& baseDir) {
        std::string blob;
        if (!pt_read_file(path.c_str(), blob)) return false;

        // ⚠️ **THE TRANSPORT STOPS BEFORE THE DOCUMENT UNDER IT IS REPLACED**, exactly as new_project
        // does — a load is a document swap and every one of them invalidates the scheduler's position.
        // Without this the sequencer keeps `isPlaying_`, its PlaybackMode and its `nextSongRowToSchedule_`
        // from the OLD song and walks them through the NEW project's data: the rows there are empty, so
        // scheduler.h's `maxChainLength == 0` arm advances the row WITHOUT advancing
        // `nextFrameToSchedule_`, burning through the rest of the stale range for free. By the time the
        // walk wraps to row 0 and finds real content, the schedule frame is behind the clock and the
        // song's first row lands in the PAST. What the user sees is a lit PLAY, silence, a playhead
        // drifting through the previous song's position, and then a late start missing its first row.
        //
        // A parse failure below therefore leaves the transport stopped while the previous project stays
        // intact (push_project's contract). That is deliberate: the only alternative is playing on over a
        // LOAD FAILED, and there is nothing to resume to — the position that made sense belonged to the
        // document the user was trying to leave.
        const bool wasPlaying = seq_.is_playing();
        stop();

        if (!push_project(blob)) return false;

        // ⚠️ The two calls that must follow a push, in this order, or the project you loaded is not
        // the project you hear. load_media opens the files and learns the Routing; push_params pushes
        // everything the ENGINE holds that no note carries. Phase 3 S4 is the session that found out
        // what happens when the second one is missing (84.4% of the render's bytes differed).
        load_media(baseDir);
        push_params();

        // Loading a project while the transport runs SWITCHES SONGS: the new one takes over from its
        // top rather than making the user press START again. It cannot be gapless — load_media opens
        // and decodes every sample on the calling thread — so this is a hard cut, and `play_song`
        // re-reads the engine's frame counter (sync_clock) so the schedule starts from where the clock
        // actually is after that wait, not from where it was before the disk work.
        //
        // Always the SONG from row 0, whatever mode was running: a CHAIN or PHRASE id from the old
        // document names unrelated material in the new one, and row 0 is the only position a project
        // the user has not seen yet is known to have. Nothing starts here that was not already playing,
        // which is what keeps the launch and autosave-recovery callers silent.
        if (wasPlaying) play_song(0);
        return true;
    }

    // ⚠️ **songcore WRITES NO USER FILE, and that is deliberate rather than an omission.** Every
    // path that saves something a user would miss — the `.ptp`, the autosave, the template, a
    // `.pti` — goes through `ui::FileSystem::write_file`, which writes a temp and renames it over
    // the target and checks the close. songcore cannot depend on `ui::FileSystem` (it has to keep
    // compiling for the NDK, where *where files live* is the host's problem), so the write belongs
    // one layer up, in `ui/project_actions.cpp` and `ui/lifecycle.cpp`. A truncating `ofstream`
    // writer sitting here is a trap for the next caller: it destroys the previous file at open time
    // and cannot report a failure that only surfaces at the flush.

    // ── PROJECT screen: NEW, and the two COMPACTs ────────────────────────────────────────────────
    //
    // The document surgery itself is pure and lives in project_ops.h. What lands HERE is the half
    // that has to tell the ENGINE, and it is the whole reason these are host verbs rather than three
    // free functions the dispatcher could call: the engine holds state that the document does not,
    // and every one of these invalidates some of it.

    /** PROJECT → NEW. A blank document, and an engine that has forgotten the last one. */
    void new_project() {
        // ⚠️ This comment used to read "before the samples go: no voice may be reading PCM we are about
        // to free" — which was BOTH a false description of stop() (it stopped no voices until the Phase-4
        // fix above) and the wrong reason. The PCM is not safe because of this line: `clearAllSamples()`
        // stops every voice and clears all three queues INSIDE `sampleEditMutex`, which is the only way
        // to do it correctly, since the audio thread can be mid-read and takes that lock with try_to_lock.
        // A stop() out here races it by construction. This is the TRANSPORT stopping because NEW ends the
        // session — a user-facing fact, not a memory-safety mechanism. Do not lean on it for the latter.
        stop();
        songcore::new_project(project_);

        if (engine_) {
            engine_->clearAllSamples();
            engine_->clearAllSoundfonts();
        }
        routing_.reset();     // the ratios and SF slots described the OLD project's media

        invalidate_tables();  // Kotlin's audioEngine.clearLoadedTables()
        push_params();        // Kotlin's syncVolumesToAudioBackend()
    }

    /**
     * PROJECT → COMPACT → SEQ. Unused chains and phrases back to factory.
     *
     * No engine call, and that is not an omission: a chain and a phrase are pure arrangement. They
     * hold no PCM, no table, no param — nothing the engine has ever been told about.
     */
    void clean_seq() { songcore::clean_unused_seq(project_); }

    /**
     * PROJECT → COMPACT → INST. Unused instruments, tables and grooves back to factory.
     *
     * ⚠️ Every part of the engine sync below is load-bearing. Compacting replaced unused instruments
     * with empty ones in the DOCUMENT, but their sample and SoundFont buffers are still loaded — so
     * the RAM would only drop after a save and a reload (Kotlin hits `reloadProjectSamples` here for
     * exactly this). The tables it reset are worse than stale: the consumer caches which tables it
     * has already handed the engine, so without `invalidate_tables` a compacted table 5 would go on
     * playing the OLD table 5's rows.
     */
    void clean_inst(const std::string& baseDir) {
        songcore::clean_unused_inst(project_);
        load_media(baseDir);   // clearAllSamples + clearAllSoundfonts + routing.reset + reload
        invalidate_tables();
        push_params();
    }

    /** A sample (wav/mp3/flac/ogg/opus) → instrument `id`. The browser's A on a sampler slot. */
    bool load_sample(int id, const std::string& path) {
        if (!load_instrument_sample(engine_, project_, id, path, routing_)) return false;
        push_instrument(id);   // the fresh source needs its slot's filter/window/loop pushed at it
        return true;
    }

    /** An .sf2/.sf3 → instrument `id`, which becomes a SOUNDFONT slot. The browser's A on one. */
    bool load_soundfont(int id, const std::string& path) {
        if (!load_instrument_soundfont(engine_, project_, id, path, routing_)) return false;
        push_instrument(id);
        return true;
    }

    /**
     * True when the last media load failed because the device could not hold it, rather than because
     * the file would not parse. The two want opposite messages, and every loader reports failure the
     * same way without this.
     *
     * ⚠️ It forwards ONE bool rather than exposing the engine. The UI has no business reaching the
     * engine directly — that seam is why `pt-ui` is portable — and the question it actually needs
     * answered is this narrow.
     */
    bool last_load_ran_out_of_memory() const {
        return engine_ && engine_->lastLoadFailure() == AudioEngine::LoadFailure::OUT_OF_MEMORY;
    }

    /**
     * Read a .pti into instrument `id`. False if the file will not parse, OR if its source file is
     * gone — but the PARAMETERS land either way in the second case, which is deliberate (a preset
     * whose sample has moved should still give you back its filter and its mod slots).
     */
    bool load_instrument_preset(int id, const std::string& path) {
        std::string blob;
        if (!pt_read_file(path.c_str(), blob)) return false;

        json j = json::parse(blob, /*cb=*/nullptr, /*allow_exceptions=*/false);
        if (j.is_discarded() || !j.is_object()) return false;

        const InstrumentPreset ip = parse_instrument_preset(j);
        const bool sourceOk = apply_instrument_preset(engine_, project_, id, ip, routing_);
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

    // ── ↕ THE SAMPLE EDITOR (Phase 3 S6b) ────────────────────────────────────────────────────────
    //
    // Almost every verb below is ONE LINE, and that is the whole story of this session: the sample
    // editor's DSP has been in the engine since long before songcore existed (`native/sample-editor.cpp`
    // + `native/transient-detector.cpp`), because Android's JNI layer was always a thin forward. So the
    // port writes no DSP — it writes the SEAM, and the seam is this list.
    //
    // The `if (!engine_)` guard on each is not defensive noise: it is what lets `tools/ptdispatch` drive
    // the whole editor — its cursor, its row map, its slice arithmetic, its save paths — with **no audio
    // device at all**. An op that cannot run simply does not run, and the document is unharmed.

    // ── Reading the sample (the feed) ────────────────────────────────────────────────────────────
    int  sample_length(int id) const { return engine_ ? engine_->getSampleLength(id) : 0; }
    bool has_stereo_data(int id) const { return engine_ && engine_->hasStereoData(id); }

    /** The FILE's rate (deviceRate / ratio); 44100 when the slot is empty. See sample_edit.h. */
    int sample_rate_of(int id) const { return original_sample_rate(engine_, routing_, id); }

    /** 0..1 while the sample is sounding, −1 when it is not — the waveform's playhead. */
    float sample_playback_position(int id) const {
        return engine_ ? engine_->getSamplePlaybackPosition(id) : -1.0f;
    }

    /**
     * `bins` (min, max) PAIRS — so the returned vector is 2 × bins long. `channel` is 0 = left,
     * 1 = right, 2 = averaged, and is only consulted for a stereo sample.
     *
     * A frame range that COVERS the sample — (0, 0), or (0, length) — means "the whole thing", and takes
     * the engine's whole-sample entry point. Both spellings must land on the same call, because both
     * callers exist: the editor opens on (0, 0) and the zoom-0 view computes (0, length), and a waveform
     * that changed subtly depending on which of those asked for it would be a bug nobody could see.
     */
    std::vector<float> sample_waveform(int id, int bins, int startFrame = 0, int endFrame = 0,
                                       int channel = 2) const {
        std::vector<float> out(static_cast<size_t>(std::max(bins, 0)) * 2, 0.0f);
        if (!engine_ || bins <= 0) return out;

        const int  len   = engine_->getSampleLength(id);
        const bool whole = (startFrame <= 0) && (endFrame <= 0 || endFrame >= len);

        if (engine_->hasStereoData(id)) {
            // A stereo sample ALWAYS goes through the channel-aware entry point, even un-zoomed: the
            // plain one averages, and SOURCE=LEFT must draw the left channel, not a downmix.
            engine_->getSampleWaveformRangeSource(id, whole ? 0 : startFrame, whole ? len : endFrame,
                                                  out.data(), bins, channel);
        } else if (whole) {
            engine_->getSampleWaveform(id, out.data(), bins);
        } else {
            engine_->getSampleWaveformRange(id, startFrame, endFrame, out.data(), bins);
        }
        return out;
    }

    /** The slice boundaries the detector finds at `sensitivity`. Capped at 128, as Kotlin's JNI is. */
    std::vector<int> detect_transients(int id, int sensitivity) const {
        if (!engine_) return {};
        int       markers[128];
        const int n = engine_->detectTransients(id, sensitivity, markers, 128);
        return std::vector<int>(markers, markers + std::max(n, 0));
    }

    /**
     * The nearest zero crossing to `frame` in direction `dir` (−1 back, +1 forward, 0 either), in the
     * signal the editor's SOURCE mode says the cut will be made in — both channels under STEREO.
     */
    int find_zero_crossing(int id, int frame, int dir, int sourceMode = 0) const {
        return engine_ ? engine_->findZeroCrossing(id, frame, dir, 512, sourceMode) : frame;
    }

    int clipboard_length() const { return engine_ ? engine_->getClipboardLength() : 0; }

    // ── The twelve operations. Every one of them is already written; these only name them. ───────
    void backup_sample(int id) { if (engine_) engine_->backupSample(id); }
    void undo_sample(int id) { if (engine_) engine_->undoSample(id); }
    /** The editor is closing: its single-level undo is unreachable now, so it is just held memory. */
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

    // ── The FX row: a NON-destructive preview, and a destructive apply ───────────────────────────
    //
    // The backup is what makes the FX row auditionable at all: START applies the effect for real, plays
    // it, and the next gesture puts the clean audio back. Only APPLY keeps it. Two separate backups
    // (this one and the undo slot) because they answer different questions — "what did it sound like
    // before I *previewed*" and "what did it sound like before I *committed*".
    void save_fx_preview_backup(int id) { if (engine_) engine_->saveFxPreviewBackup(id); }
    void restore_fx_preview_backup() { if (engine_) engine_->restoreFxPreviewBackup(); }

    void apply_sample_fx(int id, int fxType, int fxValue) {
        if (!engine_) return;
        engine_->applySampleFx(id, fxType, fxValue,
                               static_cast<float>(sample_rate_of(id)), project_.limiterPreGain);
    }

    // ── The three that move the ratio, and therefore live in songcore (sample_edit.h) ────────────
    void apply_rate_mode(int id, int factor) {
        songcore::apply_rate_mode(engine_, routing_, rateCache_, id, factor);
    }
    void pitch_shift_sample(int id, float semitones) {
        songcore::pitch_shift_sample(engine_, rateCache_, id, semitones);
    }
    void time_stretch_sample(int id, float ratio) {
        songcore::time_stretch_sample(engine_, rateCache_, id, ratio);
    }

    // ── The audition ─────────────────────────────────────────────────────────────────────────────

    /**
     * Play the sample DRY at its root, through the SOURCE mode's channel, with the SELECTION as its
     * window — the editor's START.
     *
     * ⚠️ THE WINDOW GOES IN AS FRAMES (`setInstrumentFrameWindow`), NOT through the instrument's own
     * 0-255 `sampleStart` / `sampleEnd`. Those are a 1/255 grid — 8 ms on a 2-second sample — so an
     * audition driven through them cannot hear a one-frame nudge of the selection at all, and CROP
     * then cuts somewhere the user never heard. The frame window is exactly what CROP will keep.
     *
     * ⚠️ It still temporarily mutates `sampleId` when a scratch slot is in play, and the frame window
     * outlives this call: the engine reads both when the note FIRES, 100 frames later, not when it is
     * scheduled. `finish_sample_preview()` is what ends them, on the dispatcher's deadline.
     */
    void preview_sample_editor(int id, int sourceMode, int64_t selStart, int64_t selEnd,
                               int totalFrames, int pitchSemitones) {
        if (!engine_ || id < 0 || id >= POOL_INSTRUMENTS) return;
        Instrument& ins = project_.instruments[static_cast<size_t>(id)];

        const Note savedRoot = ins.root;
        // The PENDING pitch shift is auditioned by transposing the ROOT — nothing is resampled until
        // SAVE bakes it, so this is the only way to hear what it will do.
        if (pitchSemitones != 0)
            ins.root = note_from_midi(std::clamp(note_to_midi(ins.root) + pitchSemitones, 0, 127));

        const int slot = prepare_source_preview(*engine_, id, sourceMode);

        // The engine keys a voice's window on the SLOT it plays from, so the params must be pushed at
        // the scratch slot when there is one — pushing them at the instrument left slot 254 at its
        // 0/255 default, playing the whole sample and ignoring the markers entirely.
        const int savedSampleId = ins.sampleId;
        if (slot != id) ins.sampleId = slot;
        push_instrument_playback_params(*engine_, ins);
        // AFTER the push, which clears any window left over from the previous audition. The scratch
        // slot holds a frame-for-frame copy of the instrument's audio, so the selection indexes both.
        if (totalFrames > 0 && selEnd > selStart)
            engine_->setInstrumentFrameWindow(slot, static_cast<int>(selStart), static_cast<int>(selEnd));
        preview_instrument_dry(*engine_, ins, slot, routing_.sampleRateRatio[id]);
        if (slot != id) ins.sampleId = savedSampleId;

        ins.root = savedRoot;   // the root is read at SCHEDULE time, so it can go back at once
    }

    /**
     * Put the instrument back: its real sample window, its EQ, its sends, its modulation. Runs on the
     * dispatcher's 100 ms deadline, and IMMEDIATELY if a second START arrives inside that window.
     *
     * The window needs no saved copy to restore FROM — the preview never wrote to the project, and the
     * push below is itself what disarms the engine's frame window (`setInstrumentParams`).
     */
    void finish_sample_preview(int id) {
        if (!engine_ || id < 0 || id >= POOL_INSTRUMENTS) return;
        Instrument& ins = project_.instruments[static_cast<size_t>(id)];
        push_instrument_playback_params(*engine_, ins);
        // The push above only reaches the slot the INSTRUMENT points at; a channel-selected audition
        // played from the scratch, so that one is disarmed by name.
        engine_->setInstrumentFrameWindow(SOURCE_PREVIEW_SLOT, -1, -1);
        // The three the DRY preview switched off.
        push_instrument_mod_eq_sends(*engine_, ins, project_.tempo, engine_->getSampleRate());
    }

    // ── SAVE and CHOP ────────────────────────────────────────────────────────────────────────────

    /** The edited PCM → a WAV at `path`, with its slice boundaries in the `cue ` chunk. */
    bool save_sample_wav(int id, const std::string& path, const std::vector<int>& cuePoints,
                         int sourceMode, bool hasStereo) {
        if (!engine_) return false;
        return songcore::save_sample_wav(*engine_, routing_, id, path, cuePoints, sourceMode, hasStereo);
    }

    /** Every slice → its own WAV in `dir`. Returns how many were written. */
    int chop_sample(int id, const std::string& dir, const std::string& baseName,
                    const std::vector<std::pair<int64_t, int64_t>>& slices) {
        if (!engine_) return 0;
        return songcore::chop_sample(*engine_, routing_, id, dir, baseName, slices);
    }

    // ── ↕ live editing — the SDL shell's UI *is* the editing model ────────────────────────────────
    //
    // On Android the Kotlin UI owns a SECOND copy of the project (Compose needs an observable object
    // graph to recompose against) and pushes it down as a whole JSON blob whenever it changes. There
    // is no Kotlin on Linux: the C++ UI edits THIS project, in place.
    //
    // That is not a shortcut — it is what push_project's own contract already describes. The project
    // never changes address, and the Sequencer reads "the one live Project object, seeing edits as
    // they land", which is exactly how the Kotlin sequencer sees Kotlin's edits. Editing here simply
    // removes the serialize → parse round trip from the path, so a cursor keystroke on a handheld does
    // not re-encode ~440 KB of JSON to move one byte.
    //
    // Two obligations come with the reference, both of them the same ones the Android path has:
    //   • after an edit WHILE PLAYING → notify_data_changed(), or the change is not heard until the
    //     lookahead happens to pass it;
    //   • after editing a TABLE → invalidate_tables(), because the consumer caches which tables it has
    //     already pushed to the engine (push_project does this for you; an in-place edit cannot).
    Project& edit_project() { return project_; }
    void     invalidate_tables() { consumer_.invalidate_tables(); }

    // ── ↑ live-edit reaction ─────────────────────────────────────────────────────────────────────
    // Roll the lookahead back to the earliest unplayed phrase boundary and drop the notes already
    // queued past it, so an edit is heard on the next phrase loop. The Sequencer computes the
    // boundary; clearing the queue is the host's job because only the host holds the engine.
    void notify_data_changed() {
        sync_clock();
        apply_rollback(seq_.notify_data_changed(seq_.clock()));
    }

    // ── ↕ LIVE mode ──────────────────────────────────────────────────────────────────────────────
    //
    // Queue-and-launch. Each verb arms a slot and rewinds the track it belongs to so the launch can
    // still land on the boundary it was aimed at — the scheduler runs two phrases ahead, so without
    // the rewind a launch aimed at a boundary already inside the buffer would arrive a lap late.
    // ⚠️ Dropping the notes past that frame is the HOST's half, exactly as it is for a live edit:
    // only the host holds the engine.

    bool               live_mode() const              { return seq_.live_mode(); }
    songcore::LiveSlot live_queue(int track) const    { return seq_.live_queue(track); }
    bool               live_silent(int track) const   { return seq_.live_silent(track); }

    void set_live_mode(bool on)                       { sync_clock(); apply_rollback(seq_.set_live_mode(on, seq_.clock())); }
    void queue_live(int track, int songRow, bool now) { sync_clock(); apply_rollback(seq_.queue_live(track, songRow, now, seq_.clock())); }
    void queue_live_stop(int track, bool now)         { sync_clock(); apply_rollback(seq_.queue_live_stop(track, now, seq_.clock())); }
    void queue_live_row(int songRow, bool now)        { sync_clock(); apply_rollback(seq_.queue_live_row(songRow, now, seq_.clock())); }

    // ── ↕ the note preview — "hear the note you just dialled in" ──────────────────────────────────
    //
    // The C++ twin of AudioEngine.previewNoteWithTimeout. It plays on the DEDICATED PREVIEW LANE
    // (AudioEngine::PREVIEW_LANE == track 8, the ninth voice), which is why auditioning a note while
    // a song is playing steals nothing: the eight song tracks are untouched.
    //
    // ⚠️ It goes through `plan_note_on` — the same derivation the sequencer's own notes take, the one
    // `tools/ptvoice` goldens against the real Kotlin `AudioEngine.scheduleNote`. Hand-rolling the
    // engine calls here (Kotlin does, and its sampler branch has quietly drifted from its own
    // scheduleNote as a result) would mean a second, unmeasured copy of the note path — the exact
    // thing S5 ported the consumer to avoid. The payload below is a note with NO phrase behind it:
    // no FX, no transpose, velocity −1, the instrument's own volume and pan, and `tableId = -1`,
    // which derive_sampler_note resolves to the instrument's own table — exactly what Kotlin passes.
    //
    // ⚠️ **AND IT GOES TO THE CABLE (MIDI plan B5), which `plan_note_on` alone cannot do.** An
    // EXTERNAL instrument has no voice to raise, so an audition that only ever called the engine was
    // silent by construction — the one screen where you pick the instrument type was the one place
    // you could not hear the choice. The event is handed to `external_` DIRECTLY rather than through
    // `router_`, and that is deliberate: the router also feeds the trace writer, whose 36 goldens have
    // never contained a preview, and the engine consumer, which cannot carry this event's
    // `rootAudition` flag or its fresh table cache (voice_derive.h says so — a preview is not a bus
    // event and the ratified schema has no field for either). The routing VERDICT is still the one
    // model predicate both consumers ask, so the three cannot disagree about who owns a note.
    //
    // `durationFrames <= 0` means NO TIMED KILL — the voice rings out on its own (endlessly, for a
    // sustaining SoundFont preset) until stop_preview(). That is the instrument audition's contract,
    // not an edge case: an audition of a pad that dies after a 16th note tells you nothing about it.
    //
    // `tableIdOverride < 0` means "the instrument's own table" (which is what derive_sampler_note
    // resolves −1 to). The TABLE screen passes the table it is SHOWING instead, so that auditioning
    // from there plays the automation you are looking at.
    void preview_note(int instrumentId, const Note& note, int64_t durationFrames,
                      bool rootAudition = false, int tableIdOverride = -1) {
        if (!engine_) return;
        if (note == Note::EMPTY()) return;   // Kotlin's first line, and it matters: A on an empty
                                             // cell that inserts nothing must not thump the lane
        if (instrumentId < 0 || instrumentId >= static_cast<int>(project_.instruments.size())) return;
        const Instrument& ins = project_.instruments[instrumentId];
        const bool external = instrument_routes_external(ins);

        engine_->requestResume();
        const int64_t frame = engine_->getCurrentFrame() + 100;  // Kotlin's +100-frame lead-in

        Event ev{};
        ev.type       = EV_NOTE_ON;
        ev.frame      = frame;
        ev.track      = AudioEngine::PREVIEW_LANE;
        ev.instrument = instrumentId;

        NoteOnPayload& n = ev.noteOn;
        n.note        = static_cast<uint8_t>(note_to_midi(note));
        // ⚠️ **The three velocity fields are wired DIFFERENTLY per destination, and the difference is
        // a fix rather than a shortcut.** The engine's wiring is Kotlin's historically-crossed one
        // (scheduler.h says so): the instrument's own VOL arrives as the velocity-curve gain
        // `velGain`, with `phraseVol` at 1.0. `midi_velocity` (midi_out.h) is written against the
        // SCHEDULER's meaning of the same fields — it reads `velocity == -1` as "derive from velGain,
        // which the scheduler built as (V/127)²" and takes the square root. Handed a raw VOL that
        // comes out sqrt-BOOSTED: VOL 0x80 previewed at velocity 90, where the very same instrument
        // SEQUENCED at V=7F sends 64. So the wire's copy states what it means — a preview has no V
        // column, hence full velocity, scaled by VOL in the field that scales by VOL.
        n.velocity    = external ? 127 : -1;
        n.velGainBits = f32_bits(external ? 1.0f : hex_to_float(ins.volume));   // seam arg `volume`
        n.volGainBits = f32_bits(external ? hex_to_float(ins.volume) : 1.0f);   // seam arg `phraseVol`
        n.panBits     = f32_bits(hex_to_float(ins.pan));
        n.start = -1; n.slice = -1; n.tableId = tableIdOverride; n.tableRow = -1;
        n.transpose = 0; n.pit = 0; n.arp = 0;
        n.pslOffBits = n.pslDurBits = n.pbnRateBits = n.vibSpdBits = n.vibDepBits = f32_bits(0.0f);

        // ⚠️ UNCONDITIONALLY, whichever way this instrument routes — and that is the whole of the
        // preview lane's note lifecycle. `ExternalConsumer::consume` answers the gate itself, and its
        // INTERNAL arm is the one that matters here: a note-on for a non-external instrument on a lane
        // that was last EXTERNAL ends the note the cable is still holding. Without it, auditioning an
        // external instrument and then a sampler one — two STARTs, and START is exempt from
        // `on_stop_preview` (button_mapper.h) — leaves a note sounding on the gear with nothing left
        // in the app that resolves to it. On a lane that was never external this costs one lookup.
        external_.consume(ev);

        if (external) {
            // A timed audition owes the cable its own note-off; the engine's half of the same thing is
            // the `scheduleKill` below. `end_note` takes the MIN of this frame and the instrument's
            // LEN gate, so a LEN shorter than the preview still wins. `durationFrames <= 0` is the
            // ring-out contract — an instrument audition — and `stop_preview()` is what ends that.
            if (durationFrames > 0) preview_note_off(frame + durationFrames);
            return;   // …and NO voice, cable or no cable: EXTERNAL means "not this engine"
        }

        // A fresh cache every preview, so an edit to the instrument's table is heard immediately —
        // Kotlin calls forceReloadTable here for the same reason.
        bool tableLoaded[POOL_TABLES] = {false};
        plan_note_on(*engine_, ev, project_, routing_, tableLoaded, rootAudition);

        if (durationFrames > 0) engine_->scheduleKill(frame + durationFrames, AudioEngine::PREVIEW_LANE);
    }

    /**
     * Audition an instrument at its own ROOT — what START does on INSTRUMENT / INST_POOL / MODS, and
     * (with a table override) on TABLE.
     *
     * Two things separate it from the phrase preview above, and both are Kotlin's
     * `AudioEngine.previewInstrument`:
     *   • it RINGS OUT (no timed kill) until the next plain button press stops it — you are listening
     *     to an instrument, not to a step;
     *   • it is a ROOT AUDITION, which the SoundFont path must be told about or ROOT does nothing (the
     *     note IS the root, so the usual 60 − root transpose would cancel it to a flat C-4 every time).
     */
    void preview_instrument(int instrumentId, int tableIdOverride = -1) {
        if (instrumentId < 0 || instrumentId >= static_cast<int>(project_.instruments.size())) return;
        preview_note(instrumentId, project_.instruments[instrumentId].root, /*durationFrames=*/0,
                     /*rootAudition=*/true, tableIdOverride);
    }

    /**
     * Point the preview lane at a mixer channel — `trackId` in 0..7, or −1 for the unity gain it has
     * always had.
     *
     * ⭐ THE TRACK, NOT THE VOLUME. A stored gain goes stale the moment the fader moves or a VTR
     * lands; a stored index re-reads the live fader every block, for nothing. The lane keeps its own
     * ninth voice either way — an audition over a running song must go on stealing nothing.
     */
    void set_preview_track(int trackId) {
        if (engine_) engine_->setPreviewTrack(trackId);
    }

    /**
     * Silence the audition lane. Backs the "press any button to stop the preview" gesture.
     *
     * ⚠️ Both halves, and the second one is not symmetry for its own sake: an EXTERNAL audition with
     * `midiLen == 0` is gate-to-next and there is no next note, so THIS is the only thing that ends
     * it. An external synth has no voice allocator to save us — a note-on we fail to answer sounds
     * until the gear is power-cycled. On a lane holding no external note the consumer's `active_` flag
     * says so and it costs a branch.
     */
    void stop_preview() {
        if (!engine_) return;
        const int64_t now = engine_->getCurrentFrame();
        engine_->scheduleKill(now, AudioEngine::PREVIEW_LANE);
        preview_note_off(now);
        // ⚠️ AN AUDITIONED TABLE CAN CARRY AN EQM, and the master bus outlives the lane that moved it.
        // The TABLE screen's START runs the table on the screen through the preview lane, so this is
        // the only end that audition has — without the restore here, auditioning a table with an EQM
        // on it leaves the master EQ on that preset with no transport stop to put it back.
        //
        // ⚠️ ONLY WHILE THE TRANSPORT IS IDLE. Playing, the latch belongs to stop(): consuming it here
        // would disarm the restore the SONG's own table rows are counting on, and the bus would keep
        // the last EQM for good.
        if (!seq_.is_playing() && seq_.has_live_project() && engine_->takeTableMasterEqTouched())
            engine_->setMasterEqSlot(project_.masterEqSlot);
    }

    // ── ↑ feedback ───────────────────────────────────────────────────────────────────────────────

    // Where ONE track's playhead is — the only form there is. In SONG mode the eight run
    // independently, so a single whole-song answer would be one track's number wearing the song's
    // name; the UI asks eight times, once per marker it might draw (ui/playhead.h).
    PlaybackPosition playheads(int trackId) {
        sync_clock();
        return seq_.getPlaybackPosition(trackId);
    }

    bool is_playing() const { return seq_.is_playing(); }

    /** The device rate the sequencer is running at — the note preview needs it for framesPerStep. */
    int sample_rate() const { return sampleRate_; }

    // ── ↑ debug: the conformance trace (event-schema §6) ─────────────────────────────────────────
    // Same output contract as the Kotlin EventTrace tap — same path, same bytes — so the S1 device
    // cross-check procedure compares a C++-engine trace against the goldens without changing a step.
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
    // Drop what the rolled-back tracks had already queued. ⚠️ ONE FRAME PER TRACK, not one for the
    // engine: the eight song cursors have their own boundaries, so clearing every track from the
    // earliest of them would drop notes a track ahead of it had queued and is not going to schedule
    // again. Written once below its four callers — the live edit and the three LIVE verbs, which are
    // the same motion for different reasons.
    void apply_rollback(const songcore::RollbackPlan& plan) {
        if (!engine_) return;
        for (int t = 0; t < 8; ++t)
            if (plan.frames[t] >= 0) engine_->clearScheduledNotesFrom(plan.frames[t], t);
    }

    // songcore owns no clock: the engine's frame counter IS the transport clock (getCurrentFrame() is
    // what the Kotlin scheduler polled). With no engine the clock stays where a test put it.
    void sync_clock() {
        if (!engine_) return;
        seq_.set_clock(engine_->getCurrentFrame());
        int sr = engine_->getSampleRate();
        if (sr > 0) sampleRate_ = sr;
        seq_.set_sample_rate(sampleRate_);
    }

    int64_t after_play() {
        flush_trace();
        return seq_.playback_start_frame();
    }

    /**
     * ⭐⭐ **PHASE E4 — ONE BUS RECORD FROM A LIVE KEY, HANDED TO THE CONSUMERS THAT OWN IT.**
     *
     * ── WHY NOT `router_` ────────────────────────────────────────────────────────────────────────
     *
     * The obvious move is `router_.note_on(...)` and let the bus fan out. It is wrong here, for three
     * reasons, and B5 already answered the same question the same way for the preview:
     *
     *   1. ⚠️ **THE BUS'S ORDERING INVARIANT.** `TrackInstruments` (router.h) assumes events arrive per
     *      track in NON-DECREASING frame order, which the sequencer guarantees because it schedules
     *      forward. A live key is stamped `clock + 100` while the lookahead has already emitted records
     *      up to two phrases into the future — so a key pressed mid-song would arrive *behind* records
     *      already dispatched. The consumers each keep their own map and are handed the record
     *      directly, which is the only arrangement in which that is harmless.
     *   2. **THE TRACE.** `writer_` is a bus consumer and the 36 goldens have never contained a live
     *      event. A key press is not part of the song, exactly as a preview is not.
     *   3. There is nothing to gain: the two consumers below ARE the bus's audio subscribers.
     *
     * ── WHAT EACH CONSUMER IS ASKED ──────────────────────────────────────────────────────────────
     *
     * `consume` is the ONE DOOR on both (B5's `preview_note_off` says why): each answers the routing
     * gate itself, resolves the record's owner through its own `TrackInstruments`, and honours the LEN
     * gate — so a live key and a sequenced note cannot disagree about who owns a track.
     *
     *   • the ENGINE consumer, **unconditionally and whichever way the instrument routes**. Its gate
     *     drops EXTERNAL records, and its internal→external arm is the one that ends a voice a flip
     *     would otherwise leave sounding.
     *   • the CABLE, only when THRU is on — see `set_midi_in_thru`. A suppressed record is COUNTED,
     *     because "the key is silent" needs to name which of the reasons it was.
     */
    void inject(const Event& ev) {
        ++midiInInjected_;

        // ⚠️ **THE PAUSED-STREAM RESUME IS NOT MISSING HERE — IT IS ONE LAYER DOWN.** With the transport
        // stopped the audio stream may be paused, and a note scheduled into a paused engine is a note
        // nobody hears at a frame counter that is not moving. `plan_note_on` calls `requestResume()`
        // immediately before both of its schedule calls (voice_derive.h), so every path that raises a
        // voice already resumes; a second call here would be a second, unmeasured copy of that rule.
        // (`preview_note` above does have its own — it predates the seam and is Kotlin's line for
        // Kotlin's reason. This one does not need it.)

        // ⚠️ Asked BEFORE either consume, and only to COUNT: `ExternalConsumer::consume` learns from a
        // note-on, so a verdict taken afterwards would be answering about the state this record just
        // created rather than the one it arrived into.
        const bool external = record_is_external(ev);

        consumer_.consume(ev);

        if (midiInThru_) {
            external_.consume(ev);
            if (external) ++midiInThruSent_;
        } else if (external) {
            ++midiInThruSuppressed_;
        }
    }

    /**
     * Would this record have gone out on the cable? Asked ONLY to count a suppression — the consumers
     * never ask it, they answer it themselves.
     *
     * ⚠️ A note-on carries its instrument; every other record on the bus is track-scoped and rides
     * `INSTRUMENT_NONE`, so the owner comes from the same `TrackInstruments` the cable's own consumer
     * keeps. A second opinion about track ownership is the one thing this file must not invent.
     */
    bool record_is_external(const Event& ev) const {
        int16_t instrument = ev.instrument;
        if (instrument == INSTRUMENT_NONE) instrument = external_.track_instruments().current(ev.track);
        if (instrument < 0 || static_cast<size_t>(instrument) >= project_.instruments.size()) return false;
        return instrument_routes_external(project_.instruments[static_cast<size_t>(instrument)]);
    }

    /**
     * The preview lane's note-off on the CABLE (MIDI plan B5) — the timed audition's end, and what
     * `stop_preview` sends.
     *
     * It is a bus record rather than a call into the consumer's internals on purpose: `consume` is the
     * one door, so the note-off answers the SAME routing gate the note-on did, resolves the lane's
     * instrument through the SAME `TrackInstruments`, and honours the LEN gate through the same `min`.
     * A private back door would be a second opinion about who owns the note.
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

    // Drain the writer's buffer to disk after each verb: a long session can't grow unbounded in RAM,
    // and a crash mid-take still leaves the trace up to the last poll on disk.
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
    std::string appRoot_;   // set_app_root(); "" ⇒ no media re-rooting (the tools' default)
    Routing routing_;

    /**
     * The RATE row's ratio cache (sample_edit.h). Editor-session state, not project state — it exists
     * only so LOFI → HIGH restores the ratio the FILE was loaded with instead of compounding factors,
     * and it is meaningless the moment the sample is resampled.
     */
    RateCache rateCache_;

    MidiRouter       router_;
    Sequencer        seq_;
    EngineConsumer   consumer_;
    ExternalConsumer external_;
    bool             midiPumpExternal_ = false;   // B3: a sender thread owns the release, not poll()

    // MIDI in (E2). The queue is the only member here another thread ever touches, and it locks.
    // Kotlin's `+100` preview lead-in, in frames — see the note in `poll()`.
    static constexpr int64_t MIDI_IN_LEAD_FRAMES = 100;
    MidiInQueue       midiInQueue_;
    MidiParser        midiInParser_;
    MidiInputRouter   midiInRouter_;
    IMidiInObserver*  midiInObserver_ = nullptr;
    uint64_t          midiInBytes_ = 0, midiInMessages_ = 0;
    // E4: the injection. `midiInThru_` defaults to the FEATURE — see set_midi_in_thru.
    bool              midiInThru_ = true;
    uint64_t          midiInInjected_ = 0, midiInThruSent_ = 0, midiInThruSuppressed_ = 0;

    TraceWriter   writer_;
    std::string   traceBuf_;
    std::ofstream traceFile_;
    bool          traceEnabled_ = false;
};

}  // namespace songcore

#endif  // POCKETTRACKER_SONGCORE_HOST_H
