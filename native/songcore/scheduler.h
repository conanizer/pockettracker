#ifndef POCKETTRACKER_SONGCORE_SCHEDULER_H
#define POCKETTRACKER_SONGCORE_SCHEDULER_H

// ─── The sequencer spine ─────────────────────────────────────────────────────────────────────────
//
// A 1:1 port of core/logic/PlaybackController.kt (+ its TrackState) — the "doomed" Kotlin
// sequencing zone rewritten as C++ songcore (linux-port-plan §4.3, order-of-work zone C). It walks
// the project by transport position exactly as the Kotlin scheduler does — grooves, HOP, RPT/ARP
// grids, LAT, KIL, pitch mods, per-note/mixer FX — and emits the identical event stream through the
// MidiRouter seam (router.h), which the trace writer serializes for the byte-for-byte conformance
// check against /tools/testdata/traces (event-schema §6).
//
// PlaybackController.kt is the executable spec; every method, branch, and float expression here
// mirrors it, including the historically-crossed velGain/volGain wiring and the intentional groove
// rounding drift. The floats are computed as binary32 with the SAME operation order as Kotlin's `Xf`
// literals, so the raw-bits trace fields reproduce (S3 already proved the shared arithmetic bitwise;
// tools/ptplay proves the whole spine).
//
// S4 shipped the event-emitting spine alone. S5 adds the three pieces a LIVE app needs that carry no
// bus event and therefore no golden (event-schema §5 / SC-4) — they are pure SIDE-RECORDS kept
// alongside the walk, and the proof that they stay side-records is that tools/ptplay must remain
// byte-green on all 32 traces with them in:
//   * getPlaybackPosition() + its chainRowStartFrames / songPositionStartFrames maps — the UI cursor;
//   * the scheduling-checkpoint ring + notify_data_changed() rollback — the live-edit reaction
//     (SC-2: only the POSITION rolls back, never TrackState — the state smear is today's behavior);
//   * eqm_active() — setMasterEqSlot is not a bus event, so the master-EQ restore on stop() is the
//     host's job; the flag tells it whether an EQM ran (PlaybackController.eqmActive).
//   * mixer_vol_active() — the same shape for VTR/VMV, which REPLACE the mixer faders and hold. The
//     CCs themselves ARE bus events and are goldened; what is not an event is putting the faders back.
// Random FX (CHA/RND/RNL/ARP-RANDOM) are excluded from the goldens (SC-1) — a stream seeded from the
// wall clock has no byte-comparable golden, on either engine. They are therefore the one part of the
// spine measured statistically instead: rng.h holds the generator and the reasoning, and
// tools/ptrandom checks its distributions against the real Kotlin sequencer's (S7).

#include <algorithm>
#include <cstdint>
#include <deque>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>
#include "model.h"
#include "timing.h"
#include "effects.h"
#include "automation.h"
#include "rng.h"
#include "router.h"
#include "scales.h"      // the quantizer emit_note() puts every scheduled note through
#include "traversal.h"   // chain_at / phrase_at — the ids a playhead is IN, not just its row numbers

namespace songcore {

// note_to_midi / note_from_midi moved to model.h — they are Note's own arithmetic (TrackerData.Note),
// not scheduling, and the UI needs them without pulling the whole sequencer in.

// ─── small helpers ───────────────────────────────────────────────────────────────────────────────
inline int   clampi(int v, int lo, int hi)       { return v < lo ? lo : (v > hi ? hi : v); }
inline float clampf(float v, float lo, float hi)  { return v < lo ? lo : (v > hi ? hi : v); }
// hex_to_float MOVED to model.h (MIDI phase E) — the MIDI-in router needs it and has no business
// including the sequencer, exactly as note_to_midi moved for the UI. Callers here are unchanged.

// `step_fx_type` / `step_fx_value` / `step_set_fx` / `step_set_fx_value` / `step_empty` live in
// model.h beside PhraseStep, and `chain_is_empty` with them — a step's slot indexing and a chain
// row's emptiness are both read by layers that have no business including the sequencer. See the
// notes there.

enum class PlaybackMode { STOPPED, PHRASE, CHAIN, SONG };

// ─── UI cursor feedback (SC-4 — never goldened) ──────────────────────────────────────────────────
//
// Where ONE track is. There is no whole-song answer: the eight song cursors run independently, so a
// caller names the track it is asking about.
//
// ⚠️ **EVERY FIELD IS −1 WHEN THERE IS NO ANSWER, AND −1 IS NOT ROW 0.** A phrase auditioned on its
// own is in no chain and in no song; a track whose song column has run out has stopped. Filling
// those with zeros is what put a frozen playhead on row 0 of CHAIN and SONG while a PHRASE played,
// and any consumer that draws a marker on a zero will do it again.
//
// ⚠️ **THE IDS ARE PART OF THE POSITION.** A chain row means nothing without the chain it is a row
// of: the CHAIN screen shows one chain and two tracks may be inside it at two different rows while
// a third is inside a chain the screen is not showing. Same for a phrase step.
//
// `row` doubles as the phrase step in every mode (that is how PlaybackController filled it).
struct PlaybackPosition {
    int row = -1;
    int chainRow = -1;
    int phraseStep = -1;
    int songRow = -1;
    int chainId = -1;    // the chain `chainRow` is a row OF
    int phraseId = -1;   // the phrase `phraseStep` is a step OF
};

// Where one track is in the song, as the scheduler queued it. ⚠️ The TRACK is part of the key now:
// with independent cursors "the song is on row 5" is not a fact anybody can state.
struct SongPos {
    int track = 0;
    int songRow = 0;
    int chainRow = 0;
};

// What notify_data_changed() asks the host to drop, per track: the frame that track's lookahead was
// rolled back to, or −1 for "this one has nothing queued past now". ⚠️ A single frame cannot express
// this once the eight cursors are independent — see notify_data_changed.
struct RollbackPlan {
    int64_t frames[8] = {-1, -1, -1, -1, -1, -1, -1, -1};
};

// ─── LIVE mode: what one channel is waiting to do ───────────────────────────────────────────────
//
// LIVE is a MODIFIER ON SONG, not a fifth PlaybackMode: the scheduler runs its SONG arm and only
// what happens at a track's boundary changes. Everything that branches on `playbackMode_` — the
// playhead readback, the live-edit rollback's one-track test, the trace writer and the 36
// byte-compared goldens — therefore needs no arm for a mode it has no answer for, and a project that
// never enters LIVE produces the identical schedule.
//
// ⚠️ `targetRow < 0 && !stop` is the empty slot. A stop queue carries no row, which is why "nothing
// queued" cannot be expressed by the row alone.
//
// ⚠️⚠️ **A SLOT IS SCHEDULED LONG BEFORE IT IS HEARD, AND THE SCREEN ANSWERS TO THE SECOND.** The
// lookahead runs two phrases ahead, so a queue consumed by the walk has up to two phrases still to
// play before anything changes — and clearing the slot on consumption made the blinking marker go
// out a bar or two early, while the player was still waiting for the launch they could see coming.
// `firesAt` is the frame it lands on: the SCHEDULER treats the slot as spent the moment it is set,
// and the DISPLAY keeps showing it until the transport reaches it.
struct LiveSlot {
    int     targetRow = -1;      // the song row to launch on this channel
    bool    stop      = false;   // …or silence it instead
    bool    immediate = false;   // at the next PHRASE boundary rather than the next CHAIN boundary
    int64_t firesAt   = -1;      // the frame it was scheduled to land on; −1 = still waiting
    // ⭐ There is deliberately NO "not before frame X" here. A rewind can only ever land on a
    // boundary PAST the press (rewind_song_track takes the earliest checkpoint with `frame >
    // currentFrame`), so "the launch cannot land at or before the moment it was queued" is already
    // true by construction — and a second copy of it, keyed on the lap ORIGIN, put the launch a full
    // lap late whenever the rewound cursor had already crossed into the next lap.

    /** Something is queued here at all — the question a MARKER asks. */
    bool pending() const { return targetRow >= 0 || stop; }
    /** …and the walk has not spent it yet — the question the SCHEDULER asks. */
    bool armed() const { return pending() && firesAt < 0; }
};

// ─── Per-track persistent effect state (TrackState) ─────────────────────────────────────────────
struct TrackState {
    Note  lastNote = Note::EMPTY();
    int   lastInstrument = 0;
    float lastVolume = 1.0f;
    float lastPan = 0.5f;
    int   lastStartPoint = -1;

    int   repeatActiveColumn = 0;
    int   repeatTicInterval = 0;
    int   repeatVolRamp = 0;
    int64_t repeatStartFrame = 0;
    int   repeatRetrigCount = 0;
    float repeatBaseVolume = 1.0f;

    int   arpeggioActiveColumn = 0;
    int   arpeggioValue = 0;
    int   arpeggioMode = 0;
    int   arpeggioSpeed = 4;
    int64_t arpeggioStartFrame = 0;

    int   hopTargetRow = -1;
    bool  trackStopped = false;

    bool  pitchBendActive = false;
    bool  vibratoActive = false;
    int   lastNoteMidi = -1;

    int   lastTableOverride = -1;
    int   lastTableStartRow = -1;

    int   grooveId = 0;
    int   grooveStep = 0;

    // Where SCA / SCG put this track. `scaleKey = -1` is "the project's KEY setting" rather than a
    // key of its own, so a default-constructed TrackState reproduces exactly what the song says — and
    // that is what makes STOP, the render path and a rollback all land back on the song's own scale
    // with no code of their own.
    int   scaleSlot = 0;
    int   scaleKey  = -1;

    int   lastColFxType[4] = {0, 0, 0, 0};   // 1-indexed: [1]=FX1 …
    int   lastColFxValue[4] = {0, 0, 0, 0};

    bool hasActiveRepeat() const { return repeatActiveColumn > 0 && repeatTicInterval > 0; }
    void clearRepeat() {
        repeatActiveColumn = 0; repeatTicInterval = 0; repeatVolRamp = 0;
        repeatStartFrame = 0; repeatRetrigCount = 0; repeatBaseVolume = 1.0f;
    }
    bool hasActiveArpeggio() const { return arpeggioActiveColumn > 0 && arpeggioValue > 0; }
    void clearArpeggio() { arpeggioActiveColumn = 0; arpeggioValue = 0; arpeggioStartFrame = 0; }
    int  consumeHopTarget() { int t = hopTargetRow; hopTargetRow = -1; return t; }
    bool hasPitchMod() const { return pitchBendActive || vibratoActive; }
    void clearPitchMod() { pitchBendActive = false; vibratoActive = false; }
};

// ─── The sequencer ───────────────────────────────────────────────────────────────────────────────
class Sequencer {
  public:
    Sequencer(MidiRouter& router, const Project& project, int sample_rate)
        : router_(router), project_(&project), sampleRate_(sample_rate) {
        // ⚠️ Rng's own default seeding folds a clock read with the address of a function-local static
        // (rng.h). Eight of them constructed in a row share that address and can share the tick, so
        // the array as built may hold EIGHT COPIES OF ONE STREAM — every track's chance gate passing
        // and failing together. Re-derive all eight from track 0's platform draw instead, which keeps
        // the entropy and makes the streams distinct by construction.
        uint64_t hi = rngs_[0].next_u32();   // ⚠️ separate statements: C++ leaves the evaluation
        uint64_t lo = rngs_[0].next_u32();   //    order of two calls in one expression unspecified
        seed_rng((hi << 32) ^ lo);
    }

    // The transport clock — the driver advances it and polls updatePlaybackBuffer(). In the app the
    // driver is the host, which copies the engine's frame counter in (that IS what the Kotlin
    // scheduler polled); in the harness it is TraceHarness's synthetic clock. getCurrentFrame() reads
    // it verbatim either way.
    void set_clock(int64_t f) { currentFrame_ = f; }
    int64_t clock() const { return currentFrame_; }

    // Re-read per verb by the host, mirroring PlaybackController, which asks the backend for the
    // device rate on every poll rather than caching it (a headphone swap can change it mid-session).
    void set_sample_rate(int sr) { if (sr > 0) sampleRate_ = sr; }

    // Pin the random FX (CHA/RND/RNL/ARP-RANDOM) to a known stream. For tools/ptrandom, which must be
    // able to fail reproducibly; the app never calls it, and a fresh Sequencer seeds itself from the
    // platform exactly as kotlin.random.Random.Default does (rng.h).
    //
    // ⚠️ EIGHT STREAMS, one per track, and track 0's is seeded with `s` UNCHANGED — tools/ptrandom
    // drives track 0, so it measures the same stream it has always measured. The offset is the
    // golden ratio in 64 bits, PCG's own stream-selector idiom, so no two tracks walk the same
    // sequence at an offset of each other.
    void seed_rng(uint64_t s) {
        for (int t = 0; t < 8; ++t) rngs_[t].seed(s + static_cast<uint64_t>(t) * 0x9E3779B97F4A7C15ULL);
    }
    int  sample_rate() const { return sampleRate_; }

    // The frame the current session latched at T PLAY — the trace's session base.
    int64_t playback_start_frame() const { return playbackStartFrame_; }

    static constexpr int64_t LOOKAHEAD_MS = 50;
    static constexpr int BUFFER_PHRASES = 2;
    // How many per-track scheduling steps one SONG poll may take. Eight tracks × two buffered
    // phrases is 16 phrases of real work; the rest of the budget is headroom for rows that cost no
    // frames at all (an unauthored row, a spent chain), which are what could otherwise spin.
    static constexpr int SONG_STEPS_PER_POLL = 64;

    bool is_playing() const { return isPlaying_; }
    PlaybackMode playback_mode() const { return playbackMode_; }

    // ── UI cursor + live-edit reaction (S5 side-records — no bus events, SC-4) ──

    // PlaybackController.getPlaybackPosition, verbatim (incl. the tempo fallback: currentProject_ is
    // null on the render path, but so is isPlaying_, so this reads the live tempo in practice).
    //
    // ⚠️ `trackId < 0` means "whichever track's marker is oldest in the window" — one number for a
    // song that no longer has one. Nothing in the app calls it; it survives for the harness, which
    // drives PHRASE mode, where there is only ever one track playing and the question is well posed.
    PlaybackPosition getPlaybackPosition() { return getPlaybackPosition(-1); }

    // Where ONE track is. In PHRASE and CHAIN mode only the track being played has a position and
    // every other track answers −1 across the board — which is the honest answer, and the whole
    // answer: a phrase auditioned on its own is in no chain and in no song, so the CHAIN and SONG
    // screens draw nothing for it rather than freezing a marker on row 0.
    PlaybackPosition getPlaybackPosition(int trackId) {
        PlaybackPosition pos;
        if (!isPlaying_) return pos;
        if (trackId >= 0 && playbackMode_ != PlaybackMode::SONG && trackId != playbackTrack_) return pos;

        int64_t currentFrame = getCurrentFrame();
        int64_t elapsedFrames = currentFrame - playbackStartFrame_;
        int tempo = currentProject_ ? currentProject_->tempo : 120;
        int64_t framesPerStep = frames_per_step(tempo, sampleRate_);
        if (framesPerStep <= 0) return pos;   // unreachable for any legal tempo; a 60 Hz UI poll must not divide by zero
        int64_t framesPerPhrase = framesPerStep * 16;

        switch (playbackMode_) {
            case PlaybackMode::PHRASE: {
                // ⚠️ BOTH fields, and `phraseStep` is the load-bearing one: the shell reads the phrase
                // cursor out of `phraseStep`, not `row`. Filling only `row` here (as the Kotlin original
                // does, where the UI read `row`) leaves `phraseStep` at its default, so the PHRASE
                // screen's marker sits frozen on step 0 for the whole loop while CHAIN and SONG —
                // which fill both — move normally. Same shape as the two arms below.
                pos.phraseStep = clampi(static_cast<int>((elapsedFrames % framesPerPhrase) / framesPerStep), 0, 15);
                pos.row = pos.phraseStep;
                pos.phraseId = currentPhraseId_;
                return pos;
            }
            case PlaybackMode::CHAIN: {
                prune_past(chainRowStartFrames_, currentFrame, framesPerPhrase);
                for (const auto& e : chainRowStartFrames_) {
                    int64_t into = currentFrame - e.second;
                    if (into >= 0 && into < framesPerPhrase) {
                        pos.chainRow = e.first;
                        pos.phraseStep = clampi(static_cast<int>(into / framesPerStep), 0, 15);
                        pos.chainId = currentChainId_;
                        pos.phraseId = project_ ? phrase_at(*project_, pos.chainId, pos.chainRow) : -1;
                        break;
                    }
                }
                pos.row = pos.phraseStep;
                return pos;
            }
            case PlaybackMode::SONG: {
                prune_past(songPositionStartFrames_, currentFrame, framesPerPhrase);
                for (const auto& e : songPositionStartFrames_) {
                    if (trackId >= 0 && e.first.track != trackId) continue;
                    int64_t into = currentFrame - e.second;
                    if (into >= 0 && into < framesPerPhrase) {
                        pos.songRow = e.first.songRow;
                        pos.chainRow = e.first.chainRow;
                        pos.phraseStep = clampi(static_cast<int>(into / framesPerStep), 0, 15);
                        // ⭐ Re-derived from the project rather than banked in SongPos, so an edit to
                        // the song cell or the chain row under a running track shows the phrase the
                        // NEXT lap will play, not the one the entry was queued from.
                        if (project_) {
                            pos.chainId  = chain_at(*project_, e.first.track, pos.songRow);
                            pos.phraseId = phrase_at(*project_, pos.chainId, pos.chainRow);
                        }
                        break;
                    }
                }
                pos.row = pos.phraseStep;
                return pos;
            }
            default: return pos;
        }
    }

