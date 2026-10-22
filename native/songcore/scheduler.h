#ifndef POCKETTRACKER_SONGCORE_SCHEDULER_H
#define POCKETTRACKER_SONGCORE_SCHEDULER_H

// The sequencer: walks the project by transport position and emits events through the router.
// ⚠️ The stream is byte-compared against golden traces: floats are binary32 in a fixed operation order.
// Beside the walk, with no event (and so no golden): the playheads, the LIVE rewind's checkpoints and
// the flags the host reads to restore what EQM, VTR/VMV and TIM replaced.

#include <algorithm>
#include <array>
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
#include "chords.h"
#include "traversal.h"   // chain_at / phrase_at

namespace songcore {


enum class PlaybackMode { STOPPED, PHRASE, CHAIN, SONG };

// Where ONE track is, for the playheads. `row` is the phrase step in every mode.
// ⚠️ −1 means "no answer", not row 0 (a phrase auditioned alone is in no chain or song).
// ⚠️ The ids are part of the position: two tracks can sit in one chain at different rows.
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

// The phrase row one track is on, stamped at each row's real start frame.
// ⚠️ Never computed off the phrase start: HOP and grooves make rows unequal.
struct StepPos {
    int track = 0;
    int step  = 0;
};

// What a LIVE rewind asks the host to drop: per track, the frame it rolled back to, or −1.
struct RollbackPlan {
    int64_t frames[8] = {-1, -1, -1, -1, -1, -1, -1, -1};
};

// What one LIVE channel is waiting to do. LIVE is a modifier on SONG, not a fifth PlaybackMode: only
// a track's boundary changes. `targetRow < 0 && !stop` is the empty slot.
// ⚠️ Scheduled before it is heard: once `firesAt` is set the scheduler treats it as spent, while the
// screen keeps showing it until the transport gets there.
struct LiveSlot {
    int     targetRow = -1;      // the song row to launch on this channel
    bool    stop      = false;   // …or silence it instead
    bool    immediate = false;   // at the next PHRASE boundary rather than the next CHAIN boundary
    int64_t firesAt   = -1;      // the frame it was scheduled to land on; −1 = still waiting

    // Something is queued — the marker's question.
    bool pending() const { return targetRow >= 0 || stop; }
    // …and not yet spent — the scheduler's question.
    bool armed() const { return pending() && firesAt < 0; }
};

// What the sounding note is playing with, handed to each ARP/RPT retrigger — a retrigger is a new
// voice, and would otherwise undo a VOL, PAN, CUT… on an empty step, or a fade. Reset by the next note.
// ⚠️ A new per-voice command must be added here.
// One slot per controller, in re-apply order; LPF/HPF/BPF share one (each sets type AND cutoff).
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
    int     chord    = -1;                  // CHD byte from the note's own row, or -1
    int     strum    = -1;                  // ARC's mode on that row: the chord is strummed (NoteArgs)
    int64_t strumFrames = 0;

    void set_cc(int id, float v) {
        const int s = carry_cc_slot(id);
        if (s < 0) return;
        ccId[s] = static_cast<uint8_t>(id);
        ccValue[s] = v;
        // A filter switched on carries its own cutoff, so an earlier CUT no longer describes it.
        if (s == carry_cc_slot(CC_FILTER_LP)) ccId[carry_cc_slot(CC_FILTER_CUT)] = 0;
    }
};

// Where an AUS/AUF ramp has got to: a BYTE or VIBRATO ramp's last byte, or an EQ morph's band set.
struct RampLastValue {
    int               byte = 0;
    ExtEqMorphPayload eq{};
};

// A phrase part-way through being scheduled, row by row. In TrackState, so a checkpoint (taken before
// a phrase begins) and every reset leave it inactive.
struct PhraseWalk {
    bool    active     = false;
    int     phraseId   = 0;
    int     chainId    = -1;      // −1: PHRASE mode, ramps pair within the phrase
    int     transpose  = 0;
    int64_t startFrame = 0;
    int     row        = 0;       // the next row to schedule
    int     localGrooveStep = 0;
    bool    anyGrooveActive = false;
    int64_t frameOffset = 0;      // frames scheduled so far, from `startFrame`
    int     rowsScheduled = 0;
    int     pass = 1;             // which entry of this phrase on this track this is (NTH), from 1
    // The phrase's ramps, paired when it began (automation.h), and where each has got to.
    std::vector<RampSpec>      ramps;
    std::vector<RampLastValue> rampLast;
};

