#ifndef POCKETTRACKER_SONGCORE_SCHEDULER_H
#define POCKETTRACKER_SONGCORE_SCHEDULER_H

// ─── The sequencer spine ─────────────────────────────────────────────────────────────────────────
//
// Walks the project by transport position — grooves, HOP, RPT/ARP grids, LAT, KIL, pitch mods,
// per-note and mixer FX — and emits events through the MidiRouter (router.h). The tests byte-compare
// the stream against the golden traces, so the wiring is exact: floats are binary32 in a fixed
// operation order, the velGain/volGain names are crossed (event.h), and grooves round as recorded.
//
// Kept alongside the walk, carrying no bus event (and so no golden):
//   * getPlaybackPosition() and its frame maps — the UI's playheads;
//   * the checkpoint ring + notify_data_changed() — the live-edit rollback (only the POSITION rolls
//     back, never TrackState);
//   * eqm_active() / mixer_vol_active() / delay_time_active() — EQM, VTR/VMV and TIM REPLACE engine
//     state, and the host restores it on stop().
// Random FX (CHA/RND/RNL/ARP-RANDOM) are not in the goldens; a test checks their distributions
// (rng.h).

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
#include "program.h"     // clampi / clampf
#include "effects.h"
#include "automation.h"
#include "rng.h"
#include "router.h"
#include "scales.h"      // the quantizer every scheduled note goes through
#include "traversal.h"   // chain_at / phrase_at

namespace songcore {

// Note arithmetic, hex_to_float and the step-slot helpers live in model.h; clampi/clampf in
// program.h — layers below the sequencer need them.

enum class PlaybackMode { STOPPED, PHRASE, CHAIN, SONG };

// ─── UI cursor feedback (never goldened) ─────────────────────────────────────────────────────────
//
// Where ONE track is; the eight song cursors run independently.
// ⚠️ Every field is −1 when there is no answer, and −1 is NOT row 0: a phrase auditioned alone is in
// no chain or song. A consumer that draws a marker on 0 shows a frozen playhead.
// ⚠️ The ids are part of the position — two tracks can be in one chain at different rows, and a third
// in a chain the screen is not showing. `row` doubles as the phrase step in every mode.
struct PlaybackPosition {
    int row = -1;
    int chainRow = -1;
    int phraseStep = -1;
    int songRow = -1;
    int chainId = -1;    // the chain `chainRow` is a row OF
    int phraseId = -1;   // the phrase `phraseStep` is a step OF
};

// Where one track is in the song, as scheduled. The track is part of the key.
struct SongPos {
    int track = 0;
    int songRow = 0;
    int chainRow = 0;
};

// Which ROW of a phrase one track is on, stamped by the walk as it passes each row's real start frame.
// ⚠️ Never arithmetic off the phrase start: HOP, grooves and `00` groove steps make rows anything but
// sixteen equal steps.
struct StepPos {
    int track = 0;
    int step  = 0;
};

// What notify_data_changed() asks the host to drop: per track, the frame its lookahead rolled back to,
// or −1 for nothing queued past now. One frame cannot express this with eight independent cursors.
struct RollbackPlan {
    int64_t frames[8] = {-1, -1, -1, -1, -1, -1, -1, -1};
};

// ─── LIVE mode: what one channel is waiting to do ───────────────────────────────────────────────
//
// LIVE is a MODIFIER ON SONG, not a fifth PlaybackMode: only what happens at a track's boundary
// changes, so nothing that switches on `playbackMode_` (playheads, rollback, traces) needs a new arm,
// and a project that never enters LIVE schedules identically.
// `targetRow < 0 && !stop` is the empty slot.
// ⚠️ A slot is SCHEDULED up to two phrases before it is HEARD. `firesAt` is the frame it lands on: the
// scheduler treats it as spent once set, the display keeps showing it until the transport gets there.
struct LiveSlot {
    int     targetRow = -1;      // the song row to launch on this channel
    bool    stop      = false;   // …or silence it instead
    bool    immediate = false;   // at the next PHRASE boundary rather than the next CHAIN boundary
    int64_t firesAt   = -1;      // the frame it was scheduled to land on; −1 = still waiting
    // No "not before frame X" field: a rewind only lands on a boundary PAST the press
    // (rewind_song_track), so a launch can never land early — and a second copy keyed on the lap
    // origin put launches a full lap late.

    /** Something is queued here — the question a MARKER asks. */
    bool pending() const { return targetRow >= 0 || stop; }
    /** …and the walk has not spent it yet — the question the SCHEDULER asks. */
    bool armed() const { return pending() && firesAt < 0; }
};

// ─── What the sounding note is playing with (NoteCarry) ─────────────────────────────────────────
//
// ⚠️ An ARP or RPT retrigger is a NEW VOICE, and a new voice starts from the instrument — so a VOL,
// PAN, CUT … on an empty step, or a fade, would be undone by the next retrigger unless handed on.
// This carries it: set from the note's own step, moved by later commands and fade ticks, reset by the
// next real note. ⚠️ A new per-voice command must be added here.
// One slot per controller, in re-apply order. LPF/HPF/BPF share a slot (each sets type AND cutoff).
constexpr int CARRY_CC_SLOTS = 8;
inline constexpr int carry_cc_slot(int ccId) {
    switch (ccId) {
        case CC_REVERB_SEND: return 0;
        case CC_DELAY_SEND:  return 1;
        case CC_FILTER_LP: case CC_FILTER_HP: case CC_FILTER_BP: return 2;
        case CC_FILTER_CUT:  return 3;
        case CC_FILTER_RES:  return 4;
        case CC_DRIVE:       return 5;
        case CC_CRUSH:       return 6;
        case CC_FINE_TUNE:   return 7;
        default:             return -1;
    }
}

struct NoteCarry {
    float velGain   = 1.0f;   // the note's velocity curve, without the VOL channel
    float phraseVol = 1.0f;   // the VOL channel: instrument VOL, a VOL command, or a fade
    float pan       = 0.5f;
    int   pit       = 0;
    int   slice     = -1;
    float vibSpeed  = 0.0f;   // PVB / PVX; depth 0 = none
    float vibDepth  = 0.0f;
    uint8_t ccId[CARRY_CC_SLOTS]    = {};   // 0 = not moved since the note
    float   ccValue[CARRY_CC_SLOTS] = {};
    int     eqnSlot  = -1;                  // EQN, or -1
    bool    eqMorphed = false;              // …or where an EQN fade left the voice
    ExtEqMorphPayload eqMorph{};
    int     reverse  = -1;                  // BCK: -1 untouched, else the `reverse` flag it sent

    void set_cc(int id, float v) {
        const int s = carry_cc_slot(id);
        if (s < 0) return;
        ccId[s] = static_cast<uint8_t>(id);
        ccValue[s] = v;
        // A filter switched on carries its own cutoff, so an earlier CUT no longer describes it.
        if (s == carry_cc_slot(CC_FILTER_LP)) ccId[carry_cc_slot(CC_FILTER_CUT)] = 0;
    }
};

// ─── Per-track persistent effect state ──────────────────────────────────────────────────────────
struct TrackState {
    Note  lastNote = Note::EMPTY();
    int   lastInstrument = 0;
    int   lastStartPoint = -1;
    NoteCarry carry;

    int   repeatActiveColumn = 0;
    int   repeatTicInterval = 0;
    int   repeatVolRamp = 0;
    int64_t repeatStartFrame = 0;
    int   repeatRetrigCount = 0;
    // The ramp runs on velocity × the VOL channel; `repeatBasePhraseVol` is the channel it was taken
    // at, so a later VOL scales the hits.
    float repeatBaseVolume = 1.0f;
    float repeatBasePhraseVol = 1.0f;

    int   arpeggioActiveColumn = 0;
    int   arpeggioValue = 0;
    int   arpeggioMode = 0;
    int   arpeggioSpeed = 4;
    int64_t arpeggioStartFrame = 0;

    int   hopTargetRow = -1;
    bool  trackStopped = false;
    /**
     * Consecutive phrases that scheduled NOTHING because their entry row hopped.
     * ⚠️ A HOP row costs no time, so a ring of them (`HOP 00` on row 0 of a phrase played alone)
     * schedules zero frames for ever and the track goes silent invisibly. The count bounds the ring
     * and stops the track, as `HOP FF` would. Not a cycle detector: one hop entry is legitimate (how a
     * chain steps over a phrase); any row that plays resets it.
     */
    int   emptyHops = 0;

    bool  pitchBendActive = false;
    bool  vibratoActive = false;
    int   lastNoteMidi = -1;

    int   lastTableOverride = -1;
    int   lastTableStartRow = -1;

    int   grooveId = 0;
    int   grooveStep = 0;

    // Where SCA / SCG put this track. `scaleKey = -1` means "the project's KEY", so a default
    // TrackState is the song's own scale — STOP, render and rollback land there with no extra code.
    int   scaleSlot = 0;
    int   scaleKey  = -1;

    int   lastColFxType[4] = {0, 0, 0, 0};   // 1-indexed: [1]=FX1 …
    int   lastColFxValue[4] = {0, 0, 0, 0};

    // AUS cells a CHA ate on their last pass, so the phrases their span crosses stay silent too. Keyed
    // on the AUS position (`RampSpec::originAbs/originSlot` + chain); rewritten every time the AUS
    // step plays.
    struct EatenAus { int chain = -1; int abs = -1; int slot = 0; };
    static constexpr int EATEN_AUS_SLOTS = 4;
    EatenAus eatenAus[EATEN_AUS_SLOTS];
    int      eatenAusNext = 0;
    bool aus_eaten(int chain, int abs, int slot) const {
        for (const EatenAus& e : eatenAus)
            if (e.chain == chain && e.abs == abs && e.slot == slot) return true;
        return false;
    }
    void set_aus_eaten(int chain, int abs, int slot, bool eaten) {
        for (EatenAus& e : eatenAus)
            if (e.chain == chain && e.abs == abs && e.slot == slot) {
                if (!eaten) e = EatenAus{};
                return;
            }
        if (!eaten) return;
        eatenAus[eatenAusNext] = EatenAus{chain, abs, slot};
        eatenAusNext = (eatenAusNext + 1) % EATEN_AUS_SLOTS;
    }

    bool hasActiveRepeat() const { return repeatActiveColumn > 0 && repeatTicInterval > 0; }
    void clearRepeat() {
        repeatActiveColumn = 0; repeatTicInterval = 0; repeatVolRamp = 0;
        repeatStartFrame = 0; repeatRetrigCount = 0; repeatBaseVolume = 1.0f; repeatBasePhraseVol = 1.0f;
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
        // ⚠️ Default Rng seeding folds a clock read with a static's address, so eight built in a row
        // can be eight copies of ONE stream (every chance gate in lockstep). Re-derive all eight from
        // track 0's draw.
        uint64_t hi = rngs_[0].next_u32();   // ⚠️ separate statements: the evaluation order of two
        uint64_t lo = rngs_[0].next_u32();   //    calls in one expression is unspecified
        seed_rng((hi << 32) ^ lo);
    }

    // The transport clock: the host copies the engine's frame counter in and polls
    // updatePlaybackBuffer(); the tools drive a synthetic one.
    void set_clock(int64_t f) { currentFrame_ = f; }
    int64_t clock() const { return currentFrame_; }

    // Re-set by the host on every verb — a device change can alter the rate mid-session.
    void set_sample_rate(int sr) { if (sr > 0) sampleRate_ = sr; }

