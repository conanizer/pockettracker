// SongcoreHost's MIDI side: the live input's drain, the mapped knobs and MIDI LEARN, and SYNC IN's
// follow. What each one is for is said at its declaration in host.h.

#include "host.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace songcore {

// ─── MIDI in ─────────────────────────────────────────────────────────────────────────────────────

void SongcoreHost::set_midi_in_play(int instrument, int track, int voices, bool velocity) {
    midiInInstrument_ = instrument;
    midiInTrack_      = track;
    midiInVoices_     = voices;
    midiInVelocity_   = velocity;
}

void SongcoreHost::reset_midi_in() {
    midiIn_.request_reset();
    if (!engine_) midiIn_.service_requests();
}

void SongcoreHost::LiveInput::apply(const Event& ev, bool external) {
    switch (ev.type) {
        case EV_NOTE_ON:
            // The routing gate's live form: EXTERNAL raises no voice, and a flip to one ends
            // the sounding internal note.
            if (external) { engine->scheduleKill(ev.frame, ev.track); return; }
            engine->scheduleProgramNote(ev.frame, ev.track, ev.instrument, ev.noteOn, engine->tempo());
            return;
        case EV_NOTE_OFF:
            switch (ev.noteOff.mode) {
                case NOTE_OFF_CUT: engine->scheduleCut(ev.frame, ev.track);        return;
                case NOTE_OFF_KEY: engine->scheduleKeyRelease(ev.frame, ev.track); return;
                default:           engine->scheduleNoteOff(ev.frame, ev.track);    return;
            }
        case EV_CC:
            // A cable sends literal controller numbers, never a CCA-CCD slot.
            EngineConsumer::apply_cc(*engine, ev.frame, ev.track, ev.cc.param, f32_from_bits(ev.cc.valueBits));
            return;
        default:
            return;   // a bend arrives through `wheel`; a program change stays in the router
    }
}

void SongcoreHost::arm_midi_in() {
    const MidiRoute route = build_midi_route(project_, midiInInstrument_, midiInTrack_, midiInVoices_,
                                             controlChannel_, learnArmed_, midiInVelocity_);
    if (!midiRoutePublished_ || std::memcmp(&route, &midiRouteLast_, sizeof route) != 0) {
        midiIn_.publish_route(route);
        midiRouteLast_      = route;
        midiRoutePublished_ = true;
    }
    if (!engine_) return;
    consumer_.push_tables(project_);
    // ⚠️ A program carries its SF slot and rate ratio, which move (PATCH, load, eviction) without
    // passing `push_instrument`. A live key gets no per-note re-send, so re-send what moved.
    const int count = std::min(static_cast<int>(project_.instruments.size()), POOL_INSTRUMENTS);
    for (int id = 0; id < count; ++id) {
        const Instrument& ins = project_.instruments[static_cast<size_t>(id)];
        const float ratio = routing_.sampleRateRatio[id];
        if (routing_.sfSlot[id] == programRouting_.sfSlot[id] && ratio == programRouting_.sampleRateRatio[id])
            continue;
        push_instrument_params(*engine_, ins, routing_, tempo_in_force(project_), sampleRate_);
        programRouting_.sfSlot[id]          = routing_.sfSlot[id];
        programRouting_.sampleRateRatio[id] = ratio;
    }
}

void SongcoreHost::drain_midi_in() {
    MidiInSeen seen;
    while (midiIn_.pop_seen(seen)) {
        if (seen.msg.status == EV_CC) {
            lastCcChannel_ = static_cast<int>(seen.msg.channel);
            // Noticed on EVERY channel, not just the control one — "your knob is on channel 6"
            // is how the user finds the right `CTL CH`.
            if (learnArmed_) {
                learnController_ = seen.msg.data1;
                learnChannel_    = static_cast<int>(seen.msg.channel);
                ++learnEvents_;
            }
        }
        if (seen.kind == MidiInSeen::MAPPED) {
            mappedCcWrites_ += static_cast<uint64_t>(apply_mapped_cc(seen.msg.data1, seen.msg.data2));
        }
        for (int j = 0; j < seen.count; ++j) {
            const Event& ev = seen.events[j];
            // Asked BEFORE either consumer learns from the record.
            const bool external = record_is_external(ev);
            consumer_.observe_live(ev);
            if (midiInThru_) {
                external_.consume_live(ev);
                if (external) ++midiInThruSent_;
            } else if (external) {
                ++midiInThruSuppressed_;
            }
        }
        if (midiInObserver_) midiInObserver_->on_midi_in(seen.msg, seen.events, seen.count);
    }
}

bool SongcoreHost::record_is_external(const Event& ev) const {
    int16_t instrument = ev.instrument;
    if (instrument == INSTRUMENT_NONE) instrument = external_.track_instruments().current(ev.track);
    if (instrument < 0 || static_cast<size_t>(instrument) >= project_.instruments.size()) return false;
    return instrument_routes_external(project_.instruments[static_cast<size_t>(instrument)]);
}

// ─── a mapped knob ───────────────────────────────────────────────────────────────────────────────