    // PlaybackController.notifyDataChanged: roll the lookahead back to the earliest UNPLAYED phrase
    // boundary so an edit is heard on the next phrase loop instead of 2–3 phrases later.
    //
    // ⚠️ THE ANSWER IS PER TRACK, and it has to be. Each track's boundary is its own, so one frame
    // for the whole engine would either drop notes a track had already queued past that frame and
    // will not schedule again, or leave notes it is about to re-emit. The Sequencer holds no engine
    // handle — clearing the queues is the host's job, exactly as it is Kotlin's; it clears each
    // track's from that track's frame.
    RollbackPlan notify_data_changed(int64_t currentFrame) {
        RollbackPlan plan;
        if (!isPlaying_) return plan;

        // PHRASE and CHAIN schedule one track only, so only that track has anything queued.
        const bool oneTrack = playbackMode_ != PlaybackMode::SONG;

        for (int t = 0; t < 8; ++t) {
            if (oneTrack && t != playbackTrack_) continue;

            // SONG's eight cursors: the rewind is shared with a LIVE launch — see rewind_song_track,
            // which carries the note about the TrackState and the RNG coming back with the position.
            if (!oneTrack) {
                const int64_t f = rewind_song_track(t, currentFrame);
                if (f >= 0) plan.frames[t] = f;
                continue;
            }

            std::deque<Checkpoint>& ring = checkpoints_[t];
            const Checkpoint* hit = nullptr;
            for (const Checkpoint& c : ring) {
                if (c.frame > currentFrame) { hit = &c; break; }
            }
            if (!hit) continue;
            Checkpoint cp = *hit;   // by value: the pops below invalidate the pointer

            // ⚠️ AND THE STATE THE RE-SCHEDULE WILL CONSUME — see Checkpoint. Without this the
            // phrase is replayed against a groove phase, a HOP and an RNG stream the first pass has
            // already moved, so what comes back is not what was thrown away: with a groove whose
            // active length does not divide 16 the track comes back re-timed.
            trackStates_[t] = cp.trackState;
            rngs_[t] = cp.rng;
            nextFrameToSchedule_ = cp.frame;
            if (playbackMode_ == PlaybackMode::CHAIN) nextChainRowToSchedule_ = cp.chainRow;
            // PHRASE: resetting nextFrameToSchedule_ is enough
            while (!ring.empty() && ring.back().frame >= cp.frame) ring.pop_back();
            // …and the marker's side-record with them — see drop_positions_from. PHRASE has none:
            // its step is arithmetic off the start frame, so there is nothing to go stale.
            if (playbackMode_ == PlaybackMode::CHAIN)
                drop_positions_from(chainRowStartFrames_, cp.frame, [](int) { return true; });
            plan.frames[t] = cp.frame;
        }
        return plan;
    }

    /**
     * The scale one track is scheduling against — where its last SCA (or the last SCG) left it.
     *
     * ⚠️ THIS IS THE SCHEDULER'S CLOCK, NOT THE LISTENER'S: the walk runs up to two phrases ahead, so
     * this answers "what the next scheduled note will be quantized to", never "what you are hearing".
     * The playback quantization sites read it because they run on the same clock; the note cursor
     * asks `songcore::track_scale` instead, which is the song's own answer (see scales.h).
     */
    int track_scale_slot(int trackId) const { return trackStates_[clampi(trackId, 0, 7)].scaleSlot; }
    int track_scale_key(int trackId) const {
        const int k = trackStates_[clampi(trackId, 0, 7)].scaleKey;
        return k >= 0 ? k : (project_ ? project_->scaleKey : 0);
    }

    // True once an EQM has overridden the master EQ this session. The host reads it BEFORE stop()
    // (which clears it) and restores project.masterEqSlot — mirroring PlaybackController.stop(),
    // including its guard: no restore when currentProject_ is null (the render path owns its own).
    bool eqm_active() const { return eqmActive_; }

    // True once a VTR or VMV has moved a mixer fader this session — read by the host on the same
    // BEFORE-stop() edge as eqm_active(), to push the project's faders back. Derived from the two
    // below rather than latched beside them: a third thing to remember to set is a third thing to
    // forget.
    bool mixer_vol_active() const { return mixerVolTracks_ != 0 || masterVolActive_; }

    // …and WHICH faders, because a mid-take push of the authored mixer has to put back everything the
    // song did NOT move (engine_setup.h `MixerHeld`). Bit N = track N's fader is the song's now.
    int  mixer_vol_tracks() const { return mixerVolTracks_; }
    bool master_vol_active() const { return masterVolActive_; }

    bool has_live_project() const { return currentProject_ != nullptr; }

    // ── transport starts ──

    // ⚠️ `trackId` DEFAULTS TO 0, and the default is load-bearing: every tool caller asks for track 0
    // and keeps asking for it, which is why the trace goldens record track 0. They record it because
    // the caller requests it, not because the sequencer cannot do anything else.
    void playPhrase(int phraseId, int trackId = 0) {
        stop();
        currentProject_ = project_;
        currentPhraseId_ = phraseId;
        playbackTrack_ = clamp_track(trackId);
        playbackStartFrame_ = getCurrentFrame();
        if (phraseId < 0 || phraseId > 255) return;
        const Phrase& phrase = project_->phrases[phraseId];
        playbackMode_ = PlaybackMode::PHRASE;
        isPlaying_ = true;
        int tempo = project_->tempo;
        int64_t framesPerStep = frames_per_step(tempo, sampleRate_);
        router_.t_play("PHRASE", "id=" + hex2(phraseId), playbackStartFrame_, tempo, sampleRate_);
        nextFrameToSchedule_ = playbackStartFrame_;
        SchedulePhraseResult r = schedulePhrase(phrase, playbackStartFrame_, playbackTrack_,
                                                project_transpose_semitones(*project_), framesPerStep, 0);
        nextFrameToSchedule_ += r.framesScheduled;
    }

    void playChain(int chainId, int trackId = 0) {
        stop();
        currentProject_ = project_;
        currentChainId_ = chainId;
        playbackTrack_ = clamp_track(trackId);
        playbackStartFrame_ = getCurrentFrame();
        if (chainId < 0 || chainId > 255) return;
        const Chain& chain = project_->chains[chainId];
        playbackMode_ = PlaybackMode::CHAIN;
        isPlaying_ = true;
        int tempo = project_->tempo;
        int64_t framesPerStep = frames_per_step(tempo, sampleRate_);
        router_.t_play("CHAIN", "id=" + hex2(chainId), playbackStartFrame_, tempo, sampleRate_);
        nextFrameToSchedule_ = playbackStartFrame_;
        nextChainRowToSchedule_ = 0;
        chainRowStartFrames_.clear();
        int firstRow = findNextNonEmptyChainRow(0, chain);
        if (firstRow >= 0) {
            int phraseId = chain_phrase_ref(chain, firstRow);
            int transposeSemitones = chain_transpose_semitones(chain, firstRow);
            SchedulePhraseResult r = schedulePhrase(project_->phrases[phraseId], playbackStartFrame_,
                                                    playbackTrack_,
                                                    transposeSemitones + project_transpose_semitones(*project_),
                                                    framesPerStep, 0, &chain, firstRow);
            chainRowStartFrames_.emplace_back(firstRow, playbackStartFrame_);
            nextFrameToSchedule_ += r.framesScheduled;
            nextChainRowToSchedule_ = firstRow + 1;
        }
    }

    void playSong(int startRow = 0) {
        stop();
        currentProject_ = project_;
        playbackStartFrame_ = getCurrentFrame();
        playbackMode_ = PlaybackMode::SONG;
        isPlaying_ = true;
        int tempo = project_->tempo;
        router_.t_play("SONG", "row=" + hex2(startRow), playbackStartFrame_, tempo, sampleRate_);
        nextFrameToSchedule_ = playbackStartFrame_;
        // ⚠️ ALL EIGHT START TOGETHER, from the cursor's row — one transport, one downbeat. They
        // diverge from here as their chains run out at different lengths; per-track STARTING is a
        // different feature (LIVE mode) and is not this one.
        for (int t = 0; t < 8; ++t) {
            trackNextFrame_[t] = playbackStartFrame_;
            trackSongRow_[t]   = startRow;
            trackChainRow_[t]  = 0;
            trackDone_[t]      = false;
            liveLoopFrame_[t]  = playbackStartFrame_;
        }
        songPositionStartFrames_.clear();
    }

    void stop() {
        router_.t_stop();
        isPlaying_ = false;
        playbackMode_ = PlaybackMode::STOPPED;
        chainRowStartFrames_.clear();
        songPositionStartFrames_.clear();
        for (int t = 0; t < 8; ++t) checkpoints_[t].clear();
        // Both flags are read BEFORE the host calls stop(), which is what restores the master EQ and
        // the mixer faders — clearing them here is what makes the next session start clean.
        eqmActive_ = false;
        mixerVolTracks_ = 0;
        masterVolActive_ = false;
        playbackTrack_ = 0;
        // Full per-track reset: playback is a pure function of the project (see PlaybackController.stop).
        for (int i = 0; i < 8; ++i) {
            trackStates_[i] = TrackState();
            trackNextFrame_[i] = 0;
            trackSongRow_[i] = 0;
            trackChainRow_[i] = 0;
            trackDone_[i] = false;
            // ⚠️ THE QUEUES GO, THE MODE STAYS. `liveMode_` is a per-session performance choice — you
            // stop between takes and start the next one still in LIVE — while a slot waiting for a
            // boundary that will never come is a launch the next take would fire on its downbeat.
            liveQueue_[i] = LiveSlot{};
            liveSilent_[i] = false;
            liveLoopFrame_[i] = 0;
        }
    }

    // The track PHRASE/CHAIN mode is playing through — what the two live arms schedule at, and what
    // the mixer's fader, mute and peak meter are read from.
    int playback_track() const { return playbackTrack_; }

    // ── LIVE mode ────────────────────────────────────────────────────────────────────────────────
    //
    // Queue-and-launch: the song grid becomes a scene launcher. A launched cell REPEATS on its
    // channel until something else is queued, so a track in LIVE never advances down its column and
    // never runs out of one. See LiveSlot for why this is a modifier on SONG rather than a mode.

    bool     live_mode() const               { return liveMode_; }
    bool     live_silent(int trackId) const  { return liveSilent_[clamp_track(trackId)]; }

    /**
     * What this channel is still waiting to do — **the question the SCREEN asks, which is not the one
     * the scheduler asks.** A slot the walk has already spent goes on being reported until the
     * transport actually reaches the frame it landed on, because until then the launch has not
     * happened yet as far as anyone listening is concerned. Reporting the scheduler's answer made the
     * marker stop blinking a bar or two before the launch a player could still hear coming.
     */
    LiveSlot live_queue(int trackId) const {
        const LiveSlot& q = liveQueue_[clamp_track(trackId)];
        if (q.firesAt >= 0 && currentFrame_ >= q.firesAt) return LiveSlot{};
        return q;
    }

    /**
     * Start in LIVE mode with the transport stopped — LGPT's "the performance begins here". `mask`
     * bit N launches track N at `songRow`; every channel not in it starts SILENT, which is what makes
     * one press on one cell start one channel.
     */
    void playSongLive(int songRow, int mask) {
        playSong(songRow);
        liveMode_ = true;
        for (int t = 0; t < 8; ++t) {
            liveQueue_[t]  = LiveSlot{};
            liveSilent_[t] = ((mask >> t) & 1) == 0;
        }
    }

    /**
     * Toggle the mode under a running transport. Every track keeps its place and starts repeating
     * the row it is on; leaving LIVE, every track resumes walking its column from that same row.
     * Nothing jumps and nothing is silenced.
     *
     * ⚠️ IT REWINDS, and that is the whole reason it is audible at the next boundary rather than a
     * lap later: the scheduler runs two phrases ahead, so a chain end inside the lookahead has
     * already been committed as "advance the column" by the time the button is pressed.
     */
    RollbackPlan set_live_mode(bool on, int64_t currentFrame) {
        RollbackPlan plan;
        if (liveMode_ == on) return plan;
        liveMode_ = on;
        for (int t = 0; t < 8; ++t) liveQueue_[t] = LiveSlot{};

        if (!isPlaying_ || playbackMode_ != PlaybackMode::SONG) {
            for (int t = 0; t < 8; ++t) liveSilent_[t] = false;
            return plan;
        }

        if (on) {
            // ⚠️ A COLUMN THAT HAD ALREADY RUN OUT BECOMES A SILENT CHANNEL, NOT A DEAD ONE — it can
            // be launched. Its clock stopped when it finished, so it is put back on the bar grid of
            // the channels still running (the one FURTHEST BEHIND, so it cannot outrun the buffer
            // fill). A launch quantised against a frame from two minutes ago lands in the past, and
            // one offset from everybody else's grid is not a downbeat anyone can hear.
            int64_t inStep = -1;
            for (int t = 0; t < 8; ++t)
                if (!trackDone_[t]) inStep = (inStep < 0) ? trackNextFrame_[t]
                                                         : std::min(inStep, trackNextFrame_[t]);
            if (inStep < 0) inStep = currentFrame;   // every column had run out
            for (int t = 0; t < 8; ++t) {
                liveSilent_[t] = trackDone_[t];
                if (trackDone_[t]) { trackNextFrame_[t] = inStep; trackChainRow_[t] = 0; }
                trackDone_[t] = false;
            }
        } else {
            for (int t = 0; t < 8; ++t) liveSilent_[t] = false;
        }

        for (int t = 0; t < 8; ++t) {
            const int64_t f = rewind_song_track(t, currentFrame);
            if (f >= 0) plan.frames[t] = f;
        }
        return plan;
    }

    /** Queue one channel to launch `songRow`. `immediate` = the next phrase boundary, else the next chain end. */
    RollbackPlan queue_live(int trackId, int songRow, bool immediate, int64_t currentFrame) {
        return arm_live_slot(clamp_track(trackId), LiveSlot{songRow, false, immediate}, currentFrame);
    }

    /** Queue one channel to fall silent. The other seven keep playing — stopping everything is what stop() is. */
    RollbackPlan queue_live_stop(int trackId, bool immediate, int64_t currentFrame) {
        return arm_live_slot(clamp_track(trackId), LiveSlot{-1, true, immediate}, currentFrame);
    }

    /**
     * Queue a whole row as one scene. ⚠️ AN EMPTY CELL QUEUES A STOP, deliberately: a row is what the
     * user is looking at, and a blank in channel 5 has to sound the way it looks. (This is the
     * opposite of an empty cell in the MIDDLE of a column, which SONG mode plays as a bar of rest —
     * that is a column being walked, and a launcher has no middle.)
     */
    RollbackPlan queue_live_row(int songRow, bool immediate, int64_t currentFrame) {
        RollbackPlan plan;
        if (!liveMode_ || project_ == nullptr) return plan;
        for (int t = 0; t < 8; ++t) {
            const std::vector<int>& refs = project_->tracks[static_cast<size_t>(t)].chainRefs;
            const int chainId = (songRow >= 0 && songRow < static_cast<int>(refs.size()))
                                    ? refs[static_cast<size_t>(songRow)] : -1;
            const bool filled = chainId >= 0 && chainId < 256;
            const RollbackPlan one = arm_live_slot(
                t, filled ? LiveSlot{songRow, false, immediate} : LiveSlot{-1, true, immediate},
                currentFrame);
            if (one.frames[t] >= 0) plan.frames[t] = one.frames[t];
        }
        return plan;
    }