// ─── Per-track persistent effect state ──────────────────────────────────────────────────────────
struct TrackState {
    PhraseWalk walk;

    Note  lastNote = Note::EMPTY();
    int   lastInstrument = 0;
    int   lastStartPoint = -1;
    NoteCarry carry;

    int   repeatActiveColumn = 0;
    int   repeatTicInterval = 0;
    int   repeatVolRamp = 0;
    int64_t repeatStartFrame = 0;
    int   repeatRetrigCount = 0;
    // The ramp runs on velocity × the VOL channel as taken at arming, so a later VOL scales the hits.
    float repeatBaseVolume = 1.0f;
    float repeatBasePhraseVol = 1.0f;

    int   arpeggioActiveColumn = 0;
    int   arpeggioValue = 0;
    int   arpeggioMode = 0;
    int   arpeggioSpeed = 4;
    int64_t arpeggioStartFrame = 0;

    int   strumPass = 0;   // strummed chords so far: a PP strum goes up on the even ones, down on the odd

    int   hopTargetRow = -1;
    bool  trackStopped = false;
    // Consecutive phrases that scheduled nothing because their entry row hopped.
    // ⚠️ A HOP row costs no time, so a ring of them (`HOP 00` on row 0) would spin for ever; past
    // MAX_EMPTY_HOPS the track stops as on `HOP FF`. Any row that plays resets it.
    int   emptyHops = 0;

    // Entries into each phrase since PLAY — what NTH counts. Here so a LIVE rewind takes it back too.
    std::array<uint32_t, 256> phrasePasses{};

    bool  pitchBendActive = false;
    bool  vibratoActive = false;
    int   lastNoteMidi = -1;

    int   lastTableOverride = -1;
    int   lastTableStartRow = -1;

    int   grooveId = 0;
    int   grooveStep = 0;

    // Where SCA / SCG put this track; `scaleKey = -1` is the project's KEY, so a reset lands there.
    int   scaleSlot = 0;
    int   scaleKey  = -1;

    int   lastColFxType[4] = {0, 0, 0, 0};   // 1-indexed: [1]=FX1 …
    int   lastColFxValue[4] = {0, 0, 0, 0};

    // AUS cells a CHA ate on their last pass, so the phrases their span crosses stay silent too.
    // Keyed on the AUS position; rewritten every time the AUS step plays.
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

class Sequencer {
  public:
    Sequencer(MidiRouter& router, const Project& project, int sample_rate);

    // The transport clock: the host copies the engine's frame counter in; the tools drive their own.
    void set_clock(int64_t f) { currentFrame_ = f; }
    int64_t clock() const { return currentFrame_; }

    // Re-set by the host on every verb — a device change can alter the rate mid-session.
    void set_sample_rate(int sr) { if (sr > 0) sampleRate_ = sr; }

    // Pin the random FX to a known stream (tests only). One stream per track; track 0 gets `s`.
    void seed_rng(uint64_t s);
    int  sample_rate() const { return sampleRate_; }

    // PHRASE/CHAIN: walk the phrase in progress to its end now (tests only).
    void finish_phrase_now();

    // The frame latched at PLAY — the trace's session base.
    int64_t playback_start_frame() const { return playbackStartFrame_; }

    // ── SYNC IN: a clock from another device leads ──

    // Each fill walks `framesPerStep` (0 = from the tempo in force), and no row starting at or past
    // `limit` (−1 = none). The host sets both before every fill.
    void set_follow(int64_t framesPerStep, int64_t limit) {
        followStep_  = framesPerStep;
        followLimit_ = limit;
    }

    // Plain step `grid_step()` of the take begins on `grid_frame()`, the next not yet walked. Every
    // plain track's rows begin on it: the rows one fill walks share one step length.
    int64_t grid_step() const { return gridStep_; }
    int64_t grid_frame() const { return gridFrame_; }