    // Pin the random FX to a known stream (for the tests; the app never calls it).
    // Eight streams, one per track; track 0 gets `s` unchanged. The offset is the 64-bit golden
    // ratio, so no two tracks walk the same sequence shifted.
    void seed_rng(uint64_t s) {
        for (int t = 0; t < 8; ++t) rngs_[t].seed(s + static_cast<uint64_t>(t) * 0x9E3779B97F4A7C15ULL);
    }
    int  sample_rate() const { return sampleRate_; }

    // The frame the current session latched at T PLAY — the trace's session base.
    int64_t playback_start_frame() const { return playbackStartFrame_; }

    static constexpr int64_t LOOKAHEAD_MS = 50;
    static constexpr int BUFFER_PHRASES = 2;
    // Per-track scheduling steps one SONG poll may take: 8 tracks × 2 buffered phrases of real work,
    // plus headroom for rows that cost no frames (which could otherwise spin).
    static constexpr int SONG_STEPS_PER_POLL = 64;

    bool is_playing() const { return isPlaying_; }
    PlaybackMode playback_mode() const { return playbackMode_; }

    // ── UI cursor + live-edit reaction (side-records, no bus events) ──

    // `trackId < 0` means "whichever track's marker is oldest" — only meaningful in PHRASE mode, where
    // one track plays; the tools use it, the app does not.
    PlaybackPosition getPlaybackPosition() { return getPlaybackPosition(-1); }

    // Where ONE track is. In PHRASE and CHAIN mode every other track answers −1 throughout — a phrase
    // auditioned alone is in no chain or song, so those screens draw no marker.
    PlaybackPosition getPlaybackPosition(int trackId) {
        PlaybackPosition pos;
        if (!isPlaying_) return pos;
        if (trackId >= 0 && playbackMode_ != PlaybackMode::SONG && trackId != playbackTrack_) return pos;

        int64_t currentFrame = getCurrentFrame();
        int tempo = currentProject_ ? currentProject_->tempo : 120;
        int64_t framesPerStep = frames_per_step(tempo, sampleRate_);
        if (framesPerStep <= 0) return pos;   // unreachable for a legal tempo; never divide by zero

        // ⚠️ The entry in force is the LATEST one at or before now — never "first inside a nominal
        // phrase window", which a HOP-shortened phrase would keep winning. Future (lookahead) entries
        // are excluded by the `<=`.
        // The prune horizon is generous: a HALFTIME phrase is twice the nominal length.
        const int64_t framesPerPhrase   = framesPerStep * 16;
        const int64_t positionHorizon   = framesPerPhrase * 4;

        // Which ROW the walk actually put under this frame. −1 until the first row is stamped.
        prune_past(phraseStepStartFrames_, currentFrame, positionHorizon);
        const int stepInForce =
            step_in_force(trackId >= 0 ? trackId : playbackTrack_, currentFrame);
        if (stepInForce < 0) return pos;

        switch (playbackMode_) {
            case PlaybackMode::PHRASE: {
                // ⚠️ Both fields; the shell reads the phrase cursor from `phraseStep`.
                pos.phraseStep = stepInForce;
                pos.row = pos.phraseStep;
                pos.phraseId = currentPhraseId_;
                return pos;
            }
            case PlaybackMode::CHAIN: {
                prune_past(chainRowStartFrames_, currentFrame, positionHorizon);
                const std::pair<int, int64_t>* held = nullptr;
                for (const auto& e : chainRowStartFrames_)
                    if (e.second <= currentFrame && (held == nullptr || e.second >= held->second))
                        held = &e;
                if (held != nullptr) {
                    pos.chainRow = held->first;
                    pos.phraseStep = stepInForce;
                    pos.chainId = currentChainId_;
                    pos.phraseId = project_ ? phrase_at(*project_, pos.chainId, pos.chainRow) : -1;
                }
                pos.row = pos.phraseStep;
                return pos;
            }
            case PlaybackMode::SONG: {
                prune_past(songPositionStartFrames_, currentFrame, positionHorizon);
                const std::pair<SongPos, int64_t>* held = nullptr;
                for (const auto& e : songPositionStartFrames_) {
                    if (trackId >= 0 && e.first.track != trackId) continue;
                    if (e.second <= currentFrame && (held == nullptr || e.second >= held->second))
                        held = &e;
                }
                if (held != nullptr) {
                    pos.songRow = held->first.songRow;
                    pos.chainRow = held->first.chainRow;
                    pos.phraseStep = stepInForce;
                    // Re-derived from the project, not banked in SongPos, so an edit under a running
                    // track shows the phrase the NEXT lap will play.
                    if (project_) {
                        pos.chainId  = chain_at(*project_, held->first.track, pos.songRow);
                        pos.phraseId = phrase_at(*project_, pos.chainId, pos.chainRow);
                    }
                }
                pos.row = pos.phraseStep;
                return pos;
            }
            default: return pos;
        }
    }

    // Roll the lookahead back to the earliest UNPLAYED phrase boundary, so an edit is heard on the
    // next loop instead of 2–3 phrases later.
    // ⚠️ The answer is PER TRACK: each track's boundary is its own, and one engine-wide frame would drop
    // notes a track will not re-schedule or keep ones it is about to re-emit. The host clears the
    // queues; this holds no engine.
    RollbackPlan notify_data_changed(int64_t currentFrame) {
        RollbackPlan plan;
        if (!isPlaying_) return plan;

        // SONG's eight cursors share the rewind with LIVE launches (rewind_song_track).
        if (playbackMode_ == PlaybackMode::SONG) return rewind_all_song_tracks(currentFrame);

        // PHRASE and CHAIN schedule one track only, so only that track has anything queued.
        const int t = playbackTrack_;
        std::deque<Checkpoint>& ring = checkpoints_[t];
        const Checkpoint* hit = nullptr;
        for (const Checkpoint& c : ring) {
            if (c.frame > currentFrame) { hit = &c; break; }
        }
        if (!hit) return plan;
        Checkpoint cp = *hit;   // by value: the pops below invalidate the pointer

        // ⚠️ And the state the re-schedule consumes (see Checkpoint): replayed against an already-moved
        // groove phase, HOP and RNG, the phrase comes back different — re-timed, with a groove whose
        // length does not divide 16.
        trackStates_[t] = cp.trackState;
        rngs_[t] = cp.rng;
        nextFrameToSchedule_ = cp.frame;
        if (playbackMode_ == PlaybackMode::CHAIN) nextChainRowToSchedule_ = cp.chainRow;
        // PHRASE: resetting nextFrameToSchedule_ is enough.
        while (!ring.empty() && ring.back().frame >= cp.frame) ring.pop_back();
        // …and the marker's row stamps with them (drop_positions_from), in both modes.
        if (playbackMode_ == PlaybackMode::CHAIN)
            drop_positions_from(chainRowStartFrames_, cp.frame, [](int) { return true; });
        drop_positions_from(phraseStepStartFrames_, cp.frame,
                            [&](const StepPos& s) { return s.track == t; });
        plan.frames[t] = cp.frame;
        return plan;
    }

    /**
     * The scale one track is scheduling against — where its last SCA (or SCG) left it.
     * ⚠️ The SCHEDULER's clock, up to two phrases ahead of what is heard. The note cursor asks
     * `songcore::track_scale` instead (scales.h).
     */
    int track_scale_slot(int trackId) const { return trackStates_[clampi(trackId, 0, 7)].scaleSlot; }
    int track_scale_key(int trackId) const {
        const int k = trackStates_[clampi(trackId, 0, 7)].scaleKey;
        return k >= 0 ? k : (project_ ? project_->scaleKey : 0);
    }

    // True once an EQM overrode the master EQ this session. The host reads it BEFORE stop() (which
    // clears it) and restores project.masterEqSlot — not on the render path, which restores its own.
    bool eqm_active() const { return eqmActive_; }

    // True once a VTR or VMV moved a fader this session; read on the same before-stop() edge. Derived
    // from the two below, not latched separately.
    bool mixer_vol_active() const { return mixerVolTracks_ != 0 || masterVolActive_; }

    // …and WHICH faders, so a mid-take push of the authored mixer restores everything else
    // (engine_setup.h `MixerHeld`). Bit N = track N's fader belongs to the song now.
    int  mixer_vol_tracks() const { return mixerVolTracks_; }
    bool master_vol_active() const { return masterVolActive_; }

    // ⚠️ The hand takes a fader back: a mapped knob is a press, and the flags above would make the next
    // ordinary push skip that fader. The next VTR claims it again.
    void release_mixer_vol_track(int track) {
        if (track >= 0 && track < 8) mixerVolTracks_ &= ~(1 << track);
    }
    void release_master_vol() { masterVolActive_ = false; }
    void release_delay_time() { delayTimeActive_ = false; }

    // True once a TIM took over the delay time this session — same edge, same reason: it REPLACES the
    // DELAY screen's time and nothing later restores it.
    bool delay_time_active() const { return delayTimeActive_; }

    bool has_live_project() const { return currentProject_ != nullptr; }

    // ── transport starts ──

    // `trackId` defaults to 0, which every tool caller (and so every trace golden) relies on.
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
        phraseStepStartFrames_.clear();
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
        // ⚠️ All eight start together from the cursor's row — one downbeat. They diverge as their
        // chains end at different lengths. Per-track starting is LIVE mode.
        for (int t = 0; t < 8; ++t) {
            trackNextFrame_[t] = playbackStartFrame_;
            trackSongRow_[t]   = startRow;
            trackChainRow_[t]  = 0;
            trackDone_[t]      = false;
            liveLoopFrame_[t]  = playbackStartFrame_;
        }
        songPositionStartFrames_.clear();
        phraseStepStartFrames_.clear();
    }

    void stop() {
        router_.t_stop();
        isPlaying_ = false;
        playbackMode_ = PlaybackMode::STOPPED;
        chainRowStartFrames_.clear();
        songPositionStartFrames_.clear();
        phraseStepStartFrames_.clear();
        for (int t = 0; t < 8; ++t) checkpoints_[t].clear();
        // The host reads these flags BEFORE calling stop(); clearing them starts the next take clean.
        eqmActive_ = false;
        mixerVolTracks_ = 0;
        masterVolActive_ = false;
        delayTimeActive_ = false;
        playbackTrack_ = 0;
        // Full per-track reset: playback is a pure function of the project.
        for (int i = 0; i < 8; ++i) {
            trackStates_[i] = TrackState();
            trackNextFrame_[i] = 0;
            trackSongRow_[i] = 0;
            trackChainRow_[i] = 0;
            trackDone_[i] = false;
            // ⚠️ The queues go, the MODE stays: LIVE is a per-session choice, but a slot waiting for a
            // boundary would fire on the next take's downbeat.
            liveQueue_[i] = LiveSlot{};
            liveSilent_[i] = false;
            liveLoopFrame_[i] = 0;
        }
    }

    // The track PHRASE/CHAIN mode plays through — the mixer fader, mute and meter it uses.
    int playback_track() const { return playbackTrack_; }

    // ── LIVE mode ────────────────────────────────────────────────────────────────────────────────
    //
    // Queue-and-launch: the song grid becomes a scene launcher. A launched cell REPEATS on its channel
    // until something else is queued, so a LIVE track never walks down or runs out of its column.

    bool     live_mode() const               { return liveMode_; }
    bool     live_silent(int trackId) const  { return liveSilent_[clamp_track(trackId)]; }