    // ── the polling scheduler (live modes) ──

    void updatePlaybackBuffer() {
        if (!isPlaying_ || project_ == nullptr) return;
        const Project& project = *project_;
        int tempo = project.tempo;
        int64_t framesPerStep = frames_per_step(tempo, sampleRate_);
        int64_t framesPerPhrase = framesPerStep * 16;
        int64_t currentFrame = getCurrentFrame();

        // ⚠️ SONG MODE HAS EIGHT LOOKAHEADS, so "is the buffer deep enough" is asked of the track
        // that is FURTHEST BEHIND — one track running ahead must never let a lagging one arrive
        // late. PHRASE and CHAIN play a single track and keep the one shared cursor.
        //
        // ⚠️ When every track has finished its column the head is pinned to `currentFrame` rather
        // than left at +∞: the arm below is what restarts the song (§2.A call B), and returning here
        // would leave it silent forever.
        int64_t bufferHead = nextFrameToSchedule_;
        if (playbackMode_ == PlaybackMode::SONG) {
            bufferHead = currentFrame;
            bool anyLive = false;
            for (int t = 0; t < 8; ++t) {
                if (trackDone_[t]) continue;
                if (!anyLive || trackNextFrame_[t] < bufferHead) bufferHead = trackNextFrame_[t];
                anyLive = true;
            }
        }
        int64_t bufferRemaining = bufferHead - currentFrame;
        int64_t minBuffer = static_cast<int64_t>(BUFFER_PHRASES) * framesPerPhrase;
        if (bufferRemaining >= minBuffer) return;

        switch (playbackMode_) {
            case PlaybackMode::PHRASE: {
                const Phrase& phrase = project.phrases[currentPhraseId_];
                TrackState& trackState = trackStates_[playbackTrack_];
                save_checkpoint(playbackTrack_, Checkpoint{nextFrameToSchedule_});
                int hopStartRow = trackState.consumeHopTarget();
                int effectiveStartRow = hopStartRow >= 0 ? hopStartRow : 0;
                SchedulePhraseResult r = schedulePhrase(phrase, nextFrameToSchedule_, playbackTrack_,
                                                        project_transpose_semitones(project), framesPerStep,
                                                        effectiveStartRow);
                nextFrameToSchedule_ += r.framesScheduled;
                break;
            }
            case PlaybackMode::CHAIN: {
                const Chain& chain = project.chains[currentChainId_];
                TrackState& trackState = trackStates_[playbackTrack_];
                if (trackState.trackStopped) {
                    nextChainRowToSchedule_ = (nextChainRowToSchedule_ + 1) % 16;
                    nextFrameToSchedule_ += framesPerPhrase;
                    return;
                }
                int nextRow = findNextNonEmptyChainRow(nextChainRowToSchedule_, chain);
                if (nextRow >= 0) {
                    int phraseId = chain_phrase_ref(chain, nextRow);
                    int transposeSemitones = chain_transpose_semitones(chain, nextRow)
                                             + project_transpose_semitones(project);
                    save_checkpoint(playbackTrack_, Checkpoint{nextFrameToSchedule_, nextRow});
                    int hopStartRow = trackState.consumeHopTarget();
                    int effectiveStartRow = hopStartRow >= 0 ? hopStartRow : 0;
                    SchedulePhraseResult r = schedulePhrase(project.phrases[phraseId], nextFrameToSchedule_,
                                                            playbackTrack_,
                                                            transposeSemitones, framesPerStep, effectiveStartRow,
                                                            &chain, nextRow);
                    chainRowStartFrames_.emplace_back(nextRow, nextFrameToSchedule_);
                    nextFrameToSchedule_ += r.framesScheduled;
                    nextChainRowToSchedule_ = (nextRow + 1) % 16;
                } else {
                    stop();
                }
                break;
            }
            case PlaybackMode::SONG: {
                int songLength = 0;
                for (int t = 0; t < 8; ++t)
                    songLength = std::max(songLength, static_cast<int>(project.tracks[t].chainRefs.size()));
                if (songLength == 0) { stop(); break; }

                // ─── EIGHT INDEPENDENT CURSORS ───────────────────────────────────────────────────
                //
                // Fill whichever LIVE track is furthest behind, one phrase at a time, until every
                // one of them is BUFFER_PHRASES ahead. A two-row chain therefore moves on while the
                // sixteen-row chain beside it is still running, which is the whole feature; the
                // per-track walk is in schedule_track_unit().
                //
                // ⚠️ THE STEP CAP IS LOAD-BEARING, not a nervous guard. A song row nobody has
                // authored costs ZERO frames, so a project of empty rows would advance its cursors
                // forever without the buffer ever filling. The lock-step arm this replaces was
                // bounded the same way, by doing exactly one row per poll.
                for (int step = 0; step < SONG_STEPS_PER_POLL; ++step) {
                    int nextTrack = -1;
                    int64_t earliest = 0;
                    for (int t = 0; t < 8; ++t) {
                        if (trackDone_[t]) continue;
                        if (nextTrack < 0 || trackNextFrame_[t] < earliest) {
                            nextTrack = t;
                            earliest = trackNextFrame_[t];
                        }
                    }
                    // Every column has run out → the song starts again, all eight together. One
                    // restart per poll: the lap after it is scheduled by the next poll, exactly as
                    // the old arm advanced one row per poll.
                    if (nextTrack < 0) { restart_all_tracks(); break; }
                    if (earliest - currentFrame >= minBuffer) break;
                    schedule_track_unit(project, nextTrack, framesPerStep, framesPerPhrase);
                }
                break;
            }
            default: break;
        }
    }

    // ── the render-path scheduler (render mode) ──
    // trackFilter == nullptr schedules all tracks; inaudible ones (muted, or unsoloed while another
    // track is soloed) are always skipped. Mirrors scheduleSongRowRange; ptplay only uses the full
    // (null-filter) form.
    int64_t scheduleSongRowRange(int startRow, int endRow, const std::set<int>* trackFilter = nullptr) {
        const Project& project = *project_;
        for (int i = 0; i < 8; ++i) trackStates_[i] = TrackState();
        int64_t framesPerStep = frames_per_step(project.tempo, sampleRate_);
        router_.t_play("RENDER", "rows=" + hex2(startRow) + "-" + hex2(endRow), 0, project.tempo, sampleRate_);

        const int64_t framesPerPhrase = framesPerStep * 16;

        for (int trackId = 0; trackId < 8; ++trackId) {
            trackNextFrame_[trackId] = 0;
            trackSongRow_[trackId]   = startRow;
            trackChainRow_[trackId]  = 0;
            // ⚠️ THE RENDER SKIPS AN INAUDIBLE TRACK; THE LIVE ARM ABOVE DOES NOT, AND THE
            // ASYMMETRY IS DELIBERATE. An export is a file you keep: a muted track is left out of it
            // entirely, which is what this arm has always done. Live, mute is a performance control
            // on the mixer and the sequencer must keep running under it, or unmuting mid-phrase
            // reveals a stale voice instead of the sequence.
            //
            // The audio agrees either way — `push_mixer` gates a muted track to zero in both — so
            // what the asymmetry costs is only the global FX (EQM/VMV) authored ON a muted track,
            // which a render drops and a live play still applies.
            //
            // ⚠️ WITH INDEPENDENT CURSORS IT ALSO SHORTENS THE FILE when the longest chain on the
            // last row is a muted one: the render now ends with the last AUDIBLE material instead of
            // padding to a silence nobody can hear. It cannot re-time anything — that was the reason
            // the old row-length pass ignored audibility, and per-track clocks remove it.
            trackDone_[trackId] = (trackFilter && trackFilter->find(trackId) == trackFilter->end())
                                  || !track_audible(project, trackId);
        }

        // The same per-track walk the live arm takes, and deliberately the SAME FUNCTION: a render
        // that disagrees with what was played is worse than no feature. ⚠️ It does NOT loop — a
        // render plays its range once — and it takes no checkpoints, because nothing edits a project
        // mid-export. Every unit either ends a track or advances one of its two row cursors, so the
        // walk terminates on the range without a step cap.
        for (;;) {
            int nextTrack = -1;
            int64_t earliest = 0;
            for (int t = 0; t < 8; ++t) {
                if (trackDone_[t]) continue;
                if (nextTrack < 0 || trackNextFrame_[t] < earliest) {
                    nextTrack = t;
                    earliest = trackNextFrame_[t];
                }
            }
            if (nextTrack < 0) break;
            schedule_track_unit(project, nextTrack, framesPerStep, framesPerPhrase, endRow, false);
        }

        int64_t endFrame = 0;
        for (int t = 0; t < 8; ++t) endFrame = std::max(endFrame, trackNextFrame_[t]);
        router_.t_stop();
        return endFrame;
    }

  private:
    static int clamp_track(int trackId) { return (trackId >= 0 && trackId < 8) ? trackId : 0; }

    /**
     * Rewind ONE song-mode track to its earliest boundary past `currentFrame`, and hand back the
     * frame the caller must drop queued notes from (−1 = this track has nothing queued past now).
     *
     * Written once below its two callers, which are the same motion for different reasons: a live
     * EDIT has to be heard on the next loop rather than three phrases later, and a LIVE launch has to
     * land on the next boundary rather than after the two phrases already in the buffer.
     *
     * ⚠️ THE TrackState AND THE RNG COME BACK WITH IT. Without them the phrase is replayed against a
     * groove phase, a HOP and a random stream the first pass has already moved, so what comes back is
     * not what was thrown away — with a groove whose active length does not divide 16 the track
     * returns re-timed. It is what the checkpoint ring carries those two fields for.
     */
    int64_t rewind_song_track(int trackId, int64_t currentFrame) {
        std::deque<Checkpoint>& ring = checkpoints_[trackId];
        const Checkpoint* hit = nullptr;
        for (const Checkpoint& c : ring) {
            if (c.frame > currentFrame) { hit = &c; break; }
        }
        if (!hit) return -1;
        Checkpoint cp = *hit;   // by value: the pops below invalidate the pointer

        trackStates_[trackId] = cp.trackState;
        rngs_[trackId]        = cp.rng;
        liveLoopFrame_[trackId]  = cp.liveLoopFrame;
        trackNextFrame_[trackId] = cp.frame;
        trackSongRow_[trackId]   = cp.songRow;
        trackChainRow_[trackId]  = cp.songChainRow;
        // ⚠️ A track that had finished its column is LIVE again: the edit may well be the chain it
        // was missing, and leaving it done would keep it silent until the song looped.
        trackDone_[trackId] = false;
        // ⚠️ …AND A LAUNCH THIS REWIND ROLLED BACK OVER IS WAITING AGAIN. A slot stamped with a frame
        // the cursor no longer reaches is a launch nothing will replay: the walk skips it as spent,
        // and the channel goes on playing the chain the queue was meant to end.
        if (LiveSlot& q = liveQueue_[trackId]; q.firesAt >= cp.frame) q.firesAt = -1;

        while (!ring.empty() && ring.back().frame >= cp.frame) ring.pop_back();
        drop_positions_from(songPositionStartFrames_, cp.frame,
                            [&](const SongPos& p) { return p.track == trackId; });
        return cp.frame;
    }

    /**
     * Put one slot in the queue and rewind that track so the launch can still land on the boundary it
     * was aimed at.
     *
     * ⚠️ **THE REWIND IS FOR THE CHAIN-BOUNDARY QUEUE TOO, not only the immediate one.** The
     * scheduler runs two phrases ahead, so the chain end the user is aiming at may already have been
     * committed as "loop the same row again" before the button was pressed — and without the rewind
     * the launch would land a whole lap late, on a boundary nobody was counting to.
     */
    RollbackPlan arm_live_slot(int trackId, LiveSlot slot, int64_t currentFrame) {
        RollbackPlan plan;
        if (!liveMode_) return plan;
        const int64_t f = rewind_song_track(trackId, currentFrame);
        if (f >= 0) plan.frames[trackId] = f;
        // ⚠️ AFTER the rewind, which is what un-stamps a launch it rolled back over — this slot is a
        // fresh one either way, and setting it first would only hide that ordering.
        liveQueue_[trackId] = slot;
        return plan;
    }

    /**
     * Take the queued slot if this boundary is the one it was waiting for, and say whether it fired.
     * `chainEnd` is false mid-lap and true on the unit that begins one — so an immediate queue fires
     * at either and a chain-boundary queue only at the second. **The two launch quantizations are one
     * code path with two trigger points**, not two mechanisms.
     *
     * ⚠️ The slot is STAMPED rather than cleared — see LiveSlot. `armed()` is what makes that safe:
     * a stamped slot can never fire twice.
     */
    bool consume_live_queue(int trackId, bool chainEnd) {
        LiveSlot& q = liveQueue_[trackId];
        if (!q.armed()) return false;                    // nothing waiting, or already fired
        if (!q.immediate && !chainEnd) return false;      // still mid-lap

        if (q.stop) {
            liveSilent_[trackId] = true;
        } else {
            liveSilent_[trackId] = false;
            trackSongRow_[trackId] = q.targetRow;
        }
        trackChainRow_[trackId] = 0;
        trackStates_[trackId].trackStopped = false;
        liveLoopFrame_[trackId] = trackNextFrame_[trackId];   // a lap begins here
        q.firesAt = trackNextFrame_[trackId];
        return true;
    }

    struct SchedulePhraseResult {
        int rowsScheduled = 0;
        bool hopTriggered = false;
        bool trackStopped = false;
        int64_t framesScheduled = 0;
    };
    struct ScheduleStepResult {
        bool noteScheduled = false;
        bool hopTriggered = false;
        // The frame the step's note-on was scheduled at, or -1. Carried out because an AUS/AUF ramp
        // tick that lands ON a note-on reaches the voice the note REPLACES — the same hazard STEP
        // 2.3's `voiceFxFrame` +1 exists for, and the ramp cannot see the LAT that moved the note.
        int64_t noteFrame = -1;
        // STEP 2.3's `voiceFxFrame` — the frame this step's own live FX writes landed on, LAT and the
        // note-on offset included. A ramp crossing a step that writes the same parameter yields every
        // tic up to it, and the ramp cannot work out where it went: LAT is unclamped, and whether the
        // write took the +1 depends on whether the note survived CHA.
        int64_t fxFrame = -1;
        // The step AFTER CHA/RND/RNL — the slots as they were actually written, which is the only thing
        // that can tell a crossing ramp whether it has to yield the frame above.
        //
        // ⚠️ Pairing still reads the AUTHORED step (automation.h), and the two are asking different
        // questions. Whether a fade EXISTS, and between which bytes, must not depend on dice. Whether
        // some frame inside it is already spoken for is a question about what was really emitted — and
        // a `RND` that turns into a VOL is as real a write as a typed one.
        PhraseStep effectiveStep;
    };

    // Snapshot taken just BEFORE scheduling a phrase, so notify_data_changed() can roll the buffer
    // back to the earliest future phrase boundary without disturbing the phrase now playing.
    //
    // ⚠️⚠️ **A ROLLBACK RE-RUNS schedulePhrase(), AND schedulePhrase() CONSUMES STATE.** It advances
    // `TrackState::grooveStep`, it takes the pending HOP with `consumeHopTarget()`, and it draws from
    // the track's own `rngs_` stream for CHA/RND/RNL and a random ARP. Rolling the FRAME back while
    // leaving those where the first pass left them replays the phrase against a track that has
    // already moved on — so the re-scheduled phrase is not the one that was thrown away.
    // A groove whose active length does not
    // divide 16 comes back at a different phase and RE-TIMES every track, muted or not, and the dice
    // are thrown again. The frame and the row cursors alone are not a checkpoint; this is.
    struct Checkpoint {
        int64_t frame = 0;
        int chainRow = 0;
        int songRow = 0;
        int songChainRow = 0;
        // ⚠️ Filled by save_checkpoint(), NEVER by the call sites — there are four of them and a
        // fifth is one edit away. Captured below the sites, exactly so none of them can forget.
        //
        // ⚠️ ONE TRACK'S STATE, not all eight. With eight independent lookaheads a single checkpoint
        // is no longer a single moment: rolling every track back to one frame either wipes material
        // a track had queued and will not schedule again, or leaves material it is about to schedule
        // twice. Each track carries its own ring and its own boundary.
        TrackState trackState{};
        Rng        rng{};
        // ⚠️⚠️ LIVE's lap origin, and it is here for exactly the reason TrackState and Rng are: a
        // rewind that leaves it behind leaves a frame from the FUTURE beside a cursor from the past,
        // and the starvation guard then reads a lap that scheduled a full chain as one that cost
        // nothing. It rests a bar, and the launch lands a bar late — on the offsets where the poll
        // happened to have crossed the boundary already, and nowhere else.
        int64_t liveLoopFrame = 0;
    };

