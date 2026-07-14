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
    // (engine_setup.h). Android does this in Kotlin — its loader also drives MediaCodec for m4a — and
    // pushes the result in via push_routing() instead.
    //
    // ⚠️ It also WRITES to the project: a WAV's `cue ` chunk is where its slice boundaries live, and
    // loading the audio is what learns them (S6b). See load_project_media.
    MediaLoadResult load_media(const std::string& baseDir) {
        if (!engine_) return MediaLoadResult();
        return load_project_media(*engine_, project_, baseDir, routing_);
    }

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
        push_mixer(*engine_, project_);
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
    // directly (project_io.h has always parsed a blob; the engine has always opened a .wav), and the
    // `ui::FileSystem` abstraction exists for the BROWSER — for listing, sorting, renaming — which is a
    // UI concern. Two layers, each doing its own job.

    /** Replace the project from a .ptp on disk: parse → push → load its media → push its params. */
    bool load_project_file(const std::string& path, const std::string& baseDir) {
        std::ifstream f(path, std::ios::binary);
        if (!f) return false;
        std::string blob((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        if (!push_project(blob)) return false;

        // ⚠️ The three calls that must follow a push, in this order, or the project you loaded is not
        // the project you hear. load_media opens the files and learns the Routing; push_params pushes
        // everything the ENGINE holds that no note carries. Phase 3 S4 is the session that found out
        // what happens when the second one is missing (84.4% of the render's bytes differed).
        load_media(baseDir);
        push_params();
        return true;
    }

    /** Serialize the live project to a .ptp. The bytes are kotlinx-exact (project_io.h, S2). */
    bool save_project_file(const std::string& path) const {
        const std::string blob = serialize_project(project_);
        std::ofstream f(path, std::ios::binary | std::ios::trunc);
        if (!f) return false;
        f.write(blob.data(), static_cast<std::streamsize>(blob.size()));
        return f.good();
    }

    // ── PROJECT screen: NEW, and the two COMPACTs ────────────────────────────────────────────────
    //
    // The document surgery itself is pure and lives in project_ops.h. What lands HERE is the half
    // that has to tell the ENGINE, and it is the whole reason these are host verbs rather than three
    // free functions the dispatcher could call: the engine holds state that the document does not,
    // and every one of these invalidates some of it.

    /** PROJECT → NEW. A blank document, and an engine that has forgotten the last one. */
    void new_project() {
        stop();   // before the samples go: no voice may be reading PCM we are about to free
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

    /** Write instrument `id` (and its table, if it has content) as a .pti. */
    bool save_instrument_preset(int id, const std::string& path) const {
        const InstrumentPreset ip = make_instrument_preset(project_, id);
        const std::string blob = serialize_instrument_preset(ip);
        std::ofstream f(path, std::ios::binary | std::ios::trunc);
        if (!f) return false;
        f.write(blob.data(), static_cast<std::streamsize>(blob.size()));
        return f.good();
    }

    /**
     * Read a .pti into instrument `id`. False if the file will not parse, OR if its source file is
     * gone — but the PARAMETERS land either way in the second case, which is deliberate (a preset
     * whose sample has moved should still give you back its filter and its mod slots).
     */
    bool load_instrument_preset(int id, const std::string& path) {
        std::ifstream f(path, std::ios::binary);
        if (!f) return false;
        std::string blob((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());

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

    /** The nearest zero crossing to `frame` in direction `dir` (−1 back, +1 forward, 0 either). */
    int find_zero_crossing(int id, int frame, int dir) const {
        return engine_ ? engine_->findZeroCrossing(id, frame, dir) : frame;
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
     * ⚠️ It TEMPORARILY mutates the instrument (`sampleStart` / `sampleEnd`, and `sampleId` when a
     * scratch slot is in play), because that is the only channel the engine has for a voice's window.
     * The caller MUST call `finish_sample_preview()` once the voice has triggered, or the project keeps
     * the preview's window as if the user had dialled it in. That is why the dispatcher carries a
     * deadline rather than restoring immediately: the engine reads those fields when the note FIRES,
     * 100 frames later, not when it is scheduled.
     */
    void preview_sample_editor(int id, int sourceMode, int64_t selStart, int64_t selEnd,
                               int totalFrames, int pitchSemitones) {
        if (!engine_ || id < 0 || id >= POOL_INSTRUMENTS) return;
        Instrument& ins = project_.instruments[static_cast<size_t>(id)];

        const Note savedRoot = ins.root;
        if (totalFrames > 0 && selEnd > selStart) {
            ins.sampleStart = static_cast<int>(std::clamp<int64_t>((selStart * 255) / totalFrames, 0, 255));
            ins.sampleEnd   = static_cast<int>(std::clamp<int64_t>((selEnd   * 255) / totalFrames, 0, 255));
        }
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
        preview_instrument_dry(*engine_, ins, slot, routing_.sampleRateRatio[id]);
        if (slot != id) ins.sampleId = savedSampleId;

        ins.root = savedRoot;   // the root is read at SCHEDULE time, so it can go back at once
    }

    /**
     * Put the instrument back: its real sample window, its EQ, its sends, its modulation. Runs on the
     * dispatcher's 100 ms deadline, and IMMEDIATELY if a second START arrives inside that window —
     * otherwise the second preview would save the FIRST preview's window as the "real" one.
     */
    void finish_sample_preview(int id, int savedStart, int savedEnd) {
        if (!engine_ || id < 0 || id >= POOL_INSTRUMENTS) return;
        Instrument& ins = project_.instruments[static_cast<size_t>(id)];
        ins.sampleStart = savedStart;
        ins.sampleEnd   = savedEnd;
        push_instrument_playback_params(*engine_, ins);
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
        int64_t rollbackFrame = seq_.notify_data_changed(seq_.clock());
        if (rollbackFrame >= 0 && engine_) engine_->clearScheduledNotesFrom(rollbackFrame);
    }

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

        engine_->requestResume();
        const int64_t frame = engine_->getCurrentFrame() + 100;  // Kotlin's +100-frame lead-in

        Event ev{};
        ev.type       = EV_NOTE_ON;
        ev.frame      = frame;
        ev.track      = AudioEngine::PREVIEW_LANE;
        ev.instrument = instrumentId;

        NoteOnPayload& n = ev.noteOn;
        n.note        = static_cast<uint8_t>(note_to_midi(note));
        n.velocity    = -1;                                        // no V column behind a preview
        n.velGainBits = f32_bits(hex_to_float(ins.volume));        // seam arg `volume`
        n.volGainBits = f32_bits(1.0f);                            // seam arg `phraseVol`
        n.panBits     = f32_bits(hex_to_float(ins.pan));
        n.start = -1; n.slice = -1; n.tableId = tableIdOverride; n.tableRow = -1;
        n.transpose = 0; n.pit = 0; n.arp = 0;
        n.pslOffBits = n.pslDurBits = n.pbnRateBits = n.vibSpdBits = n.vibDepBits = f32_bits(0.0f);

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

    /** Silence the audition lane. Backs the "press any button to stop the preview" gesture. */
    void stop_preview() {
        if (!engine_) return;
        engine_->scheduleKill(engine_->getCurrentFrame(), AudioEngine::PREVIEW_LANE);
    }

    // ── ↑ feedback ───────────────────────────────────────────────────────────────────────────────
    PlaybackPosition playheads() {
        sync_clock();
        return seq_.getPlaybackPosition();
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

    /**
     * The RATE row's ratio cache (sample_edit.h). Editor-session state, not project state — it exists
     * only so LOFI → HIGH restores the ratio the FILE was loaded with instead of compounding factors,
     * and it is meaningless the moment the sample is resampled.
     */
    RateCache rateCache_;

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