    /**
     * What this channel is still waiting to do — the SCREEN's question. A slot the walk has spent is
     * still reported until the transport reaches its frame: until then, nobody has heard the launch.
     */
    LiveSlot live_queue(int trackId) const {
        const LiveSlot& q = liveQueue_[clamp_track(trackId)];
        if (q.firesAt >= 0 && currentFrame_ >= q.firesAt) return LiveSlot{};
        return q;
    }

    /**
     * Start in LIVE mode from stopped. `mask` bit N launches track N at `songRow`; the rest start
     * SILENT, so one press on one cell starts one channel.
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
     * Toggle the mode under a running transport: every track keeps its place and repeats (or, leaving
     * LIVE, resumes walking from) the row it is on. Nothing jumps or goes silent.
     * ⚠️ It REWINDS: a chain end inside the two-phrase lookahead has already been committed as
     * "advance", and without the rewind the change would land a lap late.
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
            // ⚠️ A column that had run out becomes a SILENT channel that can be launched. Its clock
            // stopped when it finished, so it rejoins the bar grid of the channel FURTHEST BEHIND (so
            // it cannot outrun the buffer fill) — not a frame from minutes ago.
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

        return rewind_all_song_tracks(currentFrame);
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
     * Queue a whole row as one scene. ⚠️ An EMPTY cell queues a STOP: a row is what the user sees, and
     * a blank must sound as it looks. (Mid-column, SONG mode plays an empty cell as a rest.)
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

        // ⚠️ SONG has eight lookaheads, so the buffer depth is asked of the track FURTHEST BEHIND.
        // PHRASE and CHAIN keep the one shared cursor.
        // With every track finished, the head is pinned to `currentFrame` rather than +∞, so the fill
        // below still runs.
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
                // Fill whichever track is furthest behind, a phrase at a time, until each is
                // BUFFER_PHRASES ahead — so a two-row chain moves on while a sixteen-row one beside it
                // runs. The per-track walk is schedule_track_unit().
                // ⚠️ The step cap bounds the work one poll can do.
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
                    // Nothing left to fill, and the transport keeps running: a block loops for ever,
                    // so all eight done means PLAY landed on an unwritten row. STOP is the only end.
                    if (nextTrack < 0) break;
                    if (earliest - currentFrame >= minBuffer) break;
                    schedule_track_unit(project, nextTrack, framesPerStep, framesPerPhrase);
                }
                break;
            }
            default: break;
        }
    }

    // ── the render-path scheduler ──
    // trackFilter == nullptr schedules all tracks; inaudible ones (muted, or unsoloed while another is
    // soloed) are always skipped.
    // `repeat` plays the range that many times END TO END IN ONE PASS, so reverb, delay, releases and
    // table positions cross each seam as they do live; concatenated files would cut at every join.
    int64_t scheduleSongRowRange(int startRow, int endRow, const std::set<int>* trackFilter = nullptr,
                                 int repeat = 1) {
        const Project& project = *project_;
        const int64_t framesPerStep   = frames_per_step(project.tempo, sampleRate_);
        const int64_t framesPerPhrase = framesPerStep * 16;
        router_.t_play("RENDER", "rows=" + hex2(startRow) + "-" + hex2(endRow), 0, project.tempo, sampleRate_);

        // ⚠️ Repetitions restart TOGETHER at the longest track's end, not at each track's own end:
        // unequal blocks drift within a pass, and restarting per track would compound it every time.
        // ⚠️ A pass that schedules nothing ends the loop, so an empty range cannot be walked `repeat`
        // times for no frames.
        if (repeat < 1) repeat = 1;
        int64_t passStart = 0;
        for (int pass = 0; pass < repeat; ++pass) {
            const int64_t passEnd = schedule_range_pass(project, startRow, endRow, trackFilter,
                                                        passStart, framesPerStep, framesPerPhrase);
            if (passEnd <= passStart) break;
            passStart = passEnd;
        }

        router_.t_stop();
        return passStart;
    }

  private:
    /** One play-through of `startRow..endRow`, beginning at `passStart`. Returns the frame it ends on. */
    int64_t schedule_range_pass(const Project& project, int startRow, int endRow,
                                const std::set<int>* trackFilter, int64_t passStart,
                                int64_t framesPerStep, int64_t framesPerPhrase) {
        for (int i = 0; i < 8; ++i) trackStates_[i] = TrackState();

        for (int trackId = 0; trackId < 8; ++trackId) {
            trackNextFrame_[trackId] = passStart;
            trackSongRow_[trackId]   = startRow;
            trackChainRow_[trackId]  = 0;
            // ⚠️ The render SKIPS an inaudible track; live playback does not — deliberately. A muted
            // track is left out of a file; live, mute is a mixer gate and the sequence must keep
            // running under it so unmuting reveals the sequence, not a stale voice. The audio agrees
            // either way; the difference is only global FX (EQM/VMV) authored on a muted track.
            // With per-track clocks this can only shorten a file that ended on a muted track's chain.
            trackDone_[trackId] = (trackFilter && trackFilter->find(trackId) == trackFilter->end())
                                  || !track_audible(project, trackId);
        }

        // The SAME per-track walk the live arm takes — a render must not disagree with playback. It
        // does not loop and takes no checkpoints (nothing edits mid-export). Every unit ends a track or
        // advances a cursor, so it terminates without a step cap.
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

        int64_t passEnd = passStart;
        for (int t = 0; t < 8; ++t) passEnd = std::max(passEnd, trackNextFrame_[t]);
        return passEnd;
    }

    static int clamp_track(int trackId) { return (trackId >= 0 && trackId < 8) ? trackId : 0; }

    /**
     * Rewind ONE song-mode track to its earliest boundary past `currentFrame`; returns the frame to
     * drop queued notes from (−1 = nothing queued past now). Shared by a live EDIT (heard on the next
     * loop) and a LIVE launch (lands on the next boundary, not after the buffered two phrases).
     * ⚠️ TrackState and the RNG come back with it (see Checkpoint).
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
        // ⚠️ A track that had finished is live again: the edit may be the chain it was missing.
        trackDone_[trackId] = false;
        // ⚠️ …and a launch this rewind rolled back over is waiting again; its stamp names a frame the
        // cursor will no longer reach, and the walk would skip it as spent.
        if (LiveSlot& q = liveQueue_[trackId]; q.firesAt >= cp.frame) q.firesAt = -1;

        while (!ring.empty() && ring.back().frame >= cp.frame) ring.pop_back();
        drop_positions_from(songPositionStartFrames_, cp.frame,
                            [&](const SongPos& p) { return p.track == trackId; });
        drop_positions_from(phraseStepStartFrames_, cp.frame,
                            [&](const StepPos& s) { return s.track == trackId; });
        return cp.frame;
    }

    /** Rewind all eight SONG cursors — eight independent rewinds; each track loops its own block. */
    RollbackPlan rewind_all_song_tracks(int64_t currentFrame) {
        RollbackPlan plan;
        for (int t = 0; t < 8; ++t) {
            const int64_t f = rewind_song_track(t, currentFrame);
            if (f >= 0) plan.frames[t] = f;
        }
        return plan;
    }

    /**
     * Queue one slot and rewind its track so the launch lands on the boundary it was aimed at.
     * ⚠️ The chain-boundary queue needs the rewind too: that chain end may already be committed as
     * "loop the row again" inside the lookahead, and the launch would land a lap late.
     */
    RollbackPlan arm_live_slot(int trackId, LiveSlot slot, int64_t currentFrame) {
        RollbackPlan plan;
        if (!liveMode_) return plan;
        const int64_t f = rewind_song_track(trackId, currentFrame);
        if (f >= 0) plan.frames[trackId] = f;
        // ⚠️ After the rewind, which un-stamps a launch it rolled back over.
        liveQueue_[trackId] = slot;
        return plan;
    }

    /**
     * Take the queued slot if this boundary is the one it waits for; say whether it fired. `chainEnd`
     * is true on the unit that begins a lap: an immediate queue fires at either, a chain-boundary
     * queue only there — one code path, two trigger points.
     * The slot is STAMPED rather than cleared (LiveSlot); `armed()` keeps it from firing twice.
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
        trackStates_[trackId].emptyHops = 0;
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
        // The frame of this step's note-on, or -1. A ramp tick landing ON a note-on would reach the
        // voice the note REPLACES, and the ramp cannot see the LAT that moved the note.
        int64_t noteFrame = -1;
        // The frame this step's own live FX writes landed on (`voiceFxFrame`, LAT and the note-on
        // offset included). A ramp crossing a step that writes the same parameter yields that tic.
        int64_t fxFrame = -1;
        // The step AFTER CHA/RND/RNL — what was really written, which decides whether a crossing ramp
        // must yield. ⚠️ Pairing reads the AUTHORED step (automation.h): whether a fade exists must not
        // depend on dice; whether a frame inside it is taken does.
        PhraseStep effectiveStep;
    };

    // Snapshot taken just BEFORE scheduling a phrase, so notify_data_changed() can roll back to the
    // earliest future phrase boundary.
    // ⚠️ schedulePhrase() CONSUMES STATE — the groove step, the pending HOP, the track's RNG stream. A
    // rollback restoring only the frame replays the phrase against a track that has moved on: a groove
    // whose length does not divide 16 re-times the track, and the dice roll again. This is the state.
    struct Checkpoint {
        int64_t frame = 0;
        int chainRow = 0;
        int songRow = 0;
        int songChainRow = 0;
        // ⚠️ Filled by save_checkpoint(), never by the call sites, so none can forget.
        // ONE track's state: with eight lookaheads, each track has its own ring and boundary.
        TrackState trackState{};
        Rng        rng{};
        // ⚠️ LIVE's lap origin, for the same reason: left behind, a future frame beside a past cursor
        // makes the starvation guard rest a bar and the launch land a bar late.
        int64_t liveLoopFrame = 0;
    };

    int64_t getCurrentFrame() const { return currentFrame_; }

    // ⚠️ By value; the state is captured HERE, below the four call sites.
    void save_checkpoint(int trackId, Checkpoint cp) {
        cp.trackState = trackStates_[trackId];
        cp.rng = rngs_[trackId];
        cp.liveLoopFrame = liveLoopFrame_[trackId];
        checkpoints_[trackId].push_back(cp);
        // A ring of 4; the oldest is the earliest unplayed.
        if (checkpoints_[trackId].size() > 4) checkpoints_[trackId].pop_front();
    }

    // APPEND, never overwrite. A song shorter than the two-phrase lookahead comes back round to a
    // (songRow, chainRow) it already queued; overwriting would replace the frame of the row SOUNDING
    // NOW with its next occurrence and freeze the playhead. Duplicates cannot pile up: prune_past runs
    // on every read and the lookahead is bounded. The entry names the TRACK — eight cursors, eight
    // places.
    void put_song_position(int trackId, int songRow, int chainRow, int64_t frame) {
        songPositionStartFrames_.emplace_back(SongPos{trackId, songRow, chainRow}, frame);
    }

    /**
     * Stamp the row the walk is standing on — once per row that PLAYS (a groove `00` skips the row, so
     * the marker must too).
     * ⚠️ The one place that knows a row's real start frame (groove and HOP folded in); never derive it
     * a second time.
     * ⚠️ Capped: a render schedules a whole song through here and nobody reads (and so prunes) it
     * offline. The cap is far above any live lookahead; past it, the oldest half goes.
     */
    void put_phrase_step_position(int trackId, int step, int64_t frame) {
        if (phraseStepStartFrames_.size() >= STEP_POSITION_CAP)
            phraseStepStartFrames_.erase(
                phraseStepStartFrames_.begin(),
                phraseStepStartFrames_.begin() + static_cast<long>(STEP_POSITION_CAP / 2));
        phraseStepStartFrames_.emplace_back(StepPos{trackId, step}, frame);
    }