    int64_t getCurrentFrame() const { return currentFrame_; }

    // ⚠️ BY VALUE, and the state is captured HERE rather than at the four call sites. Every one of
    // them names only the frame and the cursors it knows about; what a rollback has to put back is
    // the sequencer's business, and deriving it once below the sites is what stops the fifth caller
    // from being the one that forgets.
    void save_checkpoint(int trackId, Checkpoint cp) {
        cp.trackState = trackStates_[trackId];
        cp.rng = rngs_[trackId];
        cp.liveLoopFrame = liveLoopFrame_[trackId];
        checkpoints_[trackId].push_back(cp);
        // ring of 4, oldest = earliest unplayed
        if (checkpoints_[trackId].size() > 4) checkpoints_[trackId].pop_front();
    }

    // APPEND, never overwrite — a DELIBERATE divergence from the Kotlin original, which is buggy here.
    //
    // Kotlin keeps this in a `mutableMapOf`, so re-scheduling a position it already holds REPLACES that
    // key's start frame. That breaks the moment a song laps itself inside the lookahead: the scheduler
    // runs BUFFER_PHRASES (2) phrases ahead, so a song shorter than that comes back round to a
    // (songRow, chainRow) it has already queued and rewrites its start frame to the NEXT time that row
    // will play — clobbering the frame of the row that is sounding RIGHT NOW. getPlaybackPosition()
    // then finds every `into` negative, matches no window, and returns its zero-initialised struct: the
    // playhead sits frozen at 0/0/0 for the entire song.
    //
    // It survived this long because it is invisible on real music, which is many phrases long, so the
    // key being rewritten is always far in the future. The SDL shell surfaced it immediately by playing
    // a one-row golden (g7-audio: 1 song row over a 2-row chain — exactly the lookahead depth, so the
    // clobber lands on every poll). Playheads carry no bus event and therefore no golden (SC-4), which
    // is precisely why nothing caught it: ptplay compares events, and this is a side-record.
    //
    // Appending is what the CHAIN-mode sibling has always done — chainRowStartFrames_ is an emplace_back
    // list with no de-duplication — so SONG stops being the odd one out. Duplicates cannot pile up:
    // prune_past() drops everything more than a phrase old on every read and the lookahead is bounded,
    // so the list stays a handful of entries. Insertion order is still load-bearing — getPlaybackPosition
    // takes the FIRST in-window entry, which is now the OLDEST, i.e. the row actually sounding, instead
    // of a future one that had overwritten it.
    // ⚠️ AND THE TRACK, because the eight cursors are at eight different places: the entry is now
    // "track 3 is on song row 5, chain row 2", not "the song is". `prune_past` is unchanged — the
    // frame is still the pair's second, which is all it reads.
    void put_song_position(int trackId, int songRow, int chainRow, int64_t frame) {
        songPositionStartFrames_.emplace_back(SongPos{trackId, songRow, chainRow}, frame);
    }

    // Drop entries that are definitely in the past (> 1 phrase ago) — Kotlin prunes both containers
    // on every position read so the scan stays bounded in a long song.
    template <typename C>
    static void prune_past(C& c, int64_t currentFrame, int64_t framesPerPhrase) {
        c.erase(std::remove_if(c.begin(), c.end(),
                               [&](const typename C::value_type& e) {
                                   return currentFrame > e.second + framesPerPhrase;
                               }),
                c.end());
    }

    /**
     * Drop the position entries a rollback has just invalidated — everything this cursor had recorded
     * at or past the frame it was rewound to. `match` picks the entries the rewound cursor owns.
     *
     * ⚠️⚠️ **A ROLLBACK HAS TO REACH THE SIDE-RECORD, AND DROPPING THE QUEUED NOTES CANNOT DO IT — A
     * MARKER IS NOT AN EVENT.** getPlaybackPosition takes the FIRST in-window entry, which is the
     * OLDEST, so a stale entry the re-schedule has already replaced goes on winning the lookup until
     * it falls out of the window — and there is one per phrase the lookahead had reached, so the
     * marker sits on the row the launch left behind for as many bars as were in the buffer (measured
     * at two). It only shows when the re-scheduled position DIFFERS — a LIVE launch queued in the
     * last phrase of a chain, where the rewind frame is inside the lookahead and the new lap is a
     * different song row.
     */
    template <typename C, typename Match>
    static void drop_positions_from(C& c, int64_t frame, Match match) {
        c.erase(std::remove_if(c.begin(), c.end(),
                               [&](const typename C::value_type& e) {
                                   return e.second >= frame && match(e.first);
                               }),
                c.end());
    }

    // The next row at or after `startRow` that has a phrase in it, wrapping; -1 if the chain is
    // empty. The result is always a valid row, so `chain_phrase_ref` on it is never -1.
    //
    // ⚠️ `startRow` is normalised HERE rather than at the callers, because "a chain row is
    // 0..CHAIN_ROWS-1" is this function's own rule and two callers already disagreed about it once:
    // playChain advances past the row it scheduled without a modulo, so a chain whose first phrase
    // sits on the last row hands this function CHAIN_ROWS. Deriving the wrap here means the next
    // caller cannot reintroduce it.
    int findNextNonEmptyChainRow(int startRow, const Chain& chain) {
        const int seed = ((startRow % CHAIN_ROWS) + CHAIN_ROWS) % CHAIN_ROWS;
        for (int i = 0; i < CHAIN_ROWS; ++i) {
            const int row = (seed + i) % CHAIN_ROWS;
            if (!chain_is_empty(chain, row)) return row;
        }
        return -1;
    }

    // ─── the per-track SONG walk ─────────────────────────────────────────────────────────────────
    //
    // Each track owns a frame, a song row and a chain row, and none of them is anybody else's
    // business. What replaced the shared cursor is written here rather than in the arm above so the
    // poll reads as "fill the track that is furthest behind" and nothing more.

    // The last song row this track has a chain on; −1 when the column is empty. It is what "the
    // column ran out" MEANS — an empty cell in the middle is a rest, not an ending, which is the
    // deliberate divergence from LGPT and M8 (both treat the first blank as terminal).
    static int last_filled_song_row(const Project& project, int trackId) {
        const std::vector<int>& refs = project.tracks[trackId].chainRefs;
        for (int r = static_cast<int>(refs.size()) - 1; r >= 0; --r)
            if (refs[r] >= 0 && refs[r] < 256) return r;
        return -1;
    }

    // How many rows of a chain hold a phrase. ⚠️ A COUNT, and it is now used only where a count is
    // meant: the lock-step arm used it as an INDEX BOUND, so a chain with a hole in it lost every
    // row past the hole, and two goldens recorded that as the specification.
    static int chain_filled_rows(const Project& project, int chainId) {
        if (chainId < 0 || chainId >= 256) return 0;
        const Chain& chain = project.chains[chainId];
        int n = 0;
        for (int i = 0; i < CHAIN_ROWS; ++i) if (!chain_is_empty(chain, i)) n++;
        return n;
    }

    // The chain a track has authored on one song row, or −1 for a blank cell. ⚠️ An out-of-range id
    // is a blank too — a column is a plain vector and the pools are 0..255.
    static int song_cell_chain(const Project& project, int trackId, int songRow) {
        const std::vector<int>& refs = project.tracks[trackId].chainRefs;
        if (songRow < 0 || songRow >= static_cast<int>(refs.size())) return -1;
        const int id = refs[songRow];
        return (id >= 0 && id < 256) ? id : -1;
    }

    // Is the rest on a blank cell over?
    //
    // ⚠️⚠️ THE QUESTION IS "HAS ANYONE ELSE REACHED THE NEXT ROW", NOT "HOW LONG IS THIS ROW". A
    // blank cell is how a track is told to wait, and a track that arrives at one EARLY — its own
    // chain on the row above being shorter than its neighbours' — has to wait LONGER, not the same.
    // A length measured from this row alone carries that lead straight through the blank, and the
    // next chain in the column then starts ahead of everybody else's.
    //
    // ⭐ Asking the live cursors also FOLLOWS a groove or a HOP on the track being waited for, which
    // song_row_span can only approximate. When every track arrives together the two answers are the
    // same, so an arrangement that never runs a track ahead is unchanged.
    //
    // ⚠️ A TRACK RESTING ON A BLANK OF ITS OWN IS NOT WAITED FOR — two of them would wait on each
    // other for ever, and the song would never move. It is read off the cell rather than kept as a
    // flag, so no rollback can leave it stale.
    //
    // ⚠️ The RENDER path marks inaudible tracks done before the walk starts, so a rest waiting on a
    // MUTED neighbour falls back to the span there and can end earlier than it does live. That is
    // the asymmetry scheduleSongRowRange already documents, reaching one row further than it did.
    bool blank_cell_rest_is_over(const Project& project, int trackId, int songRow,
                                 int barsRested) const {
        const int64_t here = trackNextFrame_[trackId];
        bool someone_to_wait_for = false;
        for (int t = 0; t < 8; ++t) {
            if (t == trackId || trackDone_[t]) continue;
            if (trackSongRow_[t] > songRow) return true;               // already turned the row
            if (track_still_holds_row(project, t, songRow, here)) someone_to_wait_for = true;
        }
        return !someone_to_wait_for && barsRested >= song_row_span(project, songRow);
    }

    // Is track `t` still standing on song row `songRow` or an earlier one, at frame `here`?
    //
    // ⚠️⚠️ "ITS CURSOR SAYS THAT ROW" IS NOT "IT IS STILL PLAYING THAT ROW", AND THE DIFFERENCE IS
    // ONE BAR IN BOTH DIRECTIONS. A track keeps its song row until the unit AFTER its chain runs
    // out, and the eight cursors are filled one at a time in frame order — so a track whose row has
    // just ended still reads as standing there, and waiting for it costs an extra bar on a song
    // where nobody was ahead of anybody. Four goldens said exactly that. But the row it has just
    // SCHEDULED is still sounding, and not waiting for that ends the rest a bar early.
    //
    // Both are answered by the same two questions — has it rows left to play here, and is its clock
    // committed past mine — plus what it has authored between there and here.
    //
    // ⭐ A track resting on a blank cell of its own holds nothing, which is what stops two resting
    // tracks from waiting on each other for ever. Read off the cell, so no rollback can leave it
    // stale.
    bool track_still_holds_row(const Project& project, int t, int songRow, int64_t here) const {
        const int otherRow = trackSongRow_[t];
        const int chainId  = song_cell_chain(project, t, otherRow);
        if (chainId >= 0) {
            if (next_chain_row_no_wrap(project.chains[chainId], trackChainRow_[t]) >= 0) return true;
            if (trackNextFrame_[t] > here) return true;    // its last chain row is still sounding
        }
        // Anything authored between where it stands and the row being rested on is still to come.
        for (int r = otherRow + 1; r <= songRow; ++r)
            if (song_cell_chain(project, t, r) >= 0) return true;
        return false;
    }

    // How long a song row is, in phrases, as AUTHORED: the longest column standing on it. It is the
    // FALLBACK length of a rest — what a blank cell costs when there is no other track left to wait
    // for. blank_cell_rest_is_over is the rule; this is what it falls back to.
    //
    // ⚠️ There is nothing else to derive it from once the cursors are independent, and it has to
    // cost SOMETHING: a track that drops out for eight rows and comes back must come back where it
    // always did, or every song already written with a gap in it is silently re-timed. A row nobody
    // has authored spans zero phrases and costs nothing, which is what the old arm did too.
    //
    // ⚠️⚠️ **THE CALLER SPENDS THIS ONE PHRASE AT A TIME AND ASKS AGAIN EACH TIME.** It must never be
    // multiplied out into a single jump. A rest is often several phrases long, the lookahead is two,
    // and the user is editing the very chains this reads: a resting track that banked the whole span
    // in one go is holding a number no later edit can reach — lengthen a neighbour's chain while the
    // rest is playing and the track still comes back on the old, shorter answer. It has no
    // checkpoint to roll back to either, because a jump is not a schedule. Reloading the project was
    // the only thing that cleared it.
    //
    // ⚠️ It is still the NOMINAL length — a groove or a HOP on another track makes the row itself a
    // little longer or shorter, so a resting track can rejoin slightly early or late.
    static int song_row_span(const Project& project, int songRow) {
        int span = 0;
        for (int t = 0; t < 8; ++t) {
            const std::vector<int>& refs = project.tracks[t].chainRefs;
            if (songRow < 0 || songRow >= static_cast<int>(refs.size())) continue;
            span = std::max(span, chain_filled_rows(project, refs[songRow]));
        }
        return span;
    }

    // The next row at or after `startRow` holding a phrase, or −1 when the chain has no more.
    // ⚠️ NO WRAP, and that is the whole difference from findNextNonEmptyChainRow: wrapping is right
    // for CHAIN mode, which loops one chain forever, and wrong inside a song, where running out is
    // exactly the event that moves the track to its next row.
    static int next_chain_row_no_wrap(const Chain& chain, int startRow) {
        for (int r = std::max(0, startRow); r < CHAIN_ROWS; ++r)
            if (!chain_is_empty(chain, r)) return r;
        return -1;
    }

    // One song row finished for this track: drop the per-row state and move on. It costs nothing —
    // every phrase and every bar of rest inside the row has already been paid for, one at a time.
    void advance_track_song_row(int trackId) {
        trackSongRow_[trackId]++;
        trackChainRow_[trackId] = 0;
        trackStates_[trackId].trackStopped = false;
    }

    // The snapshot every unit of work takes before it commits, written once below the three sites
    // that need it — a phrase, a bar of rest, and a bar sat out after HOP FF. ⚠️ A unit that takes
    // no checkpoint is a unit `notify_data_changed` cannot revise, which is exactly how a resting
    // track came to hold a stale answer.
    void checkpoint_track(int trackId, int songRow, int rowUnit, bool take) {
        if (!take) return;
        Checkpoint cp;
        cp.frame = trackNextFrame_[trackId];
        cp.songRow = songRow;
        cp.songChainRow = rowUnit;
        save_checkpoint(trackId, cp);
    }

    // Every column has run out. The song starts again with all eight together on one downbeat, at
    // the latest of their end frames — the same loop point the shared cursor had when it wrapped
    // past the longest column, and the reason a jam does not stop on its own.
    void restart_all_tracks() {
        int64_t at = trackNextFrame_[0];
        for (int t = 1; t < 8; ++t) at = std::max(at, trackNextFrame_[t]);
        for (int t = 0; t < 8; ++t) {
            trackNextFrame_[t] = at;
            trackSongRow_[t] = 0;
            trackChainRow_[t] = 0;
            trackDone_[t] = false;
            trackStates_[t].trackStopped = false;
        }
    }

