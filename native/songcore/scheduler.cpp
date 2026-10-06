// The sequencer's bodies; what each one is for is said at its declaration in scheduler.h.

#include "scheduler.h"

namespace songcore {

Sequencer::Sequencer(MidiRouter& router, const Project& project, int sample_rate)
    : router_(router), project_(&project), sampleRate_(sample_rate) {
    // ⚠️ Default Rng seeding folds a clock read with a static's address, so eight built in a row
    // can be eight copies of ONE stream (every chance gate in lockstep). Re-derive all eight from
    // track 0's draw.
    uint64_t hi = rngs_[0].next_u32();   // ⚠️ separate statements: the evaluation order of two
    uint64_t lo = rngs_[0].next_u32();   //    calls in one expression is unspecified
    seed_rng((hi << 32) ^ lo);
}

void Sequencer::seed_rng(uint64_t s) {
    for (int t = 0; t < 8; ++t) rngs_[t].seed(s + static_cast<uint64_t>(t) * 0x9E3779B97F4A7C15ULL);
}

void Sequencer::finish_phrase_now() {
    if (!isPlaying_ || project_ == nullptr) return;
    if (playbackMode_ != PlaybackMode::PHRASE && playbackMode_ != PlaybackMode::CHAIN) return;
    nextFrameToSchedule_ += finish_walk(playbackTrack_, frames_per_step(project_->tempo, sampleRate_));
}

PlaybackPosition Sequencer::getPlaybackPosition(int trackId) {
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

int Sequencer::track_scale_key(int trackId) const {
    const int k = trackStates_[clampi(trackId, 0, 7)].scaleKey;
    return k >= 0 ? k : (project_ ? project_->scaleKey : 0);
}

void Sequencer::release_mixer_vol_track(int track) {
    if (track >= 0 && track < 8) mixerVolTracks_ &= ~(1 << track);
}

void Sequencer::playPhrase(int phraseId, int trackId) {
    stop();
    currentProject_ = project_;
    currentPhraseId_ = phraseId;
    playbackTrack_ = clamp_track(trackId);
    playbackStartFrame_ = getCurrentFrame();
    if (phraseId < 0 || phraseId > 255) return;
    playbackMode_ = PlaybackMode::PHRASE;
    isPlaying_ = true;
    int tempo = project_->tempo;
    router_.t_play("PHRASE", "id=" + hex2(phraseId), playbackStartFrame_, tempo, sampleRate_);
    nextFrameToSchedule_ = playbackStartFrame_;
    begin_phrase(phraseId, playbackStartFrame_, playbackTrack_, project_transpose_semitones(*project_), 0);
    updatePlaybackBuffer();
}

void Sequencer::playChain(int chainId, int trackId) {
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
    router_.t_play("CHAIN", "id=" + hex2(chainId), playbackStartFrame_, tempo, sampleRate_);
    nextFrameToSchedule_ = playbackStartFrame_;
    nextChainRowToSchedule_ = 0;
    chainRowStartFrames_.clear();
    phraseStepStartFrames_.clear();
    int firstRow = findNextNonEmptyChainRow(0, chain);
    if (firstRow >= 0) {
        int phraseId = chain_phrase_ref(chain, firstRow);
        int transposeSemitones = chain_transpose_semitones(chain, firstRow);
        begin_phrase(phraseId, playbackStartFrame_, playbackTrack_,
                     transposeSemitones + project_transpose_semitones(*project_), 0, &chain, firstRow);
        chainRowStartFrames_.emplace_back(firstRow, playbackStartFrame_);
        nextChainRowToSchedule_ = firstRow + 1;
        updatePlaybackBuffer();
    }
}

void Sequencer::playSong(int startRow) {
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

void Sequencer::stop() {
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

LiveSlot Sequencer::live_queue(int trackId) const {
    const LiveSlot& q = liveQueue_[clamp_track(trackId)];
    if (q.firesAt >= 0 && currentFrame_ >= q.firesAt) return LiveSlot{};
    return q;
}

void Sequencer::playSongLive(int songRow, int mask) {
    playSong(songRow);
    liveMode_ = true;
    for (int t = 0; t < 8; ++t) {
        liveQueue_[t]  = LiveSlot{};
        liveSilent_[t] = ((mask >> t) & 1) == 0;
    }
}

RollbackPlan Sequencer::set_live_mode(bool on, int64_t currentFrame) {
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

RollbackPlan Sequencer::queue_live(int trackId, int songRow, bool immediate, int64_t currentFrame) {
    return arm_live_slot(clamp_track(trackId), LiveSlot{songRow, false, immediate}, currentFrame);
}

RollbackPlan Sequencer::queue_live_stop(int trackId, bool immediate, int64_t currentFrame) {
    return arm_live_slot(clamp_track(trackId), LiveSlot{-1, true, immediate}, currentFrame);
}

RollbackPlan Sequencer::queue_live_row(int songRow, bool immediate, int64_t currentFrame) {
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

int64_t Sequencer::buffer_head() const {
    if (playbackMode_ != PlaybackMode::SONG) return nextFrameToSchedule_;
    int64_t head = getCurrentFrame();
    bool anyLive = false;
    for (int t = 0; t < 8; ++t) {
        if (trackDone_[t]) continue;
        if (!anyLive || trackNextFrame_[t] < head) head = trackNextFrame_[t];
        anyLive = true;
    }
    return head;
}

void Sequencer::updatePlaybackBuffer(int64_t aheadMs) {
    if (!isPlaying_ || project_ == nullptr) return;
    const Project& project = *project_;
    int tempo = project.tempo;
    int64_t framesPerStep = frames_per_step(tempo, sampleRate_);
    int64_t framesPerPhrase = framesPerStep * 16;
    int64_t currentFrame = getCurrentFrame();
    const int64_t minBuffer = aheadMs * sampleRate_ / 1000;
    if (buffer_head() - currentFrame >= minBuffer) return;
    // ⚠️ The cap bounds the work one fill can do.
    const int stepCap = STEPS_PER_PHRASE_OF_DEPTH * static_cast<int>(1 + minBuffer / framesPerPhrase);

    switch (playbackMode_) {
        case PlaybackMode::PHRASE: {
            TrackState& trackState = trackStates_[playbackTrack_];
            for (int step = 0; step < stepCap && nextFrameToSchedule_ - currentFrame < minBuffer; ++step) {
                if (!trackState.walk.active) {
                    int hopStartRow = trackState.consumeHopTarget();
                    int effectiveStartRow = hopStartRow >= 0 ? hopStartRow : 0;
                    // HOP FF stopped the track: the phrase is silent from here.
                    if (!begin_phrase(currentPhraseId_, nextFrameToSchedule_, playbackTrack_,
                                      project_transpose_semitones(project), effectiveStartRow))
                        break;
                }
                nextFrameToSchedule_ += walk_phrase_row(playbackTrack_, framesPerStep);
            }
            break;
        }
        case PlaybackMode::CHAIN: {
            const Chain& chain = project.chains[currentChainId_];
            TrackState& trackState = trackStates_[playbackTrack_];
            for (int step = 0; step < stepCap && nextFrameToSchedule_ - currentFrame < minBuffer; ++step) {
                if (trackState.walk.active) {
                    nextFrameToSchedule_ += walk_phrase_row(playbackTrack_, framesPerStep);
                    continue;
                }
                if (trackState.trackStopped) {
                    nextChainRowToSchedule_ = (nextChainRowToSchedule_ + 1) % 16;
                    nextFrameToSchedule_ += framesPerPhrase;
                    continue;
                }
                int nextRow = findNextNonEmptyChainRow(nextChainRowToSchedule_, chain);
                if (nextRow < 0) { stop(); return; }
                int phraseId = chain_phrase_ref(chain, nextRow);
                int transposeSemitones = chain_transpose_semitones(chain, nextRow)
                                         + project_transpose_semitones(project);
                int hopStartRow = trackState.consumeHopTarget();
                int effectiveStartRow = hopStartRow >= 0 ? hopStartRow : 0;
                chainRowStartFrames_.emplace_back(nextRow, nextFrameToSchedule_);
                nextChainRowToSchedule_ = (nextRow + 1) % 16;
                begin_phrase(phraseId, nextFrameToSchedule_, playbackTrack_, transposeSemitones,
                             effectiveStartRow, &chain, nextRow);
                nextFrameToSchedule_ += walk_phrase_row(playbackTrack_, framesPerStep);
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
            // Fill whichever track is furthest behind, a row at a time, until each is deep
            // enough — so a two-row chain moves on while a sixteen-row one beside it runs. The
            // per-track walk is schedule_track_unit().
            for (int step = 0; step < stepCap; ++step) {
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

int64_t Sequencer::scheduleSongRowRange(int startRow, int endRow, const std::set<int>* trackFilter,
                                        int repeat) {
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

int64_t Sequencer::schedule_range_pass(const Project& project, int startRow, int endRow,
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
    // does not loop and takes no checkpoints (an export has no LIVE launch). Every unit ends a
    // track or advances a cursor, so it terminates without a step cap.
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

int64_t Sequencer::rewind_song_track(int trackId, int64_t currentFrame) {
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
    // A rewound track walks again; the walk decides afresh whether it is done.
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

RollbackPlan Sequencer::rewind_all_song_tracks(int64_t currentFrame) {
    RollbackPlan plan;
    for (int t = 0; t < 8; ++t) {
        const int64_t f = rewind_song_track(t, currentFrame);
        if (f >= 0) plan.frames[t] = f;
    }
    return plan;
}

RollbackPlan Sequencer::arm_live_slot(int trackId, LiveSlot slot, int64_t currentFrame) {
    RollbackPlan plan;
    if (!liveMode_) return plan;
    const int64_t f = rewind_song_track(trackId, currentFrame);
    if (f >= 0) plan.frames[trackId] = f;
    // ⚠️ After the rewind, which un-stamps a launch it rolled back over.
    liveQueue_[trackId] = slot;
    return plan;
}

bool Sequencer::consume_live_queue(int trackId, bool chainEnd) {
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

void Sequencer::save_checkpoint(int trackId, Checkpoint cp) {
    cp.trackState = trackStates_[trackId];
    cp.rng = rngs_[trackId];
    cp.liveLoopFrame = liveLoopFrame_[trackId];
    checkpoints_[trackId].push_back(cp);
    // A ring of 4; the oldest is the earliest unplayed.
    if (checkpoints_[trackId].size() > 4) checkpoints_[trackId].pop_front();
}

void Sequencer::put_song_position(int trackId, int songRow, int chainRow, int64_t frame) {
    songPositionStartFrames_.emplace_back(SongPos{trackId, songRow, chainRow}, frame);
}

void Sequencer::put_phrase_step_position(int trackId, int step, int64_t frame) {
    if (phraseStepStartFrames_.size() >= STEP_POSITION_CAP)
        phraseStepStartFrames_.erase(
            phraseStepStartFrames_.begin(),
            phraseStepStartFrames_.begin() + static_cast<long>(STEP_POSITION_CAP / 2));
    phraseStepStartFrames_.emplace_back(StepPos{trackId, step}, frame);
}

int Sequencer::step_in_force(int trackId, int64_t currentFrame) const {
    int     step  = -1;
    int64_t stamp = 0;
    for (const auto& e : phraseStepStartFrames_) {
        if (e.first.track != trackId || e.second > currentFrame) continue;
        if (step < 0 || e.second >= stamp) { stamp = e.second; step = e.first.step; }
    }
    return step;
}

int Sequencer::findNextNonEmptyChainRow(int startRow, const Chain& chain) {
    const int seed = ((startRow % CHAIN_ROWS) + CHAIN_ROWS) % CHAIN_ROWS;
    for (int i = 0; i < CHAIN_ROWS; ++i) {
        const int row = (seed + i) % CHAIN_ROWS;
        if (!chain_is_empty(chain, row)) return row;
    }
    return -1;
}

int Sequencer::song_cell_chain(const Project& project, int trackId, int songRow) {
    const std::vector<int>& refs = project.tracks[trackId].chainRefs;
    if (songRow < 0 || songRow >= static_cast<int>(refs.size())) return -1;
    const int id = refs[songRow];
    return (id >= 0 && id < 256) ? id : -1;
}

bool Sequencer::song_cell_plays(const Project& project, int trackId, int songRow) {
    const int chainId = song_cell_chain(project, trackId, songRow);
    return chainId >= 0 && !chain_is_empty(project.chains[chainId], 0);
}

int Sequencer::block_start_row(const Project& project, int trackId, int songRow) {
    int row = songRow;
    while (row > 0 && song_cell_plays(project, trackId, row - 1)) row--;
    return row;
}

int Sequencer::next_chain_row_no_wrap(const Chain& chain, int startRow) {
    for (int r = std::max(0, startRow); r < CHAIN_ROWS; ++r)
        if (!chain_is_empty(chain, r)) return r;
    return -1;
}

void Sequencer::advance_track_song_row(const Project& project, int trackId, int lastSongRow) {
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

void Sequencer::checkpoint_track(int trackId, int songRow, int rowUnit, bool take) {
    if (!take) return;
    Checkpoint cp;
    cp.frame = trackNextFrame_[trackId];
    cp.songRow = songRow;
    cp.songChainRow = rowUnit;
    save_checkpoint(trackId, cp);
}

void Sequencer::schedule_track_unit(const Project& project, int trackId, int64_t framesPerStep,
                                    int64_t framesPerPhrase, int lastSongRow,
                                    bool takeCheckpoint) {
    // LIVE is a separate function so SONG's path stays exactly as goldened. Never on the RENDER
    // path — an export has no transport to queue at.
    if (liveMode_ && lastSongRow < 0) {
        schedule_live_unit(project, trackId, framesPerStep, framesPerPhrase, takeCheckpoint);
        return;
    }

    TrackState& trackState = trackStates_[trackId];
    if (trackState.walk.active) {   // a phrase part-way through goes on where it stopped
        trackNextFrame_[trackId] += walk_phrase_row(trackId, framesPerStep);
        return;
    }
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
    // the sequence where it really is.
    const int transposeSemitones = chain_transpose_semitones(chain, chainRow)
                                   + project_transpose_semitones(project);
    const int hopStartRow = trackState.consumeHopTarget();
    const int effectiveStartRow = hopStartRow >= 0 ? hopStartRow : 0;
    // ⚠️ Recorded for a muted track too: the playhead says where the track IS.
    put_song_position(trackId, songRow, chainRow, trackNextFrame_[trackId]);
    trackChainRow_[trackId] = chainRow + 1;
    begin_phrase(chain_phrase_ref(chain, chainRow), trackNextFrame_[trackId], trackId, transposeSemitones,
                 effectiveStartRow, &chain, chainRow);
    trackNextFrame_[trackId] += walk_phrase_row(trackId, framesPerStep);
}

void Sequencer::schedule_live_unit(const Project& project, int trackId, int64_t framesPerStep,
                                   int64_t framesPerPhrase, bool takeCheckpoint) {
    TrackState& trackState = trackStates_[trackId];
    // A phrase part-way through goes on first: a launch lands only on a phrase boundary.
    if (trackState.walk.active) {
        trackNextFrame_[trackId] += walk_phrase_row(trackId, framesPerStep);
        return;
    }

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
    put_song_position(trackId, songRow, chainRow, trackNextFrame_[trackId]);
    trackChainRow_[trackId] = chainRow + 1;
    begin_phrase(chain_phrase_ref(chain, chainRow), trackNextFrame_[trackId], trackId, transposeSemitones,
                 effectiveStartRow, &chain, chainRow);
    trackNextFrame_[trackId] += walk_phrase_row(trackId, framesPerStep);
}

int64_t Sequencer::finish_walk(int trackId, int64_t framesPerStep) {
    int64_t frames = 0;
    while (trackStates_[clampi(trackId, 0, 7)].walk.active) frames += walk_phrase_row(trackId, framesPerStep);
    return frames;
}

bool Sequencer::begin_phrase(int phraseId, int64_t startFrame, int trackId, int transposeSemitones, int startRow,
                             const Chain* chain, int chainRow) {
    const Project& project = *project_;
    TrackState& trackState = trackStates_[clampi(trackId, 0, 7)];
    if (trackState.trackStopped) return false;

    PhraseWalk& w = trackState.walk;
    w.active          = true;
    w.phraseId        = clampi(phraseId, 0, 255);
    w.chainId         = chain ? chain->id : -1;
    w.transpose       = transposeSemitones;
    w.startFrame      = startFrame;
    w.row             = clampi(startRow, 0, 15);
    w.localGrooveStep = trackState.grooveStep;
    w.anyGrooveActive = false;
    w.frameOffset     = 0;
    w.rowsScheduled   = 0;

    // ─── The ramps this phrase declares (AUS/AUF — automation.h) ─────────────────────────────
    //
    // Pairing gives spans in step indices; the walk emits them using each step's real duration —
    // so grooves cost nothing and a HOP truncates a fade by ending the walk. A phrase entered below
    // its AUS runs no ramp. With a chain, spans can cross phrases (`find_ramps_in_chain`,
    // re-derived for every phrase).
    w.ramps = chain ? find_ramps_in_chain(project, *chain, chainRow, w.row)
                    : find_ramps(project.phrases[w.phraseId], w.row);
    // The last value each ramp emitted, for de-duplication (an ease curve holds one byte for many
    // ticks). Seeded with the curve's value one tic BEFORE this phrase: where the AUS is here that
    // clamps to the start byte the start effect already sent; in a crossed phrase it is where the
    // previous phrase left off. An EQ morph seeds the same way against its start preset.
    w.rampLast.clear();
    w.rampLast.reserve(w.ramps.size());
    for (const RampSpec& r : w.ramps) {
        const double seedT = (static_cast<double>(r.stepOffset) * TICS_PER_STEP - 1.0) /
                             (static_cast<double>(r.span) * TICS_PER_STEP);
        RampLastValue seed;
        if (r.kind == RampKind::EQ_PRESET)
            seed.eq = eq_morph_at(project, r.startByte, r.destByte, r.curveByte, seedT);
        else
            seed.byte = automation_value_byte(r.startByte, r.destByte, r.curveByte, seedT);
        w.rampLast.push_back(seed);
    }
    return true;
}

int64_t Sequencer::walk_phrase_row(int trackId, int64_t framesPerStep) {
    const Project& project = *project_;
    TrackState& trackState = trackStates_[clampi(trackId, 0, 7)];
    PhraseWalk& w = trackState.walk;
    if (!w.active) return 0;
    // Every random draw happens inside this call, so the track's stream is selected once here.
    schedulingTrack_ = clampi(trackId, 0, 7);
    const Phrase& phrase = project.phrases[w.phraseId];

    for (; w.row < 16; ++w.row) {
        const int stepIndex = w.row;
        const PhraseStep& step = phrase.steps[stepIndex];

        // Pre-scan GRV so a new groove takes effect on its own step; the last GRV wins.
        for (int fxSlot = 1; fxSlot <= 3; ++fxSlot) {
            if (step_fx_type(step, fxSlot) == FX_GRV) {
                trackState.grooveId = step_fx_value(step, fxSlot);
                w.localGrooveStep = 0;
            }
        }

        const Groove& currentGroove = project.grooves[clampi(trackState.grooveId, 0,
                                                             static_cast<int>(project.grooves.size()) - 1)];
        bool currentGrooveActive = groove_active_length(currentGroove) > 0;

        // The length comes from `timing.h` — one definition, the one the tools measure.
        if (currentGrooveActive) w.anyGrooveActive = true;
        int64_t stepDuration = groove_step_duration(currentGroove, w.localGrooveStep, framesPerStep);

        if (stepDuration == 0) {
            w.rowsScheduled++;
            w.localGrooveStep++;
            continue;
        }

        int64_t targetFrame = w.startFrame + w.frameOffset;

        stepRamps_ = w.ramps.empty() ? nullptr : &w.ramps;
        stepIndex_ = stepIndex;
        ScheduleStepResult stepResult = scheduleStepWithEffects(step, targetFrame, stepDuration, trackId,
                                                                w.transpose, trackState, stepIndex);
        stepRamps_ = nullptr;

        // ⚠️ A HOP row costs NOTHING — no time, no marker, no ramp tic; it IS the jump. So the walk
        // leaves before `frameOffset`, the playhead or a fade moves. `localGrooveStep` stays too,
        // or the next phrase enters on the wrong tic.
        if (stepResult.hopTriggered) {
            if (w.anyGrooveActive) trackState.grooveStep = w.localGrooveStep;
            // A hop leaving with nothing played may be part of a ring (TrackState::emptyHops).
            if (w.frameOffset == 0) {
                if (++trackState.emptyHops > MAX_EMPTY_HOPS) trackState.trackStopped = true;
            } else {
                trackState.emptyHops = 0;
            }
            w.active = false;
            return 0;
        }

        // The playhead's row stamp, made as the walk passes (put_phrase_step_position).
        put_phrase_step_position(trackId, stepIndex, targetFrame);

        // After the step's own events: a ramp is emitted as the walk passes, never ahead — a fade
        // baked past a HOP would keep moving the parameter after the phrase ended.
        if (!w.ramps.empty())
            emit_ramp_ticks(w.ramps, w.rampLast, w.chainId, stepResult.effectiveStep, stepIndex, targetFrame,
                            stepDuration, trackId, trackState, stepResult.noteFrame, stepResult.fxFrame);
        w.rowsScheduled++;
        w.frameOffset += stepDuration;
        if (currentGrooveActive) w.localGrooveStep++;
        ++w.row;
        if (w.row < 16) return stepDuration;
        finish_phrase(trackState);
        return stepDuration;
    }

    finish_phrase(trackState);
    return 0;
}

void Sequencer::finish_phrase(TrackState& trackState) {
    PhraseWalk& w = trackState.walk;
    if (w.anyGrooveActive) trackState.grooveStep = w.localGrooveStep;
    if (w.rowsScheduled > 0) trackState.emptyHops = 0;
    w.active = false;
}

void Sequencer::emit_ramp_ticks(const std::vector<RampSpec>& ramps, std::vector<RampLastValue>& lastValue,
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

void Sequencer::emit_eq_morph(int64_t frame, const RampSpec& r, int trackId, const ExtEqMorphPayload& m) {
    if (r.global) router_.ext_master_eq_morph(frame, m);
    else          router_.ext_eq_morph(frame, trackId, m);
}

PhraseStep Sequencer::applyChanceAndRandomize(const PhraseStep& step, TrackState& trackState, bool& skipNote) {
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
    // RND/RNL ADD a random 0..XY to the value already there, capped at the effect's ceiling.
    // 00 adds and draws nothing. In FX1, RNL adds 0..X to the note and 0..Y to the
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

Sequencer::ScheduleStepResult Sequencer::scheduleStepWithEffects(const PhraseStep& step, int64_t targetFrame, int64_t stepDuration,
                                                                 int trackId, int transposeSemitones, TrackState& trackState,
                                                                 int /*stepIndex*/) {
    StepPlay s(step, trackState, targetFrame, stepDuration, trackId, transposeSemitones);
    end_repeat_and_arpeggio(s);
    resolve_step(s);
    if (s.params.hopValue.has_value()) return take_hop(s);
    place_step(s);
    apply_step_state(s);
    expose_step_to_voice_at(s);
    if (s.triggers()) schedule_step_note(s);
    schedule_kill(s);
    emit_step_fx(s);
    if (!s.hasNote) emit_mid_note_pitch_and_volume(s);
    schedule_repeat(s);
    schedule_arpeggio(s);
    remember_column_fx(s);
    return ScheduleStepResult{s.triggers(), false, s.scheduledNoteFrame, s.voiceFxFrame, s.effectiveStep};
}

Note Sequencer::transposed(const Note& note, int semitones) {
    if (semitones == 0) return note;
    const int midi = note_to_midi(note);
    return midi >= 0 ? note_from_midi(clampi(midi + semitones, 0, 127)) : note;
}

void Sequencer::vibrato_from(int v, bool wide, int tempo, float& speed, float& depth) {
    const int speedNibble = (v >> 4) & 0x0F;
    const int depthNibble = v & 0x0F;
    if (wide) {
        speed = (2.0f + speedNibble * 0.5f) * 2.0f * (tempo / 120.0f);
        depth = depthNibble * 0.125f * 4.0f;
    } else {
        speed = (2.0f + speedNibble * 0.5f) * (tempo / 120.0f);
        depth = depthNibble * 0.125f;
    }
}

void Sequencer::end_repeat_and_arpeggio(StepPlay& s) {
    TrackState& ts = s.ts;
    const PhraseStep& step = s.step;
    const bool hasKill = step.fx1Type == FX_KILL || step.fx2Type == FX_KILL || step.fx3Type == FX_KILL;
    if (hasKill) { ts.clearRepeat(); ts.clearArpeggio(); }

    s.hasNote = !step_empty(step);
    if (s.hasNote) { ts.clearRepeat(); ts.clearArpeggio(); }

    s.savedRampPhraseVol = ts.repeatBasePhraseVol;
    if (ts.hasActiveRepeat() && ts.repeatRetrigCount > 0) {
        float oldDelta = REPEAT_RAMP_DELTAS[clampi(ts.repeatVolRamp, 0, 15)];
        s.savedRampVolume = clampf(ts.repeatBaseVolume + ts.repeatRetrigCount * oldDelta, 0.0f, 1.0f);
    }

    if (ts.hasActiveRepeat() && step_fx_type(step, ts.repeatActiveColumn) != FX_NONE) ts.clearRepeat();
    if (ts.hasActiveArpeggio() && step_fx_type(step, ts.arpeggioActiveColumn) != FX_NONE) ts.clearArpeggio();
}

void Sequencer::resolve_step(StepPlay& s) {
    const Project& project = *project_;
    s.effectiveStep = applyChanceAndRandomize(s.step, s.ts, s.skipNote);

    const Instrument& instrument = project.instruments[clampi(s.effectiveStep.instrument, 0,
                                                              static_cast<int>(project.instruments.size()) - 1)];
    s.velocityByte = clampi(s.effectiveStep.volume, 0, 127);
    s.velocityGain = (s.velocityByte / 127.0f) * (s.velocityByte / 127.0f);
    s.params = resolve_step_params(s.effectiveStep, s.targetFrame, hex_to_float(instrument.volume));
    s.phraseVol = s.params.volume;
    s.notePan = s.params.panValue.has_value() ? (*s.params.panValue / 255.0f) : hex_to_float(instrument.pan);
}

Sequencer::ScheduleStepResult Sequencer::take_hop(StepPlay& s) {
    if (*s.params.hopValue == 0xFF) s.ts.trackStopped = true;
    else                            s.ts.hopTargetRow = *s.params.hopValue & 0x0F;
    ScheduleStepResult hop;
    hop.hopTriggered  = true;
    hop.effectiveStep = s.effectiveStep;
    return hop;
}

void Sequencer::place_step(StepPlay& s) const {
    s.transpose = effective_transpose_semitones(s.transpose, *project_, s.effectiveStep.instrument,
                                                s.params.tsxMultiplier);
    s.delayTicks = s.params.delayTicks.value_or(0);
    s.effectiveTargetFrame = s.delayTicks > 0
                           ? s.targetFrame + s.delayTicks * (s.stepDuration / TICS_PER_STEP)
                           : s.targetFrame;
    s.voiceFxFrame       = s.triggers() ? s.effectiveTargetFrame + 1 : s.effectiveTargetFrame;
    s.scheduledNoteFrame = s.triggers() ? s.effectiveTargetFrame : -1;
}

void Sequencer::apply_step_state(StepPlay& s) {
    TrackState& ts = s.ts;
    const ResolvedStepParams& params = s.params;
    if (params.tableOverride.has_value() && *params.tableOverride >= 0) {
        ts.lastTableOverride = *params.tableOverride;
        s.tableId = *params.tableOverride;
    } else if (s.hasNote) {
        ts.lastTableOverride = -1;
    } else {
        s.tableId = ts.lastTableOverride;
    }

    if (params.tableHopTarget.has_value()) {
        const int targetRow = *params.tableHopTarget % 16;
        ts.lastTableStartRow = targetRow;
        if (!s.hasNote) router_.ext_table_row(s.effectiveTargetFrame, s.trackId, targetRow);
        s.tableRow = targetRow;
    }

    if (params.grooveId.has_value()) {
        ts.grooveId = *params.grooveId;
        ts.grooveStep = 0;
    }

    // SCG writes all eight TrackStates; on the same step SCA is applied second and wins.
    if (params.scaleGlobalByte.has_value()) {
        const int key = scale_cmd_key(*params.scaleGlobalByte);
        const int slot = scale_cmd_slot(*params.scaleGlobalByte);
        for (int t = 0; t < 8; ++t) { trackStates_[t].scaleSlot = slot; trackStates_[t].scaleKey = key; }
    }
    if (params.scaleTrackByte.has_value()) {
        ts.scaleSlot = scale_cmd_slot(*params.scaleTrackByte);
        ts.scaleKey  = scale_cmd_key(*params.scaleTrackByte);
    }
}

void Sequencer::expose_step_to_voice_at(const StepPlay& s) {
    stepEffective_   = &s.effectiveStep;
    stepTarget_      = s.targetFrame;
    stepDuration_    = s.stepDuration;
    stepNoteFrame_   = s.scheduledNoteFrame;
    stepFxFrame_     = s.voiceFxFrame;
    stepCarryBefore_ = s.ts.carry;
    stepCarryFrom_   = s.effectiveTargetFrame;
}

void Sequencer::schedule_step_note(StepPlay& s) {
    TrackState& ts = s.ts;
    const ResolvedStepParams& params = s.params;
    const Note note = transposed(s.effectiveStep.note, s.transpose);
    const int previousMidi = ts.lastNoteMidi;

    float pslInitialOffset = 0.0f, pslDuration = 0.0f, pbnRate = 0.0f, vibratoSpeed = 0.0f, vibratoDepth = 0.0f;

    if (params.pslDuration.has_value() && *params.pslDuration > 0 && previousMidi >= 0) {
        int currentMidi = note_to_midi(note);
        if (currentMidi >= 0 && previousMidi != currentMidi) {
            pslInitialOffset = static_cast<float>(previousMidi - currentMidi);
            pslDuration = static_cast<float>(*params.pslDuration);
        }
    }
    if (params.pbnValue.has_value() && *params.pbnValue != 0) {
        pbnRate = pitch_bend_rate(*params.pbnValue);
        ts.pitchBendActive = true;
    }
    if (params.pvbValue.has_value() && *params.pvbValue != 0) {
        vibrato_from(*params.pvbValue, false, project_->tempo, vibratoSpeed, vibratoDepth);
        ts.vibratoActive = true;
    }
    if (params.pvxValue.has_value() && *params.pvxValue != 0) {
        vibrato_from(*params.pvxValue, true, project_->tempo, vibratoSpeed, vibratoDepth);
        ts.vibratoActive = true;
    }

    NoteCarry& carry = ts.carry;
    carry = NoteCarry{};
    carry.velGain = s.velocityGain;
    carry.phraseVol = s.phraseVol;
    carry.pan = s.notePan;
    carry.vibSpeed = vibratoSpeed;
    carry.vibDepth = vibratoDepth;
    record_voice_commands(carry, params);
    // A note inside a VOL or PAN fade starts where the fade has got to; the fade's next tic need
    // not move to correct it.
    {
        const NoteCarry v = voice_at(ts, s.effectiveTargetFrame);
        carry.phraseVol = v.phraseVol;
        carry.pan = v.pan;
    }

    NoteArgs a;
    a.frame = s.effectiveTargetFrame; a.track = s.trackId; a.instrument = s.effectiveStep.instrument;
    a.notePitch = note.pitch; a.noteOctave = note.octave;
    a.velocity = s.velocityByte; a.velGain = s.velocityGain; a.volGain = carry.phraseVol; a.pan = carry.pan;
    a.start = params.startPoint; a.slice = params.sliIndex.value_or(-1);
    a.transpose = s.transpose; a.pit = params.pitSemitones.value_or(0); a.arp = 0;
    a.tableId = s.tableId; a.tableRow = s.tableRow;
    a.pslOff = pslInitialOffset; a.pslDur = pslDuration; a.pbnRate = pbnRate;
    a.vibSpd = vibratoSpeed; a.vibDep = vibratoDepth;
    emit_note(a, note);

    ts.lastNote = note;
    ts.lastInstrument = s.effectiveStep.instrument;
    ts.lastStartPoint = params.startPoint;
    ts.lastNoteMidi = note_to_midi(note);

    if (ts.hasPitchMod() && pbnRate == 0.0f && vibratoDepth == 0.0f) ts.clearPitchMod();
}

void Sequencer::schedule_kill(StepPlay& s) {
    if (!s.params.killAtFrame.has_value()) return;
    int64_t fpt = s.stepDuration / TICS_PER_STEP;
    int64_t killFrame = *s.params.killAtFrame + (s.delayTicks + s.params.killOffsetTicks) * fpt;
    router_.note_off(killFrame, s.trackId, NOTE_OFF_RELEASE);
    s.ts.clearPitchMod();
}

void Sequencer::emit_step_fx(StepPlay& s) {
    TrackState& ts = s.ts;
    const ResolvedStepParams& params = s.params;
    const int trackId = s.trackId;
    const int64_t voiceFxFrame = s.voiceFxFrame;
    const bool triggeredNote = s.triggers();
    if (!triggeredNote && params.panValue.has_value()) {
        router_.cc(s.effectiveTargetFrame, trackId, CC_PAN, *params.panValue / 255.0f);
        ts.carry.pan = *params.panValue / 255.0f;
    }
    // The note step recorded its own, on a freshly reset carry.
    if (!triggeredNote) record_voice_commands(ts.carry, params);
    if (params.reverbSendValue.has_value())
        router_.cc(voiceFxFrame, trackId, CC_REVERB_SEND, *params.reverbSendValue / 255.0f);
    if (params.delaySendValue.has_value())
        router_.cc(voiceFxFrame, trackId, CC_DELAY_SEND, *params.delaySendValue / 255.0f);
    if (params.bckValue.has_value())
        router_.ext_reverse(voiceFxFrame, trackId, *params.bckValue == 0, triggeredNote);
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
    // CRU's byte goes over whole; the engine splits it.
    if (params.driveValue.has_value())
        router_.cc(voiceFxFrame, trackId, CC_DRIVE, *params.driveValue / 255.0f);
    if (params.crushValue.has_value())
        router_.cc(voiceFxFrame, trackId, CC_CRUSH, *params.crushValue / 255.0f);
    if (params.fineTuneValue.has_value())
        router_.cc(voiceFxFrame, trackId, CC_FINE_TUNE, *params.fineTuneValue / 255.0f);
    // LPO sends the AUTHORED byte (the engine decodes it).
    if (params.loopSlideValue.has_value())
        router_.cc(voiceFxFrame, trackId, CC_LOOP_SLIDE, (*params.loopSlideValue & 0xFF) / 255.0f);
    if (params.eqnSlot.has_value())
        router_.ext_eq_slot(voiceFxFrame, trackId, *params.eqnSlot);
    // The mixer faders REPLACE the authored value and hold, so the host restores it on stop()
    // (as for EQM).
    if (params.trackVolValue.has_value()) {
        router_.cc(voiceFxFrame, trackId, CC_TRACK_VOL, *params.trackVolValue / 255.0f);
        mixerVolTracks_ |= 1 << clampi(trackId, 0, 7);
    }
    // Master volume and delay time ride TRACK_GLOBAL: they belong to no track, and the track lane
    // is where the EXTERNAL gate would swallow them (event.h).
    if (params.masterVolValue.has_value()) {
        router_.cc(s.effectiveTargetFrame, TRACK_GLOBAL, CC_MASTER_VOL, *params.masterVolValue / 255.0f);
        masterVolActive_ = true;
    }
    if (params.delayTimeValue.has_value()) {
        router_.cc(s.effectiveTargetFrame, TRACK_GLOBAL, CC_DELAY_TIME, *params.delayTimeValue / 255.0f);
        delayTimeActive_ = true;
    }
    if (params.eqmSlot.has_value()) {
        // Master EQ — global, held until the next EQM; the host restores it on stop().
        router_.ext_master_eq(s.effectiveTargetFrame, *params.eqmSlot);
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

void Sequencer::emit_mid_note_pitch_and_volume(StepPlay& s) {
    TrackState& ts = s.ts;
    const ResolvedStepParams& params = s.params;
    const int64_t frame = s.effectiveTargetFrame;
    // ⚠️ Deliberate: an empty-step pitch rate uses the LIVE tempo during playback but 120 on the
    // render path (currentProject_ is set only by live starts). The goldens record it.
    const int tempo = currentProject_ ? currentProject_->tempo : 120;
    if (params.volumeFromVxx) {
        router_.cc(frame, s.trackId, CC_VOLUME, s.phraseVol);
        ts.carry.phraseVol = s.phraseVol;
    }
    if (params.pbnValue.has_value()) {
        const int v = *params.pbnValue;
        router_.ext_pitch_rate(frame, s.trackId, v == 0 ? 0.0f : pitch_bend_rate(v), tempo);
        ts.pitchBendActive = v != 0;
    }
    auto vibrato = [&](int v, bool wide) {
        float speed = 0.0f, depth = 0.0f;
        if (v != 0) vibrato_from(v, wide, tempo, speed, depth);
        router_.ext_vibrato(frame, s.trackId, speed, depth);
        ts.vibratoActive = v != 0;
        ts.carry.vibSpeed = speed;
        ts.carry.vibDepth = depth;
    };
    if (params.pvbValue.has_value()) vibrato(*params.pvbValue, false);
    if (params.pvxValue.has_value()) vibrato(*params.pvxValue, true);
}

void Sequencer::schedule_repeat(StepPlay& s) {
    TrackState& ts = s.ts;
    const PhraseStep& effectiveStep = s.effectiveStep;
    const int64_t targetFrame = s.targetFrame;
    const int64_t stepDuration = s.stepDuration;

    int newRepeatColumn = 0;
    if (effectiveStep.fx1Type == FX_REPEAT && effectiveStep.fx1Value > 0) newRepeatColumn = 1;
    else if (effectiveStep.fx2Type == FX_REPEAT && effectiveStep.fx2Value > 0) newRepeatColumn = 2;
    else if (effectiveStep.fx3Type == FX_REPEAT && effectiveStep.fx3Value > 0) newRepeatColumn = 3;
    int newRepeatTicInterval = s.params.repeatCount.value_or(0);
    int newRepeatVolRamp = s.params.repeatVolRamp.value_or(0);

    if (newRepeatColumn > 0) {
        ts.repeatActiveColumn = newRepeatColumn;
        ts.repeatTicInterval = newRepeatTicInterval;
        ts.repeatVolRamp = newRepeatVolRamp;
        ts.repeatStartFrame = targetFrame;
        ts.repeatRetrigCount = 0;
        if (!s.hasNote && s.savedRampVolume >= 0.0f) {
            ts.repeatBaseVolume    = s.savedRampVolume;
            ts.repeatBasePhraseVol = s.savedRampPhraseVol;
        } else {
            ts.repeatBaseVolume    = ts.carry.velGain * ts.carry.phraseVol;
            ts.repeatBasePhraseVol = ts.carry.phraseVol;
        }
    }

    int activeRepeatInterval = newRepeatTicInterval > 0 ? newRepeatTicInterval
                             : (ts.hasActiveRepeat() ? ts.repeatTicInterval : 0);
    int activeVolRamp = newRepeatTicInterval > 0 ? newRepeatVolRamp
                      : (ts.hasActiveRepeat() ? ts.repeatVolRamp : 0);
    if (activeRepeatInterval <= 0 || ts.lastNote == Note::EMPTY()) return;

    const Note retrigNote = s.hasNote ? transposed(effectiveStep.note, s.transpose) : ts.lastNote;
    int retrigInstrument = s.hasNote ? effectiveStep.instrument : ts.lastInstrument;
    int retrigStartPoint = s.hasNote ? s.params.startPoint : ts.lastStartPoint;
    float rampDelta = REPEAT_RAMP_DELTAS[clampi(activeVolRamp, 0, 15)];

    int64_t stepEndFrame = targetFrame + stepDuration;
    int64_t gridStep = static_cast<int64_t>(activeRepeatInterval) * stepDuration;
    int64_t gridDenom = TICS_PER_STEP;
    if (gridStep <= 0) return;
    int64_t framesSinceStart = targetFrame - ts.repeatStartFrame;
    int64_t k = framesSinceStart <= 0 ? 0
              : (framesSinceStart * gridDenom + gridStep - 1) / gridStep;
    while (true) {
        int64_t triggerFrame = ts.repeatStartFrame + (k * gridStep) / gridDenom;
        if (triggerFrame >= stepEndFrame) break;
        if (triggerFrame >= targetFrame && triggerFrame != s.scheduledNoteFrame) {
            ts.repeatRetrigCount++;
            float retrigVolume = clampf(ts.repeatBaseVolume + ts.repeatRetrigCount * rampDelta, 0.0f, 1.0f);
            // The ramp's product with the VOL channel it was taken at divided out;
            // `emit_retrigger` multiplies in the channel as it stands NOW. A base taken at
            // VOL 00 has nothing to divide, and ramps the velocity alone.
            const float retrigVelGain = ts.repeatBasePhraseVol > 0.0f
                ? retrigVolume / ts.repeatBasePhraseVol
                : clampf(ts.carry.velGain + ts.repeatRetrigCount * rampDelta, 0.0f, 1.0f);
            NoteArgs a;
            a.frame = triggerFrame; a.track = s.trackId; a.instrument = retrigInstrument;
            a.notePitch = retrigNote.pitch; a.noteOctave = retrigNote.octave;
            a.velocity = -1; a.start = retrigStartPoint;
            a.transpose = s.transpose; a.arp = 0;
            a.tableId = ts.lastTableOverride; a.tableRow = -1;
            emit_retrigger(a, retrigNote, s.trackId, ts, retrigVelGain);
        }
        k++;
    }
}

void Sequencer::schedule_arpeggio(StepPlay& s) {
    TrackState& ts = s.ts;
    const PhraseStep& effectiveStep = s.effectiveStep;
    if (s.params.arcValue.has_value()) {
        int v = *s.params.arcValue;
        int mode = (v >> 4) & 0x0F;
        int speed = v & 0x0F;
        ts.arpeggioMode = clampi(mode, 0, 3);
        ts.arpeggioSpeed = speed > 0 ? speed : 4;
    }

    int newArpColumn = 0, newArpValue = 0;
    if (effectiveStep.fx1Type == FX_ARPEGGIO) { newArpColumn = 1; newArpValue = effectiveStep.fx1Value; }
    else if (effectiveStep.fx2Type == FX_ARPEGGIO) { newArpColumn = 2; newArpValue = effectiveStep.fx2Value; }
    else if (effectiveStep.fx3Type == FX_ARPEGGIO) { newArpColumn = 3; newArpValue = effectiveStep.fx3Value; }

    if (newArpColumn > 0 && newArpValue == 0) {
        ts.clearArpeggio();
    } else if (newArpColumn > 0 && newArpValue > 0) {
        ts.arpeggioActiveColumn = newArpColumn;
        ts.arpeggioValue = newArpValue;
        ts.arpeggioStartFrame = s.targetFrame;
    }

    int activeArpValue = newArpValue > 0 ? newArpValue
                       : (ts.hasActiveArpeggio() ? ts.arpeggioValue : 0);
    if (activeArpValue > 0 && ts.lastNote != Note::EMPTY()) schedule_arpeggio_notes(s);
}

void Sequencer::schedule_arpeggio_notes(StepPlay& s) {
    TrackState& ts = s.ts;
    int semi1 = (ts.arpeggioValue >> 4) & 0x0F;
    int semi2 = ts.arpeggioValue & 0x0F;

    const Note baseNote = s.hasNote ? transposed(s.effectiveStep.note, s.transpose) : ts.lastNote;
    int baseMidi = note_to_midi(baseNote);
    if (baseMidi < 0) return;

    int64_t framesPerTic = s.stepDuration / TICS_PER_STEP;
    int ticInterval = ts.arpeggioSpeed;
    int64_t framesPerArpNote = static_cast<int64_t>(ticInterval) * framesPerTic;
    if (framesPerArpNote <= 0) return;  // guard against division by zero

    int patternLength = ts.arpeggioMode == 2 ? 4 : 3;

    int instrumentId = s.hasNote ? s.effectiveStep.instrument : ts.lastInstrument;
    int startPoint = s.hasNote ? s.params.startPoint : ts.lastStartPoint;

    int64_t stepEndFrame = s.targetFrame + s.stepDuration;
    int64_t framesSinceStart = s.targetFrame - ts.arpeggioStartFrame;
    if (framesSinceStart < 0) return;

    int64_t triggerIndex = (framesSinceStart + framesPerArpNote - 1) / framesPerArpNote;
    int64_t triggerFrame = ts.arpeggioStartFrame + triggerIndex * framesPerArpNote;
    while (triggerFrame < stepEndFrame) {
        if (triggerFrame >= s.targetFrame && triggerFrame != s.scheduledNoteFrame) {
            int patternPosition = static_cast<int>(triggerIndex % patternLength);
            int arpMidi = getArpeggioNote(baseMidi, semi1, semi2, ts.arpeggioMode, patternPosition);
            NoteArgs a;
            a.frame = triggerFrame; a.track = s.trackId; a.instrument = instrumentId;
            a.notePitch = baseNote.pitch; a.noteOctave = baseNote.octave;
            a.velocity = -1; a.start = startPoint;
            a.transpose = s.transpose;
            a.arp = arpMidi - baseMidi;
            a.tableId = ts.lastTableOverride; a.tableRow = -1;
            emit_retrigger(a, baseNote, s.trackId, ts);
        }
        triggerIndex++;
        triggerFrame += framesPerArpNote;
    }
}

void Sequencer::remember_column_fx(StepPlay& s) {
    for (int col = 1; col <= 3; ++col) {
        int fxType = step_fx_type(s.step, col);
        if (fxType != FX_NONE && fxType != FX_RND && fxType != FX_RNL && fxType != FX_CHA) {
            s.ts.lastColFxType[col] = fxType;
            s.ts.lastColFxValue[col] = step_fx_value(s.step, col);
        }
    }
}

int Sequencer::getArpeggioNote(int baseMidi, int semi1, int semi2, int mode, int position) {
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

void Sequencer::record_voice_commands(NoteCarry& c, const ResolvedStepParams& p) {
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

int64_t Sequencer::fade_tic_frame(int k) const {
    const int64_t raw = stepTarget_ + k * (stepDuration_ / TICS_PER_STEP);
    return raw == stepNoteFrame_ ? raw + 1 : raw;
}

double Sequencer::fade_t_at(const RampSpec& r, int64_t f) const {
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

NoteCarry Sequencer::voice_at(const TrackState& ts, int64_t frame, unsigned* faded) const {
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

void Sequencer::reapply_voice(int64_t frame, int trackId, const NoteCarry& v, unsigned only) {
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

void Sequencer::emit_retrigger(NoteArgs a, const Note& note, int trackId, const TrackState& ts, float velGain) {
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

void Sequencer::carry_fade(NoteCarry& c, const RampSpec& r, int byte) {
    if (r.ccId == CC_VOLUME)   c.phraseVol = byte / 255.0f;
    else if (r.ccId == CC_PAN) c.pan = byte / 255.0f;
    else                       c.set_cc(r.ccId, byte / 255.0f);
}

void Sequencer::emit_note(NoteArgs a, const Note& note) {
    if (note == Note::EMPTY()) return;
    apply_track_scale(a);
    router_.note_on(a);
}

void Sequencer::apply_track_scale(NoteArgs& a) const {
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

}  // namespace songcore