    /** The row `trackId` is on at `currentFrame`: the latest one stamped at or before it, else −1. */
    int step_in_force(int trackId, int64_t currentFrame) const {
        int     step  = -1;
        int64_t stamp = 0;
        for (const auto& e : phraseStepStartFrames_) {
            if (e.first.track != trackId || e.second > currentFrame) continue;
            if (step < 0 || e.second >= stamp) { stamp = e.second; step = e.first.step; }
        }
        return step;
    }

    // Drop entries more than `framesPerPhrase` in the past; run on every position read so the scan
    // stays bounded.
    template <typename C>
    static void prune_past(C& c, int64_t currentFrame, int64_t framesPerPhrase) {
        c.erase(std::remove_if(c.begin(), c.end(),
                               [&](const typename C::value_type& e) {
                                   return currentFrame > e.second + framesPerPhrase;
                               }),
                c.end());
    }

    /**
     * Drop the position entries a rollback invalidated — everything this cursor recorded at or past
     * the frame it was rewound to. `match` picks the cursor's own entries.
     * ⚠️ Dropping the queued notes cannot do this: a marker is not an event. Left behind, a stale entry
     * from the discarded schedule can win the lookup, and the marker sits on the row a LIVE launch left
     * for as many bars as were buffered.
     */
    template <typename C, typename Match>
    static void drop_positions_from(C& c, int64_t frame, Match match) {
        c.erase(std::remove_if(c.begin(), c.end(),
                               [&](const typename C::value_type& e) {
                                   return e.second >= frame && match(e.first);
                               }),
                c.end());
    }

    // The next row at or after `startRow` holding a phrase, wrapping; -1 if the chain is empty.
    // `startRow` is normalised HERE (playChain passes CHAIN_ROWS for a chain starting on its last
    // row), so no caller can get the wrap wrong.
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
    // Each track owns a frame, a song row and a chain row.

    // The chain a track has on one song row, or −1 for a blank cell (an out-of-range id is blank too).
    static int song_cell_chain(const Project& project, int trackId, int songRow) {
        const std::vector<int>& refs = project.tracks[trackId].chainRefs;
        if (songRow < 0 || songRow >= static_cast<int>(refs.size())) return -1;
        const int id = refs[songRow];
        return (id >= 0 && id < 256) ? id : -1;
    }

    /**
     * Can the walk ENTER this song cell? The one definition of a block boundary.
     * ⚠️ A cell the walk cannot enter ENDS A BLOCK — it is not a rest. A track reaching one loops back to
     * the top of its block, for ever (as M8 and LGPT do), so unrelated sketches can share a project.
     * Unequal blocks drift apart; a track that should rest a few bars needs a chain of empty phrases.
     * ⚠️ A chain whose FIRST row is empty is a boundary too. Later holes are walked over
     * (`next_chain_row_no_wrap`).
     */
    static bool song_cell_plays(const Project& project, int trackId, int songRow) {
        const int chainId = song_cell_chain(project, trackId, songRow);
        return chainId >= 0 && !chain_is_empty(project.chains[chainId], 0);
    }

    /** The first row of the block `songRow` sits in — the row a track loops back to. */
    static int block_start_row(const Project& project, int trackId, int songRow) {
        int row = songRow;
        while (row > 0 && song_cell_plays(project, trackId, row - 1)) row--;
        return row;
    }

    // The next row at or after `startRow` holding a phrase, or −1 when the chain has no more.
    // ⚠️ NO WRAP: inside a song, running out is what moves the track to its next row.
    static int next_chain_row_no_wrap(const Chain& chain, int startRow) {
        for (int r = std::max(0, startRow); r < CHAIN_ROWS; ++r)
            if (!chain_is_empty(chain, r)) return r;
        return -1;
    }

    /**
     * One song row finished for this track: step down the column, or LOOP BACK to its block's top.
     * ⚠️ A RENDER ENDS the track where playback would loop (`lastSongRow` ≥ 0 only there) — a forever
     * loop has no length. Repetition in a file is the render's own count.
     */
    void advance_track_song_row(const Project& project, int trackId, int lastSongRow) {
        const int from = trackSongRow_[trackId];
        trackChainRow_[trackId] = 0;
        trackStates_[trackId].trackStopped = false;
        trackStates_[trackId].emptyHops = 0;

        const bool bounded = lastSongRow >= 0;
        if ((!bounded || from + 1 <= lastSongRow) && song_cell_plays(project, trackId, from + 1)) {
            trackSongRow_[trackId] = from + 1;
            return;
        }
        if (bounded) { trackDone_[trackId] = true; return; }
        trackSongRow_[trackId] = block_start_row(project, trackId, from);
    }

    // The snapshot every unit of work takes before it commits — a phrase, a bar of rest, a bar sat
    // out after HOP FF. ⚠️ A unit with no checkpoint is one `notify_data_changed` cannot revise.
    void checkpoint_track(int trackId, int songRow, int rowUnit, bool take) {
        if (!take) return;
        Checkpoint cp;
        cp.frame = trackNextFrame_[trackId];
        cp.songRow = songRow;
        cp.songChainRow = rowUnit;
        save_checkpoint(trackId, cp);
    }

    // Advance ONE track by one unit: a phrase, a bar sat out, or the end of its block (in SONG, a loop
    // — see advance_track_song_row).
    // `lastSongRow` bounds the RENDER path's walk (−1 = the column's own end); it takes no checkpoints.
    void schedule_track_unit(const Project& project, int trackId, int64_t framesPerStep,
                             int64_t framesPerPhrase, int lastSongRow = -1,
                             bool takeCheckpoint = true) {
        // LIVE is a separate function so SONG's path stays exactly as goldened. Never on the RENDER
        // path — an export has no transport to queue at.
        if (liveMode_ && lastSongRow < 0) {
            schedule_live_unit(project, trackId, framesPerStep, framesPerPhrase, takeCheckpoint);
            return;
        }

        TrackState& trackState = trackStates_[trackId];
        const int songRow = trackSongRow_[trackId];

        // ⚠️ A cell the walk cannot enter SILENCES this track until STOP — it does not search upward.
        // Pressing PLAY on a row this column leaves blank must not start a block from elsewhere in the
        // arrangement; silence is what isolating a sketch means.
        // ⚠️ Re-derived per unit, never latched at PLAY: the project is edited under a running transport.
        if ((lastSongRow >= 0 && songRow > lastSongRow) ||
            !song_cell_plays(project, trackId, songRow)) {
            trackDone_[trackId] = true;
            return;
        }

        const Chain& chain = project.chains[song_cell_chain(project, trackId, songRow)];

        // HOP FF stopped this track: it sits out the rest of its chain, a bar at a time, and rejoins on
        // the next song row.
        if (trackState.trackStopped) {
            const int satOut = next_chain_row_no_wrap(chain, trackChainRow_[trackId]);
            if (satOut < 0) { advance_track_song_row(project, trackId, lastSongRow); return; }
            checkpoint_track(trackId, songRow, satOut, takeCheckpoint);
            trackNextFrame_[trackId] += framesPerPhrase;
            trackChainRow_[trackId] = satOut + 1;
            return;
        }

        const int chainRow = next_chain_row_no_wrap(chain, trackChainRow_[trackId]);
        if (chainRow < 0) { advance_track_song_row(project, trackId, lastSongRow); return; }   // chain spent

        checkpoint_track(trackId, songRow, chainRow, takeCheckpoint);

        // ⚠️ NO AUDIBILITY TEST — mute is a MIXER gate, never a sequencer one. A muted track is scheduled
        // like any other and the engine zeroes it (`setTrackMuted`), so unmuting mid-phrase lands in
        // the sequence where it really is. (LGPT does the same.)
        const int transposeSemitones = chain_transpose_semitones(chain, chainRow)
                                       + project_transpose_semitones(project);
        const int hopStartRow = trackState.consumeHopTarget();
        const int effectiveStartRow = hopStartRow >= 0 ? hopStartRow : 0;
        SchedulePhraseResult r = schedulePhrase(project.phrases[chain_phrase_ref(chain, chainRow)],
                                                trackNextFrame_[trackId], trackId, transposeSemitones,
                                                framesPerStep, effectiveStartRow, &chain, chainRow);
        // ⚠️ Recorded for a muted track too: the playhead says where the track IS.
        put_song_position(trackId, songRow, chainRow, trackNextFrame_[trackId]);
        trackNextFrame_[trackId] += r.framesScheduled;
        trackChainRow_[trackId] = chainRow + 1;
    }

    // ─── LIVE mode's unit of work ────────────────────────────────────────────────────────────────
    //
    // The launched song row REPEATS: a spent chain re-enters the SAME row, so a LIVE track never moves
    // down or runs out of its column. Separate from its SONG twin so SONG's path gains no branch.
    void schedule_live_unit(const Project& project, int trackId, int64_t framesPerStep,
                            int64_t framesPerPhrase, bool takeCheckpoint) {
        TrackState& trackState = trackStates_[trackId];

        // Every unit begins on a phrase boundary, so an IMMEDIATE queue lands here; a chain-boundary
        // one lands here too, on the unit that BEGINS A LAP.
        // ⚠️ Asks "is the cursor at a lap start", NOT "did the chain just run out": after a rewind into
        // the last bar of a chain, the lookahead has already passed the end, and the second question
        // would land the launch one whole repeat late.
        // A silent channel's cursor never leaves 0, so it is always at a lap start — no extra term.
        consume_live_queue(trackId, /*chainEnd=*/trackChainRow_[trackId] == 0);

        const std::vector<int>& refs = project.tracks[static_cast<size_t>(trackId)].chainRefs;
        const int songRow = trackSongRow_[trackId];
        const int chainId = (songRow >= 0 && songRow < static_cast<int>(refs.size()))
                                ? refs[static_cast<size_t>(songRow)] : -1;

        // ⚠️ A SILENT channel (stopped, or launched on an empty cell) still spends its bar, so its clock
        // stays on the others' grid — and takes its checkpoint.
        if (liveSilent_[trackId] || chainId < 0 || chainId >= 256) {
            checkpoint_track(trackId, songRow, trackChainRow_[trackId], takeCheckpoint);
            trackNextFrame_[trackId] += framesPerPhrase;
            return;
        }

        const Chain& chain = project.chains[static_cast<size_t>(chainId)];

        // HOP FF sat this track out: it rests to the end of the chain, a bar at a time, as in SONG.
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
            // The one place a chain-boundary queue lands and a loop happens.
            trackChainRow_[trackId] = 0;
            trackState.trackStopped = false;
            trackState.emptyHops    = 0;   // a rejoining track starts the hop-ring count clean

            // ⚠️ A lap that cost nothing RESTS a bar instead of looping — re-entering it would keep this
            // track furthest behind on every pass and starve the other seven (liveLoopFrame_).
            if (trackNextFrame_[trackId] == liveLoopFrame_[trackId]) {
                checkpoint_track(trackId, songRow, 0, takeCheckpoint);
                trackNextFrame_[trackId] += framesPerPhrase;
                liveLoopFrame_[trackId] = trackNextFrame_[trackId];
                return;
            }

            // The queue is NOT read here: the cursor is now at a lap start, which the unit's first
            // line tests on the next pass — one question, asked in one place.
            liveLoopFrame_[trackId] = trackNextFrame_[trackId];   // the next lap of the same row
            return;
        }