    // Advance ONE track by one unit of work: a phrase, a rest, or the end of its column.
    //
    // `lastSongRow` bounds the walk for the RENDER path, which plays a range rather than a column;
    // −1 means the track's own column end. `takeCheckpoint` is false there for the same reason —
    // nothing edits a project mid-export.
    void schedule_track_unit(const Project& project, int trackId, int64_t framesPerStep,
                             int64_t framesPerPhrase, int lastSongRow = -1,
                             bool takeCheckpoint = true) {
        // LIVE mode replaces the column walk with a launcher, and it is a SEPARATE function rather
        // than arms inside this one: SONG's path must stay exactly what it was, because "a project
        // that never enters LIVE schedules identically" is what the 36 goldens check.
        // ⚠️ Never on the RENDER path — an export walks a row range and has no transport to queue at.
        if (liveMode_ && lastSongRow < 0) {
            schedule_live_unit(project, trackId, framesPerStep, framesPerPhrase, takeCheckpoint);
            return;
        }

        TrackState& trackState = trackStates_[trackId];
        const std::vector<int>& refs = project.tracks[trackId].chainRefs;
        const int songRow = trackSongRow_[trackId];

        // ⚠️ RE-DERIVED PER UNIT, never cached at play time: the project is edited underneath a
        // running transport (host.h `edit_project`), so a column length latched at T PLAY would keep
        // playing rows the user has just cleared.
        const int lastRow = (lastSongRow >= 0) ? lastSongRow : last_filled_song_row(project, trackId);
        if (lastRow < 0 || songRow > lastRow) { trackDone_[trackId] = true; return; }

        const int chainId = (songRow < static_cast<int>(refs.size())) ? refs[songRow] : -1;

        // ⭐ A BLANK CELL AND A SHORT CHAIN ARE NOT THE SAME THING. A blank is how a track is told to
        // WAIT FOR THE OTHERS, so it lasts until one of them starts the next song row; a cell that
        // NAMES a chain is played for as long as that chain has rows — however few — because not
        // waiting is the request.
        //
        // ⚠️⚠️ ONE BAR PER UNIT, AND THE QUESTION ASKED AGAIN ON EVERY ONE — see
        // blank_cell_rest_is_over. The rest is never banked as a single jump; `trackChainRow_` counts
        // the bars spent, exactly as it counts chain rows for a cell that has a chain in it.
        if (chainId < 0 || chainId >= 256) {
            if (blank_cell_rest_is_over(project, trackId, songRow, trackChainRow_[trackId])) {
                advance_track_song_row(trackId);
                return;
            }
            checkpoint_track(trackId, songRow, trackChainRow_[trackId], takeCheckpoint);
            trackNextFrame_[trackId] += framesPerPhrase;
            trackChainRow_[trackId]++;
            return;
        }

        const Chain& chain = project.chains[chainId];

        // HOP FF stopped this track: it sits out the rest of its chain and rejoins on the next song
        // row, which is what the lock-step arm did by skipping it for the row's remaining rows.
        // ⚠️ A bar at a time, for the same reason the rest above is.
        if (trackState.trackStopped) {
            const int satOut = next_chain_row_no_wrap(chain, trackChainRow_[trackId]);
            if (satOut < 0) { advance_track_song_row(trackId); return; }
            checkpoint_track(trackId, songRow, satOut, takeCheckpoint);
            trackNextFrame_[trackId] += framesPerPhrase;
            trackChainRow_[trackId] = satOut + 1;
            return;
        }

        const int chainRow = next_chain_row_no_wrap(chain, trackChainRow_[trackId]);
        if (chainRow < 0) { advance_track_song_row(trackId); return; }   // the chain is spent

        checkpoint_track(trackId, songRow, chainRow, takeCheckpoint);

        // ⚠️ NO AUDIBILITY TEST — MUTE IS A MIXER GATE, NOT A SEQUENCER ONE, and a per-track loop is
        // exactly where "skip this track" starts to look natural. A muted track is scheduled like
        // any other and the ENGINE zeroes its output (`setTrackMuted`, audio-engine.cpp), which is
        // what makes an unmute mid-phrase drop you into the middle of the sequence where it actually
        // is: the notes were being triggered all along. Gating it here means unmuting reveals only
        // the voice still ringing from BEFORE the mute. LittleGPTracker does it the same way —
        // Player::SetChannelMute only reaches the mixer.
        const int transposeSemitones = chain_transpose_semitones(chain, chainRow)
                                       + project_transpose_semitones(project);
        const int hopStartRow = trackState.consumeHopTarget();
        const int effectiveStartRow = hopStartRow >= 0 ? hopStartRow : 0;
        SchedulePhraseResult r = schedulePhrase(project.phrases[chain_phrase_ref(chain, chainRow)],
                                                trackNextFrame_[trackId], trackId, transposeSemitones,
                                                framesPerStep, effectiveStartRow, &chain, chainRow);
        // ⚠️ RECORDED FOR A MUTED TRACK TOO — this is the PLAYHEAD, and it answers "where is this
        // track", not "what can I hear".
        put_song_position(trackId, songRow, chainRow, trackNextFrame_[trackId]);
        trackNextFrame_[trackId] += r.framesScheduled;
        trackChainRow_[trackId] = chainRow + 1;
    }

    // ─── LIVE mode's unit of work ────────────────────────────────────────────────────────────────
    //
    // The launched song row REPEATS: a chain that runs out re-enters the SAME row instead of moving
    // down the column, so a track here never advances on its own and never runs out of one. That is
    // what makes the song grid a scene launcher rather than an arrangement.
    //
    // It is a separate function from its SONG twin rather than a set of arms inside it, because
    // "a project that never enters LIVE schedules identically" is what the 36 goldens check — and the
    // cheapest way to keep that true is for SONG's path not to gain a branch at all.
    void schedule_live_unit(const Project& project, int trackId, int64_t framesPerStep,
                            int64_t framesPerPhrase, bool takeCheckpoint) {
        TrackState& trackState = trackStates_[trackId];

        // Every unit begins on a PHRASE boundary, so an IMMEDIATE queue always lands here. A
        // chain-boundary one lands here too, on the unit that BEGINS A LAP.
        //
        // ⚠️⚠️ **"THE LAP BEGINS HERE" AND "THE CHAIN JUST RAN OUT" ARE THE SAME INSTANT, AND ONLY
        // THE FIRST OF THEM CAN STILL BE REACHED.** Watching for the chain to run out — which is what
        // this did — asks a question the LOOKAHEAD has usually already answered: the scheduler runs
        // two phrases ahead, so a START pressed during the LAST bar of a chain rewinds to a
        // checkpoint that is already PAST that chain's end, and the walk resuming there finds a chain
        // with rows still in it. The launch then waits for the end after that — **one whole repeat
        // late**, and late by exactly the amount a player is most likely to press. Asking where the
        // cursor IS makes the same boundary answerable from either side of it.
        //
        // ⭐ A SILENT CHANNEL ANSWERS TRUE HERE WITHOUT NEEDING A TERM OF ITS OWN, and that is worth
        // knowing rather than guessing at: both ways of silencing one — a stop queue, and a channel a
        // single-cell START never launched — leave the cursor AT zero, and the silent branch below
        // never moves it. So a channel with no lap is permanently at a lap start, which is the honest
        // answer for it, and an explicit `|| liveSilent_` beside this was measured to change nothing.
        consume_live_queue(trackId, /*chainEnd=*/trackChainRow_[trackId] == 0);

        const std::vector<int>& refs = project.tracks[static_cast<size_t>(trackId)].chainRefs;
        const int songRow = trackSongRow_[trackId];
        const int chainId = (songRow >= 0 && songRow < static_cast<int>(refs.size()))
                                ? refs[static_cast<size_t>(songRow)] : -1;

        // ⚠️ A SILENT CHANNEL STILL SPENDS ITS BAR — one that has been stopped, or launched at a cell
        // with no chain in it. Its clock has to stay on the same bar grid as the seven that are
        // sounding, or the next launch would be quantised against a frame that has already gone by.
        // The checkpoint goes with it: a unit that takes none is a unit the rollback cannot revise.
        if (liveSilent_[trackId] || chainId < 0 || chainId >= 256) {
            checkpoint_track(trackId, songRow, trackChainRow_[trackId], takeCheckpoint);
            trackNextFrame_[trackId] += framesPerPhrase;
            return;
        }

        const Chain& chain = project.chains[static_cast<size_t>(chainId)];

        // HOP FF sat this track out: it rests to the end of the chain and rejoins on the loop, the
        // same bar at a time SONG mode rests it, ending in the same place.
        if (trackState.trackStopped) {
            const int satOut = next_chain_row_no_wrap(chain, trackChainRow_[trackId]);
            if (satOut >= 0) {
                checkpoint_track(trackId, songRow, satOut, takeCheckpoint);
                trackNextFrame_[trackId] += framesPerPhrase;
                trackChainRow_[trackId] = satOut + 1;
                return;
            }
        }

        int chainRow = next_chain_row_no_wrap(chain, trackChainRow_[trackId]);
        if (chainRow < 0) {
            // ─── THE CHAIN BOUNDARY ──────────────────────────────────────────────────────────────
            // The one place a chain-boundary queue can land, and the one place a loop happens.
            trackChainRow_[trackId] = 0;
            trackState.trackStopped = false;

            // ⚠️⚠️ A LAP THAT COST NOTHING RESTS INSTEAD OF LOOPING — see liveLoopFrame_. Re-entering
            // it would leave this track the furthest-behind cursor on every pass and starve the other
            // seven; a bar of rest costs the same silence and keeps the launch grid intact.
            if (trackNextFrame_[trackId] == liveLoopFrame_[trackId]) {
                checkpoint_track(trackId, songRow, 0, takeCheckpoint);
                trackNextFrame_[trackId] += framesPerPhrase;
                liveLoopFrame_[trackId] = trackNextFrame_[trackId];
                return;
            }

            // ⭐ AND THE QUEUE IS NOT READ HERE. The cursor now sits at the top of a lap, which is
            // the one condition the unit's own first line tests — so hand the next pass a clean
            // re-read rather than asking the same question in a second place with a second answer.
            // It costs one of the poll's 64 steps, and a launch changes the song row, which would
            // have made every local read above this point stale anyway.
            liveLoopFrame_[trackId] = trackNextFrame_[trackId];   // the next lap of the same row
            return;
        }

        checkpoint_track(trackId, songRow, chainRow, takeCheckpoint);

        // ⚠️ NO AUDIBILITY TEST, for the reason its SONG twin gives at length: mute is a mixer gate
        // and never a sequencer one, and a per-channel launcher is exactly where "skip this track"
        // starts to look natural.
        const int transposeSemitones = chain_transpose_semitones(chain, chainRow)
                                       + project_transpose_semitones(project);
        const int hopStartRow = trackState.consumeHopTarget();
        const int effectiveStartRow = hopStartRow >= 0 ? hopStartRow : 0;
        SchedulePhraseResult r = schedulePhrase(project.phrases[chain_phrase_ref(chain, chainRow)],
                                                trackNextFrame_[trackId], trackId, transposeSemitones,
                                                framesPerStep, effectiveStartRow, &chain, chainRow);
        put_song_position(trackId, songRow, chainRow, trackNextFrame_[trackId]);
        trackNextFrame_[trackId] += r.framesScheduled;
        trackChainRow_[trackId] = chainRow + 1;
    }

    // `chain`/`chainRow` are the phrase's place in the chain being played, and they exist for AUS/AUF
    // alone: a fade may run from one phrase into a later one of the same chain, and the pairing needs
    // to see past the phrase in hand to know how long the span is. PHRASE mode passes nullptr and gets
    // the per-phrase pairing, which is the same answer whenever a span does not cross a boundary.
    SchedulePhraseResult schedulePhrase(const Phrase& phrase, int64_t startFrame, int trackId,
                                        int transposeSemitones, int64_t framesPerStep, int startRow,
                                        const Chain* chain = nullptr, int chainRow = 0) {
        const Project& project = *project_;
        int scheduledNotes = 0;
        int rowsScheduled = 0;
        TrackState& trackState = trackStates_[clampi(trackId, 0, 7)];
        // Every random draw in the sequencer is reached from inside this call, so selecting the
        // track's stream once here is what keeps rng_int/rng_range from needing a track argument.
        schedulingTrack_ = clampi(trackId, 0, 7);

        if (trackState.trackStopped) return SchedulePhraseResult{0, false, true, 0};

        int64_t framesPerTic = framesPerStep / TICS_PER_STEP;
        int localGrooveStep = trackState.grooveStep;
        bool anyGrooveActive = false;

        int effectiveStartRow = clampi(startRow, 0, 15);
        int64_t frameOffset = 0;

        // ─── The ramps this phrase declares (AUS/AUF — automation.h) ─────────────────────────────
        //
        // Pairing is pure and frame-free: it answers WHICH SPANS the phrase declares, in step indices,
        // and the walk below turns each into events using the duration it is already holding for the
        // step it is standing on. That is why a groove-warped step costs nothing here, and why a HOP
        // truncates a fade for free — the walk ends, and no tick was ever emitted ahead of it.
        //
        // `effectiveStartRow` is passed through: a phrase entered BELOW its AUS runs no ramp at all,
        // because the step that opens it never plays.
        //
        // With a chain in hand the pairing can see the whole chain, so a span may open here and close
        // several phrases later, or simply pass through this one — `find_ramps_in_chain` re-derives
        // that from (chain, chainRow) every time rather than carrying an open ramp in `TrackState`
        // (automation.h says why that matters).
        const std::vector<RampSpec> ramps =
            chain ? find_ramps_in_chain(project, *chain, chainRow, effectiveStartRow)
                  : find_ramps(phrase, effectiveStartRow);
        // Where each ramp has got to — the last byte it emitted. Both ends of an ease curve hold the
        // same byte for many ticks, and a CC repeating what the last one said is a bus record, a queue
        // slot and a golden row spent on nothing.
        //
        // The seed is the byte the curve held one tic BEFORE this phrase's first, which is the same
        // expression in both cases and needs no branch: where the AUS is in this phrase `stepOffset` is
        // −ausStep, so the position comes out at or below zero, the shape clamps to 0 and the seed is
        // the authored start byte the start effect has already emitted. Where the phrase is one the
        // span is crossing, it is the byte the previous phrase signed off on — so the fade continues
        // instead of restating itself at every boundary.
        //
        // An EQ morph seeds the same way and for the same reason: on the AUS step the EQN/EQM effect
        // has already emitted the real start preset, whose types are the types the morph wears and
        // whose FREQ/GAIN/Q are what it holds at t≈0 — so the first tick de-dups against it exactly
        // as a byte ramp's does, with no special case.
        std::vector<RampLastValue> rampLast;
        rampLast.reserve(ramps.size());
        for (const RampSpec& r : ramps) {
            const double seedT = (static_cast<double>(r.stepOffset) * TICS_PER_STEP - 1.0) /
                                 (static_cast<double>(r.span) * TICS_PER_STEP);
            RampLastValue seed;
            if (r.kind == RampKind::EQ_PRESET)
                seed.eq = eq_morph_at(project, r.startByte, r.destByte, r.curveByte, seedT);
            else
                seed.byte = automation_value_byte(r.startByte, r.destByte, r.curveByte, seedT);
            rampLast.push_back(seed);
        }

        for (int stepIndex = effectiveStartRow; stepIndex < 16; ++stepIndex) {
            const PhraseStep& step = phrase.steps[stepIndex];

            // Pre-scan GRV so a new groove takes effect on its own step; last GRV wins (matches
            // resolveStepParams 1..3 overwrite order).
            for (int fxSlot = 1; fxSlot <= 3; ++fxSlot) {
                if (step_fx_type(step, fxSlot) == FX_GRV) {
                    trackState.grooveId = step_fx_value(step, fxSlot);
                    localGrooveStep = 0;
                }
            }

            const Groove& currentGroove = project.grooves[clampi(trackState.grooveId, 0,
                                                                 static_cast<int>(project.grooves.size()) - 1)];
            bool currentGrooveActive = groove_active_length(currentGroove) > 0;

            // ⚠️ THE LENGTH COMES FROM `timing.h`, NOT FROM A COPY HERE. This block held its own
            // `framesPerTic × tics` for years while `groove_step_duration` sat beside it computing the
            // same thing — so the two could disagree, and when the truncation was corrected only the
            // one the tools call would have moved. One definition, and the tools measure what runs.
            if (currentGrooveActive) anyGrooveActive = true;
            int64_t stepDuration = groove_step_duration(currentGroove, localGrooveStep, framesPerStep);

            if (stepDuration == 0) {
                rowsScheduled++;
                localGrooveStep++;
                continue;
            }

            int64_t targetFrame = startFrame + frameOffset;

            ScheduleStepResult stepResult = scheduleStepWithEffects(step, targetFrame, stepDuration, trackId,
                                                                    transposeSemitones, trackState, stepIndex);
            // AFTER the step's own events, and inside the same iteration: a ramp is emitted as the walk
            // passes over it, never ahead of it. The HOP check below is what makes that matter — a fade
            // baked into frames the transport then jumps away from would go on moving the parameter
            // after the phrase had ended.
            if (!ramps.empty())
                emit_ramp_ticks(ramps, rampLast, stepResult.effectiveStep, stepIndex, targetFrame,
                                stepDuration, trackId, stepResult.noteFrame, stepResult.fxFrame);
            rowsScheduled++;
            frameOffset += stepDuration;
            if (currentGrooveActive) localGrooveStep++;
            if (stepResult.noteScheduled) scheduledNotes++;

            if (stepResult.hopTriggered) {
                if (anyGrooveActive) trackState.grooveStep = localGrooveStep;
                return SchedulePhraseResult{rowsScheduled, true, trackState.trackStopped, frameOffset};
            }
        }

        if (anyGrooveActive) trackState.grooveStep = localGrooveStep;
        return SchedulePhraseResult{rowsScheduled, false, false, frameOffset};
    }