    // The next transport start begins on `frame` instead of the clock — once.
    void set_start_frame(int64_t frame) { nextStartFrame_ = frame; }

    // Every boundary not yet walked, and the grid, `delta` frames later (or earlier).
    // ⚠️ Only while every track stands on the same boundary: a take's first step, held there by the
    // follow limit while the clock's tempo is not yet known.
    void shift_unwalked(int64_t delta);

    // How far past the clock the walk keeps scheduled; an edit is heard from the next row not walked.
    // ⚠️ It must outlast the longest gap between two polls; a longer UI hold fills to
    // LONG_OPERATION_MS first.
    static constexpr int64_t HORIZON_MS = 250;
    static constexpr int64_t LONG_OPERATION_MS = 2000;
    // Units (phrase rows) one fill may take per phrase of depth: 8 × 16 plus headroom for free units.
    static constexpr int STEPS_PER_PHRASE_OF_DEPTH = 512;

    bool is_playing() const { return isPlaying_; }
    PlaybackMode playback_mode() const { return playbackMode_; }

    // ── playheads ──

    // The oldest marker of any track — PHRASE mode only (tools).
    PlaybackPosition getPlaybackPosition() { return getPlaybackPosition(-1); }

    // Where ONE track is. In PHRASE and CHAIN mode every other track answers −1.
    PlaybackPosition getPlaybackPosition(int trackId);

    // The scale one track is scheduling against.
    // ⚠️ On the scheduler's clock, ahead of what is heard; the note cursor asks `track_scale` (scales.h).
    int track_scale_slot(int trackId) const { return trackStates_[clampi(trackId, 0, 7)].scaleSlot; }
    int track_scale_key(int trackId) const;

    // ── what the take replaced: the host reads these BEFORE stop() clears them, and restores ──

    // An EQM overrode the master EQ (the render path restores its own).
    bool eqm_active() const { return eqmActive_; }

    // A VTR or VMV moved a fader.
    bool mixer_vol_active() const { return mixerVolTracks_ != 0 || masterVolActive_; }

    // …and which: bit N = track N's fader belongs to the song now (engine_setup.h `MixerHeld`).
    int  mixer_vol_tracks() const { return mixerVolTracks_; }
    bool master_vol_active() const { return masterVolActive_; }

    // ⚠️ The hand takes a fader back (a mapped knob); otherwise the next push would skip it.
    void release_mixer_vol_track(int track);
    void release_master_vol() { masterVolActive_ = false; }
    void release_delay_time() { delayTimeActive_ = false; }

    // A TIM took over the delay time.
    bool delay_time_active() const { return delayTimeActive_; }

    bool has_live_project() const { return currentProject_ != nullptr; }

    // ── transport starts ──

    // Every trace golden relies on `trackId` defaulting to 0.
    void playPhrase(int phraseId, int trackId = 0);

    void playChain(int chainId, int trackId = 0);

    // `offsetSteps` into the row: each track that far into its own column (a song position).
    void playSong(int startRow = 0, int offsetSteps = 0, bool live = false);

    void stop();


    // ── LIVE mode: the song grid as a scene launcher; a launched cell repeats until something else is
    // queued ──

    bool     live_mode() const               { return liveMode_; }


    // What this channel is still waiting to do, for the screen — a spent slot is reported until the
    // transport reaches its frame.
    LiveSlot live_queue(int trackId) const;

    // Start LIVE from stopped: `mask` bit N launches track N at `songRow`, the rest start silent.
    void playSongLive(int songRow, int mask, int offsetSteps = 0);

    // Toggle LIVE under a running transport; every track keeps the row it is on.
    // ⚠️ It rewinds: a chain end in the lookahead is already committed, and the change would land a
    // lap late.
    RollbackPlan set_live_mode(bool on, int64_t currentFrame);

    // Queue one channel to launch `songRow` at the next phrase boundary (`immediate`) or chain end.
    RollbackPlan queue_live(int trackId, int songRow, bool immediate, int64_t currentFrame);

    // Queue one channel to fall silent; the other seven keep playing.
    RollbackPlan queue_live_stop(int trackId, bool immediate, int64_t currentFrame);