        checkpoint_track(trackId, songRow, chainRow, takeCheckpoint);

        // ⚠️ No audibility test — mute is a mixer gate (see the SONG twin).
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

    // `chain`/`chainRow` locate the phrase in the chain being played, for AUS/AUF: a fade may span
    // into a later phrase. PHRASE mode passes nullptr and pairs within the phrase.
    SchedulePhraseResult schedulePhrase(const Phrase& phrase, int64_t startFrame, int trackId,
                                        int transposeSemitones, int64_t framesPerStep, int startRow,
                                        const Chain* chain = nullptr, int chainRow = 0) {
        const Project& project = *project_;
        int rowsScheduled = 0;
        TrackState& trackState = trackStates_[clampi(trackId, 0, 7)];
        // Every random draw happens inside this call, so the track's stream is selected once here.
        schedulingTrack_ = clampi(trackId, 0, 7);

        if (trackState.trackStopped) return SchedulePhraseResult{0, false, true, 0};

        int localGrooveStep = trackState.grooveStep;
        bool anyGrooveActive = false;

        int effectiveStartRow = clampi(startRow, 0, 15);
        int64_t frameOffset = 0;

        // ─── The ramps this phrase declares (AUS/AUF — automation.h) ─────────────────────────────
        //
        // Pairing gives spans in step indices; the walk below emits them using each step's real
        // duration — so grooves cost nothing and a HOP truncates a fade by ending the walk. A phrase
        // entered below its AUS runs no ramp. With a chain, spans can cross phrases
        // (`find_ramps_in_chain`, re-derived every time).
        const std::vector<RampSpec> ramps =
            chain ? find_ramps_in_chain(project, *chain, chainRow, effectiveStartRow)
                  : find_ramps(phrase, effectiveStartRow);
        // The last value each ramp emitted, for de-duplication (an ease curve holds one byte for many
        // ticks). Seeded with the curve's value one tic BEFORE this phrase: where the AUS is here that
        // clamps to the start byte the start effect already sent; in a crossed phrase it is where the
        // previous phrase left off. An EQ morph seeds the same way against its start preset.
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

            // Pre-scan GRV so a new groove takes effect on its own step; the last GRV wins.
            for (int fxSlot = 1; fxSlot <= 3; ++fxSlot) {
                if (step_fx_type(step, fxSlot) == FX_GRV) {
                    trackState.grooveId = step_fx_value(step, fxSlot);
                    localGrooveStep = 0;
                }
            }

            const Groove& currentGroove = project.grooves[clampi(trackState.grooveId, 0,
                                                                 static_cast<int>(project.grooves.size()) - 1)];
            bool currentGrooveActive = groove_active_length(currentGroove) > 0;

            // The length comes from `timing.h` — one definition, the one the tools measure.
            if (currentGrooveActive) anyGrooveActive = true;
            int64_t stepDuration = groove_step_duration(currentGroove, localGrooveStep, framesPerStep);

            if (stepDuration == 0) {
                rowsScheduled++;
                localGrooveStep++;
                continue;
            }

            int64_t targetFrame = startFrame + frameOffset;

            stepRamps_ = ramps.empty() ? nullptr : &ramps;
            stepIndex_ = stepIndex;
            ScheduleStepResult stepResult = scheduleStepWithEffects(step, targetFrame, stepDuration, trackId,
                                                                    transposeSemitones, trackState, stepIndex);
            stepRamps_ = nullptr;

            // ⚠️ A HOP row costs NOTHING — no time, no marker, no ramp tic; it IS the jump. So the walk
            // leaves before `frameOffset`, the playhead or a fade moves. `localGrooveStep` stays too,
            // or the next phrase enters on the wrong tic.
            if (stepResult.hopTriggered) {
                if (anyGrooveActive) trackState.grooveStep = localGrooveStep;
                // A hop leaving with nothing played may be part of a ring (TrackState::emptyHops).
                if (frameOffset == 0) {
                    if (++trackState.emptyHops > MAX_EMPTY_HOPS) trackState.trackStopped = true;
                } else {
                    trackState.emptyHops = 0;
                }
                return SchedulePhraseResult{rowsScheduled, true, trackState.trackStopped, frameOffset};
            }

            // The playhead's row stamp, made as the walk passes (put_phrase_step_position).
            put_phrase_step_position(trackId, stepIndex, targetFrame);

            // After the step's own events: a ramp is emitted as the walk passes, never ahead — a fade
            // baked past a HOP would keep moving the parameter after the phrase ended.
            if (!ramps.empty())
                emit_ramp_ticks(ramps, rampLast, chain ? chain->id : -1, stepResult.effectiveStep, stepIndex, targetFrame,
                                stepDuration, trackId, trackState, stepResult.noteFrame, stepResult.fxFrame);
            rowsScheduled++;
            frameOffset += stepDuration;
            if (currentGrooveActive) localGrooveStep++;
        }