    // ─── AUS / AUF — a declared span, emitted as the walk crosses it ────────────────────────────────
    //
    // A ramp is nothing but the parameter's own CC, emitted more often (automation.h): one event per
    // tic, at the byte the curve holds there, sent over the same `byte / 255` the per-step effect
    // already sends. So every value a fade produces is a member of the same 256-value set the goldens
    // already contain, and nothing below the seam has to agree about a float.
    //
    // ⭐ **`t` IS MEASURED IN STEPS, NOT FRAMES**, and that is what makes the emitter groove-proof
    // without looking ahead. Position is `(stepsSoFar + tic/12) / span`, so a fade has covered an exact
    // fraction of its distance at every step boundary whatever the groove did to the lengths in
    // between — written across eight steps, it arrives with the note on the eighth, on any groove.
    // Normalising over the span's total FRAMES instead would need the durations of steps the walk has
    // not reached yet, and would leave the value at every intermediate boundary depending on the swing.
    //
    // A step is twelve tics however long it is: `stepDuration / TICS_PER_STEP` is the same warped
    // frames-per-tic LAT and KIL offset by, so the fade sits on the grid the rest of the step sits on.
    // Where a ramp has got to. One of the two members is live, chosen by the ramp's kind — a BYTE
    // ramp's last emitted byte, or an EQ morph's last emitted band set.
    struct RampLastValue {
        int               byte = 0;
        ExtEqMorphPayload eq{};
    };

    void emit_ramp_ticks(const std::vector<RampSpec>& ramps, std::vector<RampLastValue>& lastValue,
                         const PhraseStep& effectiveStep, int stepIndex, int64_t targetFrame,
                         int64_t stepDuration, int trackId, int64_t noteFrame, int64_t fxFrame) {
        const int64_t framesPerTic = stepDuration / TICS_PER_STEP;

        // A parameter scheduled on a note's own frame reaches the voice that note is REPLACING — the
        // reason STEP 2.3 releases its per-note effects one frame late. A tic that happens to coincide
        // takes the same +1; the rest stay exactly on the tic grid. `noteFrame` rather than the step
        // frame, because LAT may have moved the note somewhere else in the step entirely.
        auto place = [noteFrame](int64_t frame) { return frame == noteFrame ? frame + 1 : frame; };

        for (size_t i = 0; i < ramps.size(); ++i) {
            const RampSpec& r = ramps[i];
            // −1 at either end means that end is in a DIFFERENT phrase of the chain: before the AUS is
            // "not yet" only when the AUS is here, and past the AUF is "already arrived" only when the
            // AUF is here. A phrase the span merely crosses matches neither and emits all sixteen steps.
            if (r.ausStep >= 0 && stepIndex < r.ausStep) continue;
            if (r.aufStep >= 0 && stepIndex > r.aufStep) continue;
            const int lane = r.global ? TRACK_GLOBAL : trackId;

            // ⚠️ VTR/VMV REPLACE the mixer fader and hold, so the host puts the authored value back on
            // stop() — and the flag telling it to is otherwise set only by the per-step effect. That is
            // not enough here: pairing reads the AUTHORED step, so a CHA that zeroes the slot the ramp
            // took its start value from leaves a fade moving a fader nothing will restore. Keyed on the
            // CC the ramp actually sends, which is the thing that moves it.
            if (r.ccId == CC_TRACK_VOL)  mixerVolTracks_ |= 1 << clampi(trackId, 0, 7);
            if (r.ccId == CC_MASTER_VOL) masterVolActive_ = true;
            // ⚠️ EQM carries the same debt, and is keyed the same way — on what the ramp MOVES, not on
            // the cell that declared it. A morph left the master EQ somewhere no preset names, and
            // without this nothing puts the project's value back on stop().
            if (r.kind == RampKind::EQ_PRESET && r.global) eqmActive_ = true;

            // ⚠️⚠️ **A STEP THAT WRITES THIS PARAMETER ITSELF OWNS THE FRAME IT WRITES ON — THE RAMP
            // YIELDS, AND RESUMES AFTER IT.** The AUS step is the obvious case (its start effect writes
            // the byte the ramp fades FROM, or carries it in the note-on's own gain or pan), but any
            // step of the span may write the same parameter again, and the two must not be queued at
            // one frame: `ScheduledParamUpdate`'s comparator looks at the target frame and nothing else
            // (note-queue.h), so which of two updates due at the same frame is applied LAST is decided
            // by the heap, not by the order they were emitted — and nothing on the grid says which cell
            // the author will hear. Yielding leaves exactly one write per frame, so the question cannot
            // be asked.
            //
            // ⭐ It yields to where the write ACTUALLY LANDED (`fxFrame`) rather than to a second
            // derivation of it: LAT moves the write deeper into the step and is unclamped, and it takes
            // a further frame when a note-on survived the step. A ramp doing that arithmetic again is
            // two copies of STEP 2.1 + 2.3 that agree until one of them changes.
            //
            // ⚠️ Read off the EFFECTIVE step, which is where this parts company with pairing: the frame
            // has to be yielded to the write that really happened, so a `RND` that turned into a VOL
            // takes its frame and a `CHA` that ate one gives it back. On the AUS step, a start effect
            // eaten by CHA hands the ramp a tic whose value is the start byte it is already holding —
            // so the de-dup drops it, and the eaten case emits exactly what it did before.
            const bool ownsStep = step_has_fx(effectiveStep, r.fxCode);

            // The arrival carries the destination byte the author typed, not an interpolation that
            // happens to round to it. It stays on its own step — that is what makes a fade written
            // across eight steps land WITH the note on the eighth — so where the step also writes the
            // parameter it takes the frame after that write rather than the next tic.
            //
            // ⚠️ AN EQ MORPH ARRIVES AT t=1, NOT AT THE DESTINATION PRESET, and the two are the same
            // thing only when the band types agree. The morph wears the START preset's types for its
            // whole length (automation.h), so writing the destination preset whole here would put a
            // snap on the last step of every mismatched pair — the one artefact a fade exists to
            // avoid. Landing on the real preset is one visible cell: `EQM 12` on the next step.
            if (stepIndex == r.aufStep) {
                const int64_t arriveFrame = place(ownsStep ? fxFrame + 1 : targetFrame);
                if (r.kind == RampKind::EQ_PRESET) {
                    const ExtEqMorphPayload m =
                        eq_morph_at(*project_, r.startByte, r.destByte, r.curveByte, 1.0);
                    if (!eq_morph_equal(m, lastValue[i].eq)) {
                        emit_eq_morph(arriveFrame, r, trackId, m);
                        lastValue[i].eq = m;
                    }
                } else if (r.destByte != lastValue[i].byte) {
                    router_.cc(arriveFrame, lane, r.ccId, r.destByte / 255.0f);
                    lastValue[i].byte = r.destByte;
                }
                continue;
            }

            int firstTic = 0;
            if (ownsStep)
                while (firstTic < TICS_PER_STEP && targetFrame + firstTic * framesPerTic <= fxFrame)
                    ++firstTic;
            for (int tic = firstTic; tic < TICS_PER_STEP; ++tic) {
                // `stepOffset + stepIndex` is the steps elapsed since the AUS, whether that AUS is in
                // this phrase (the offset is −ausStep and this is the old `stepIndex − ausStep`) or
                // several phrases back. `span` is the whole ramp, so a fade written across four
                // phrases is one curve, not four.
                const double t = (static_cast<double>(r.stepOffset + stepIndex) +
                                  tic / static_cast<double>(TICS_PER_STEP)) / static_cast<double>(r.span);
                const int64_t frame = place(targetFrame + tic * framesPerTic);
                if (r.kind == RampKind::EQ_PRESET) {
                    const ExtEqMorphPayload m =
                        eq_morph_at(*project_, r.startByte, r.destByte, r.curveByte, t);
                    if (eq_morph_equal(m, lastValue[i].eq)) continue;
                    emit_eq_morph(frame, r, trackId, m);
                    lastValue[i].eq = m;
                    continue;
                }
                const int b = automation_value_byte(r.startByte, r.destByte, r.curveByte, t);
                if (b == lastValue[i].byte) continue;
                router_.cc(frame, lane, r.ccId, b / 255.0f);
                lastValue[i].byte = b;
            }
        }
    }

    // EQM rides TRACK_GLOBAL and EQN the track's own lane — the same split, and for the same reason,
    // as the per-step `ext_master_eq` / `ext_eq_slot` pair these ticks interpolate between.
    void emit_eq_morph(int64_t frame, const RampSpec& r, int trackId, const ExtEqMorphPayload& m) {
        if (r.global) router_.ext_master_eq_morph(frame, m);
        else          router_.ext_eq_morph(frame, trackId, m);
    }

    // CHA gate + RND/RNL randomize, evaluated before effect resolution. The byte-exact goldens are
    // random-free (SC-1): with no CHA/RND/RNL slot present this returns (step, skipNote=false)
    // unchanged, which is why they can be compared at all. The draws themselves are measured instead
    // by tools/ptrandom, against the same draws taken from the real Kotlin sequencer (S7).
    PhraseStep applyChanceAndRandomize(const PhraseStep& step, TrackState& trackState, bool& skipNote) {
        bool hasNote = !step_empty(step);
        skipNote = false;
        PhraseStep effectiveStep = step;
        for (int slot = 1; slot <= 3; ++slot) {
            int fxType = step_fx_type(step, slot);
            int fxValue = step_fx_value(step, slot);
            if (fxType == FX_CHA) {
                int probability = (fxValue >> 4) & 0x0F;
                int target = fxValue & 0x0F;
                int roll = rng_int(15);  // 0-14, so probability F always passes and 0 never does
                bool passed = roll < probability;
                if (!passed) {
                    if (target == 0) skipNote = true;
                    else if (target >= 1 && target <= 3) step_set_fx(effectiveStep, target, 0x00, 0x00);
                }
            }
        }
        for (int slot = 1; slot <= 3; ++slot) {
            int fxType = step_fx_type(effectiveStep, slot);
            int fxValue = step_fx_value(effectiveStep, slot);
            int minNibble = (fxValue >> 4) & 0x0F;
            int maxNibble = fxValue & 0x0F;
            if (fxType == FX_RND) {
                int prevType = trackState.lastColFxType[slot];
                if (prevType == 0x00) continue;
                int minVal = minNibble << 4;
                int maxVal = (maxNibble << 4) | 0x0F;
                int randomValue = (minVal <= maxVal) ? rng_range(minVal, maxVal + 1) : rng_range(maxVal, minVal + 1);
                step_set_fx(effectiveStep, slot, prevType, randomValue);
            } else if (fxType == FX_RNL) {
                if (slot == 1) {
                    if (hasNote) {
                        int noteMidi = note_to_midi(step.note);
                        if (noteMidi >= 0) {
                            int noteRange = minNibble, instRange = maxNibble;
                            int noteOffset = noteRange > 0 ? rng_range(-noteRange, noteRange + 1) : 0;
                            int instOffset = instRange > 0 ? rng_range(-instRange, instRange + 1) : 0;
                            effectiveStep.note = note_from_midi(clampi(noteMidi + noteOffset, 0, 127));
                            effectiveStep.instrument = clampi(step.instrument + instOffset, 0, 255);
                        }
                    }
                } else {
                    int targetSlot = slot - 1;
                    int minVal = minNibble << 4;
                    int maxVal = (maxNibble << 4) | 0x0F;
                    int randomValue = (minVal <= maxVal) ? rng_range(minVal, maxVal + 1) : rng_range(maxVal, minVal + 1);
                    step_set_fx_value(effectiveStep, targetSlot, randomValue);
                }
            }
        }
        return effectiveStep;
    }

