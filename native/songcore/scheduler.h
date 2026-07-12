#ifndef POCKETTRACKER_SONGCORE_SCHEDULER_H
#define POCKETTRACKER_SONGCORE_SCHEDULER_H

// ─── The sequencer spine ─────────────────────────────────────────────────────────────────────────
//
// A 1:1 port of core/logic/PlaybackController.kt (+ its TrackState) — the "doomed" Kotlin
// sequencing zone rewritten as C++ songcore (linux-port-plan §4.3, order-of-work zone C). It walks
// the project by transport position exactly as the Kotlin scheduler does — grooves, HOP, RPT/ARP
// grids, LAT, KIL, pitch mods, per-note/mixer FX — and emits the identical event stream through the
// MidiRouter seam (router.h), which the trace writer serializes for the byte-for-byte conformance
// check against /testdata/traces (event-schema §6).
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
#include "rng.h"
#include "router.h"

namespace songcore {

// ─── Note ↔ MIDI (TrackerData.Note) ─────────────────────────────────────────────────────────────
inline int note_to_midi(const Note& n) {
    if (n.pitch == -1) return -1;
    return (n.octave + 1) * 12 + n.pitch;  // C-4 = 60 (standard MIDI)
}
inline Note note_from_midi(int midi) {
    if (midi < 0 || midi > 127) return Note::EMPTY();
    return Note{midi % 12, midi / 12 - 1};
}

// ─── small helpers ───────────────────────────────────────────────────────────────────────────────
inline int   clampi(int v, int lo, int hi)       { return v < lo ? lo : (v > hi ? hi : v); }
inline float clampf(float v, float lo, float hi)  { return v < lo ? lo : (v > hi ? hi : v); }
inline float hex_to_float(int hex)                { return (hex & 0xFF) / 255.0f; }  // VolumeUtils.hexToFloat

// Indexed FX access on a PhraseStep (model.h keeps the flat fx{1,2,3}{Type,Value}) — mirrors
// PhraseStep.fx / fxType / setFx / setFxValue.
inline int  step_fx_type(const PhraseStep& s, int slot) {
    return slot == 1 ? s.fx1Type : slot == 2 ? s.fx2Type : slot == 3 ? s.fx3Type : 0;
}
inline int  step_fx_value(const PhraseStep& s, int slot) {
    return slot == 1 ? s.fx1Value : slot == 2 ? s.fx2Value : slot == 3 ? s.fx3Value : 0;
}
inline void step_set_fx(PhraseStep& s, int slot, int type, int value) {
    if (slot == 1) { s.fx1Type = type; s.fx1Value = value; }
    else if (slot == 2) { s.fx2Type = type; s.fx2Value = value; }
    else if (slot == 3) { s.fx3Type = type; s.fx3Value = value; }
}
inline void step_set_fx_value(PhraseStep& s, int slot, int value) {
    if (slot == 1) s.fx1Value = value;
    else if (slot == 2) s.fx2Value = value;
    else if (slot == 3) s.fx3Value = value;
}
inline bool step_empty(const PhraseStep& s) { return s.note == Note::EMPTY(); }
inline bool chain_is_empty(const Chain& c, int index) { return c.phraseRefs[index] == -1; }

enum class PlaybackMode { STOPPED, PHRASE, CHAIN, SONG };

// ─── UI cursor feedback (SC-4 — never goldened) ──────────────────────────────────────────────────
// PlaybackController.PlaybackPosition field-for-field. `row` doubles as the phrase step in every
// mode (that is how Kotlin fills it), so the UI reads one struct wherever the cursor lives.
struct PlaybackPosition {
    int row = 0;
    int chainRow = 0;
    int phraseStep = 0;
    int songRow = 0;
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
        : router_(router), project_(&project), sampleRate_(sample_rate) {}

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
    void seed_rng(uint64_t s) { rng_.seed(s); }
    int  sample_rate() const { return sampleRate_; }

    // The frame the current session latched at T PLAY — the trace's session base.
    int64_t playback_start_frame() const { return playbackStartFrame_; }

    static constexpr int64_t LOOKAHEAD_MS = 50;
    static constexpr int BUFFER_PHRASES = 2;

    bool is_playing() const { return isPlaying_; }
    PlaybackMode playback_mode() const { return playbackMode_; }

    // ── UI cursor + live-edit reaction (S5 side-records — no bus events, SC-4) ──