    // Queue a whole row as one scene. ⚠️ An empty cell queues a STOP — a blank sounds as it looks.
    RollbackPlan queue_live_row(int songRow, bool immediate, int64_t currentFrame);

    // ── the polling scheduler (live modes) ──

    // The first frame not yet scheduled — in SONG the track furthest behind. With every track
    // finished it is pinned to the clock, so a fill still runs.
    int64_t buffer_head() const;

    // Fill a row at a time until every track is `aheadMs` past the clock; returns at once if it is.
    void updatePlaybackBuffer(int64_t aheadMs = HORIZON_MS);

    // ── the render path ──
    // nullptr `trackFilter` = all tracks; inaudible ones are always skipped. `repeat` plays the range
    // end to end in one pass, so tails and table positions cross each seam as they do live.
    int64_t scheduleSongRowRange(int startRow, int endRow, const std::set<int>* trackFilter = nullptr,
                                 int repeat = 1);

  private:
    // One play-through of `startRow..endRow` from `passStart`; returns the frame it ends on.
    int64_t schedule_range_pass(const Project& project, int startRow, int endRow,
                                const std::set<int>* trackFilter, int64_t passStart,
                                int64_t framesPerStep, int64_t framesPerPhrase);

    static int clamp_track(int trackId) { return (trackId >= 0 && trackId < 8) ? trackId : 0; }

    // The take's first frame — `set_start_frame`'s, else the clock — where the grid starts too.
    int64_t take_start_frame();

    // One track `offsetSteps` into its column from `startRow`, counted as the walk goes (chain rows,
    // block loops; LIVE stays on its row); the phrase is entered at the step through the HOP target.
    // ⚠️ Nominal, as a song position is: what a GRV, HOP or held note before it did is not replayed.
    void place_track_at(int trackId, int startRow, int offsetSteps, bool live);

    // Rewind ONE SONG track to its earliest boundary past `currentFrame`, so a LIVE launch lands there
    // rather than after the lookahead. Returns the frame to drop queued notes from, or −1.
    // ⚠️ TrackState and the RNG come back with it (Checkpoint).
    int64_t rewind_song_track(int trackId, int64_t currentFrame);

    RollbackPlan rewind_all_song_tracks(int64_t currentFrame);

    // Queue one slot and rewind its track.
    // ⚠️ A chain-boundary queue needs the rewind too: that chain end may already be committed as
    // "loop again", and the launch would land a lap late.
    RollbackPlan arm_live_slot(int trackId, LiveSlot slot, int64_t currentFrame);

    // Take the queued slot if this boundary is the one it waits for; `chainEnd` = this unit begins a
    // lap. The slot is stamped, not cleared (LiveSlot).
    bool consume_live_queue(int trackId, bool chainEnd);

    struct ScheduleStepResult {
        bool noteScheduled = false;
        bool hopTriggered = false;
        // This step's note-on, or -1 — a ramp tick on that frame would reach the voice it replaces.
        int64_t noteFrame = -1;
        // The frame this step's own FX writes landed on; a crossing ramp on the same parameter yields it.
        int64_t fxFrame = -1;
        // The step after CHA/RND/RNL, which decides whether a crossing ramp yields.
        // ⚠️ Pairing reads the AUTHORED step: whether a fade exists must not depend on dice.
        PhraseStep effectiveStep;
    };

    // Snapshot taken before a SONG unit, so a LIVE rewind can return to that boundary.
    // ⚠️ The walk consumes state (groove step, pending HOP, RNG); restoring only the frame replays the
    // phrase against a track that has moved on.
    struct Checkpoint {
        int64_t frame = 0;
        int songRow = 0;
        int songChainRow = 0;
        // ⚠️ Filled by save_checkpoint(), never by the call sites. One track's state.
        TrackState trackState{};
        Rng        rng{};
        // ⚠️ LIVE's lap origin too — left behind, the launch lands a bar late.
        int64_t liveLoopFrame = 0;
    };

    int64_t getCurrentFrame() const { return currentFrame_; }

    // ⚠️ By value; the state is captured HERE, below the call sites.
    void save_checkpoint(int trackId, Checkpoint cp);