        if (anyGrooveActive) trackState.grooveStep = localGrooveStep;
        if (rowsScheduled > 0) trackState.emptyHops = 0;
        return SchedulePhraseResult{rowsScheduled, false, false, frameOffset};
    }

    // ─── AUS / AUF — a declared span, emitted as the walk crosses it ────────────────────────────────
    //
    // One CC per tic at the byte the curve holds there, sent as `byte / 255` like the per-step effect
    // — every value is one the goldens already contain.
    // `t` is measured in STEPS, not frames: `(stepsSoFar + tic/12) / span`, so a fade covers an exact
    // fraction at every step boundary whatever the groove does, with no look-ahead.
    // A tic is `stepDuration / TICS_PER_STEP` — the same warped grid LAT and KIL use.
    // Where a ramp has got to: a BYTE ramp's last byte, or an EQ morph's last band set.
    struct RampLastValue {
        int               byte = 0;
        ExtEqMorphPayload eq{};
    };

    void emit_ramp_ticks(const std::vector<RampSpec>& ramps, std::vector<RampLastValue>& lastValue,
                         int chainId, const PhraseStep& effectiveStep, int stepIndex, int64_t targetFrame,
                         int64_t stepDuration, int trackId, TrackState& trackState,
                         int64_t noteFrame, int64_t fxFrame) {
        const int64_t framesPerTic = stepDuration / TICS_PER_STEP;

        // A parameter on a note's own frame would reach the voice that note REPLACES (why STEP 2.3 is
        // one frame late). A coinciding tic takes the same +1. `noteFrame`, because LAT may have moved
        // the note within the step.
        auto place = [noteFrame](int64_t frame) { return frame == noteFrame ? frame + 1 : frame; };

        for (size_t i = 0; i < ramps.size(); ++i) {
            const RampSpec& r = ramps[i];
            // −1 at either end = that end is in another phrase; a phrase the span merely crosses emits
            // all sixteen steps.
            if (r.ausStep >= 0 && stepIndex < r.ausStep) continue;
            if (r.aufStep >= 0 && stepIndex > r.aufStep) continue;

            // A CHA that ate the AUS eats the ramp, on every step of the span in every phrase it
            // crosses. Pairing reads the authored step, so the roll is consulted here.
            if (r.ausStep >= 0 && stepIndex == r.ausStep)
                trackState.set_aus_eaten(chainId, r.originAbs, r.originSlot,
                                         step_fx_type(effectiveStep, r.ausSlot) != FX_AUS);
            if (trackState.aus_eaten(chainId, r.originAbs, r.originSlot)) continue;

            const int lane = r.global ? TRACK_GLOBAL : trackId;

            // ⚠️ VTR/VMV/TIM replace engine state and the host restores it on stop() when these flags
            // say so. Set here too, keyed on the CC the ramp SENDS: a CHA zeroing the start slot would
            // otherwise leave a fade nothing restores.
            if (r.ccId == CC_TRACK_VOL)  mixerVolTracks_ |= 1 << clampi(trackId, 0, 7);
            if (r.ccId == CC_MASTER_VOL) masterVolActive_ = true;
            if (r.ccId == CC_DELAY_TIME) delayTimeActive_ = true;
            // ⚠️ EQM: same debt, keyed the same way.
            if (r.kind == RampKind::EQ_PRESET && r.global) eqmActive_ = true;

            // ⚠️ A step that writes this parameter itself OWNS its frame: the ramp yields and resumes
            // after it. Two updates due on one frame are ordered by the heap, not by emission
            // (note-queue.h), so the author could not tell which they would hear.
            // It yields to where the write ACTUALLY LANDED (`fxFrame` — LAT, unclamped, plus a frame
            // after a note-on), never to a re-derivation.
            // Read off the EFFECTIVE step: a `RND` that became a VOL takes its frame; a `CHA` that ate one
            // gives it back (the de-dup then drops the redundant start byte).
            const bool ownsStep = step_has_fx(effectiveStep, r.fxCode);
            const bool perVoice = ramp_moves_voice(r);

            // ⚠️ A new note inside the fade starts from the instrument, and an ease curve's next tic may
            // not move — so the fade is re-asserted one frame after the note, whatever it holds. VOL and
            // PAN need not: the note-on carries the fade's value (`voice_at`), as do ARP/RPT retriggers.
            int64_t reassertAt = (noteFrame >= 0 && perVoice && !ramp_rides_note_on(r) && !ownsStep)
                               ? noteFrame + 1 : -1;
            auto emit_byte = [&](int64_t frame, int b, bool force) {
                if (!force && b == lastValue[i].byte) return;
                router_.cc(frame, lane, r.ccId, b / 255.0f);
                lastValue[i].byte = b;
                if (perVoice) carry_fade(trackState.carry, r, b);
            };
            auto emit_eq = [&](int64_t frame, const ExtEqMorphPayload& m, bool force) {
                if (!force && eq_morph_equal(m, lastValue[i].eq)) return;
                emit_eq_morph(frame, r, trackId, m);
                lastValue[i].eq = m;
                if (perVoice) { trackState.carry.eqMorph = m; trackState.carry.eqMorphed = true; }
            };
            auto reassert_held = [&]() {
                if (r.kind == RampKind::EQ_PRESET) emit_eq(reassertAt, lastValue[i].eq, true);
                else                               emit_byte(reassertAt, lastValue[i].byte, true);
                reassertAt = -1;
            };

            // The arrival sends the destination byte as typed, on the AUF's own step (so a fade across
            // eight steps lands WITH the note on the eighth) — a frame after the step's own write of
            // the parameter, if it has one.
            // ⚠️ An EQ morph arrives at t=1, which equals the destination preset only when the band types
            // agree (the start preset's types hold throughout). Snapping to the destination would undo
            // the fade; landing on it is one visible cell, `EQM xx` on the next step.
            if (stepIndex == r.aufStep) {
                const int64_t arriveFrame = place(ownsStep ? fxFrame + 1 : targetFrame);
                const bool force = arriveFrame == reassertAt;
                if (r.kind == RampKind::EQ_PRESET)
                    emit_eq(arriveFrame, eq_morph_at(*project_, r.startByte, r.destByte, r.curveByte, 1.0), force);
                else
                    emit_byte(arriveFrame, r.destByte, force);
                if (reassertAt > arriveFrame) reassert_held();
                continue;
            }

            int firstTic = 0;
            if (ownsStep)
                while (firstTic < TICS_PER_STEP && targetFrame + firstTic * framesPerTic <= fxFrame)
                    ++firstTic;
            for (int tic = firstTic; tic < TICS_PER_STEP; ++tic) {
                // `stepOffset + stepIndex` = steps since the AUS, wherever it is; `span` is the whole ramp,
                // so a fade across four phrases is one curve.
                const double t = (static_cast<double>(r.stepOffset + stepIndex) +
                                  tic / static_cast<double>(TICS_PER_STEP)) / static_cast<double>(r.span);
                const int64_t frame = place(targetFrame + tic * framesPerTic);
                bool force = false;
                if (reassertAt >= 0 && frame >= reassertAt) {
                    if (frame == reassertAt) { force = true; reassertAt = -1; }
                    else reassert_held();
                }
                if (r.kind == RampKind::EQ_PRESET)
                    emit_eq(frame, eq_morph_at(*project_, r.startByte, r.destByte, r.curveByte, t), force);
                else
                    emit_byte(frame, automation_value_byte(r.startByte, r.destByte, r.curveByte, t), force);
            }
            if (reassertAt >= 0) reassert_held();
        }
    }

    // EQM rides TRACK_GLOBAL and EQN the track's lane, like the per-step pair.
    void emit_eq_morph(int64_t frame, const RampSpec& r, int trackId, const ExtEqMorphPayload& m) {
        if (r.global) router_.ext_master_eq_morph(frame, m);
        else          router_.ext_eq_morph(frame, trackId, m);
    }

    // CHA gate + RND/RNL randomize, before effect resolution. With no CHA/RND/RNL slot the step comes
    // back unchanged — why the goldens can be byte-compared at all. A test checks the draws.
    PhraseStep applyChanceAndRandomize(const PhraseStep& step, TrackState& trackState, bool& skipNote) {
        bool hasNote = !step_empty(step);
        skipNote = false;
        PhraseStep effectiveStep = step;
        // CHA XY: X is the chance of the nearest filled FX column on the LEFT (or the note, if none),
        // Y of the one on the RIGHT (nothing, if none); 0 never, F always (a roll is 0-14). A CHA an
        // earlier one cleared gates nothing.
        for (int slot = 1; slot <= 3; ++slot) {
            if (step_fx_type(effectiveStep, slot) != FX_CHA) continue;
            const int value = step_fx_value(effectiveStep, slot);
            int left = slot - 1, right = slot + 1;
            while (left >= 1 && step_fx_type(effectiveStep, left) == FX_NONE) --left;
            while (right <= 3 && step_fx_type(effectiveStep, right) == FX_NONE) ++right;
            if (rng_int(15) >= ((value >> 4) & 0x0F)) {
                if (left >= 1) step_set_fx(effectiveStep, left, 0x00, 0x00);
                else           skipNote = true;
            }
            if (right <= 3 && rng_int(15) >= (value & 0x0F)) step_set_fx(effectiveStep, right, 0x00, 0x00);
        }
        // RND/RNL ADD a random 0..XY to the value already there, capped at the effect's ceiling (as
        // on the M8). 00 adds and draws nothing. In FX1, RNL adds 0..X to the note and 0..Y to the
        // instrument instead.
        const int lastInstrument = (project_ && !project_->instruments.empty())
                                 ? static_cast<int>(project_->instruments.size()) - 1 : 127;
        int instOffset = 0;
        auto add_random = [this](int base, int range, int ceiling) {
            const int added = range > 0 ? rng_range(0, range + 1) : 0;
            return clampi(base + added, 0, ceiling);
        };
        for (int slot = 1; slot <= 3; ++slot) {
            int fxType = step_fx_type(effectiveStep, slot);
            int fxValue = step_fx_value(effectiveStep, slot);
            if (fxType == FX_RND) {
                int prevType = trackState.lastColFxType[slot];
                if (prevType == 0x00) continue;
                int base = trackState.lastColFxValue[slot];
                step_set_fx(effectiveStep, slot, prevType, add_random(base, fxValue, effect_value_max(prevType)));
            } else if (fxType == FX_RNL) {
                if (slot == 1) {
                    if (hasNote) {
                        int noteMidi = note_to_midi(step.note);
                        if (noteMidi >= 0) {
                            int noteRange = (fxValue >> 4) & 0x0F, instRange = fxValue & 0x0F;
                            effectiveStep.note = note_from_midi(add_random(noteMidi, noteRange, 127));
                            instOffset = instRange > 0 ? rng_range(0, instRange + 1) : 0;
                            effectiveStep.instrument = clampi(step.instrument + instOffset, 0, lastInstrument);
                        }
                    }
                } else {
                    int targetSlot = slot - 1;
                    int targetType = step_fx_type(effectiveStep, targetSlot);
                    int base = step_fx_value(effectiveStep, targetSlot);
                    step_set_fx_value(effectiveStep, targetSlot,
                                      add_random(base, fxValue, effect_value_max(targetType)));
                }
            }
        }
        // INS reads the step after RND/RNL, so a randomized INS is the one heard, and FX1 RNL's
        // instrument offset lands on top of it.
        const int insInstrument = step_ins_instrument(effectiveStep);
        if (hasNote && insInstrument >= 0)
            effectiveStep.instrument = clampi(insInstrument + instOffset, 0, lastInstrument);
        return effectiveStep;
    }

    // `stepIndex` is unused; named only in a comment so the compiler does not warn.
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
        const float savedRampPhraseVol = trackState.repeatBasePhraseVol;
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

        // ⚠️ A ROW THAT HOPS IS NEVER HEARD — as on a TABLE's steering row (`processTableRow`). The jump
        // is the whole row: no note, no effects, NO TIME, so a four-row loop lasts four rows. A note
        // beside a HOP does not sound; put it on the row above.
        // ⚠️ Decided on the RESOLVED step: a `CHA` can gate the HOP away, and then the row plays.
        // A running REPEAT or ARPEGGIO still ends here (STEP 1 has run) — leaving the phrase ends them.
        if (params.hopValue.has_value()) {
            if (*params.hopValue == 0xFF) trackState.trackStopped = true;
            else                          trackState.hopTargetRow = *params.hopValue & 0x0F;
            ScheduleStepResult hop;
            hop.hopTriggered  = true;
            hop.effectiveStep = effectiveStep;
            return hop;
        }

        // TSX and the instrument's TRANSP. switch, folded in by reassignment so no later site (note,
        // REPEAT retrigger, arpeggio) can reach the unscaled value.
        transposeSemitones = effective_transpose_semitones(transposeSemitones, project,
                                                           effectiveStep.instrument,
                                                           params.tsxMultiplier);

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

        // SCA / SCG, above the note so the command's own step is already in the new scale (as GRV).
        // SCG writes all eight TrackStates; on the same step SCA is applied second and wins.
        if (params.scaleGlobalByte.has_value()) {
            const int key = scale_cmd_key(*params.scaleGlobalByte);
            const int slot = scale_cmd_slot(*params.scaleGlobalByte);
            for (int t = 0; t < 8; ++t) { trackStates_[t].scaleSlot = slot; trackStates_[t].scaleKey = key; }
        }
        if (params.scaleTrackByte.has_value()) {
            trackState.scaleSlot = scale_cmd_slot(*params.scaleTrackByte);
            trackState.scaleKey  = scale_cmd_key(*params.scaleTrackByte);
        }

        // What `voice_at` needs to know about this step — set before the first note-on it emits.
        stepEffective_  = &effectiveStep;
        stepTarget_     = targetFrame;
        stepDuration_   = stepDuration;
        stepNoteFrame_  = (hasNote && !skipNote) ? effectiveTargetFrame : -1;
        stepFxFrame_    = (hasNote && !skipNote) ? effectiveTargetFrame + 1 : effectiveTargetFrame;
        stepCarryBefore_ = trackState.carry;
        stepCarryFrom_   = effectiveTargetFrame;

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

            NoteCarry& carry = trackState.carry;
            carry = NoteCarry{};
            carry.velGain = velocityGain;
            carry.phraseVol = instrVolWithVxx;
            carry.pan = notePan;
            carry.vibSpeed = vibratoSpeed;
            carry.vibDepth = vibratoDepth;
            record_voice_commands(carry, params);
            // A note inside a VOL or PAN fade starts where the fade has got to; the fade's next tic need
            // not move to correct it.
            {
                const NoteCarry v = voice_at(trackState, effectiveTargetFrame);
                carry.phraseVol = v.phraseVol;
                carry.pan = v.pan;
            }

            NoteArgs a;
            a.frame = effectiveTargetFrame; a.track = trackId; a.instrument = effectiveStep.instrument;
            a.notePitch = note.pitch; a.noteOctave = note.octave;
            a.velocity = velocityByte; a.velGain = velocityGain; a.volGain = carry.phraseVol; a.pan = carry.pan;
            a.start = params.startPoint; a.slice = params.sliIndex.value_or(-1);
            a.transpose = transposeSemitones; a.pit = params.pitSemitones.value_or(0); a.arp = 0;
            a.tableId = tableIdOverride; a.tableRow = tableStartRow;
            a.pslOff = pslInitialOffset; a.pslDur = pslDuration; a.pbnRate = pbnRate;
            a.vibSpd = vibratoSpeed; a.vibDep = vibratoDepth;
            emit_note(a, note);
            noteScheduled = true;

            trackState.lastNote = note;
            trackState.lastInstrument = effectiveStep.instrument;
            trackState.lastStartPoint = params.startPoint;
            trackState.lastNoteMidi = note_to_midi(note);

            if (trackState.hasPitchMod() && pbnRate == 0.0f && vibratoDepth == 0.0f) trackState.clearPitchMod();
        }

        int64_t scheduledNoteFrame = noteScheduled ? effectiveTargetFrame : -1;
        // STEP 2.3's frame, hoisted: a crossing ramp yields to it (ScheduleStepResult).
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
            if (!triggeredNote && params.panValue.has_value()) {
                router_.cc(effectiveTargetFrame, trackId, CC_PAN, *params.panValue / 255.0f);
                trackState.carry.pan = *params.panValue / 255.0f;
            }
            // The note step recorded its own above, on a freshly reset carry.
            if (!triggeredNote) record_voice_commands(trackState.carry, params);
            if (params.reverbSendValue.has_value())
                router_.cc(voiceFxFrame, trackId, CC_REVERB_SEND, *params.reverbSendValue / 255.0f);
            if (params.delaySendValue.has_value())
                router_.cc(voiceFxFrame, trackId, CC_DELAY_SEND, *params.delaySendValue / 255.0f);
            if (params.bckValue.has_value())
                router_.ext_reverse(voiceFxFrame, trackId, *params.bckValue == 0, triggeredNote);
            // CUT / RES at `voiceFxFrame`, like REV and DEL: on a note step, a param at the note's own
            // frame reaches the voice the note REPLACES.
            if (params.filterCutValue.has_value())
                router_.cc(voiceFxFrame, trackId, CC_FILTER_CUT, *params.filterCutValue / 255.0f);
            if (params.filterResValue.has_value())
                router_.cc(voiceFxFrame, trackId, CC_FILTER_RES, *params.filterResValue / 255.0f);
            // LPF / HPF / BPF: one record, the type in the CC ID and the cutoff in the value, so the
            // two cannot land a block apart (event.h).
            if (params.filterModeValue.has_value())
                router_.cc(voiceFxFrame, trackId,
                           params.filterModeType == 1 ? CC_FILTER_LP :
                           params.filterModeType == 2 ? CC_FILTER_HP : CC_FILTER_BP,
                           *params.filterModeValue / 255.0f);
            // DRV / CRU at `voiceFxFrame` likewise. CRU's byte goes over whole; the engine splits it.
            if (params.driveValue.has_value())
                router_.cc(voiceFxFrame, trackId, CC_DRIVE, *params.driveValue / 255.0f);
            if (params.crushValue.has_value())
                router_.cc(voiceFxFrame, trackId, CC_CRUSH, *params.crushValue / 255.0f);
            // FIN at `voiceFxFrame`: the +1 makes it tune the note on its own step, not the one replaced.
            if (params.fineTuneValue.has_value())
                router_.cc(voiceFxFrame, trackId, CC_FINE_TUNE, *params.fineTuneValue / 255.0f);
            // LPO sends the AUTHORED byte (the engine decodes it), at `voiceFxFrame` so a slide on a
            // note step moves THAT note's window.
            if (params.loopSlideValue.has_value())
                router_.cc(voiceFxFrame, trackId, CC_LOOP_SLIDE,
                           (*params.loopSlideValue & 0xFF) / 255.0f);
            if (params.eqnSlot.has_value())
                router_.ext_eq_slot(voiceFxFrame, trackId, *params.eqnSlot);
            // The mixer faders REPLACE the authored value and hold, so the host restores it on stop()
            // (as for EQM).
            if (params.trackVolValue.has_value()) {
                router_.cc(voiceFxFrame, trackId, CC_TRACK_VOL, *params.trackVolValue / 255.0f);
                mixerVolTracks_ |= 1 << clampi(trackId, 0, 7);
            }
            if (params.masterVolValue.has_value()) {
                // TRACK_GLOBAL: the master belongs to no track, and the track lane is where the
                // EXTERNAL gate would swallow it (event.h).
                router_.cc(effectiveTargetFrame, TRACK_GLOBAL, CC_MASTER_VOL,
                           *params.masterVolValue / 255.0f);
                masterVolActive_ = true;
            }
            if (params.delayTimeValue.has_value()) {
                // TRACK_GLOBAL for the same reason: the delay send belongs to no track.
                router_.cc(effectiveTargetFrame, TRACK_GLOBAL, CC_DELAY_TIME,
                           *params.delayTimeValue / 255.0f);
                delayTimeActive_ = true;
            }
            if (params.eqmSlot.has_value()) {
                // Master EQ — global, held until the next EQM; the host restores it on stop().
                router_.ext_master_eq(effectiveTargetFrame, *params.eqmSlot);
                eqmActive_ = true;
            }

            // ── MPG / MPB / CCA-CCD ──────────────────────────────────────────────────────────────
            //
            // ⚠️ They must come AFTER the step's note-on in BOTH orders that exist:
            //  • ARRIVAL (records are consumed as emitted): both consumers resolve the instrument from
            //    the last note-on, so a command on a step that changes instrument must follow it.
            //  • QUEUE (released by due frame): a note-on carries the instrument's CC-slot DEFAULTS
            //    (midi_out.h); the step's command must be released after them, or every CCA in the song
            //    is quietly undone. `voiceFxFrame` (+1 on a note step) does both, and keeps the engine's
            //    param off the old voice.
            // On an empty step `voiceFxFrame` is the step frame: the command acts on the sounding note.
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
            // ⚠️ Deliberate: an empty-step pitch rate uses the LIVE tempo during playback but 120 on the
            // render path (currentProject_ is set only by live starts). The goldens record it.
            int tempo = currentProject_ ? currentProject_->tempo : 120;
            if (params.volumeFromVxx) {
                router_.cc(effectiveTargetFrame, trackId, CC_VOLUME, instrVolWithVxx);
                trackState.carry.phraseVol = instrVolWithVxx;
            }
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
                    trackState.carry.vibSpeed = trackState.carry.vibDepth = 0.0f;
                } else {
                    int speedNibble = (v >> 4) & 0x0F;
                    int depthNibble = v & 0x0F;
                    float speed = (2.0f + speedNibble * 0.5f) * (tempo / 120.0f);
                    float depth = depthNibble * 0.125f;
                    router_.ext_vibrato(effectiveTargetFrame, trackId, speed, depth);
                    trackState.vibratoActive = true;
                    trackState.carry.vibSpeed = speed;
                    trackState.carry.vibDepth = depth;
                }
            }
            if (params.pvxValue.has_value()) {
                int v = *params.pvxValue;
                if (v == 0) {
                    router_.ext_vibrato(effectiveTargetFrame, trackId, 0.0f, 0.0f);
                    trackState.vibratoActive = false;
                    trackState.carry.vibSpeed = trackState.carry.vibDepth = 0.0f;
                } else {
                    int speedNibble = (v >> 4) & 0x0F;
                    int depthNibble = v & 0x0F;
                    float speed = (2.0f + speedNibble * 0.5f) * 2.0f * (tempo / 120.0f);
                    float depth = depthNibble * 0.125f * 4.0f;
                    router_.ext_vibrato(effectiveTargetFrame, trackId, speed, depth);
                    trackState.vibratoActive = true;
                    trackState.carry.vibSpeed = speed;
                    trackState.carry.vibDepth = depth;
                }
            }
        }

        // A row that hops returned above, so from here on the step is a played one.
        const bool hopTriggered = false;

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
            if (!hasNote && savedRampVolume >= 0.0f) {
                trackState.repeatBaseVolume    = savedRampVolume;
                trackState.repeatBasePhraseVol = savedRampPhraseVol;
            } else {
                trackState.repeatBaseVolume    = trackState.carry.velGain * trackState.carry.phraseVol;
                trackState.repeatBasePhraseVol = trackState.carry.phraseVol;
            }
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
                        // The ramp's product with the VOL channel it was taken at divided out;
                        // `emit_retrigger` multiplies in the channel as it stands NOW. A base taken at
                        // VOL 00 has nothing to divide, and ramps the velocity alone.
                        const float retrigVelGain = trackState.repeatBasePhraseVol > 0.0f
                            ? retrigVolume / trackState.repeatBasePhraseVol
                            : clampf(trackState.carry.velGain + trackState.repeatRetrigCount * rampDelta, 0.0f, 1.0f);
                        NoteArgs a;
                        a.frame = triggerFrame; a.track = trackId; a.instrument = retrigInstrument;
                        a.notePitch = retrigNote.pitch; a.noteOctave = retrigNote.octave;
                        a.velocity = -1; a.start = retrigStartPoint;
                        a.transpose = transposeSemitones; a.arp = 0;
                        a.tableId = trackState.lastTableOverride; a.tableRow = -1;
                        emit_retrigger(a, retrigNote, trackId, trackState, retrigVelGain);
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
                                  transposeSemitones, scheduledNoteFrame);
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
                               int transposeSemitones, int64_t scheduledNoteFrame) {
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
        if (framesPerArpNote <= 0) return;  // guard against division by zero

        int patternLength = trackState.arpeggioMode == 2 ? 4 : 3;

        int instrumentId = hasNote ? step.instrument : trackState.lastInstrument;
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
                    a.velocity = -1; a.start = startPoint;
                    a.transpose = transposeSemitones;
                    a.arp = arpMidi - baseMidi;
                    a.tableId = trackState.lastTableOverride; a.tableRow = -1;
                    emit_retrigger(a, baseNote, trackId, trackState);
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
            // RANDOM: a uniform draw over the three SLOTS, not the distinct pitches, so a chord whose
            // semitones collide (A00, A33) stays weighted by slot.
            case 3: { int notes[3] = {note0, note1, note2}; return notes[rng_int(3)]; }
            default: switch (position % 3) { case 0: return note0; case 1: return note1; default: return note2; }
        }
    }

    // ─── Retriggers and the note they repeat (NoteCarry) ────────────────────────────────────────

    // `reapply_voice` mask bits beyond the controller slots.
    static constexpr unsigned CARRY_EQ      = 1u << CARRY_CC_SLOTS;
    static constexpr unsigned CARRY_REVERSE = 1u << (CARRY_CC_SLOTS + 1);
    static constexpr unsigned CARRY_ALL     = ~0u;

    /** The per-voice commands of one step, into the carry — everything except VOL, PAN and the
     *  vibrato, which each have a value only their own emit site knows. */
    static void record_voice_commands(NoteCarry& c, const ResolvedStepParams& p) {
        if (p.pitSemitones.has_value())    c.pit = *p.pitSemitones;
        if (p.sliIndex.has_value())        c.slice = *p.sliIndex;
        if (p.reverbSendValue.has_value()) c.set_cc(CC_REVERB_SEND, *p.reverbSendValue / 255.0f);
        if (p.delaySendValue.has_value())  c.set_cc(CC_DELAY_SEND, *p.delaySendValue / 255.0f);
        // The filter switched on before CUT: it clears CUT, which on the same step comes after it.
        if (p.filterModeValue.has_value())
            c.set_cc(p.filterModeType == 1 ? CC_FILTER_LP : p.filterModeType == 2 ? CC_FILTER_HP : CC_FILTER_BP,
                     *p.filterModeValue / 255.0f);
        if (p.filterCutValue.has_value())  c.set_cc(CC_FILTER_CUT, *p.filterCutValue / 255.0f);
        if (p.filterResValue.has_value())  c.set_cc(CC_FILTER_RES, *p.filterResValue / 255.0f);
        if (p.driveValue.has_value())      c.set_cc(CC_DRIVE, *p.driveValue / 255.0f);
        if (p.crushValue.has_value())      c.set_cc(CC_CRUSH, *p.crushValue / 255.0f);
        if (p.fineTuneValue.has_value())   c.set_cc(CC_FINE_TUNE, *p.fineTuneValue / 255.0f);
        if (p.eqnSlot.has_value())         { c.eqnSlot = *p.eqnSlot; c.eqMorphed = false; }
        if (p.bckValue.has_value())        c.reverse = (*p.bckValue == 0) ? 1 : 0;
    }

    /** Where emit_ramp_ticks puts tic `k` of the step being scheduled. */
    int64_t fade_tic_frame(int k) const {
        const int64_t raw = stepTarget_ + k * (stepDuration_ / TICS_PER_STEP);
        return raw == stepNoteFrame_ ? raw + 1 : raw;
    }

    /**
     * How far fade `r` has moved the voice by frame `f` of the step being scheduled — its curve
     * position, 1.0 once arrived — or −1 where it is not moving it (outside its span, or while the
     * step's own write of the parameter holds).
     * ⚠️ Must follow emit_ramp_ticks tic for tic: a retrigger re-applies this value on a frame a tic
     * may also land on, and two updates on one frame are ordered arbitrarily — they must agree.
     */
    double fade_t_at(const RampSpec& r, int64_t f) const {
        if (!ramp_moves_voice(r)) return -1.0;
        if (r.ausStep >= 0 && stepIndex_ < r.ausStep) return -1.0;
        if (r.aufStep >= 0 && stepIndex_ > r.aufStep) return -1.0;
        const bool owns = stepEffective_ != nullptr && step_has_fx(*stepEffective_, r.fxCode);
        if (stepIndex_ == r.aufStep) {
            const int64_t raw = owns ? stepFxFrame_ + 1 : stepTarget_;
            return f >= (raw == stepNoteFrame_ ? raw + 1 : raw) ? 1.0 : -1.0;
        }
        const int64_t framesPerTic = stepDuration_ / TICS_PER_STEP;
        int firstTic = 0;
        if (owns)
            while (firstTic < TICS_PER_STEP && stepTarget_ + firstTic * framesPerTic <= stepFxFrame_) ++firstTic;
        int tic = -1;
        for (int k = firstTic; k < TICS_PER_STEP && fade_tic_frame(k) <= f; ++k) tic = k;
        if (tic < 0) return -1.0;
        return (static_cast<double>(r.stepOffset + stepIndex_) + tic / static_cast<double>(TICS_PER_STEP)) /
               static_cast<double>(r.span);
    }

    /**
     * What a note-on at `frame` starts from: the carry — as it stood before this step's own commands
     * when the note lands ahead of them (LAT) — with every fade moving the voice laid over it.
     * `faded` gets a `reapply_voice` mask of what the fades set.
     */
    NoteCarry voice_at(const TrackState& ts, int64_t frame, unsigned* faded = nullptr) const {
        NoteCarry v = frame < stepCarryFrom_ ? stepCarryBefore_ : ts.carry;
        unsigned mask = 0;
        if (stepRamps_ != nullptr) {
            for (const RampSpec& r : *stepRamps_) {
                // `frame + 1`: the value is applied one frame behind the note (reapply_voice).
                const double t = fade_t_at(r, frame + 1);
                if (t < 0.0) continue;
                if (r.kind == RampKind::EQ_PRESET) {
                    v.eqMorph = eq_morph_at(*project_, r.startByte, r.destByte, r.curveByte, t < 1.0 ? t : 1.0);
                    v.eqMorphed = true;
                    mask |= CARRY_EQ;
                    continue;
                }
                const int b = t >= 1.0 ? r.destByte
                                       : automation_value_byte(r.startByte, r.destByte, r.curveByte, t);
                if (r.ccId == CC_VOLUME)   v.phraseVol = b / 255.0f;
                else if (r.ccId == CC_PAN) v.pan = b / 255.0f;
                else { v.set_cc(r.ccId, b / 255.0f); mask |= 1u << carry_cc_slot(r.ccId); }
            }
        }
        if (faded != nullptr) *faded = mask;
        return v;
    }

    /** What a note-on cannot carry itself, one frame behind the note at `frame` — on its own frame
     *  it would reach the voice the note replaces. `only` picks the parts. */
    void reapply_voice(int64_t frame, int trackId, const NoteCarry& v, unsigned only = CARRY_ALL) {
        const int64_t at = frame + 1;
        const int lp = carry_cc_slot(CC_FILTER_LP), cut = carry_cc_slot(CC_FILTER_CUT);
        for (int slot = 0; slot < CARRY_CC_SLOTS; ++slot) {
            if (v.ccId[slot] == 0 || !(only & (1u << slot))) continue;
            // A filter switched on and a CUT after it go as ONE write — the type from the one, the
            // cutoff from the other. Two on one frame would land in either order.
            if (slot == cut && v.ccId[lp] != 0 && (only & (1u << lp))) continue;
            const float value = (slot == lp && v.ccId[cut] != 0) ? v.ccValue[cut] : v.ccValue[slot];
            router_.cc(at, trackId, v.ccId[slot], value);
        }
        if (only & CARRY_EQ) {
            if (v.eqMorphed)         router_.ext_eq_morph(at, trackId, v.eqMorph);
            else if (v.eqnSlot >= 0) router_.ext_eq_slot(at, trackId, v.eqnSlot);
        }
        if ((only & CARRY_REVERSE) && v.reverse >= 0) router_.ext_reverse(at, trackId, v.reverse != 0, true);
    }

    /**
     * An ARP or RPT retrigger. It is a new voice, so it is handed what the note it repeats is playing
     * with at its own frame: the VOL channel, pan, PIT, slice and vibrato in the note-on, everything
     * else straight behind it. `velGain` < 0 takes the note's own velocity.
     *
     * ⚠️ PBN, PSL and LPO are NOT handed on: each is a movement from where the voice started, and a
     * new voice starts from nowhere — carrying them needs the engine to start a voice mid-movement.
     */
    void emit_retrigger(NoteArgs a, const Note& note, int trackId, const TrackState& ts, float velGain = -1.0f) {
        const NoteCarry v = voice_at(ts, a.frame);
        a.velGain = velGain >= 0.0f ? velGain : v.velGain;
        a.volGain = v.phraseVol;
        a.pan     = v.pan;
        a.pit     = v.pit;
        a.slice   = v.slice;
        a.vibSpd  = v.vibSpeed;
        a.vibDep  = v.vibDepth;
        emit_note(a, note);
        reapply_voice(a.frame, trackId, v);
    }

    /** A fade tick, into the carry, so a retrigger after the fade has passed still starts there. */
    static void carry_fade(NoteCarry& c, const RampSpec& r, int byte) {
        if (r.ccId == CC_VOLUME)   c.phraseVol = byte / 255.0f;
        else if (r.ccId == CC_PAN) c.pan = byte / 255.0f;
        else                       c.set_cc(r.ccId, byte / 255.0f);
    }

    void emit_note(NoteArgs a, const Note& note) {
        if (note == Note::EMPTY()) return;
        apply_track_scale(a);
        router_.note_on(a);
    }

    /**
     * Pull a scheduled note onto its track's scale — the playback half of SCA / SCG.
     * The transposes are already folded in and PIT/ARP sit beside it, so quantizing `note + pit + arp`
     * covers all of them in one place.
     * ⚠️ The correction goes back to the BASE NOTE: `pit` and `arp` are re-applied below the seam, and
     * `transpose` is subtracted back out by slice selection.
     * ⚠️ The SCHEDULER's scale, on the scheduler's clock, because this note is one of the future notes
     * it is scheduling; nothing below the seam may re-ask.
     * Left alone: a chromatic scale; an instrument with TRANSP. off; ⚠️ a note SELECTING A SLICE
     * (quantizing a kit plays a different drum); a pitch outside 0..127 before or after (an authored
     * B-9 stays 131, and an inexpressible correction is skipped, not clamped).
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

    // The random draws for CHA / RND / RNL / ARP-RANDOM. `rng_range(a, b)` is half-open at the top;
    // negative `lo` is allowed (rng.h).
    // The stream is chosen by `schedulingTrack_`, set on entry to `schedulePhrase` — the one place a
    // draw can be reached from.
    int rng_int(int bound) { return rngs_[schedulingTrack_].next_int(bound); }
    int rng_range(int lo, int hi) { return rngs_[schedulingTrack_].next_int(lo, hi); }

    // One stream per track, because the live-edit rollback is per track: a shared stream could not be
    // rewound for one track without un-drawing another's dice.
    Rng rngs_[8];
    int schedulingTrack_ = 0;
    // The step being scheduled, for `voice_at`. `stepRamps_` is set by schedulePhrase for one step
    // and null otherwise.
    const std::vector<RampSpec>* stepRamps_ = nullptr;
    int     stepIndex_     = 0;
    int64_t stepTarget_    = 0;
    int64_t stepDuration_  = 0;
    int64_t stepFxFrame_   = 0;
    int64_t stepNoteFrame_ = -1;
    const PhraseStep* stepEffective_ = nullptr;
    // The carry before this step's own commands, for a retrigger landing ahead of a LAT-delayed one.
    NoteCarry stepCarryBefore_;
    int64_t   stepCarryFrom_ = 0;
    MidiRouter& router_;
    const Project* project_ = nullptr;
    // Set only by the live transport starts, null on the render path — the STEP 2.4 empty-step tempo
    // fallback depends on it.
    const Project* currentProject_ = nullptr;
    int sampleRate_ = 44100;

    TrackState trackStates_[8];
    int64_t currentFrame_ = 0;
    // PHRASE and CHAIN play ONE track and keep the single cursor they always had.
    int64_t nextFrameToSchedule_ = 0;
    int nextChainRowToSchedule_ = 0;

    // ─── SONG's eight cursors ────────────────────────────────────────────────────────────────────
    // One per track, so a track whose chain runs short moves on alone. `trackDone_` = nothing to play:
    // set only when PLAY lands on a cell this column leaves blank (silent until STOP), or by a RENDER
    // at the range end.
    int64_t trackNextFrame_[8] = {0, 0, 0, 0, 0, 0, 0, 0};
    int  trackSongRow_[8]  = {0, 0, 0, 0, 0, 0, 0, 0};
    int  trackChainRow_[8] = {0, 0, 0, 0, 0, 0, 0, 0};
    bool trackDone_[8]     = {false, false, false, false, false, false, false, false};

    // ─── LIVE mode ───────────────────────────────────────────────────────────────────────────────
    // The row a channel loops is `trackSongRow_` itself — in LIVE the cursor never moves on its own, so
    // a second field would be one fact in two places.
    // `liveSilent_`: stopped, or launched on an empty cell. ⚠️ Not `trackDone_` — a silent channel still
    // spends its bar, so its clock stays on the grid.
    // ⚠️ `liveLoopFrame_` is the frame the current lap began at — the starvation guard. A lap costing
    // ZERO frames (empty chain rows, an all-zero groove) would keep this track furthest behind for
    // ever and starve the other seven; measuring in frames catches every way of costing none.
    bool     liveMode_ = false;
    LiveSlot liveQueue_[8];
    bool     liveSilent_[8] = {false, false, false, false, false, false, false, false};
    int64_t  liveLoopFrame_[8] = {0, 0, 0, 0, 0, 0, 0, 0};
    int currentPhraseId_ = 0;
    int currentChainId_ = 0;
    // The mixer track PHRASE/CHAIN mode plays through. Unused in SONG and render, which carry the track
    // per scheduled row.
    int playbackTrack_ = 0;
    int64_t playbackStartFrame_ = 0;
    PlaybackMode playbackMode_ = PlaybackMode::STOPPED;
    bool isPlaying_ = false;

    // ── side-records: playheads, live-edit rollback, the restore flags ──
    std::deque<Checkpoint> checkpoints_[8];                                // ring of 4, per track
    // The ring bound for TrackState::emptyHops. Sixteen is a full chain of pass-through phrases
    // (legitimate); past 32 nothing is going to play.
    static constexpr int MAX_EMPTY_HOPS = 32;
    std::deque<std::pair<int, int64_t>> chainRowStartFrames_;              // (chainRow, startFrame)
    std::vector<std::pair<SongPos, int64_t>>
        songPositionStartFrames_;                                          // (SongPos → startFrame), in insertion order
    // Every phrase ROW the walk has stamped — see put_phrase_step_position.
    static constexpr size_t STEP_POSITION_CAP = 2048;
    std::vector<std::pair<StepPos, int64_t>> phraseStepStartFrames_;
    bool eqmActive_ = false;
    int  mixerVolTracks_ = 0;      // bit N: a VTR has moved track N's fader this take
    bool masterVolActive_ = false; // …and a VMV has moved the master's
    bool delayTimeActive_ = false; // …and a TIM has taken over the delay's echo time

    // Per-retrigger additive volume delta for RPT (Rxy), indexed by the ramp nibble.
    static constexpr float REPEAT_RAMP_DELTAS[16] = {
        0.00f, -0.02f, -0.04f, -0.06f, -0.10f, -0.15f, -0.20f, -0.30f,
        0.00f,  0.02f,  0.04f,  0.06f,  0.10f,  0.15f,  0.20f,  0.30f
    };
};

}  // namespace songcore

#endif  // POCKETTRACKER_SONGCORE_SCHEDULER_H
