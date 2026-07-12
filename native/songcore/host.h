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
#include "engine_consumer.h"
#include "engine_setup.h"
#include "model.h"
#include "project_io.h"
#include "render.h"
#include "router.h"
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
          consumer_(engine, &project_, &routing_) {
        // The engine consumer is the bus's permanent subscriber: events become audio. The trace
        // writer joins it only while tracing (set_trace), and sees the identical records.
        if (engine_) router_.add_consumer(&consumer_);
    }

    ~SongcoreHost() { set_trace(false, ""); }

    MidiRouter& router() { return router_; }
    Sequencer&  sequencer() { return seq_; }
    const Project& project() const { return project_; }

    // ── ↓ data ───────────────────────────────────────────────────────────────────────────────────
    // The blob is the .ptp JSON verbatim — the bytes FileController.serializeProject() produces, which
    // S2 proved this parser reads byte-exactly. Pushing REPLACES the project in place: `project_` never
    // changes address, so the Sequencer's pointer into it stays valid even mid-playback (the Kotlin
    // sequencer likewise reads the one live Project object, seeing edits as they land).
    //
    // Whole-project push, not the §7 per-item diffs: the parser already exists and is proven, and a
    // wrong diff is a silent desync. The cost (a ~440 KB blob for a full pool) is paid on play and on
    // edit-while-playing; if that hitches on device, the diff verbs are the documented next step.
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
    int64_t play_song(int startRow)   { sync_clock(); seq_.playSong(startRow);     return after_play(); }
    int64_t play_chain(int chainId)   { sync_clock(); seq_.playChain(chainId);     return after_play(); }
    int64_t play_phrase(int phraseId) { sync_clock(); seq_.playPhrase(phraseId);   return after_play(); }

    void stop() {
        // Mirrors PlaybackController.stop() ordering: restore the master EQ (undoing a transient EQM
        // override) BEFORE the queues are cleared, and only when a live project is set — the render
        // path leaves currentProject_ null and restores its own master EQ in RenderController.
        if (engine_ && seq_.eqm_active() && seq_.has_live_project()) {
            engine_->setMasterEqSlot(project_.masterEqSlot);
        }
        sync_clock();
        seq_.stop();
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
        seq_.updatePlaybackBuffer();
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
    // (engine_setup.h). Android does this in Kotlin — its loader also drives MediaCodec for m4a and
    // reads WAV cue points — and pushes the result in via push_routing() instead.
    MediaLoadResult load_media(const std::string& baseDir) {
        if (!engine_) return MediaLoadResult();
        return load_project_media(*engine_, project_, baseDir, routing_);
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
        int64_t rollbackFrame = seq_.notify_data_changed(seq_.clock());
        if (rollbackFrame >= 0 && engine_) engine_->clearScheduledNotesFrom(rollbackFrame);
    }

    // ── ↑ feedback ───────────────────────────────────────────────────────────────────────────────
    PlaybackPosition playheads() {
        sync_clock();
        return seq_.getPlaybackPosition();
    }

    bool is_playing() const { return seq_.is_playing(); }

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

    Project project_ = make_default_project();
    std::string projectSha_ = "-";
    Routing routing_;

    MidiRouter     router_;
    Sequencer      seq_;
    EngineConsumer consumer_;

    TraceWriter   writer_;
    std::string   traceBuf_;
    std::ofstream traceFile_;
    bool          traceEnabled_ = false;
};

}  // namespace songcore

#endif  // POCKETTRACKER_SONGCORE_HOST_H