    // ⚠️ Append, never overwrite: a song shorter than the lookahead revisits a row it already queued,
    // and overwriting would freeze the playhead. prune_past keeps it bounded.
    void put_song_position(int trackId, int songRow, int chainRow, int64_t frame);

    // Stamp the row the walk is on — once per row that plays.
    // ⚠️ The one place that knows a row's real start frame; never derive it a second time.
    // ⚠️ Capped: a render writes a whole song and nobody prunes it; past the cap the oldest half goes.
    void put_phrase_step_position(int trackId, int step, int64_t frame);

    // The latest row stamped at or before `currentFrame`, else −1.
    int step_in_force(int trackId, int64_t currentFrame) const;

    // Drop entries more than a phrase in the past; run on every read.
    template <typename C>
    static void prune_past(C& c, int64_t currentFrame, int64_t framesPerPhrase) {
        c.erase(std::remove_if(c.begin(), c.end(),
                               [&](const typename C::value_type& e) {
                                   return currentFrame > e.second + framesPerPhrase;
                               }),
                c.end());
    }

    // Drop this cursor's position entries at or past the frame a rollback rewound to.
    // ⚠️ Dropping the queued notes does not do this — a marker is not an event.
    template <typename C, typename Match>
    static void drop_positions_from(C& c, int64_t frame, Match match) {
        c.erase(std::remove_if(c.begin(), c.end(),
                               [&](const typename C::value_type& e) {
                                   return e.second >= frame && match(e.first);
                               }),
                c.end());
    }

    // The next row at or after `startRow` holding a phrase, wrapping; -1 if the chain is empty.
    // `startRow` is normalised here (playChain may pass CHAIN_ROWS).
    int findNextNonEmptyChainRow(int startRow, const Chain& chain);

    // ── the per-track SONG walk: each track owns a frame, a song row and a chain row ──

    // The chain on one song cell, or −1 for a blank (an out-of-range id is blank too).
    static int song_cell_chain(const Project& project, int trackId, int songRow);

    // Can the walk enter this cell? The one definition of a block boundary.
    // ⚠️ A cell it cannot enter ENDS A BLOCK, it is not a rest: the track loops back to its block's top.
    // A chain whose first row is empty is a boundary too; later holes are walked over.
    static bool song_cell_plays(const Project& project, int trackId, int songRow);

    // The first row of the block `songRow` sits in — where a track loops back to.
    static int block_start_row(const Project& project, int trackId, int songRow);

    // The next row at or after `startRow` holding a phrase, or −1.
    // ⚠️ No wrap: inside a song, running out moves the track to its next row.
    static int next_chain_row_no_wrap(const Chain& chain, int startRow);

    // One song row finished: step down the column, or loop back to the block's top.
    // ⚠️ A render (`lastSongRow` ≥ 0) ENDS the track where playback would loop.
    void advance_track_song_row(const Project& project, int trackId, int lastSongRow);

    // The snapshot each unit takes before it commits.
    // ⚠️ A unit with no checkpoint is one a LIVE launch cannot rewind past.
    void checkpoint_track(int trackId, int songRow, int rowUnit, bool take);

    // Advance ONE track by one unit: a phrase row, a bar sat out, or its block's end.
    // `lastSongRow` bounds the render path's walk (−1 = the column's end); a render takes no checkpoints.
    void schedule_track_unit(const Project& project, int trackId, int64_t framesPerStep,
                             int64_t framesPerPhrase, int lastSongRow = -1,
                             bool takeCheckpoint = true);

    // LIVE's unit: a spent chain re-enters the SAME row. Separate so SONG's path gains no branch.
    void schedule_live_unit(const Project& project, int trackId, int64_t framesPerStep,
                            int64_t framesPerPhrase, bool takeCheckpoint);

    // The rest of the phrase in progress, in one call. Returns the frames it took.
    int64_t finish_walk(int trackId, int64_t framesPerStep);

    // Start walking a phrase; `walk_phrase_row` schedules it. `chain`/`chainRow` let an AUS/AUF span
    // into later phrases (PHRASE mode passes nullptr). False on a track HOP FF stopped.
    bool begin_phrase(int phraseId, int64_t startFrame, int trackId, int transposeSemitones, int startRow,
                      const Chain* chain = nullptr, int chainRow = 0);