void SongcoreHost::set_midi_control_channel(int ch) {
    controlChannel_ = (ch < 0 || ch > MIDI_CTL_CH_ALL) ? -1 : ch;
}

int SongcoreHost::apply_mapped_cc(int controller, int value) {
    int applied = 0;
    for (const MidiMapping& m : project_.midiMappings) {
        if (m.controller != controller) continue;
        const MapDest* d = map_dest(m.dest);
        if (!d) continue;   // an id from a newer version

        if (!write_mapped(project_, m, scale_cc(value, m.rangeMin, m.rangeMax))) continue;
        ++applied;
        if (!engine_) continue;

        if (d->scope == MapScope::INSTRUMENT) {
            // VOL and PAN are baked into a note when it is scheduled, so they reach the next row
            // the walk reaches; every other mapped parameter is engine state a voice reads live.
            push_instrument(m.scopeIndex);
        } else {
            push_mapped_dest(*engine_, project_, d->id, m.scopeIndex);
            release_song_hold(d->id, m.scopeIndex);
        }
    }
    return applied;
}

void SongcoreHost::release_song_hold(MapDestId id, int scopeIndex) {
    switch (id) {
        case MapDestId::TRACK_VOL:  seq_.release_mixer_vol_track(scopeIndex); break;
        case MapDestId::MASTER_VOL: seq_.release_master_vol();                break;
        case MapDestId::DLY_TIME:   seq_.release_delay_time();                break;
        default: break;
    }
}

// ─── SYNC IN ─────────────────────────────────────────────────────────────────────────────────────

// ⚠️ With INPUT and OUTPUT on one port (THRU is off there) and SYNC OUT on, the clock coming in is
// this device's own: following it would chase itself.
bool SongcoreHost::sync_in_active() const {
    return syncIn_ != SYNC_IN_OFF && (midiInThru_ || !external_.sync_out());
}

bool SongcoreHost::clock_present(const MidiClockLine& line) const {
    return line.usable() &&
           static_cast<double>(seq_.clock() - line.lastFrame) < MidiSyncIn::STOPPED_TICKS * line.period;
}

int64_t SongcoreHost::offset_frames() const {
    return static_cast<int64_t>(external_.offset_ms()) * sampleRate_ / 1000;
}

double SongcoreHost::midi_clock_bpm() const {
    return clock_present(clockLine_) ? clockLine_.bpm(sampleRate_) : 0.0;
}

void SongcoreHost::follow_midi_clock() {
    midiIn_.sync().read_line(clockLine_, clockLineSeen_);
    const bool on = sync_in_active();

    MidiTransportEvent ev;
    while (midiIn_.sync().pop_event(ev)) {
        if (!on || syncIn_ != SYNC_IN_FULL) continue;
        if (ev.kind == MidiTransportEvent::STOP) {
            if (seq_.is_playing()) stop();
            continue;
        }
        start_from_position(ev);
    }

    // The clock's tempo while it leads: a following take keeps its last when the clock drops out; when
    // stopped it is what the next START would play at. A take that does not follow keeps the song's.
    int bpm = 0;
    if (on) {
        const bool    leads = following_clock_now();
        const MidiClockLine& l = leads ? followLine_ : clockLine_;
        if (leads ? l.usable() : (!seq_.is_playing() && clock_present(l))) {
            const double exact = l.bpm(sampleRate_);
            const int    held  = project_.clockTempo;
            // ±0.6 BPM of hysteresis, so a clock at 127.5 does not flicker between 127 and 128.
            bpm = (held > 0 && std::fabs(exact - held) < 0.6) ? held
                                                               : std::clamp(static_cast<int>(std::lround(exact)), 20, 999);
        }
    }
    set_clock_tempo(bpm);
}

void SongcoreHost::start_from_position(const MidiTransportEvent& ev) {
    // A take already running is stopped as STOP stops it: its lookahead must not play beside the new.
    if (seq_.is_playing()) stop();
    int offset = 0;
    const int row = nominal_spp_row(project_, ev.position, offset);
    startPending_ = true;
    startEvent_   = ev;
    before_play();
    if (seq_.live_mode()) {
        // LIVE launches the row as a scene, as from a standing start.
        int mask = 0;
        for (int t = 0; t < 8 && t < static_cast<int>(project_.tracks.size()); ++t) {
            const std::vector<int>& refs = project_.tracks[static_cast<size_t>(t)].chainRefs;
            if (row < static_cast<int>(refs.size()) && refs[static_cast<size_t>(row)] >= 0 &&
                refs[static_cast<size_t>(row)] < 256)
                mask |= 1 << t;
        }
        seq_.playSongLive(row, mask, offset);
    } else {
        seq_.playSong(row, offset);
    }
    after_play();
}