    // `stepIndex` is UNUSED, and it stays because the signature is a 1:1 port of Kotlin's
    // `scheduleStepWithEffects` — which does not use it either. Dropping a parameter that the original
    // takes would make the two files stop reading side by side, which is the whole point of the port
    // being a transcription. Named in a comment rather than declared, so gcc stops warning about it.
    ScheduleStepResult scheduleStepWithEffects(const PhraseStep& step, int64_t targetFrame, int64_t stepDuration,
                                               int trackId, int transposeSemitones, TrackState& trackState,
                                               int /*stepIndex*/) {
        const Project& project = *project_;

        // STEP 1: cancellation of persistent REPEAT / ARPEGGIO
        bool hasKill = step.fx1Type == FX_KILL || step.fx2Type == FX_KILL || step.fx3Type == FX_KILL;
        if (hasKill) { trackState.clearRepeat(); trackState.clearArpeggio(); }

        bool hasNote = !step_empty(step);
        if (hasNote) { trackState.clearRepeat(); trackState.clearArpeggio(); }

        float savedRampVolume;
        if (trackState.hasActiveRepeat() && trackState.repeatRetrigCount > 0) {
            float oldDelta = REPEAT_RAMP_DELTAS[clampi(trackState.repeatVolRamp, 0, 15)];
            savedRampVolume = clampf(trackState.repeatBaseVolume + trackState.repeatRetrigCount * oldDelta, 0.0f, 1.0f);
        } else {
            savedRampVolume = -1.0f;
        }

        if (trackState.hasActiveRepeat()) {
            if (step_fx_type(step, trackState.repeatActiveColumn) != FX_NONE) trackState.clearRepeat();
        }
        if (trackState.hasActiveArpeggio()) {
            if (step_fx_type(step, trackState.arpeggioActiveColumn) != FX_NONE) trackState.clearArpeggio();
        }

        // STEP 2: CHA/RND/RNL, resolve, schedule
        bool skipNote = false;
        PhraseStep effectiveStep = applyChanceAndRandomize(step, trackState, skipNote);

        const Instrument& instrument = project.instruments[clampi(effectiveStep.instrument, 0,
                                                                  static_cast<int>(project.instruments.size()) - 1)];
        float instrVol = hex_to_float(instrument.volume);

        int velocityByte = clampi(effectiveStep.volume, 0, 127);
        float velocityGain = (velocityByte / 127.0f) * (velocityByte / 127.0f);

        ResolvedStepParams params = resolve_step_params(effectiveStep, targetFrame, instrVol);
        float instrVolWithVxx = params.volume;

        float instrumentPan = hex_to_float(instrument.pan);
        float notePan = params.panValue.has_value() ? (*params.panValue / 255.0f) : instrumentPan;

        // STEP 2.1: DEL (LAT) — offset the target frame
        int delayTicks = params.delayTicks.value_or(0);
        int64_t effectiveTargetFrame;
        if (delayTicks > 0) {
            int64_t fpt = stepDuration / TICS_PER_STEP;
            effectiveTargetFrame = targetFrame + delayTicks * fpt;
        } else {
            effectiveTargetFrame = targetFrame;
        }

        // STEP 2.2: TBL / THO
        int tableIdOverride;
        if (params.tableOverride.has_value() && *params.tableOverride >= 0) {
            trackState.lastTableOverride = *params.tableOverride;
            tableIdOverride = *params.tableOverride;
        } else if (hasNote) {
            trackState.lastTableOverride = -1;
            tableIdOverride = -1;
        } else {
            tableIdOverride = trackState.lastTableOverride;
        }

        int tableStartRow;
        if (params.tableHopTarget.has_value()) {
            int targetRow = *params.tableHopTarget % 16;
            trackState.lastTableStartRow = targetRow;
            if (!hasNote) router_.ext_table_row(effectiveTargetFrame, trackId, targetRow);
            tableStartRow = targetRow;
        } else {
            tableStartRow = -1;
        }

        // GRV assignment
        if (params.grooveId.has_value()) {
            trackState.grooveId = *params.grooveId;
            trackState.grooveStep = 0;
        }

        // SCA / SCG assignment. Above the note below rather than after it, so the step that carries
        // the command is already in the new scale — the same relationship GRV has with its own step.
        //
        // ⚠️ SCG writes all eight TrackStates, including this one, so the order matters only if both
        // are on the same step: SCA is applied second and wins, which is the narrower command winning
        // over the broader one and matches last-wins everywhere else here.
        if (params.scaleGlobalByte.has_value()) {
            const int key = scale_cmd_key(*params.scaleGlobalByte);
            const int slot = scale_cmd_slot(*params.scaleGlobalByte);
            for (int t = 0; t < 8; ++t) { trackStates_[t].scaleSlot = slot; trackStates_[t].scaleKey = key; }
        }
        if (params.scaleTrackByte.has_value()) {
            trackState.scaleSlot = scale_cmd_slot(*params.scaleTrackByte);
            trackState.scaleKey  = scale_cmd_key(*params.scaleTrackByte);
        }

        bool noteScheduled = false;
        if (hasNote && !skipNote) {
            Note note;
            if (transposeSemitones != 0) {
                int originalMidi = note_to_midi(effectiveStep.note);
                note = originalMidi >= 0 ? note_from_midi(clampi(originalMidi + transposeSemitones, 0, 127))
                                         : effectiveStep.note;
            } else {
                note = effectiveStep.note;
            }

            int previousMidi = trackState.lastNoteMidi;

            float pslInitialOffset = 0.0f, pslDuration = 0.0f, pbnRate = 0.0f, vibratoSpeed = 0.0f, vibratoDepth = 0.0f;

            if (params.pslDuration.has_value() && *params.pslDuration > 0 && previousMidi >= 0) {
                int currentMidi = note_to_midi(note);
                if (currentMidi >= 0 && previousMidi != currentMidi) {
                    pslInitialOffset = static_cast<float>(previousMidi - currentMidi);
                    pslDuration = static_cast<float>(*params.pslDuration);
                }
            }
            if (params.pbnValue.has_value() && *params.pbnValue != 0) {
                int v = *params.pbnValue;
                pbnRate = v < 0x80 ? (v / 16.0f) : -((v & 0x7F) / 16.0f);
                trackState.pitchBendActive = true;
            }
            if (params.pvbValue.has_value() && *params.pvbValue != 0) {
                int v = *params.pvbValue;
                int speedNibble = (v >> 4) & 0x0F;
                int depthNibble = v & 0x0F;
                vibratoSpeed = (2.0f + speedNibble * 0.5f) * (project.tempo / 120.0f);
                vibratoDepth = depthNibble * 0.125f;
                trackState.vibratoActive = true;
            }
            if (params.pvxValue.has_value() && *params.pvxValue != 0) {
                int v = *params.pvxValue;
                int speedNibble = (v >> 4) & 0x0F;
                int depthNibble = v & 0x0F;
                vibratoSpeed = (2.0f + speedNibble * 0.5f) * 2.0f * (project.tempo / 120.0f);
                vibratoDepth = depthNibble * 0.125f * 4.0f;
                trackState.vibratoActive = true;
            }

            NoteArgs a;
            a.frame = effectiveTargetFrame; a.track = trackId; a.instrument = effectiveStep.instrument;
            a.notePitch = note.pitch; a.noteOctave = note.octave;
            a.velocity = velocityByte; a.velGain = velocityGain; a.volGain = instrVolWithVxx; a.pan = notePan;
            a.start = params.startPoint; a.slice = params.sliIndex.value_or(-1);
            a.transpose = transposeSemitones; a.pit = params.pitSemitones.value_or(0); a.arp = 0;
            a.tableId = tableIdOverride; a.tableRow = tableStartRow;
            a.pslOff = pslInitialOffset; a.pslDur = pslDuration; a.pbnRate = pbnRate;
            a.vibSpd = vibratoSpeed; a.vibDep = vibratoDepth;
            emit_note(a, note);
            noteScheduled = true;

            trackState.lastNote = note;
            trackState.lastInstrument = effectiveStep.instrument;
            trackState.lastVolume = velocityGain * instrVolWithVxx;
            trackState.lastStartPoint = params.startPoint;
            trackState.lastPan = notePan;
            trackState.lastNoteMidi = note_to_midi(note);

            if (trackState.hasPitchMod() && pbnRate == 0.0f && vibratoDepth == 0.0f) trackState.clearPitchMod();
        }

        int64_t scheduledNoteFrame = noteScheduled ? effectiveTargetFrame : -1;
        // STEP 2.3's frame, hoisted: it is also what a crossing ramp yields to (ScheduleStepResult).
        const int64_t voiceFxFrame = (hasNote && !skipNote) ? effectiveTargetFrame + 1 : effectiveTargetFrame;

        // KIL: soft note-off at the sample-accurate kill frame (with LAT + KIL-offset latency)
        if (params.killAtFrame.has_value()) {
            int64_t fpt = stepDuration / TICS_PER_STEP;
            int64_t killFrame = *params.killAtFrame + (delayTicks + params.killOffsetTicks) * fpt;
            router_.note_off(killFrame, trackId, NOTE_OFF_RELEASE);
            trackState.clearPitchMod();
        }

        // STEP 2.3: live per-note / mixer FX (PAN / REV / DEL / BCK / CUT / RES / EQN / EQM)
        {
            bool triggeredNote = hasNote && !skipNote;
            if (!triggeredNote && params.panValue.has_value())
                router_.cc(effectiveTargetFrame, trackId, CC_PAN, *params.panValue / 255.0f);
            if (params.reverbSendValue.has_value())
                router_.cc(voiceFxFrame, trackId, CC_REVERB_SEND, *params.reverbSendValue / 255.0f);
            if (params.delaySendValue.has_value())
                router_.cc(voiceFxFrame, trackId, CC_DELAY_SEND, *params.delaySendValue / 255.0f);
            if (params.bckValue.has_value())
                router_.ext_reverse(voiceFxFrame, trackId, *params.bckValue == 0, triggeredNote);
            // CUT / RES — `voiceFxFrame` for the same reason REV and DEL take it: on a step that also
            // triggers, a param queued at the note's own frame reaches the voice the note REPLACES.
            if (params.filterCutValue.has_value())
                router_.cc(voiceFxFrame, trackId, CC_FILTER_CUT, *params.filterCutValue / 255.0f);
            if (params.filterResValue.has_value())
                router_.cc(voiceFxFrame, trackId, CC_FILTER_RES, *params.filterResValue / 255.0f);
            if (params.eqnSlot.has_value())
                router_.ext_eq_slot(voiceFxFrame, trackId, *params.eqnSlot);
            // The mixer faders. They REPLACE the authored fader and hold until the next VTR/VMV — so,
            // exactly like EQM below, the host puts the project's value back on stop().
            if (params.trackVolValue.has_value()) {
                router_.cc(voiceFxFrame, trackId, CC_TRACK_VOL, *params.trackVolValue / 255.0f);
                mixerVolTracks_ |= 1 << clampi(trackId, 0, 7);
            }
            if (params.masterVolValue.has_value()) {
                // TRACK_GLOBAL, not `trackId` — the master fader belongs to no track, and riding the
                // track lane would let EngineConsumer's external gate swallow it (event.h).
                router_.cc(effectiveTargetFrame, TRACK_GLOBAL, CC_MASTER_VOL,
                           *params.masterVolValue / 255.0f);
                masterVolActive_ = true;
            }
            if (params.eqmSlot.has_value()) {
                // Master/mixer EQ — global, persists until the next EQM; the host restores the mixer
                // value on stop() (PlaybackController.eqmActive).
                router_.ext_master_eq(effectiveTargetFrame, *params.eqmSlot);
                eqmActive_ = true;
            }

            // ── MIDI phase D: MPG / MPB / CCA-CCD ────────────────────────────────────────────────
            //
            // ⚠️ **THEY BELONG AFTER THE STEP'S NOTE-ON, IN BOTH ORDERS — and there are two orders,
            // which is the trap.** A bus record is CONSUMED the instant it is emitted (arrival order,
            // set by the code below being where it is), and then QUEUED against its due frame (queue
            // order, set by `voiceFxFrame`). Each order carries one of the reasons:
            //
            //  • ARRIVAL: both consumers answer "which instrument is this for?" from the last note-on
            //    (TrackInstruments). Emitted above the note block, a command on a step that CHANGES
            //    instrument would resolve to the previous one — and the first command of a take, with
            //    no note-on seen yet, to nothing at all.
            //  • QUEUE: a note-on carries the instrument's own patch bytes with it (bank/program and
            //    the CC-slot DEFAULTS, midi_out.h). A step command is the specific thing and the
            //    instrument default the general one, so the command must be released AFTER the default
            //    it overrides — put it one frame earlier and the note-on's defaults quietly undo every
            //    CCA in the song, with a byte stream that still looks busy. (Measured: the control
            //    that emits at `effectiveTargetFrame - 1` flips exactly those two messages.)
            //  • and `voiceFxFrame` rather than the step frame is the `+1` the PAN/REV/DEL block above
            //    already uses, for the ENGINE's sake: a param scheduled on the note's own frame
            //    reaches the OLD voice.
            //
            // On a step with no note `voiceFxFrame` is the step frame itself, so a command on an empty
            // step lands where it was written and acts on the note that is sounding.
            if (params.midiProgram.has_value())
                router_.program(voiceFxFrame, trackId, *params.midiProgram);
            if (params.midiBend.has_value())
                router_.pitch_bend(voiceFxFrame, trackId, *params.midiBend << 6);
            for (int slot = 0; slot < MIDI_CC_SLOTS; ++slot) {
                if (!params.ccSlotValue[slot].has_value()) continue;
                router_.cc(voiceFxFrame, trackId, CC_SLOT_A + slot, *params.ccSlotValue[slot] / 255.0f);
            }
        }

        // STEP 2.4: pitch/vol FX on steps WITHOUT notes (mid-note changes)
        if (!hasNote) {
            // Mirrors `currentProject?.tempo ?: 120`: currentProject is only set by the live
            // transport starts, NOT the render path, so an empty-step pitch rate rendered offline
            // carries tempo 120 (the fallback), while live playback carries the real tempo. This is
            // an intentional quirk of the Kotlin scheduler; the goldens enshrine it (g4 render vs live).
            int tempo = currentProject_ ? currentProject_->tempo : 120;
            if (params.volumeFromVxx)
                router_.cc(effectiveTargetFrame, trackId, CC_VOLUME, instrVolWithVxx);
            if (params.pbnValue.has_value()) {
                int v = *params.pbnValue;
                if (v == 0) {
                    router_.ext_pitch_rate(effectiveTargetFrame, trackId, 0.0f, tempo);
                    trackState.pitchBendActive = false;
                } else {
                    float semitonesPerTick = v < 0x80 ? (v / 16.0f) : -((v & 0x7F) / 16.0f);
                    router_.ext_pitch_rate(effectiveTargetFrame, trackId, semitonesPerTick, tempo);
                    trackState.pitchBendActive = true;
                }
            }
            if (params.pvbValue.has_value()) {
                int v = *params.pvbValue;
                if (v == 0) {
                    router_.ext_vibrato(effectiveTargetFrame, trackId, 0.0f, 0.0f);
                    trackState.vibratoActive = false;
                } else {
                    int speedNibble = (v >> 4) & 0x0F;
                    int depthNibble = v & 0x0F;
                    float speed = (2.0f + speedNibble * 0.5f) * (tempo / 120.0f);
                    float depth = depthNibble * 0.125f;
                    router_.ext_vibrato(effectiveTargetFrame, trackId, speed, depth);
                    trackState.vibratoActive = true;
                }
            }
            if (params.pvxValue.has_value()) {
                int v = *params.pvxValue;
                if (v == 0) {
                    router_.ext_vibrato(effectiveTargetFrame, trackId, 0.0f, 0.0f);
                    trackState.vibratoActive = false;
                } else {
                    int speedNibble = (v >> 4) & 0x0F;
                    int depthNibble = v & 0x0F;
                    float speed = (2.0f + speedNibble * 0.5f) * 2.0f * (tempo / 120.0f);
                    float depth = depthNibble * 0.125f * 4.0f;
                    router_.ext_vibrato(effectiveTargetFrame, trackId, speed, depth);
                    trackState.vibratoActive = true;
                }
            }
        }

        // STEP 2.5: HOP
        bool hopTriggered = false;
        if (params.hopValue.has_value()) {
            hopTriggered = true;
            if (*params.hopValue == 0xFF) {
                trackState.trackStopped = true;
            } else {
                trackState.hopTargetRow = *params.hopValue & 0x0F;
            }
        }

        // STEP 3: REPEAT
        int newRepeatColumn = 0;
        if (effectiveStep.fx1Type == FX_REPEAT && effectiveStep.fx1Value > 0) newRepeatColumn = 1;
        else if (effectiveStep.fx2Type == FX_REPEAT && effectiveStep.fx2Value > 0) newRepeatColumn = 2;
        else if (effectiveStep.fx3Type == FX_REPEAT && effectiveStep.fx3Value > 0) newRepeatColumn = 3;
        int newRepeatTicInterval = params.repeatCount.value_or(0);
        int newRepeatVolRamp = params.repeatVolRamp.value_or(0);

        if (newRepeatColumn > 0) {
            trackState.repeatActiveColumn = newRepeatColumn;
            trackState.repeatTicInterval = newRepeatTicInterval;
            trackState.repeatVolRamp = newRepeatVolRamp;
            trackState.repeatStartFrame = targetFrame;
            trackState.repeatRetrigCount = 0;
            trackState.repeatBaseVolume = hasNote ? (velocityGain * instrVolWithVxx)
                                        : (savedRampVolume >= 0.0f ? savedRampVolume : trackState.lastVolume);
        }

        int activeRepeatInterval = newRepeatTicInterval > 0 ? newRepeatTicInterval
                                 : (trackState.hasActiveRepeat() ? trackState.repeatTicInterval : 0);
        int activeVolRamp = newRepeatTicInterval > 0 ? newRepeatVolRamp
                          : (trackState.hasActiveRepeat() ? trackState.repeatVolRamp : 0);

        if (activeRepeatInterval > 0 && trackState.lastNote != Note::EMPTY()) {
            Note retrigNote;
            if (hasNote) {
                if (transposeSemitones != 0) {
                    int originalMidi = note_to_midi(effectiveStep.note);
                    retrigNote = originalMidi >= 0 ? note_from_midi(clampi(originalMidi + transposeSemitones, 0, 127))
                                                   : effectiveStep.note;
                } else {
                    retrigNote = effectiveStep.note;
                }
            } else {
                retrigNote = trackState.lastNote;
            }
            int retrigInstrument = hasNote ? effectiveStep.instrument : trackState.lastInstrument;
            float retrigPan = hasNote ? notePan : trackState.lastPan;
            int retrigStartPoint = hasNote ? params.startPoint : trackState.lastStartPoint;
            float rampDelta = REPEAT_RAMP_DELTAS[clampi(activeVolRamp, 0, 15)];

            int64_t stepEndFrame = targetFrame + stepDuration;
            int64_t gridStep = static_cast<int64_t>(activeRepeatInterval) * stepDuration;
            int64_t gridDenom = TICS_PER_STEP;
            if (gridStep > 0) {
                int64_t framesSinceStart = targetFrame - trackState.repeatStartFrame;
                int64_t k = framesSinceStart <= 0 ? 0
                          : (framesSinceStart * gridDenom + gridStep - 1) / gridStep;
                while (true) {
                    int64_t triggerFrame = trackState.repeatStartFrame + (k * gridStep) / gridDenom;
                    if (triggerFrame >= stepEndFrame) break;
                    if (triggerFrame >= targetFrame && triggerFrame != scheduledNoteFrame) {
                        trackState.repeatRetrigCount++;
                        float retrigVolume = clampf(trackState.repeatBaseVolume + trackState.repeatRetrigCount * rampDelta,
                                                    0.0f, 1.0f);
                        NoteArgs a;
                        a.frame = triggerFrame; a.track = trackId; a.instrument = retrigInstrument;
                        a.notePitch = retrigNote.pitch; a.noteOctave = retrigNote.octave;
                        a.velocity = -1; a.velGain = retrigVolume; a.volGain = 1.0f; a.pan = retrigPan;
                        a.start = retrigStartPoint; a.slice = params.sliIndex.value_or(-1);
                        a.transpose = transposeSemitones; a.pit = params.pitSemitones.value_or(0); a.arp = 0;
                        a.tableId = trackState.lastTableOverride; a.tableRow = -1;
                        emit_note(a, retrigNote);
                    }
                    k++;
                }
            }
        }

        // STEP 4: ARC (arpeggio config)
        if (params.arcValue.has_value()) {
            int v = *params.arcValue;
            int mode = (v >> 4) & 0x0F;
            int speed = v & 0x0F;
            trackState.arpeggioMode = clampi(mode, 0, 3);
            trackState.arpeggioSpeed = speed > 0 ? speed : 4;
        }

        // STEP 5: ARPEGGIO
        int newArpColumn = 0, newArpValue = 0;
        if (effectiveStep.fx1Type == FX_ARPEGGIO) { newArpColumn = 1; newArpValue = effectiveStep.fx1Value; }
        else if (effectiveStep.fx2Type == FX_ARPEGGIO) { newArpColumn = 2; newArpValue = effectiveStep.fx2Value; }
        else if (effectiveStep.fx3Type == FX_ARPEGGIO) { newArpColumn = 3; newArpValue = effectiveStep.fx3Value; }

        if (newArpColumn > 0 && newArpValue == 0) {
            trackState.clearArpeggio();
        } else if (newArpColumn > 0 && newArpValue > 0) {
            trackState.arpeggioActiveColumn = newArpColumn;
            trackState.arpeggioValue = newArpValue;
            trackState.arpeggioStartFrame = targetFrame;
        }

        int activeArpValue = newArpValue > 0 ? newArpValue
                           : (trackState.hasActiveArpeggio() ? trackState.arpeggioValue : 0);

        if (activeArpValue > 0 && trackState.lastNote != Note::EMPTY()) {
            scheduleArpeggioNotes(targetFrame, stepDuration, trackId, trackState, hasNote, effectiveStep, params,
                                  transposeSemitones, /*instrVol=*/velocityGain, /*phraseVol=*/instrVolWithVxx,
                                  notePan, scheduledNoteFrame);
        }

        // Per-column FX memory for RND — real effects only, from the ORIGINAL step.
        for (int col = 1; col <= 3; ++col) {
            int fxType = step_fx_type(step, col);
            int fxValue = step_fx_value(step, col);
            if (fxType != FX_NONE && fxType != FX_RND && fxType != FX_RNL && fxType != FX_CHA) {
                trackState.lastColFxType[col] = fxType;
                trackState.lastColFxValue[col] = fxValue;
            }
        }

        return ScheduleStepResult{noteScheduled, hopTriggered, scheduledNoteFrame, voiceFxFrame,
                                  effectiveStep};
    }