    // Schedule the next row that takes time, passing rows a groove gives no length. Returns the
    // frames it took — 0 for a HOP. The walk goes inactive when the phrase ends or HOPs.
    int64_t walk_phrase_row(int trackId, int64_t framesPerStep);

    static void finish_phrase(TrackState& trackState);

    // ── AUS / AUF: one CC per tic at the byte the curve holds, sent as `byte / 255` ──
    // `t` counts STEPS, not frames (`(stepsSoFar + tic/12) / span`), so a fade lands on an exact
    // fraction at every step whatever the groove does. A tic is `stepDuration / TICS_PER_STEP`.

    void emit_ramp_ticks(const std::vector<RampSpec>& ramps, std::vector<RampLastValue>& lastValue,
                         int chainId, const PhraseStep& effectiveStep, int stepIndex, int64_t targetFrame,
                         int64_t stepDuration, int trackId, TrackState& trackState,
                         int64_t noteFrame, int64_t fxFrame);

    // EQM rides TRACK_GLOBAL and EQN the track's lane, like the per-step pair.
    void emit_eq_morph(int64_t frame, const RampSpec& r, int trackId, const ExtEqMorphPayload& m);

    // CHA gate, then RND/RNL. Without those the step comes back unchanged.
    PhraseStep applyChanceAndRandomize(const PhraseStep& step, TrackState& trackState, bool& skipNote);

    // ── one phrase step: `scheduleStepWithEffects` runs the stages below, sharing `StepPlay` ──
    // ⚠️ The order is the event order, and the random stages share one generator — moving a stage
    // that draws reshuffles every later roll.

    struct StepPlay {
        const PhraseStep& step;      // as authored
        TrackState& ts;
        int64_t targetFrame;
        int64_t stepDuration;
        int trackId;
        int transpose;               // TSX and the instrument's TRANSP. are folded in by `place_step`

        bool hasNote  = false;
        bool skipNote = false;       // a CHA gated the note away
        PhraseStep effectiveStep;    // after CHA/RND/RNL
        ResolvedStepParams params;
        int   velocityByte = 0;
        float velocityGain = 0.0f;
        float phraseVol    = 0.0f;   // the instrument's volume, VOL applied
        float notePan      = 0.0f;
        // A running REPEAT's ramp before this step, or -1: an empty step re-arming REPEAT resumes it.
        float savedRampVolume    = -1.0f;
        float savedRampPhraseVol = 0.0f;

        int     delayTicks = 0;
        int64_t effectiveTargetFrame = 0;  // the step frame after LAT
        // +1 on a note step: a parameter on the note's own frame reaches the voice the note REPLACES.
        int64_t voiceFxFrame = 0;
        int64_t scheduledNoteFrame = -1;   // this step's note-on, or -1
        int tableId  = -1;
        int tableRow = -1;

        StepPlay(const PhraseStep& step_, TrackState& ts_, int64_t target, int64_t duration, int track, int semis)
            : step(step_), ts(ts_), targetFrame(target), stepDuration(duration), trackId(track), transpose(semis) {}
        bool triggers() const { return hasNote && !skipNote; }
    };

    ScheduleStepResult scheduleStepWithEffects(const PhraseStep& step, int64_t targetFrame, int64_t stepDuration,
                                               int trackId, int transposeSemitones, TrackState& trackState,
                                               int /*stepIndex*/);

    static Note transposed(const Note& note, int semitones);

    // PBN's rate in semitones per tic: 00-7F up, 80-FF down.
    static float pitch_bend_rate(int v) { return v < 0x80 ? (v / 16.0f) : -((v & 0x7F) / 16.0f); }

    // PVB / PVX: X is the speed, Y the depth. PVX runs twice as fast and four times as deep.
    static void vibrato_from(int v, bool wide, int tempo, float& speed, float& depth);

    // STEP 1: a KIL, a new note or any command in its column ends a running REPEAT / ARPEGGIO.
    static void end_repeat_and_arpeggio(StepPlay& s);

    // STEP 2: CHA/RND/RNL, then the step's resolved parameters.
    void resolve_step(StepPlay& s);

