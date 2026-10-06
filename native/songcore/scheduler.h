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
//   * the checkpoint ring — the LIVE rewind, so a launch lands on the boundary it was aimed at;
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

// What a LIVE rewind asks the host to drop: per track, the frame its lookahead rolled back to, or −1
// for nothing queued past now. One frame cannot express this with eight independent cursors.
struct RollbackPlan {
    int64_t frames[8] = {-1, -1, -1, -1, -1, -1, -1, -1};
};

// ─── LIVE mode: what one channel is waiting to do ───────────────────────────────────────────────
//
// LIVE is a MODIFIER ON SONG, not a fifth PlaybackMode: only what happens at a track's boundary
// changes, so nothing that switches on `playbackMode_` (playheads, rollback, traces) needs a new arm,
// and a project that never enters LIVE schedules identically.
// `targetRow < 0 && !stop` is the empty slot.
// ⚠️ A slot is SCHEDULED before it is HEARD (the lookahead). `firesAt` is the frame it lands on: the
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

// Where an AUS/AUF ramp has got to: a BYTE ramp's last byte, or an EQ morph's last band set.
struct RampLastValue {
    int               byte = 0;
    ExtEqMorphPayload eq{};
};

// ─── A phrase part-way through being scheduled ──────────────────────────────────────────────────
//
// The walk schedules a phrase ROW BY ROW and resumes here. It lives in TrackState, so the checkpoint
// (taken before a phrase begins) and every reset leave it inactive — no rewind has to remember it.
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
    // The ramps the phrase declares, paired when it began (automation.h), and where each has got to.
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
    Sequencer(MidiRouter& router, const Project& project, int sample_rate);

    // The transport clock: the host copies the engine's frame counter in and polls
    // updatePlaybackBuffer(); the tools drive a synthetic one.
    void set_clock(int64_t f) { currentFrame_ = f; }
    int64_t clock() const { return currentFrame_; }

    // Re-set by the host on every verb — a device change can alter the rate mid-session.
    void set_sample_rate(int sr) { if (sr > 0) sampleRate_ = sr; }

    // Pin the random FX to a known stream (for the tests; the app never calls it).
    // Eight streams, one per track; track 0 gets `s` unchanged. The offset is the 64-bit golden
    // ratio, so no two tracks walk the same sequence shifted.
    void seed_rng(uint64_t s);
    int  sample_rate() const { return sampleRate_; }

    // PHRASE/CHAIN: walk the phrase in progress to its end now, whatever the clock (for the tests,
    // which count one whole pass; the app never calls it).
    void finish_phrase_now();

    // The frame the current session latched at T PLAY — the trace's session base.
    int64_t playback_start_frame() const { return playbackStartFrame_; }

    // How far past the clock the walk keeps scheduled, so an edit is heard from the next row not yet
    // walked. ⚠️ It must outlast the longest gap between two polls; an operation that holds the UI
    // thread longer fills to LONG_OPERATION_MS first.
    static constexpr int64_t HORIZON_MS = 250;
    static constexpr int64_t LONG_OPERATION_MS = 2000;
    // Units one fill may take, per phrase of depth — a unit is one phrase ROW: 8 tracks × 16 rows of
    // real work, plus headroom for units that cost no frames (which could otherwise spin).
    static constexpr int STEPS_PER_PHRASE_OF_DEPTH = 512;

    bool is_playing() const { return isPlaying_; }
    PlaybackMode playback_mode() const { return playbackMode_; }

    // ── UI cursor feedback (side-records, no bus events) ──

    // `trackId < 0` means "whichever track's marker is oldest" — only meaningful in PHRASE mode, where
    // one track plays; the tools use it, the app does not.
    PlaybackPosition getPlaybackPosition() { return getPlaybackPosition(-1); }

    // Where ONE track is. In PHRASE and CHAIN mode every other track answers −1 throughout — a phrase
    // auditioned alone is in no chain or song, so those screens draw no marker.
    PlaybackPosition getPlaybackPosition(int trackId);

    /**
     * The scale one track is scheduling against — where its last SCA (or SCG) left it.
     * ⚠️ The SCHEDULER's clock, ahead of what is heard by the lookahead. The note cursor asks
     * `songcore::track_scale` instead (scales.h).
     */
    int track_scale_slot(int trackId) const { return trackStates_[clampi(trackId, 0, 7)].scaleSlot; }
    int track_scale_key(int trackId) const;

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
    void release_mixer_vol_track(int track);
    void release_master_vol() { masterVolActive_ = false; }
    void release_delay_time() { delayTimeActive_ = false; }

    // True once a TIM took over the delay time this session — same edge, same reason: it REPLACES the
    // DELAY screen's time and nothing later restores it.
    bool delay_time_active() const { return delayTimeActive_; }

    bool has_live_project() const { return currentProject_ != nullptr; }

    // ── transport starts ──

    // `trackId` defaults to 0, which every tool caller (and so every trace golden) relies on.
    void playPhrase(int phraseId, int trackId = 0);

    void playChain(int chainId, int trackId = 0);

    void playSong(int startRow = 0);

    void stop();

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
    LiveSlot live_queue(int trackId) const;

    /**
     * Start in LIVE mode from stopped. `mask` bit N launches track N at `songRow`; the rest start
     * SILENT, so one press on one cell starts one channel.
     */
    void playSongLive(int songRow, int mask);

    /**
     * Toggle the mode under a running transport: every track keeps its place and repeats (or, leaving
     * LIVE, resumes walking from) the row it is on. Nothing jumps or goes silent.
     * ⚠️ It REWINDS: a chain end inside the lookahead has already been committed as
     * "advance", and without the rewind the change would land a lap late.
     */
    RollbackPlan set_live_mode(bool on, int64_t currentFrame);

    /** Queue one channel to launch `songRow`. `immediate` = the next phrase boundary, else the next chain end. */
    RollbackPlan queue_live(int trackId, int songRow, bool immediate, int64_t currentFrame);

    /** Queue one channel to fall silent. The other seven keep playing — stopping everything is what stop() is. */
    RollbackPlan queue_live_stop(int trackId, bool immediate, int64_t currentFrame);

    /**
     * Queue a whole row as one scene. ⚠️ An EMPTY cell queues a STOP: a row is what the user sees, and
     * a blank must sound as it looks. (Mid-column, SONG mode plays an empty cell as a rest.)
     */
    RollbackPlan queue_live_row(int songRow, bool immediate, int64_t currentFrame);

    // ── the polling scheduler (live modes) ──

    // The first frame not yet scheduled. ⚠️ SONG has eight lookaheads, so it is the track FURTHEST
    // BEHIND; PHRASE and CHAIN keep the one shared cursor. With every track finished it is pinned to
    // the clock rather than +∞, so a fill still runs.
    int64_t buffer_head() const;

    // Fill until every track is `aheadMs` past the clock, a row at a time. Work-conserving: it returns
    // at once while the buffer is deep enough.
    void updatePlaybackBuffer(int64_t aheadMs = HORIZON_MS);

    // ── the render-path scheduler ──
    // trackFilter == nullptr schedules all tracks; inaudible ones (muted, or unsoloed while another is
    // soloed) are always skipped.
    // `repeat` plays the range that many times END TO END IN ONE PASS, so reverb, delay, releases and
    // table positions cross each seam as they do live; concatenated files would cut at every join.
    int64_t scheduleSongRowRange(int startRow, int endRow, const std::set<int>* trackFilter = nullptr,
                                 int repeat = 1);

  private:
    /** One play-through of `startRow..endRow`, beginning at `passStart`. Returns the frame it ends on. */
    int64_t schedule_range_pass(const Project& project, int startRow, int endRow,
                                const std::set<int>* trackFilter, int64_t passStart,
                                int64_t framesPerStep, int64_t framesPerPhrase);

    static int clamp_track(int trackId) { return (trackId >= 0 && trackId < 8) ? trackId : 0; }

    /**
     * Rewind ONE song-mode track to its earliest boundary past `currentFrame`; returns the frame to
     * drop queued notes from (−1 = nothing queued past now). A LIVE launch or toggle lands on that
     * boundary rather than after the lookahead. An edit never rewinds: it is heard from the next row
     * the walk reaches.
     * ⚠️ TrackState and the RNG come back with it (see Checkpoint).
     */
    int64_t rewind_song_track(int trackId, int64_t currentFrame);

    /** Rewind all eight SONG cursors — eight independent rewinds; each track loops its own block. */
    RollbackPlan rewind_all_song_tracks(int64_t currentFrame);

    /**
     * Queue one slot and rewind its track so the launch lands on the boundary it was aimed at.
     * ⚠️ The chain-boundary queue needs the rewind too: that chain end may already be committed as
     * "loop the row again" inside the lookahead, and the launch would land a lap late.
     */
    RollbackPlan arm_live_slot(int trackId, LiveSlot slot, int64_t currentFrame);

    /**
     * Take the queued slot if this boundary is the one it waits for; say whether it fired. `chainEnd`
     * is true on the unit that begins a lap: an immediate queue fires at either, a chain-boundary
     * queue only there — one code path, two trigger points.
     * The slot is STAMPED rather than cleared (LiveSlot); `armed()` keeps it from firing twice.
     */
    bool consume_live_queue(int trackId, bool chainEnd);

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

    // Snapshot taken just BEFORE a SONG-mode unit, so a LIVE rewind can return to the earliest future
    // phrase boundary.
    // ⚠️ The walk CONSUMES STATE — the groove step, the pending HOP, the track's RNG stream. A
    // rollback restoring only the frame replays the phrase against a track that has moved on: a groove
    // whose length does not divide 16 re-times the track, and the dice roll again. This is the state.
    struct Checkpoint {
        int64_t frame = 0;
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

    // ⚠️ By value; the state is captured HERE, below the call sites.
    void save_checkpoint(int trackId, Checkpoint cp);

    // APPEND, never overwrite. A song shorter than the lookahead comes back round to a
    // (songRow, chainRow) it already queued; overwriting would replace the frame of the row SOUNDING
    // NOW with its next occurrence and freeze the playhead. Duplicates cannot pile up: prune_past runs
    // on every read and the lookahead is bounded. The entry names the TRACK — eight cursors, eight
    // places.
    void put_song_position(int trackId, int songRow, int chainRow, int64_t frame);

    /**
     * Stamp the row the walk is standing on — once per row that PLAYS (a groove `00` skips the row, so
     * the marker must too).
     * ⚠️ The one place that knows a row's real start frame (groove and HOP folded in); never derive it
     * a second time.
     * ⚠️ Capped: a render schedules a whole song through here and nobody reads (and so prunes) it
     * offline. The cap is far above any live lookahead; past it, the oldest half goes.
     */
    void put_phrase_step_position(int trackId, int step, int64_t frame);

    /** The row `trackId` is on at `currentFrame`: the latest one stamped at or before it, else −1. */
    int step_in_force(int trackId, int64_t currentFrame) const;

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
    int findNextNonEmptyChainRow(int startRow, const Chain& chain);

    // ─── the per-track SONG walk ─────────────────────────────────────────────────────────────────
    //
    // Each track owns a frame, a song row and a chain row.

    // The chain a track has on one song row, or −1 for a blank cell (an out-of-range id is blank too).
    static int song_cell_chain(const Project& project, int trackId, int songRow);

    /**
     * Can the walk ENTER this song cell? The one definition of a block boundary.
     * ⚠️ A cell the walk cannot enter ENDS A BLOCK — it is not a rest. A track reaching one loops back to
     * the top of its block, for ever, so unrelated sketches can share a project.
     * Unequal blocks drift apart; a track that should rest a few bars needs a chain of empty phrases.
     * ⚠️ A chain whose FIRST row is empty is a boundary too. Later holes are walked over
     * (`next_chain_row_no_wrap`).
     */
    static bool song_cell_plays(const Project& project, int trackId, int songRow);

    /** The first row of the block `songRow` sits in — the row a track loops back to. */
    static int block_start_row(const Project& project, int trackId, int songRow);

    // The next row at or after `startRow` holding a phrase, or −1 when the chain has no more.
    // ⚠️ NO WRAP: inside a song, running out is what moves the track to its next row.
    static int next_chain_row_no_wrap(const Chain& chain, int startRow);

    /**
     * One song row finished for this track: step down the column, or LOOP BACK to its block's top.
     * ⚠️ A RENDER ENDS the track where playback would loop (`lastSongRow` ≥ 0 only there) — a forever
     * loop has no length. Repetition in a file is the render's own count.
     */
    void advance_track_song_row(const Project& project, int trackId, int lastSongRow);

    // The snapshot every unit of work takes before it commits — a phrase, a bar of rest, a bar sat
    // out after HOP FF. ⚠️ A unit with no checkpoint is one a LIVE launch cannot rewind past.
    void checkpoint_track(int trackId, int songRow, int rowUnit, bool take);

    // Advance ONE track by one unit: a phrase row, a bar sat out, or the end of its block (in SONG, a
    // loop — see advance_track_song_row).
    // `lastSongRow` bounds the RENDER path's walk (−1 = the column's own end); it takes no checkpoints.
    void schedule_track_unit(const Project& project, int trackId, int64_t framesPerStep,
                             int64_t framesPerPhrase, int lastSongRow = -1,
                             bool takeCheckpoint = true);

    // ─── LIVE mode's unit of work ────────────────────────────────────────────────────────────────
    //
    // The launched song row REPEATS: a spent chain re-enters the SAME row, so a LIVE track never moves
    // down or runs out of its column. Separate from its SONG twin so SONG's path gains no branch.
    void schedule_live_unit(const Project& project, int trackId, int64_t framesPerStep,
                            int64_t framesPerPhrase, bool takeCheckpoint);

    // The rest of the phrase in progress, in one call. Returns the frames it took.
    int64_t finish_walk(int trackId, int64_t framesPerStep);

    /**
     * Start walking a phrase on `trackId` from `startFrame`; `walk_phrase_row` then schedules it.
     * `chain`/`chainRow` locate the phrase in the chain being played, for AUS/AUF: a fade may span
     * into a later phrase. PHRASE mode passes nullptr and pairs within the phrase.
     * Returns false, and walks nothing, on a track HOP FF stopped.
     */
    bool begin_phrase(int phraseId, int64_t startFrame, int trackId, int transposeSemitones, int startRow,
                      const Chain* chain = nullptr, int chainRow = 0);

    /**
     * Schedule the walk's next row that takes time — rows a groove gives no length are passed on the
     * way. Returns the frames it took. The walk goes inactive when the phrase ends or HOPs; a HOP row
     * takes no time, so that call returns 0.
     */
    int64_t walk_phrase_row(int trackId, int64_t framesPerStep);

    // The phrase ran to its last row.
    static void finish_phrase(TrackState& trackState);

    // ─── AUS / AUF — a declared span, emitted as the walk crosses it ────────────────────────────────
    //
    // One CC per tic at the byte the curve holds there, sent as `byte / 255` like the per-step effect
    // — every value is one the goldens already contain.
    // `t` is measured in STEPS, not frames: `(stepsSoFar + tic/12) / span`, so a fade covers an exact
    // fraction at every step boundary whatever the groove does, with no look-ahead.
    // A tic is `stepDuration / TICS_PER_STEP` — the same warped grid LAT and KIL use.

    void emit_ramp_ticks(const std::vector<RampSpec>& ramps, std::vector<RampLastValue>& lastValue,
                         int chainId, const PhraseStep& effectiveStep, int stepIndex, int64_t targetFrame,
                         int64_t stepDuration, int trackId, TrackState& trackState,
                         int64_t noteFrame, int64_t fxFrame);

    // EQM rides TRACK_GLOBAL and EQN the track's lane, like the per-step pair.
    void emit_eq_morph(int64_t frame, const RampSpec& r, int trackId, const ExtEqMorphPayload& m);

    // CHA gate + RND/RNL randomize, before effect resolution. With no CHA/RND/RNL slot the step comes
    // back unchanged — why the goldens can be byte-compared at all. A test checks the draws.
    PhraseStep applyChanceAndRandomize(const PhraseStep& step, TrackState& trackState, bool& skipNote);

    // ─── One phrase step ────────────────────────────────────────────────────────────────────────────
    //
    // `scheduleStepWithEffects` runs the stages below in a fixed order; `StepPlay` is what they share.
    // ⚠️ The order is the event order — and CHA/RND/RNL and a RANDOM arpeggio draw from one generator,
    // so moving a stage that draws reshuffles every later roll.

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

    // `stepIndex` is unused; named only in a comment so the compiler does not warn.
    ScheduleStepResult scheduleStepWithEffects(const PhraseStep& step, int64_t targetFrame, int64_t stepDuration,
                                               int trackId, int transposeSemitones, TrackState& trackState,
                                               int /*stepIndex*/);

    static Note transposed(const Note& note, int semitones);

    // PBN's rate in semitones per tic: 00-7F up, 80-FF down.
    static float pitch_bend_rate(int v) { return v < 0x80 ? (v / 16.0f) : -((v & 0x7F) / 16.0f); }

    // PVB / PVX: X is the speed, Y the depth. PVX runs twice as fast and four times as deep.
    static void vibrato_from(int v, bool wide, int tempo, float& speed, float& depth);

    // STEP 1: a KIL or a new note ends a running REPEAT / ARPEGGIO, and so does any command in the
    // column that started it.
    static void end_repeat_and_arpeggio(StepPlay& s);

    // STEP 2: CHA/RND/RNL, then the step's resolved parameters.
    void resolve_step(StepPlay& s);

    // ⚠️ A ROW THAT HOPS IS NEVER HEARD — as on a TABLE's steering row (`processTableRow`). The jump
    // is the whole row: no note, no effects, NO TIME, so a four-row loop lasts four rows. A note
    // beside a HOP does not sound; put it on the row above.
    // ⚠️ Decided on the RESOLVED step: a `CHA` can gate the HOP away, and then the row plays.
    // A running REPEAT or ARPEGGIO still ends here (STEP 1 has run) — leaving the phrase ends them.
    static ScheduleStepResult take_hop(StepPlay& s);

    // TSX and TRANSP. folded in by reassignment, so no later site (note, REPEAT retrigger, arpeggio)
    // can reach the unscaled value; then STEP 2.1, DEL (LAT), moves the step's frame.
    void place_step(StepPlay& s) const;

    // STEP 2.2: TBL / THO, GRV, SCA / SCG — state the step's own note already plays under.
    void apply_step_state(StepPlay& s);

    // What `voice_at` needs to know about this step — set before the first note-on it emits.
    void expose_step_to_voice_at(const StepPlay& s);

    // The step's own note, with its PSL / PBN / PVB / PVX, on a freshly reset carry.
    void schedule_step_note(StepPlay& s);

    // KIL: soft note-off at the sample-accurate kill frame (with LAT + KIL-offset latency).
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

    /** The per-voice commands of one step, into the carry — everything except VOL, PAN and the
     *  vibrato, which each have a value only their own emit site knows. */
    static void record_voice_commands(NoteCarry& c, const ResolvedStepParams& p);

    /** Where emit_ramp_ticks puts tic `k` of the step being scheduled. */
    int64_t fade_tic_frame(int k) const;

    /**
     * How far fade `r` has moved the voice by frame `f` of the step being scheduled — its curve
     * position, 1.0 once arrived — or −1 where it is not moving it (outside its span, or while the
     * step's own write of the parameter holds).
     * ⚠️ Must follow emit_ramp_ticks tic for tic: a retrigger re-applies this value on a frame a tic
     * may also land on, and two updates on one frame are ordered arbitrarily — they must agree.
     */
    double fade_t_at(const RampSpec& r, int64_t f) const;

    /**
     * What a note-on at `frame` starts from: the carry — as it stood before this step's own commands
     * when the note lands ahead of them (LAT) — with every fade moving the voice laid over it.
     * `faded` gets a `reapply_voice` mask of what the fades set.
     */
    NoteCarry voice_at(const TrackState& ts, int64_t frame, unsigned* faded = nullptr) const;

    /** What a note-on cannot carry itself, one frame behind the note at `frame` — on its own frame
     *  it would reach the voice the note replaces. `only` picks the parts. */
    void reapply_voice(int64_t frame, int trackId, const NoteCarry& v, unsigned only = CARRY_ALL);

    /**
     * An ARP or RPT retrigger. It is a new voice, so it is handed what the note it repeats is playing
     * with at its own frame: the VOL channel, pan, PIT, slice and vibrato in the note-on, everything
     * else straight behind it. `velGain` < 0 takes the note's own velocity.
     *
     * ⚠️ PBN, PSL and LPO are NOT handed on: each is a movement from where the voice started, and a
     * new voice starts from nowhere — carrying them needs the engine to start a voice mid-movement.
     */
    void emit_retrigger(NoteArgs a, const Note& note, int trackId, const TrackState& ts, float velGain = -1.0f);

    /** A fade tick, into the carry, so a retrigger after the fade has passed still starts there. */
    static void carry_fade(NoteCarry& c, const RampSpec& r, int byte);

    void emit_note(NoteArgs a, const Note& note);

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
    void apply_track_scale(NoteArgs& a) const;

    // The random draws for CHA / RND / RNL / ARP-RANDOM. `rng_range(a, b)` is half-open at the top;
    // negative `lo` is allowed (rng.h).
    // The stream is chosen by `schedulingTrack_`, set on entry to `walk_phrase_row` — the one place a
    // draw can be reached from.
    int rng_int(int bound) { return rngs_[schedulingTrack_].next_int(bound); }
    int rng_range(int lo, int hi) { return rngs_[schedulingTrack_].next_int(lo, hi); }

    // One stream per track, because the LIVE rewind is per track: a shared stream could not be rewound
    // for one track without un-drawing another's dice.
    Rng rngs_[8];
    int schedulingTrack_ = 0;
    // The step being scheduled, for `voice_at`. `stepRamps_` is set by walk_phrase_row for one step
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

    // ── side-records: playheads, the LIVE rewind, the restore flags ──
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