void SongcoreHost::plan_follow_start() {
    const bool received = startPending_;
    startPending_ = false;
    followTake_   = false;
    followLine_   = MidiClockLine{};
    followLate_   = 0;
    seq_.set_follow(0, -1);
    if (!sync_in_active()) return;
    midiIn_.sync().read_line(clockLine_, clockLineSeen_);

    const int64_t offset = offset_frames();
    int64_t start = 0;
    if (received) {
        // The downbeat is heard where it arrived: the speaker's delay earlier. That has passed by now,
        // so the first step sounds from the next audio rendered.
        followDownbeat_ = startEvent_.tick;
        start           = startEvent_.frame - offset;
        followLate_     = std::max<int64_t>(0, seq_.clock() - start);
    } else if (clock_present(clockLine_)) {
        // This device's own START: on the clock's next step — its transport's steps, if it runs.
        const MidiClockLine& l = clockLine_;
        const double ahead = static_cast<double>(seq_.clock() + offset) - l.frame;
        int64_t n = l.tick + std::max<int64_t>(0, static_cast<int64_t>(std::ceil(ahead / l.period)));
        if (l.downbeat >= 0) {
            const int64_t r = ((n - l.downbeat) % 6 + 6) % 6;
            if (r != 0) n += 6 - r;
        }
        followDownbeat_ = n;
        start           = std::llround(l.frame_of(n)) - offset;
    } else {
        return;   // no clock arriving: the song's own tempo
    }
    followTake_ = true;
    seq_.set_start_frame(start);
    if (clockLine_.usable()) followLine_ = clockLine_;
}

void SongcoreHost::apply_follow_step(int64_t aheadMs) {
    if (!following_clock_now()) {
        seq_.set_follow(0, -1);
        external_.set_follow(0, 0);
        return;
    }
    midiIn_.sync().read_line(clockLine_, clockLineSeen_);
    // A take started before the tempo was known walked its first step on a guess, and nothing past it.
    const bool guessed = !followLine_.usable();
    if (clockLine_.usable()) followLine_ = clockLine_;
    if (!followLine_.usable()) {
        // The tempo is not known yet: walk only the take's first step, from where it is heard. Three
        // ticks are a third of a step, so it is known before the second step must be walked.
        const int64_t first = seq_.playback_start_frame() + followLate_;
        if (seq_.grid_step() == 0) seq_.shift_unwalked(first - seq_.grid_frame());
        seq_.set_follow(guess_step(), first + 1);
        return;
    }
    // Where the grid's next step is played: on the clock — but a take that started late comes onto it
    // a quarter of a step at a time, so no early step is cut short.
    const double nominal = 6.0 * followLine_.period;
    const double late    = std::max(0.0, static_cast<double>(followLate_) -
                                             0.25 * nominal * static_cast<double>(seq_.grid_step()));
    const double target  = followLine_.frame_of(followDownbeat_ + 6 * seq_.grid_step()) -
                           static_cast<double>(offset_frames()) + late;
    // While the line is young its period is still rough, and while the take is young it may still be
    // catching up: walk only what is about to play, and first put the next step's start where it
    // belongs — nothing has been walked from it. A step walked on a guess ends there too. A long
    // operation's deep fill is left alone.
    const bool young = (followLine_.ticks < MidiSyncIn::FIT_TICKS / 2 ||
                        followLine_.tick - followDownbeat_ < MidiSyncIn::FIT_TICKS / 2) &&
                       aheadMs <= Sequencer::HORIZON_MS;
    if (young || guessed) seq_.shift_unwalked(std::llround(target) - seq_.grid_frame());
    const int64_t gridFrame = seq_.grid_frame();
    const double  error     = target - static_cast<double>(gridFrame);
    followError_ = error;
    const int64_t block = engine_ ? engine_->getDeviceBlockFrames() : 0;
    const int64_t limit = young ? seq_.clock() + block + YOUNG_AHEAD_MS * sampleRate_ / 1000 : -1;
    // Half the error closed over the steps this fill walks, each at most a quarter of a step longer
    // or shorter — a wrong first guess is caught up in a step or two.
    const int64_t reach = limit >= 0 ? limit : seq_.clock() + aheadMs * sampleRate_ / 1000;
    const double  steps = gridFrame < reach ? std::ceil(static_cast<double>(reach - gridFrame) / nominal) : 1.0;
    const double  cap   = 0.25 * nominal;
    const double  corr  = std::max(-cap, std::min(cap, 0.5 * error / std::max(1.0, steps)));
    followStep_ = std::max<int64_t>(1, std::llround(nominal + corr));
    seq_.set_follow(followStep_, limit);
    external_.set_follow(tempo_in_force(project_), followStep_ * 4);
}

int64_t SongcoreHost::guess_step() const {
    return followStep_ > 0 ? followStep_ : frames_per_step(tempo_in_force(project_), sampleRate_);
}

void SongcoreHost::set_clock_tempo(int bpm) {
    if (project_.clockTempo == bpm) return;
    const int before = tempo_in_force(project_);
    project_.clockTempo = bpm;
    const int now = tempo_in_force(project_);
    if (now == before || !engine_) return;
    engine_->setTempo(now);
    // A synced delay counts note lengths, so it moves with the tempo — unless a TIM holds it.
    if (project_.delay.sync && !held_by_song().delayTime)
        engine_->setDelayTime(project_.delay.time, true, static_cast<float>(now));
}

}  // namespace songcore