    // ⚠️ A row that HOPs is never heard: no note, no effects, no time (as on a table's steering row).
    // Decided on the resolved step, so a CHA can gate the HOP away and the row plays.
    static ScheduleStepResult take_hop(StepPlay& s);

    // TSX and TRANSP. folded in by reassignment, so no later site can reach the unscaled value; then
    // STEP 2.1, LAT, moves the step's frame.
    void place_step(StepPlay& s) const;

    // STEP 2.2: TBL / THO, GRV, SCA / SCG — state the step's own note already plays under.
    void apply_step_state(StepPlay& s);

    // What `voice_at` needs to know about this step — set before the first note-on it emits.
    void expose_step_to_voice_at(const StepPlay& s);

    // The step's own note, with its PSL / PBN / PVB / PVX, on a freshly reset carry.
    void schedule_step_note(StepPlay& s);

    // An OFF: the sounding note goes to its release on the step's frame (CHA and LAT apply as to a note).
    void schedule_note_off(StepPlay& s);

    // KIL: cuts the note at its kill frame (LAT and the KIL offset included) — no release.
    void schedule_kill(StepPlay& s);

    // STEP 2.3: live per-note / mixer FX (PAN / REV / DEL / BCK / CUT / RES / EQN / EQM).
    void emit_step_fx(StepPlay& s);

    // STEP 2.4: VOL / PBN / PVB / PVX on a step without a note change the sounding one.
    void emit_mid_note_pitch_and_volume(StepPlay& s);

    // STEP 3: REPEAT — arm it from this step, then retrigger on its tic grid up to the step's end.
    void schedule_repeat(StepPlay& s);

    // STEP 4: ARC configures the arpeggio; STEP 5: ARP arms or clears it, then it plays.
    void schedule_arpeggio(StepPlay& s);

    void schedule_arpeggio_notes(StepPlay& s);

    // Per-column FX memory for RND — real effects only, from the AUTHORED step.
    static void remember_column_fx(StepPlay& s);

    int getArpeggioNote(int baseMidi, int semi1, int semi2, int mode, int position);

    // ─── Retriggers and the note they repeat (NoteCarry) ────────────────────────────────────────

    // `reapply_voice` mask bits beyond the controller slots.
    static constexpr unsigned CARRY_EQ      = 1u << CARRY_CC_SLOTS;
    static constexpr unsigned CARRY_REVERSE = 1u << (CARRY_CC_SLOTS + 1);
    static constexpr unsigned CARRY_ALL     = ~0u;

    // One step's per-voice commands into the carry — all but VOL, PAN and vibrato, which their own
    // emit sites record.
    static void record_voice_commands(NoteCarry& c, const ResolvedStepParams& p);

    // Where emit_ramp_ticks puts tic `k` of the step being scheduled.
    int64_t fade_tic_frame(int k) const;

    // How far fade `r` has moved the voice by frame `f` (1.0 once arrived), or −1 where it is not
    // moving it. ⚠️ Must agree with emit_ramp_ticks tic for tic: both can land on one frame.
    double fade_t_at(const RampSpec& r, int64_t f) const;

    // What a note-on at `frame` starts from: the carry (as before this step's commands, if a LAT puts
    // the note ahead of them) with every moving fade laid over it. `faded` = a reapply_voice mask.
    NoteCarry voice_at(const TrackState& ts, int64_t frame, unsigned* faded = nullptr) const;

    // What a note-on cannot carry, one frame behind it (on its own frame it would reach the voice the
    // note replaces). `only` picks the parts.
    void reapply_voice(int64_t frame, int trackId, const NoteCarry& v, unsigned only = CARRY_ALL);

    // An ARP or RPT retrigger, handed what the note it repeats is playing with. `velGain` < 0 = the
    // note's own velocity.
    // ⚠️ PBN, PSL and LPO are NOT handed on: each is a movement, and a new voice starts from nowhere.
    // An ARP's notes play the chord all at once (`strum` false): a strum would outlast the step.
    void emit_retrigger(NoteArgs a, const Note& note, int trackId, TrackState& ts, float velGain = -1.0f,
                        bool strum = true);

    // A fade tick into the carry, so a later retrigger starts where the fade left it.
    static void carry_fade(NoteCarry& c, const RampSpec& r, int byte);