    void scheduleArpeggioNotes(int64_t targetFrame, int64_t stepDuration, int trackId, TrackState& trackState,
                               bool hasNote, const PhraseStep& step, const ResolvedStepParams& params,
                               int transposeSemitones, float instrVol, float phraseVol, float finalPan,
                               int64_t scheduledNoteFrame) {
        int semi1 = (trackState.arpeggioValue >> 4) & 0x0F;
        int semi2 = trackState.arpeggioValue & 0x0F;

        Note baseNote;
        if (hasNote) {
            if (transposeSemitones != 0) {
                int originalMidi = note_to_midi(step.note);
                baseNote = originalMidi >= 0 ? note_from_midi(clampi(originalMidi + transposeSemitones, 0, 127))
                                             : step.note;
            } else {
                baseNote = step.note;
            }
        } else {
            baseNote = trackState.lastNote;
        }

        int baseMidi = note_to_midi(baseNote);
        if (baseMidi < 0) return;

        int64_t framesPerTic = stepDuration / TICS_PER_STEP;
        int ticInterval = trackState.arpeggioSpeed;
        int64_t framesPerArpNote = static_cast<int64_t>(ticInterval) * framesPerTic;
        if (framesPerArpNote <= 0) return;  // guard div-by-zero (goldens keep speed≥1, fpt≥1)

        int patternLength = trackState.arpeggioMode == 2 ? 4 : 3;

        int instrumentId = hasNote ? step.instrument : trackState.lastInstrument;
        float arpInstrVol = hasNote ? instrVol : trackState.lastVolume;
        float arpPhraseVol = hasNote ? phraseVol : 1.0f;
        float arpPan = hasNote ? finalPan : trackState.lastPan;
        int startPoint = hasNote ? params.startPoint : trackState.lastStartPoint;

        int64_t stepEndFrame = targetFrame + stepDuration;
        int64_t framesSinceStart = targetFrame - trackState.arpeggioStartFrame;

        if (framesSinceStart >= 0) {
            int64_t firstTriggerIndex = (framesSinceStart + framesPerArpNote - 1) / framesPerArpNote;
            int64_t triggerIndex = firstTriggerIndex;
            int64_t triggerFrame = trackState.arpeggioStartFrame + triggerIndex * framesPerArpNote;
            while (triggerFrame < stepEndFrame) {
                if (triggerFrame >= targetFrame && triggerFrame != scheduledNoteFrame) {
                    int patternPosition = static_cast<int>(triggerIndex % patternLength);
                    int arpMidi = getArpeggioNote(baseMidi, semi1, semi2, trackState.arpeggioMode, patternPosition);
                    NoteArgs a;
                    a.frame = triggerFrame; a.track = trackId; a.instrument = instrumentId;
                    a.notePitch = baseNote.pitch; a.noteOctave = baseNote.octave;
                    a.velocity = -1; a.velGain = arpInstrVol; a.volGain = arpPhraseVol; a.pan = arpPan;
                    a.start = startPoint; a.slice = params.sliIndex.value_or(-1);
                    a.transpose = transposeSemitones; a.pit = params.pitSemitones.value_or(0);
                    a.arp = arpMidi - baseMidi;
                    a.tableId = trackState.lastTableOverride; a.tableRow = -1;
                    emit_note(a, baseNote);
                }
                triggerIndex++;
                triggerFrame += framesPerArpNote;
            }
        }
    }

    int getArpeggioNote(int baseMidi, int semi1, int semi2, int mode, int position) {
        int note0 = baseMidi, note1 = baseMidi + semi1, note2 = baseMidi + semi2;
        switch (mode) {
            case 0: switch (position % 3) { case 0: return note0; case 1: return note1; default: return note2; }
            case 1: switch (position % 3) { case 0: return note2; case 1: return note1; default: return note0; }
            case 2: switch (position % 4) { case 0: return note0; case 1: return note1; case 2: return note2; default: return note1; }
            // RANDOM. Kotlin is `listOf(note0, note1, note2).random()` — a uniform draw over the three
            // SLOTS, not over the distinct pitches, so a chord whose semitones collide (A00, A33) stays
            // weighted by slot. Drawing an index reproduces that; picking from a de-duplicated set would
            // not, and no golden would ever show the difference.
            case 3: { int notes[3] = {note0, note1, note2}; return notes[rng_int(3)]; }
            default: switch (position % 3) { case 0: return note0; case 1: return note1; default: return note2; }
        }
    }

    // Empty-note guard mirrors AudioEngine.scheduleNote (the tap is BELOW it): an EMPTY note is
    // never an event. Real call sites never pass EMPTY, but the guard keeps the seam faithful.
    void emit_note(NoteArgs a, const Note& note) {
        if (note == Note::EMPTY()) return;
        apply_track_scale(a);
        router_.note_on(a);
    }

    /**
     * Pull a scheduled note onto the scale its track is in — the playback half of SCA / SCG.
     *
     * ⭐ **ONE SNAP HERE IS ALL FOUR OF THE PLACES M8 QUANTIZES SEPARATELY.** By the time a note
     * reaches this funnel the chain and project transposes are already folded into it and PIT and
     * ARP are resolved beside it, so the pitch that will sound is `note + pit + arp`. Quantizing
     * that sum covers the phrase note, both transposes, PIT and ARP at once — where a quantizer
     * written per site would be four sites and a fifth one added later that forgot.
     *
     * ⚠️ **THE CORRECTION IS GIVEN BACK TO THE BASE NOTE, NOT TO `pit` OR `arp`.** Those two are
     * re-applied below the seam (voice_derive.h), so moving them would move the note twice; and
     * `transpose` is what the slice derivation subtracts back out, so it cannot absorb it either.
     * The base note is the one field nothing downstream re-adds.
     *
     * ⚠️ It is the SCHEDULER's scale, read off `TrackState` on the scheduler's own clock — correct
     * precisely because this runs at schedule time, two phrases ahead of what is heard, and the note
     * being built here is one of those future notes. Nothing below the seam may re-ask this
     * question: at the far end the answer would be the scale of a bar nobody has reached.
     *
     * Four cases are deliberately left alone:
     *  · a CHROMATIC scale — the identity that makes every song written before this feature cost
     *    nothing, checked first rather than falling out of the search;
     *  · an instrument with TRANSP. off (model.h — M8's rule, wider than scales);
     *  · ⚠️ a note that is SELECTING A SLICE, where the number is not a pitch at all. M8 shipped
     *    exactly this defect and fixed it ("Scale issues with Sampler slice mode"): quantizing a
     *    sliced kit plays a different drum, not a different note;
     *  · a pitch outside 0..127 at either end of the arithmetic. No scale degree lives out there,
     *    an authored B-9 (131) must stay 131 rather than be dragged into range, and a correction
     *    that cannot be expressed on the base note is not applied at all rather than clamped into
     *    a pitch nobody asked for.
     */
    void apply_track_scale(NoteArgs& a) const {
        const int track = clampi(a.track, 0, 7);
        const Scale& scale = scale_at(*project_, track_scale_slot(track));
        if (scale_is_chromatic(scale)) return;

        if (a.instrument >= 0 && a.instrument < static_cast<int>(project_->instruments.size())) {
            const Instrument& ins = project_->instruments[static_cast<size_t>(a.instrument)];
            if (!ins.transposeEnabled) return;
            if (note_selects_slice(ins, a.slice)) return;
        }

        const int midi     = (a.noteOctave + 1) * 12 + a.notePitch;
        const int sounding = midi + a.pit + a.arp;
        if (sounding < 0 || sounding > 127) return;

        const int snapped = scale_snap(scale, track_scale_key(track), sounding);
        if (snapped == sounding) return;

        const int base = midi + (snapped - sounding);
        if (base < 0 || base > 127) return;
        a.notePitch  = base % 12;
        a.noteOctave = base / 12 - 1;
    }

    // The random draws for CHA / RND / RNL / ARP-RANDOM. Thin names kept so the port reads against
    // PlaybackController.kt line for line: `rng_int(15)` is its `Random.nextInt(15)`, `rng_range(a, b)`
    // its `Random.nextInt(a, b)` — half-open at the top, negative `lo` allowed. See rng.h for why this
    // is the one piece of songcore proven statistically rather than by a golden.
    //
    // ⚠️ The stream is chosen by `schedulingTrack_`, which `schedulePhrase` sets on entry — the ONE
    // place a draw can be reached from. Passing the track down to each draw site instead would mean
    // four signatures widened for a value every one of them already sits underneath, and a fifth
    // site one edit away from forgetting.
    int rng_int(int bound) { return rngs_[schedulingTrack_].next_int(bound); }
    int rng_range(int lo, int hi) { return rngs_[schedulingTrack_].next_int(lo, hi); }

    // ⚠️ ONE STREAM PER TRACK, because the live-edit rollback is per track: a shared stream cannot be
    // rewound for one track without un-drawing dice another track has already thrown. The draws
    // themselves carry no golden (SC-1) — tools/ptrandom measures the distribution, and it drives
    // track 0, whose stream is unchanged.
    Rng rngs_[8];
    int schedulingTrack_ = 0;
    MidiRouter& router_;
    const Project* project_ = nullptr;
    // Mirrors PlaybackController.currentProject: set only by the live transport starts, left null on
    // the render path — the STEP 2.4 empty-step tempo fallback depends on this (see there).
    const Project* currentProject_ = nullptr;
    int sampleRate_ = 44100;

    TrackState trackStates_[8];
    int64_t currentFrame_ = 0;
    // PHRASE and CHAIN play ONE track and keep the single cursor they always had.
    int64_t nextFrameToSchedule_ = 0;
    int nextChainRowToSchedule_ = 0;

    // ─── SONG's eight cursors ────────────────────────────────────────────────────────────────────
    // One per track, and the reason SONG has no shared frame, song row or chain row left: a track
    // whose chain runs short moves on alone. `trackDone_` is a column that has run out — it stays
    // silent until every other track has run out too, which is when the song loops.
    int64_t trackNextFrame_[8] = {0, 0, 0, 0, 0, 0, 0, 0};
    int  trackSongRow_[8]  = {0, 0, 0, 0, 0, 0, 0, 0};
    int  trackChainRow_[8] = {0, 0, 0, 0, 0, 0, 0, 0};
    bool trackDone_[8]     = {false, false, false, false, false, false, false, false};

    // ─── LIVE mode ───────────────────────────────────────────────────────────────────────────────
    // ⭐ THE ROW A CHANNEL LOOPS IS `trackSongRow_`, and there is deliberately no second field
    // holding it: in LIVE a chain that runs out re-enters the SAME song row instead of advancing, so
    // the cursor never moves on its own and is already the answer. A `liveRow_` beside it would be
    // one fact in two places, and the day they disagreed "loop the chain" would quietly become "loop
    // the last chain row".
    //
    // `liveSilent_` is the one thing the cursor cannot say: a channel that has been stopped, or
    // launched at a cell with no chain in it. ⚠️ It is NOT `trackDone_` — a silent channel still
    // spends its bar (schedule_live_unit), because a clock that froze would quantise the next launch
    // against a frame that has already gone by.
    //
    // ⚠️⚠️ `liveLoopFrame_` is the frame the current LAP of the looping row began at, and it is the
    // whole starvation guard. A lap that costs ZERO frames — every row of the chain empty, or a
    // groove holding every step at nought tics — would leave this track the furthest-behind cursor on
    // every pass forever, so the poll would spend all 64 of its steps here and the other seven would
    // run dry. Measuring the lap in FRAMES catches every way of costing none, which is why there is
    // no separate empty-chain test beside it.
    bool     liveMode_ = false;
    LiveSlot liveQueue_[8];
    bool     liveSilent_[8] = {false, false, false, false, false, false, false, false};
    int64_t  liveLoopFrame_[8] = {0, 0, 0, 0, 0, 0, 0, 0};
    int currentPhraseId_ = 0;
    int currentChainId_ = 0;
    // The mixer track PHRASE/CHAIN mode plays through: its fader, its mute, its voice slot and its
    // per-track FX. Unused in SONG and render mode, which carry the track per scheduled row.
    int playbackTrack_ = 0;
    int64_t playbackStartFrame_ = 0;
    PlaybackMode playbackMode_ = PlaybackMode::STOPPED;
    bool isPlaying_ = false;

    // ── side-records: UI cursor + live-edit rollback + the EQM restore flag (S5, SC-4/SC-2) ──
    std::deque<Checkpoint> checkpoints_[8];                                // ring of 4, per track
    std::deque<std::pair<int, int64_t>> chainRowStartFrames_;              // (chainRow, startFrame)
    std::vector<std::pair<SongPos, int64_t>>
        songPositionStartFrames_;                                          // (SongPos → startFrame), insertion-ordered
    bool eqmActive_ = false;
    int  mixerVolTracks_ = 0;      // bit N: a VTR has moved track N's fader this take
    bool masterVolActive_ = false; // …and a VMV has moved the master's

    // Per-retrigger additive volume delta for RPT (Rxy), indexed by ramp nibble. Same constants as
    // PlaybackController.REPEAT_RAMP_DELTAS (single source of the ramp curve).
    static constexpr float REPEAT_RAMP_DELTAS[16] = {
        0.00f, -0.02f, -0.04f, -0.06f, -0.10f, -0.15f, -0.20f, -0.30f,
        0.00f,  0.02f,  0.04f,  0.06f,  0.10f,  0.15f,  0.20f,  0.30f
    };
};

}  // namespace songcore

#endif  // POCKETTRACKER_SONGCORE_SCHEDULER_H