    // PlaybackController.getPlaybackPosition, verbatim (incl. the tempo fallback: currentProject_ is
    // null on the render path, but so is isPlaying_, so this reads the live tempo in practice).
    PlaybackPosition getPlaybackPosition() {
        PlaybackPosition pos;
        if (!isPlaying_) return pos;

        int64_t currentFrame = getCurrentFrame();
        int64_t elapsedFrames = currentFrame - playbackStartFrame_;
        int tempo = currentProject_ ? currentProject_->tempo : 120;
        int64_t framesPerStep = frames_per_step(tempo, sampleRate_);
        if (framesPerStep <= 0) return pos;   // unreachable for any legal tempo; a 60 Hz UI poll must not divide by zero
        int64_t framesPerPhrase = framesPerStep * 16;

        switch (playbackMode_) {
            case PlaybackMode::PHRASE: {
                pos.row = clampi(static_cast<int>((elapsedFrames % framesPerPhrase) / framesPerStep), 0, 15);
                return pos;
            }
            case PlaybackMode::CHAIN: {
                prune_past(chainRowStartFrames_, currentFrame, framesPerPhrase);
                for (const auto& e : chainRowStartFrames_) {
                    int64_t into = currentFrame - e.second;
                    if (into >= 0 && into < framesPerPhrase) {
                        pos.chainRow = e.first;
                        pos.phraseStep = clampi(static_cast<int>(into / framesPerStep), 0, 15);
                        break;
                    }
                }
                pos.row = pos.phraseStep;
                return pos;
            }
            case PlaybackMode::SONG: {
                prune_past(songPositionStartFrames_, currentFrame, framesPerPhrase);
                for (const auto& e : songPositionStartFrames_) {
                    int64_t into = currentFrame - e.second;
                    if (into >= 0 && into < framesPerPhrase) {
                        pos.songRow = e.first.first;
                        pos.chainRow = e.first.second;
                        pos.phraseStep = clampi(static_cast<int>(into / framesPerStep), 0, 15);
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
    // boundary so an edit is heard on the next phrase loop instead of 2–3 phrases later. Returns the
    // frame the host must clearScheduledNotesFrom(), or −1 when there is nothing to roll back (the
    // Sequencer holds no engine handle — that call is the host's, exactly as it is Kotlin's).
    int64_t notify_data_changed(int64_t currentFrame) {
        if (!isPlaying_) return -1;

        const Checkpoint* hit = nullptr;
        for (const Checkpoint& c : checkpoints_) {
            if (c.frame > currentFrame) { hit = &c; break; }
        }
        if (!hit) return -1;
        Checkpoint cp = *hit;   // by value: the pops below invalidate the pointer

        nextFrameToSchedule_ = cp.frame;
        switch (playbackMode_) {
            case PlaybackMode::CHAIN:
                nextChainRowToSchedule_ = cp.chainRow;
                break;
            case PlaybackMode::SONG:
                nextSongRowToSchedule_ = cp.songRow;
                nextSongChainRowToSchedule_ = cp.songChainRow;
                break;
            default: break;   // PHRASE: resetting nextFrameToSchedule_ is enough
        }
        while (!checkpoints_.empty() && checkpoints_.back().frame >= cp.frame) checkpoints_.pop_back();
        return cp.frame;
    }

    // True once an EQM has overridden the master EQ this session. The host reads it BEFORE stop()
    // (which clears it) and restores project.masterEqSlot — mirroring PlaybackController.stop(),
    // including its guard: no restore when currentProject_ is null (the render path owns its own).
    bool eqm_active() const { return eqmActive_; }
    bool has_live_project() const { return currentProject_ != nullptr; }

    // ── transport starts ──

    void playPhrase(int phraseId) {
        stop();
        currentProject_ = project_;
        currentPhraseId_ = phraseId;
        playbackStartFrame_ = getCurrentFrame();
        if (phraseId < 0 || phraseId > 255) return;
        const Phrase& phrase = project_->phrases[phraseId];
        playbackMode_ = PlaybackMode::PHRASE;
        isPlaying_ = true;
        int tempo = project_->tempo;
        int64_t framesPerStep = frames_per_step(tempo, sampleRate_);
        router_.t_play("PHRASE", "id=" + hex2(phraseId), playbackStartFrame_, tempo, sampleRate_);
        nextFrameToSchedule_ = playbackStartFrame_;
        SchedulePhraseResult r = schedulePhrase(phrase, playbackStartFrame_, 0,
                                                project_transpose_semitones(*project_), framesPerStep, 0);
        nextFrameToSchedule_ += r.framesScheduled;
    }

    void playChain(int chainId) {
        stop();
        currentProject_ = project_;
        currentChainId_ = chainId;
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
            int phraseId = chain.phraseRefs[firstRow];
            int transposeSemitones = chain_transpose_semitones(chain, firstRow);
            SchedulePhraseResult r = schedulePhrase(project_->phrases[phraseId], playbackStartFrame_, 0,
                                                    transposeSemitones + project_transpose_semitones(*project_),
                                                    framesPerStep, 0);
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
        nextSongRowToSchedule_ = startRow;
        nextSongChainRowToSchedule_ = 0;
        songPositionStartFrames_.clear();
    }

    void stop() {
        router_.t_stop();
        isPlaying_ = false;
        playbackMode_ = PlaybackMode::STOPPED;
        chainRowStartFrames_.clear();
        songPositionStartFrames_.clear();
        checkpoints_.clear();
        eqmActive_ = false;   // the host reads eqm_active() BEFORE calling stop() to restore the master EQ
        nextSongChainRowToSchedule_ = 0;
        // Full per-track reset: playback is a pure function of the project (see PlaybackController.stop).
        for (int i = 0; i < 8; ++i) trackStates_[i] = TrackState();
    }

    // ── the polling scheduler (live modes) ──

    void updatePlaybackBuffer() {
        if (!isPlaying_ || project_ == nullptr) return;
        const Project& project = *project_;
        int tempo = project.tempo;
        int64_t framesPerStep = frames_per_step(tempo, sampleRate_);
        int64_t framesPerPhrase = framesPerStep * 16;
        int64_t currentFrame = getCurrentFrame();

        int64_t bufferRemaining = nextFrameToSchedule_ - currentFrame;
        int64_t minBuffer = static_cast<int64_t>(BUFFER_PHRASES) * framesPerPhrase;
        if (bufferRemaining >= minBuffer) return;

        switch (playbackMode_) {
            case PlaybackMode::PHRASE: {
                const Phrase& phrase = project.phrases[currentPhraseId_];
                TrackState& trackState = trackStates_[0];
                save_checkpoint(Checkpoint{nextFrameToSchedule_});
                int hopStartRow = trackState.consumeHopTarget();
                int effectiveStartRow = hopStartRow >= 0 ? hopStartRow : 0;
                SchedulePhraseResult r = schedulePhrase(phrase, nextFrameToSchedule_, 0,
                                                        project_transpose_semitones(project), framesPerStep,
                                                        effectiveStartRow);
                nextFrameToSchedule_ += r.framesScheduled;
                break;
            }
            case PlaybackMode::CHAIN: {
                const Chain& chain = project.chains[currentChainId_];
                TrackState& trackState = trackStates_[0];
                if (trackState.trackStopped) {
                    nextChainRowToSchedule_ = (nextChainRowToSchedule_ + 1) % 16;
                    nextFrameToSchedule_ += framesPerPhrase;
                    return;
                }
                int nextRow = findNextNonEmptyChainRow(nextChainRowToSchedule_, chain);
                if (nextRow >= 0) {
                    int phraseId = chain.phraseRefs[nextRow];
                    int transposeSemitones = chain_transpose_semitones(chain, nextRow)
                                             + project_transpose_semitones(project);
                    save_checkpoint(Checkpoint{nextFrameToSchedule_, nextRow});
                    int hopStartRow = trackState.consumeHopTarget();
                    int effectiveStartRow = hopStartRow >= 0 ? hopStartRow : 0;
                    SchedulePhraseResult r = schedulePhrase(project.phrases[phraseId], nextFrameToSchedule_, 0,
                                                            transposeSemitones, framesPerStep, effectiveStartRow);
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

                int maxChainLength = 0;
                for (int trackId = 0; trackId < 8; ++trackId) {
                    if (nextSongRowToSchedule_ < static_cast<int>(project.tracks[trackId].chainRefs.size())) {
                        int chainId = project.tracks[trackId].chainRefs[nextSongRowToSchedule_];
                        if (chainId >= 0 && chainId < 256) {
                            const Chain& chain = project.chains[chainId];
                            int chainLength = 0;
                            for (int i = 0; i < 16; ++i) if (!chain_is_empty(chain, i)) chainLength++;
                            maxChainLength = std::max(maxChainLength, chainLength);
                        }
                    }
                }

                if (maxChainLength == 0) {
                    nextSongRowToSchedule_++;
                    nextSongChainRowToSchedule_ = 0;
                    for (int i = 0; i < 8; ++i) trackStates_[i].trackStopped = false;
                    if (nextSongRowToSchedule_ >= songLength) nextSongRowToSchedule_ = 0;
                } else if (nextSongChainRowToSchedule_ < maxChainLength) {
                    save_checkpoint(Checkpoint{nextFrameToSchedule_, 0,
                                               nextSongRowToSchedule_, nextSongChainRowToSchedule_});
                    bool scheduledAny = false;
                    int64_t maxFramesScheduled = 0;
                    for (int trackId = 0; trackId < 8; ++trackId) {
                        TrackState& trackState = trackStates_[trackId];
                        if (trackState.trackStopped) continue;
                        if (nextSongRowToSchedule_ < static_cast<int>(project.tracks[trackId].chainRefs.size())) {
                            int chainId = project.tracks[trackId].chainRefs[nextSongRowToSchedule_];
                            if (chainId >= 0 && chainId < 256) {
                                const Chain& chain = project.chains[chainId];
                                if (!chain_is_empty(chain, nextSongChainRowToSchedule_)) {
                                    int phraseId = chain.phraseRefs[nextSongChainRowToSchedule_];
                                    int transposeSemitones = chain_transpose_semitones(chain, nextSongChainRowToSchedule_)
                                                             + project_transpose_semitones(project);
                                    int hopStartRow = trackState.consumeHopTarget();
                                    int effectiveStartRow = hopStartRow >= 0 ? hopStartRow : 0;
                                    SchedulePhraseResult r = schedulePhrase(project.phrases[phraseId], nextFrameToSchedule_,
                                                                            trackId, transposeSemitones, framesPerStep,
                                                                            effectiveStartRow);
                                    put_song_position(nextSongRowToSchedule_, nextSongChainRowToSchedule_,
                                                      nextFrameToSchedule_);
                                    scheduledAny = true;
                                    if (r.framesScheduled > maxFramesScheduled) maxFramesScheduled = r.framesScheduled;
                                }
                            }
                        }
                    }
                    if (scheduledAny) {
                        nextFrameToSchedule_ += maxFramesScheduled;
                        nextSongChainRowToSchedule_++;
                    } else {
                        nextSongChainRowToSchedule_++;
                    }
                } else {
                    nextSongRowToSchedule_++;
                    nextSongChainRowToSchedule_ = 0;
                    for (int i = 0; i < 8; ++i) trackStates_[i].trackStopped = false;
                    if (nextSongRowToSchedule_ >= songLength) nextSongRowToSchedule_ = 0;
                }
                break;
            }
            default: break;
        }
    }

    // ── the render-path scheduler (render mode) ──
    // trackFilter == nullptr schedules all tracks; muted tracks always skipped. Mirrors
    // scheduleSongRowRange; ptplay only uses the full (null-filter) form.
    int64_t scheduleSongRowRange(int startRow, int endRow, const std::set<int>* trackFilter = nullptr) {
        const Project& project = *project_;
        for (int i = 0; i < 8; ++i) trackStates_[i] = TrackState();
        int64_t framesPerStep = frames_per_step(project.tempo, sampleRate_);
        router_.t_play("RENDER", "rows=" + hex2(startRow) + "-" + hex2(endRow), 0, project.tempo, sampleRate_);

        int64_t currentFrame = 0;
        for (int songRow = startRow; songRow <= endRow; ++songRow) {
            for (int i = 0; i < 8; ++i) trackStates_[i].trackStopped = false;

            int maxChainLength = 0;
            for (int trackId = 0; trackId < 8; ++trackId) {
                if (trackFilter && trackFilter->find(trackId) == trackFilter->end()) continue;
                const Track& track = project.tracks[trackId];
                if (track.mute) continue;
                if (songRow >= static_cast<int>(track.chainRefs.size())) continue;
                int chainId = track.chainRefs[songRow];
                if (chainId < 0 || chainId >= 256) continue;
                const Chain& chain = project.chains[chainId];
                int length = 0;
                for (int i = 0; i < 16; ++i) if (!chain_is_empty(chain, i)) length++;
                if (length > maxChainLength) maxChainLength = length;
            }

            for (int chainRow = 0; chainRow < maxChainLength; ++chainRow) {
                int64_t maxFramesScheduled = 0;
                bool scheduledAny = false;
                for (int trackId = 0; trackId < 8; ++trackId) {
                    if (trackFilter && trackFilter->find(trackId) == trackFilter->end()) continue;
                    TrackState& trackState = trackStates_[trackId];
                    if (trackState.trackStopped) continue;
                    const Track& track = project.tracks[trackId];
                    if (track.mute) continue;
                    if (songRow >= static_cast<int>(track.chainRefs.size())) continue;
                    int chainId = track.chainRefs[songRow];
                    if (chainId < 0 || chainId >= 256) continue;
                    const Chain& chain = project.chains[chainId];
                    if (chain_is_empty(chain, chainRow)) continue;
                    int phraseId = chain.phraseRefs[chainRow];
                    if (phraseId < 0 || phraseId >= 256) continue;
                    int transposeSemitones = chain_transpose_semitones(chain, chainRow)
                                             + project_transpose_semitones(project);
                    int hopStartRow = trackState.consumeHopTarget();
                    int effectiveStartRow = hopStartRow >= 0 ? hopStartRow : 0;
                    SchedulePhraseResult r = schedulePhrase(project.phrases[phraseId], currentFrame, trackId,
                                                            transposeSemitones, framesPerStep, effectiveStartRow);
                    scheduledAny = true;
                    if (r.framesScheduled > maxFramesScheduled) maxFramesScheduled = r.framesScheduled;
                }
                if (scheduledAny) currentFrame += maxFramesScheduled;
            }
        }
        router_.t_stop();
        return currentFrame;
    }

  private:
    struct SchedulePhraseResult {
        int rowsScheduled = 0;
        bool hopTriggered = false;
        bool trackStopped = false;
        int64_t framesScheduled = 0;
    };
    struct ScheduleStepResult {
        bool noteScheduled = false;
        bool hopTriggered = false;
    };

    // Snapshot taken just BEFORE scheduling a phrase, so notify_data_changed() can roll the buffer
    // back to the earliest future phrase boundary without disturbing the phrase now playing.
    struct Checkpoint {
        int64_t frame = 0;
        int chainRow = 0;
        int songRow = 0;
        int songChainRow = 0;
    };

    int64_t getCurrentFrame() const { return currentFrame_; }

    void save_checkpoint(const Checkpoint& cp) {
        checkpoints_.push_back(cp);
        if (checkpoints_.size() > 4) checkpoints_.pop_front();   // ring of 4, oldest = earliest unplayed
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
    void put_song_position(int songRow, int chainRow, int64_t frame) {
        songPositionStartFrames_.emplace_back(std::make_pair(songRow, chainRow), frame);
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

    int findNextNonEmptyChainRow(int startRow, const Chain& chain) {
        int row = startRow;
        int attempts = 0;
        while (chain_is_empty(chain, row) && attempts < 16) {
            row = (row + 1) % 16;
            attempts++;
        }
        return attempts >= 16 ? -1 : row;
    }

    SchedulePhraseResult schedulePhrase(const Phrase& phrase, int64_t startFrame, int trackId,
                                        int transposeSemitones, int64_t framesPerStep, int startRow) {
        const Project& project = *project_;
        int scheduledNotes = 0;
        int rowsScheduled = 0;
        TrackState& trackState = trackStates_[clampi(trackId, 0, 7)];

        if (trackState.trackStopped) return SchedulePhraseResult{0, false, true, 0};

        int64_t framesPerTic = framesPerStep / TICS_PER_STEP;
        int localGrooveStep = trackState.grooveStep;
        bool anyGrooveActive = false;

        int effectiveStartRow = clampi(startRow, 0, 15);
        int64_t frameOffset = 0;

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

            int64_t stepDuration;
            if (currentGrooveActive) {
                anyGrooveActive = true;
                int stepTics = groove_ticks_for_step(currentGroove, localGrooveStep);
                stepDuration = framesPerTic * stepTics;  // 0 tics = skip step
            } else {
                stepDuration = framesPerStep;
            }

            if (stepDuration == 0) {
                rowsScheduled++;
                localGrooveStep++;
                continue;
            }

            int64_t targetFrame = startFrame + frameOffset;

            ScheduleStepResult stepResult = scheduleStepWithEffects(step, targetFrame, stepDuration, trackId,
                                                                    transposeSemitones, trackState, stepIndex);
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

    ScheduleStepResult scheduleStepWithEffects(const PhraseStep& step, int64_t targetFrame, int64_t stepDuration,
                                               int trackId, int transposeSemitones, TrackState& trackState,
                                               int stepIndex) {
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

        // KIL: soft note-off at the sample-accurate kill frame (with LAT + KIL-offset latency)
        if (params.killAtFrame.has_value()) {
            int64_t fpt = stepDuration / TICS_PER_STEP;
            int64_t killFrame = *params.killAtFrame + (delayTicks + params.killOffsetTicks) * fpt;
            router_.note_off(killFrame, trackId, NOTE_OFF_RELEASE);
            trackState.clearPitchMod();
        }

        // STEP 2.3: live per-note / mixer FX (PAN / REV / DEL / BCK / EQN / EQM)
        {
            bool triggeredNote = hasNote && !skipNote;
            int64_t voiceFxFrame = triggeredNote ? effectiveTargetFrame + 1 : effectiveTargetFrame;
            if (!triggeredNote && params.panValue.has_value())
                router_.cc(effectiveTargetFrame, trackId, CC_PAN, *params.panValue / 255.0f);
            if (params.reverbSendValue.has_value())
                router_.cc(voiceFxFrame, trackId, CC_REVERB_SEND, *params.reverbSendValue / 255.0f);
            if (params.delaySendValue.has_value())
                router_.cc(voiceFxFrame, trackId, CC_DELAY_SEND, *params.delaySendValue / 255.0f);
            if (params.bckValue.has_value())
                router_.ext_reverse(voiceFxFrame, trackId, *params.bckValue == 0, triggeredNote);
            if (params.eqnSlot.has_value())
                router_.ext_eq_slot(voiceFxFrame, trackId, *params.eqnSlot);
            if (params.eqmSlot.has_value()) {
                // Master/mixer EQ — global, persists until the next EQM; the host restores the mixer
                // value on stop() (PlaybackController.eqmActive).
                router_.ext_master_eq(effectiveTargetFrame, *params.eqmSlot);
                eqmActive_ = true;
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

        return ScheduleStepResult{noteScheduled, hopTriggered};
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
    void emit_note(const NoteArgs& a, const Note& note) {
        if (note == Note::EMPTY()) return;
        router_.note_on(a);
    }

    // The random draws for CHA / RND / RNL / ARP-RANDOM. Thin names kept so the port reads against
    // PlaybackController.kt line for line: `rng_int(15)` is its `Random.nextInt(15)`, `rng_range(a, b)`
    // its `Random.nextInt(a, b)` — half-open at the top, negative `lo` allowed. See rng.h for why this
    // is the one piece of songcore proven statistically rather than by a golden.
    int rng_int(int bound) { return rng_.next_int(bound); }
    int rng_range(int lo, int hi) { return rng_.next_int(lo, hi); }

    Rng rng_;
    MidiRouter& router_;
    const Project* project_ = nullptr;
    // Mirrors PlaybackController.currentProject: set only by the live transport starts, left null on
    // the render path — the STEP 2.4 empty-step tempo fallback depends on this (see there).
    const Project* currentProject_ = nullptr;
    int sampleRate_ = 44100;

    TrackState trackStates_[8];
    int64_t currentFrame_ = 0;
    int64_t nextFrameToSchedule_ = 0;
    int nextChainRowToSchedule_ = 0;
    int nextSongRowToSchedule_ = 0;
    int nextSongChainRowToSchedule_ = 0;
    int currentPhraseId_ = 0;
    int currentChainId_ = 0;
    int64_t playbackStartFrame_ = 0;
    PlaybackMode playbackMode_ = PlaybackMode::STOPPED;
    bool isPlaying_ = false;

    // ── side-records: UI cursor + live-edit rollback + the EQM restore flag (S5, SC-4/SC-2) ──
    std::deque<Checkpoint> checkpoints_;                                   // ring of 4
    std::deque<std::pair<int, int64_t>> chainRowStartFrames_;              // (chainRow, startFrame)
    std::vector<std::pair<std::pair<int, int>, int64_t>>
        songPositionStartFrames_;                                          // ((songRow, chainRow) → startFrame), insertion-ordered
    bool eqmActive_ = false;

    // Per-retrigger additive volume delta for RPT (Rxy), indexed by ramp nibble. Same constants as
    // PlaybackController.REPEAT_RAMP_DELTAS (single source of the ramp curve).
    static constexpr float REPEAT_RAMP_DELTAS[16] = {
        0.00f, -0.02f, -0.04f, -0.06f, -0.10f, -0.15f, -0.20f, -0.30f,
        0.00f,  0.02f,  0.04f,  0.06f,  0.10f,  0.15f,  0.20f,  0.30f
    };
};

}  // namespace songcore

#endif  // POCKETTRACKER_SONGCORE_SCHEDULER_H