    void emit_note(NoteArgs a, const Note& note, TrackState& ts);

    // Pull a scheduled note onto its track's scale (SCA / SCG), quantizing `note + pit + arp`.
    // ⚠️ The correction goes back to the BASE NOTE; pit, arp and transpose are re-applied below.
    // Left alone: a chromatic scale, TRANSP. off, a note selecting a slice, a pitch outside 0..127
    // before or after (skipped, not clamped).
    void apply_track_scale(NoteArgs& a) const;
    // The scale `apply_track_scale` would pull this note onto, or null where it leaves it alone.
    const Scale* scale_for_note(const NoteArgs& a) const;
    // CHD: the chord's notes from where the note sounds, each pulled onto the track's scale on its
    // own; two that land on one pitch are one note.
    void apply_chord(NoteArgs& a) const;
    // ARC on a chord's row: the order its notes come in. RND draws; PP alternates per strummed chord.
    void apply_strum(NoteArgs& a, TrackState& ts);

    // The random draws, from the stream of `schedulingTrack_` (set in `walk_phrase_row`).
    // `rng_range` is half-open at the top (rng.h).
    int rng_int(int bound) { return rngs_[schedulingTrack_].next_int(bound); }
    int rng_range(int lo, int hi) { return rngs_[schedulingTrack_].next_int(lo, hi); }

    // One stream per track, so a LIVE rewind of one track does not un-draw another's dice.
    Rng rngs_[8];
    int schedulingTrack_ = 0;
    // The step being scheduled, for `voice_at`; `stepRamps_` is null between steps.
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
    // Null on the render path — STEP 2.4's tempo fallback depends on it.
    const Project* currentProject_ = nullptr;
    int sampleRate_ = 44100;

    TrackState trackStates_[8];
    int64_t currentFrame_ = 0;
    // PHRASE and CHAIN play ONE track and keep the single cursor they always had.
    int64_t nextFrameToSchedule_ = 0;
    int nextChainRowToSchedule_ = 0;

    // SONG's eight cursors. `trackDone_` = nothing to play: PLAY landed on a blank cell (silent until
    // STOP), or a render reached its range end.
    int64_t trackNextFrame_[8] = {0, 0, 0, 0, 0, 0, 0, 0};
    int  trackSongRow_[8]  = {0, 0, 0, 0, 0, 0, 0, 0};
    int  trackChainRow_[8] = {0, 0, 0, 0, 0, 0, 0, 0};
    bool trackDone_[8]     = {false, false, false, false, false, false, false, false};

    // LIVE: the row a channel loops is `trackSongRow_` itself.
    // `liveSilent_`: stopped, or launched on an empty cell. ⚠️ Not `trackDone_` — a silent channel still
    // spends its bar, so its clock stays on the grid.
    // ⚠️ `liveLoopFrame_` is where the current lap began — the guard against a lap costing zero frames,
    // which would stay furthest behind for ever and starve the other seven.
    bool     liveMode_ = false;
    LiveSlot liveQueue_[8];
    bool     liveSilent_[8] = {false, false, false, false, false, false, false, false};
    int64_t  liveLoopFrame_[8] = {0, 0, 0, 0, 0, 0, 0, 0};
    int currentPhraseId_ = 0;
    int currentChainId_ = 0;
    // The mixer track PHRASE/CHAIN mode plays through.
    int playbackTrack_ = 0;
    int64_t playbackStartFrame_ = 0;
    int64_t nextStartFrame_ = -1;   // set_start_frame's, until a start takes it
    // SYNC IN: the clock's step and limit for the fill, and the plain-step grid as walked.
    int64_t followStep_  = 0;
    int64_t followLimit_ = -1;
    int64_t gridStep_    = 0;
    int64_t gridFrame_   = 0;
    PlaybackMode playbackMode_ = PlaybackMode::STOPPED;
    bool isPlaying_ = false;

    // ── side-records: playheads, the LIVE rewind, the restore flags ──
    std::deque<Checkpoint> checkpoints_[8];                                // ring of 4, per track
    // TrackState::emptyHops' bound: 16 is a full chain of pass-through phrases, past 32 nothing plays.
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
