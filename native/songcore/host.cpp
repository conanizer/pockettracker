// SongcoreHost's bodies; what each one is for is said at its declaration in host.h.

#include "host.h"

namespace songcore {

SongcoreHost::SongcoreHost(AudioEngine* engine, int sampleRate)
    : engine_(engine),
      sampleRate_(sampleRate),
      seq_(router_, project_, sampleRate),
      consumer_(engine, &project_, &routing_),
      external_(&project_) {
    // The engine consumer is the bus's permanent subscriber; the trace writer joins only while
    // tracing (set_trace) and sees the identical records.
    if (engine_) router_.add_consumer(&consumer_);
    // ⚠️ The EXTERNAL consumer is attached always, port or not: half its job is bookkeeping (which
    // track owns which note), and attaching mid-song would leave it blind to what is sounding.
    router_.add_consumer(&external_);

    // MIDI in: the drain runs on the ENGINE's thread (see below). Nothing here touches a port.
    midiLive_.pipeline = &midiIn_;
    midiLive_.engine   = engine_;
    if (engine_) engine_->setLiveInput(&midiLive_);
}

SongcoreHost::~SongcoreHost() {
    if (engine_) engine_->setLiveInput(nullptr);
    set_trace(false, "");
}

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

bool SongcoreHost::push_project(const std::string& blob) {
    // allow_exceptions=false: a malformed blob leaves the previous project intact.
    json j = json::parse(blob, /*cb=*/nullptr, /*allow_exceptions=*/false);
    if (j.is_discarded() || !j.is_object()) return false;

    Project parsed = parse_project(j);
    normalize_and_migrate(parsed);   // pool repair + v0→1 table-volume migration
    project_ = std::move(parsed);
    projectSha_ = sha1_hex(blob);
    // Table data may have changed, so the consumer's "already sent" cache must go.
    consumer_.invalidate_tables();
    return true;
}

int64_t SongcoreHost::play_song_live(int songRow, int mask) {
    before_play(); seq_.playSongLive(songRow, mask); return after_play();
}

int64_t SongcoreHost::play_chain(int chainId, int trackId) {
    before_play(); seq_.playChain(chainId, trackId);   return after_play();
}

int64_t SongcoreHost::play_phrase(int phraseId, int trackId) {
    before_play(); seq_.playPhrase(phraseId, trackId); return after_play();
}

void SongcoreHost::stop() {
    // ⚠️ Two sources arm this restore: a phrase EQM (the scheduler knows) and a TABLE row's EQM
    // (applied inside the engine). Read the engine's latch UNCONDITIONALLY, or it leaks into the
    // next take.
    const bool table_eqm = engine_ && engine_->takeTableMasterEqTouched();
    if (engine_ && (seq_.eqm_active() || table_eqm) && seq_.has_live_project()) {
        engine_->setMasterEqSlot(project_.masterEqSlot);
    }
    // VTR/VMV replaced the faders; put the authored values back, also before the queues clear so
    // a queued fader move cannot land after the restore.
    if (engine_ && seq_.mixer_vol_active() && seq_.has_live_project()) {
        const int tracks = static_cast<int>(project_.tracks.size());
        for (int i = 0; i < 8 && i < tracks; ++i)
            engine_->setTrackVolume(i, hex_to_float(project_.tracks[static_cast<size_t>(i)].volume));
        engine_->setMasterVolume(hex_to_float(project_.masterVolume));
    }
    // The delay time a TIM took over — two sources again; read the engine's latch unconditionally.
    const bool table_tim = engine_ && engine_->takeTableDelayTimeTouched();
    if (engine_ && (seq_.delay_time_active() || table_tim) && seq_.has_live_project()) {
        engine_->setDelayTime(project_.delayTime, project_.delaySync,
                              static_cast<float>(project_.tempo));
    }
    sync_clock();
    seq_.stop();
    // Every note the cable holds, ended now; `seq_.stop()` does not reach the router.
    external_.panic();
    // …and the incoming keys, so the next chord does not steal from a note that is over. A
    // request: the allocator is the drain's.
    midiIn_.request_release_keys();
    if (!engine_) midiIn_.service_requests();
    if (engine_) {
        engine_->clearScheduledNotes();   // the lookahead: notes, kills and param updates
        // The sounding voices, RAMPED rather than cut (a cut is a full-scale step). The audio
        // thread finishes the ramp after this returns.
        engine_->stopAllRamped();
        engine_->stopMetronome();         // the click is neither queued nor a voice
    }
    consumer_.clear_track_mask();
    flush_trace();
}

void SongcoreHost::poll() {
    sync_clock();
    // The audio thread's drain needs the routing facts and every table a key could land on; with
    // no engine, this thread IS the drain.
    arm_midi_in();
    if (!engine_) midiIn_.run(seq_.clock(), nullptr);
    // What the drain handled since the last poll: mapped knobs, thru, counters, observer.
    drain_midi_in();
    keep_walking();
    // TEMPO is editable while playing, so the beat length is pushed every time.
    if (engine_) engine_->setMetronomeBeat(frames_per_quarter());
}

void SongcoreHost::keep_walking(int64_t aheadMs) {
    sync_clock();
    seq_.updatePlaybackBuffer(aheadMs);
    // ⚠️ The MIDI queue is released here (unless a sender thread owns it), even when stopped: a
    // LEN gate and a panic's note-offs are owed after the last note.
    if (!midiPumpExternal_) external_.pump(seq_.clock());
    flush_trace();
}

int64_t SongcoreHost::schedule_song_range(int startRow, int endRow, const std::set<int>* trackFilter,
                                          int repeat) {
    sync_clock();
    int64_t frames = seq_.scheduleSongRowRange(startRow, endRow, trackFilter, repeat);
    flush_trace();
    return frames;
}

void SongcoreHost::prepare_render(int startRow, int endRow) {
    if (!engine_) return;
    // ⚠️ The cable is detached for a render: the whole song is scheduled at once and never
    // polled, so an attached ExternalConsumer would fire it all at the hardware afterwards.
    external_.panic();
    router_.remove_consumer(&external_);
    songcore::prepare_render(*engine_, project_, routing_, startRow, endRow);
    consumer_.clear_track_mask();
    sync_clock();                   // the frame counter is back at 0 — re-read it
}

RenderStats SongcoreHost::render_to_wav(const std::string& path, int64_t songFrames,
                                        int stemsMode, bool applyMasterBus,
                                        const std::function<void(float)>& progress) {
    if (!engine_) return RenderStats();
    RenderOptions opts;
    opts.stemsMode      = stemsMode;
    opts.applyMasterBus = applyMasterBus;
    return songcore::render_to_wav(*engine_, project_, songFrames, path, opts, progress);
}

void SongcoreHost::finish_render() {
    if (!engine_) return;
    songcore::finish_render(*engine_, project_);
    consumer_.clear_track_mask();
    router_.add_consumer(&external_);   // the cable is live again (add_consumer is idempotent)
}

RenderStats SongcoreHost::render_song_range_to_wav(int startRow, int endRow, const std::string& path,
                                                   const RenderOptions& opts,
                                                   const std::function<void(float)>& progress,
                                                   int repeat) {
    if (!engine_) return RenderStats();
    prepare_render(startRow, endRow);
    const int64_t songFrames = schedule_song_range(startRow, endRow, nullptr, repeat);
    RenderStats stats = render_to_wav(path, songFrames, opts.stemsMode, opts.applyMasterBus, progress);
    finish_render();
    return stats;
}

RenderStats SongcoreHost::render_song_to_wav(const std::string& path,
                                             const RenderOptions& opts,
                                             const std::function<void(float)>& progress) {
    const SongBounds b = find_song_bounds(project_);
    if (b.empty()) return RenderStats();
    return render_song_range_to_wav(b.startRow, b.endRow, path, opts, progress);
}

MediaLoadResult SongcoreHost::load_media(const std::string& baseDir) {
    lastMediaLoad_ = MediaLoadResult();
    if (!engine_) return lastMediaLoad_;
    mediaRoots_.baseDir = baseDir;
    lastMediaLoad_ =
        load_project_media(*engine_, project_, baseDir, mediaRoots_.appRoot, routing_);
    return lastMediaLoad_;
}

void SongcoreHost::push_params() {
    if (!engine_) return;
    push_live_params(*engine_, project_, routing_);
}

void SongcoreHost::push_instrument(int id, bool refreshSounding) {
    if (!engine_) return;
    if (id < 0 || id >= static_cast<int>(project_.instruments.size())) return;
    push_instrument_params(*engine_, project_.instruments[id], routing_, project_.tempo, sampleRate_);
    // `sampleId` is the index the params were written at and the one a voice remembers.
    // ⚠️ Only for an EDIT: the refresh wipes what a table row or phrase command set on the voice.
    if (refreshSounding) engine_->refreshSoundingInstrument(project_.instruments[id].sampleId);
}

void SongcoreHost::push_globals() {
    if (!engine_) return;
    push_mixer(*engine_, project_, held_by_song());
}

void SongcoreHost::set_midi_control_channel(int ch) {
    controlChannel_ = (ch < 0 || ch > MIDI_CTL_CH_ALL) ? -1 : ch;
}

AudioEngine::BlockTiming SongcoreHost::block_timing() const {
    return engine_ ? engine_->getBlockTiming() : AudioEngine::BlockTiming{};
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

MixerHeld SongcoreHost::held_by_song() const {
    MixerHeld held;
    if (!engine_ || !seq_.has_live_project()) return held;
    held.faderTracks = seq_.mixer_vol_tracks();
    held.masterFader = seq_.master_vol_active();
    held.masterEq    = seq_.eqm_active() || engine_->tableMasterEqTouchedPeek();
    held.delayTime   = seq_.delay_time_active() || engine_->tableDelayTimeTouchedPeek();
    return held;
}

void SongcoreHost::set_eq_band(int slot, int band, int type, int freqHex, int gainHex, int qHex) {
    if (!engine_) return;
    engine_->setEqBand(slot, band, type, freqHex, gainHex, qHex);
}

void SongcoreHost::set_master_eq_slot(int slot) {
    if (!engine_) return;
    engine_->setMasterEqSlot(slot);
}

void SongcoreHost::set_instrument_eq_slot(int id, int slot) {
    if (!engine_) return;
    engine_->setInstrumentEqSlot(id, slot);
}

void SongcoreHost::set_reverb_input_eq(int slot) {
    if (!engine_) return;
    engine_->setReverbInputEq(slot);
}

void SongcoreHost::set_delay_input_eq(int slot) {
    if (!engine_) return;
    engine_->setDelayInputEq(slot);
}

bool SongcoreHost::spectrum_for_source(int source, int instrId, int numBins, float* out) const {
    if (!engine_ || numBins <= 0 || !out) return false;
    engine_->getSpectrumMagnitudesForSource(source, instrId, numBins, out);
    return true;
}

void SongcoreHost::set_instrument_type(int id, InstrumentType type) {
    songcore::set_instrument_type(engine_, project_, id, type, routing_);
    push_instrument(id);   // a no-op without an engine
}

void SongcoreHost::clear_instrument(int id) {
    songcore::clear_instrument(engine_, project_, id, routing_);
    push_instrument(id);
}

int SongcoreHost::sf_preset_count(int id) const {
    if (!engine_ || id < 0 || id >= POOL_INSTRUMENTS) return 0;
    return soundfont_preset_count(*engine_, project_.instruments[static_cast<size_t>(id)],
                                  mediaRoots_);
}

int SongcoreHost::sf_preset_index(int id) const {
    if (!engine_ || id < 0 || id >= POOL_INSTRUMENTS) return 0;
    return soundfont_preset_index(*engine_, project_.instruments[static_cast<size_t>(id)],
                                  mediaRoots_);
}

std::string SongcoreHost::sf_preset_name(int id) const {
    if (!engine_ || id < 0 || id >= POOL_INSTRUMENTS) return "---";
    return soundfont_preset_name(*engine_, project_.instruments[static_cast<size_t>(id)],
                                 mediaRoots_);
}

void SongcoreHost::set_sf_preset_by_index(int id, int index) {
    if (!engine_ || id < 0 || id >= POOL_INSTRUMENTS) return;
    songcore::set_soundfont_preset_by_index(*engine_, project_.instruments[static_cast<size_t>(id)],
                                            index, mediaRoots_);
}

void SongcoreHost::sync_sf_preset(int id) {
    if (!engine_ || id < 0 || id >= POOL_INSTRUMENTS) return;
    songcore::sync_instrument_soundfont(*engine_, project_.instruments[static_cast<size_t>(id)],
                                        routing_, mediaRoots_);
}

bool SongcoreHost::request_sf_preset(int id) {
    if (!engine_ || id < 0 || id >= POOL_INSTRUMENTS) return true;
    return songcore::request_instrument_soundfont(
        *engine_, project_.instruments[static_cast<size_t>(id)], routing_, mediaRoots_);
}

void SongcoreHost::poll_sf_load() {
    if (!engine_) return;
    songcore::collect_instrument_soundfont(*engine_, project_, routing_, mediaRoots_);
}

bool SongcoreHost::load_project_file(const std::string& path, const std::string& baseDir) {
    std::string blob;
    if (!pt_read_file(path.c_str(), blob)) return false;

    // ⚠️ The transport stops BEFORE the document is replaced. Left running, the scheduler walks
    // the old song's position through the new data, falls behind the clock, and the new song
    // starts late and without its first row. A parse failure therefore leaves the transport
    // stopped with the previous project intact — there is no position to resume to.
    const bool wasPlaying = seq_.is_playing();
    stop();

    if (!push_project(blob)) return false;

    // ⚠️ Both, in this order, or the project you hear is not the one you loaded.
    load_media(baseDir);
    push_params();

    // Loading while playing SWITCHES SONGS: a hard cut (the media decode blocks), then the new song
    // from row 0 — an old CHAIN or PHRASE id means nothing in the new document. `play_song`
    // re-reads the clock after the disk work. Nothing starts that was not already playing.
    if (wasPlaying) play_song(0);
    return true;
}

void SongcoreHost::new_project() {
    // This stops the transport because NEW ends the session. It does NOT make freeing PCM safe:
    // `clearAllSamples()` does that itself, stopping voices under `sampleEditMutex`.
    stop();
    songcore::new_project(project_);

    if (engine_) {
        engine_->clearAllSamples();
        engine_->clearAllSoundfonts();
    }
    routing_.reset();     // the ratios and SF slots described the old project's media

    invalidate_tables();
    push_params();
}

void SongcoreHost::clean_inst(const std::string& baseDir) {
    songcore::clean_unused_inst(project_);
    load_media(baseDir);   // clears samples, SoundFonts and routing, then reloads
    invalidate_tables();
    push_params();
}

bool SongcoreHost::load_sample(int id, const std::string& path) {
    if (!load_instrument_sample(engine_, project_, id, path, routing_)) return false;
    push_instrument(id);   // the fresh source needs its slot's filter/window/loop
    return true;
}

bool SongcoreHost::load_soundfont(int id, const std::string& path) {
    if (!load_instrument_soundfont(engine_, project_, id, path, routing_)) return false;
    push_instrument(id);
    return true;
}

bool SongcoreHost::last_load_ran_out_of_memory() const {
    return engine_ && engine_->lastLoadFailure() == AudioEngine::LoadFailure::OUT_OF_MEMORY;
}

bool SongcoreHost::last_load_cancelled() const {
    return engine_ && engine_->lastLoadFailure() == AudioEngine::LoadFailure::CANCELLED;
}

bool SongcoreHost::load_instrument_preset(int id, const std::string& path) {
    std::string blob;
    if (!pt_read_file(path.c_str(), blob)) return false;

    json j = json::parse(blob, /*cb=*/nullptr, /*allow_exceptions=*/false);
    if (j.is_discarded() || !j.is_object()) return false;

    const InstrumentPreset ip = parse_instrument_preset(j);
    const bool sourceOk = apply_instrument_preset(engine_, project_, id, ip, routing_, mediaRoots_);
    invalidate_tables();   // the preset may have brought a table with it
    push_instrument(id);
    return sourceOk;
}

bool SongcoreHost::preview_file(const std::string& path) {
    if (!engine_) return false;
    return preview_sample_file(*engine_, path) > 0;
}

void SongcoreHost::clear_previews() {
    if (!engine_) return;
    clear_preview_slots(*engine_);
}

float SongcoreHost::sample_playback_position(int id) const {
    return engine_ ? engine_->getSamplePlaybackPosition(id) : -1.0f;
}

std::vector<float> SongcoreHost::sample_waveform(int id, int bins, int startFrame, int endFrame,
                                                 int channel) const {
    std::vector<float> out(static_cast<size_t>(std::max(bins, 0)) * 2, 0.0f);
    if (!engine_ || bins <= 0) return out;

    const int  len   = engine_->getSampleLength(id);
    const bool whole = (startFrame <= 0) && (endFrame <= 0 || endFrame >= len);

    if (engine_->hasStereoData(id)) {
        // Stereo always uses the channel-aware entry point: the plain one averages, and
        // SOURCE=LEFT must draw the left channel.
        engine_->getSampleWaveformRangeSource(id, whole ? 0 : startFrame, whole ? len : endFrame,
                                              out.data(), bins, channel);
    } else if (whole) {
        engine_->getSampleWaveform(id, out.data(), bins);
    } else {
        engine_->getSampleWaveformRange(id, startFrame, endFrame, out.data(), bins);
    }
    return out;
}

std::vector<int> SongcoreHost::detect_transients(int id, int sensitivity) const {
    if (!engine_) return {};
    int       markers[128];
    const int n = engine_->detectTransients(id, sensitivity, markers, 128);
    return std::vector<int>(markers, markers + std::max(n, 0));
}

int SongcoreHost::find_zero_crossing(int id, int frame, int dir, int sourceMode) const {
    return engine_ ? engine_->findZeroCrossing(id, frame, dir, 512, sourceMode) : frame;
}

void SongcoreHost::apply_sample_fx(int id, int fxType, int fxValue) {
    if (!engine_) return;
    engine_->applySampleFx(id, fxType, fxValue,
                           static_cast<float>(sample_rate_of(id)), project_.limiterPreGain);
}

void SongcoreHost::apply_rate_and_bits(int id, int factor, int bits) {
    songcore::apply_rate_and_bits(engine_, routing_, rateCache_, id, factor, bits);
}

void SongcoreHost::pitch_shift_sample(int id, float semitones) {
    songcore::pitch_shift_sample(engine_, rateCache_, id, semitones);
}

void SongcoreHost::time_stretch_sample(int id, float ratio) {
    songcore::time_stretch_sample(engine_, rateCache_, id, ratio);
}

void SongcoreHost::preview_sample_editor(int id, int sourceMode, int64_t selStart, int64_t selEnd,
                                         int totalFrames, int pitchSemitones) {
    if (!engine_ || id < 0 || id >= POOL_INSTRUMENTS) return;
    Instrument& ins = project_.instruments[static_cast<size_t>(id)];

    const Note savedRoot = ins.root;
    // A pending pitch shift is heard by transposing the ROOT; nothing is resampled until SAVE.
    if (pitchSemitones != 0)
        ins.root = note_from_midi(std::clamp(note_to_midi(ins.root) + pitchSemitones, 0, 127));

    const int slot = prepare_source_preview(*engine_, id, sourceMode);

    // A voice's window is keyed on the SLOT it plays from, so the params go to the scratch slot
    // when there is one.
    const int savedSampleId = ins.sampleId;
    if (slot != id) ins.sampleId = slot;
    push_instrument_playback_params(*engine_, ins);
    // After the push, which clears any previous window. The scratch slot is a frame-for-frame
    // copy, so the selection indexes both.
    if (totalFrames > 0 && selEnd > selStart)
        engine_->setInstrumentFrameWindow(slot, static_cast<int>(selStart), static_cast<int>(selEnd));
    preview_instrument_dry(*engine_, ins, slot, routing_.sampleRateRatio[id]);
    if (slot != id) ins.sampleId = savedSampleId;

    ins.root = savedRoot;   // read at SCHEDULE time, so it can go back at once
}

void SongcoreHost::finish_sample_preview(int id) {
    if (!engine_ || id < 0 || id >= POOL_INSTRUMENTS) return;
    Instrument& ins = project_.instruments[static_cast<size_t>(id)];
    push_instrument_playback_params(*engine_, ins);
    // The push only reaches the instrument's own slot; a channel audition used the scratch slot.
    engine_->setInstrumentFrameWindow(SOURCE_PREVIEW_SLOT, -1, -1);
    // The three the DRY preview switched off.
    push_instrument_mod_eq_sends(*engine_, ins, project_.tempo, engine_->getSampleRate());
}

bool SongcoreHost::save_sample_wav(int id, const std::string& path, const std::vector<int>& cuePoints,
                                   int sourceMode, bool hasStereo, int bits) {
    if (!engine_) return false;
    return songcore::save_sample_wav(*engine_, routing_, id, path, cuePoints, sourceMode, hasStereo,
                                     bits);
}

void SongcoreHost::adopt_saved_sample(int id, int bits) {
    if (!engine_) return;
    const int depth = songcore::resolve_save_bits(*engine_, id, bits);
    engine_->adoptSavedSampleFormat(id, depth, depth == 32 && engine_->isSampleFloat(id));
    rateCache_.clear(id);
}

int SongcoreHost::chop_sample(int id, const std::string& dir, const std::string& baseName,
                              const std::vector<std::pair<int64_t, int64_t>>& slices, int bits) {
    if (!engine_) return 0;
    return songcore::chop_sample(*engine_, routing_, id, dir, baseName, slices, bits);
}

void SongcoreHost::preview_note(int instrumentId, const Note& note, int64_t durationFrames,
                                bool rootAudition, int tableIdOverride) {
    if (!engine_) return;
    if (note == Note::EMPTY()) return;   // A on an empty cell must not thump the lane
    if (instrumentId < 0 || instrumentId >= static_cast<int>(project_.instruments.size())) return;
    const Instrument& ins = project_.instruments[instrumentId];
    const bool external = instrument_routes_external(ins);

    engine_->requestResume();
    const int64_t frame = engine_->getCurrentFrame() + 100;  // a short lead-in

    Event ev{};
    ev.type       = EV_NOTE_ON;
    ev.frame      = frame;
    ev.track      = AudioEngine::PREVIEW_LANE;
    ev.instrument = static_cast<int16_t>(instrumentId);

    NoteOnPayload& n = ev.noteOn;
    n.note        = static_cast<uint8_t>(note_to_midi(note));
    // ⚠️ The velocity fields are wired differently per destination. The sampler takes VOL as
    // `velGain` (the crossed wiring, see event.h). The cable and the SoundFont read velocity −1
    // as "derive from velGain = (V/127)²" and take a square root, which would boost a raw VOL. So
    // they get full velocity, with VOL in the field that scales by VOL — as a played note has it.
    const bool fullVelocity = external || ins.instrumentType == InstrumentType::SOUNDFONT;
    n.velocity    = fullVelocity ? 127 : -1;
    n.velGainBits = f32_bits(fullVelocity ? 1.0f : hex_to_float(ins.volume));   // seam arg `volume`
    n.volGainBits = f32_bits(fullVelocity ? hex_to_float(ins.volume) : 1.0f);   // seam arg `phraseVol`
    n.panBits     = f32_bits(hex_to_float(ins.pan));
    n.start = -1; n.slice = -1; n.tableId = tableIdOverride; n.tableRow = -1;
    n.transpose = 0; n.pit = 0; n.arp = 0;
    n.pslOffBits = n.pslDurBits = n.pbnRateBits = n.vibSpdBits = n.vibDepBits = f32_bits(0.0f);

    // ⚠️ Unconditionally, whichever way this instrument routes: a note-on for an internal
    // instrument on a lane last used by an EXTERNAL one ends the note the cable still holds.
    // START is exempt from `on_stop_preview`, so nothing else would.
    external_.consume(ev);

    if (external) {
        // A timed audition owes the cable a note-off (the engine's half is `scheduleKill` below).
        // `end_note` takes the earlier of this and the LEN gate.
        if (durationFrames > 0) preview_note_off(frame + durationFrames);
        return;   // no voice, cable or not: EXTERNAL means "not this engine"
    }

    // A fresh cache every preview, so a table edit is heard at once.
    bool tableLoaded[POOL_TABLES] = {false};
    plan_note_on(*engine_, ev, project_, routing_, tableLoaded, rootAudition);

    if (durationFrames > 0) engine_->scheduleKill(frame + durationFrames, AudioEngine::PREVIEW_LANE);
}

void SongcoreHost::preview_instrument(int instrumentId, int tableIdOverride) {
    if (instrumentId < 0 || instrumentId >= static_cast<int>(project_.instruments.size())) return;
    preview_note(instrumentId, project_.instruments[instrumentId].root, /*durationFrames=*/0,
                 /*rootAudition=*/true, tableIdOverride);
}

void SongcoreHost::set_preview_track(int trackId) {
    if (engine_) engine_->setPreviewTrack(trackId);
}

void SongcoreHost::stop_preview(bool cut) {
    if (!engine_) return;
    const int64_t now = engine_->getCurrentFrame();
    if (cut) engine_->scheduleCut(now, AudioEngine::PREVIEW_LANE);
    else     engine_->scheduleKill(now, AudioEngine::PREVIEW_LANE);
    preview_note_off(now);
    // ⚠️ An auditioned TABLE can carry an EQM, and no transport stop follows an audition — so the
    // master EQ is restored here. Only while IDLE: playing, the latch belongs to stop() and the
    // song's own table rows.
    if (!seq_.is_playing() && seq_.has_live_project() && engine_->takeTableMasterEqTouched())
        engine_->setMasterEqSlot(project_.masterEqSlot);
}

PlaybackPosition SongcoreHost::playheads(int trackId) {
    sync_clock();
    return seq_.getPlaybackPosition(trackId);
}

void SongcoreHost::set_trace(bool enabled, const std::string& path) {
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

void SongcoreHost::release_song_hold(MapDestId id, int scopeIndex) {
    switch (id) {
        case MapDestId::TRACK_VOL:  seq_.release_mixer_vol_track(scopeIndex); break;
        case MapDestId::MASTER_VOL: seq_.release_master_vol();                break;
        case MapDestId::DLY_TIME:   seq_.release_delay_time();                break;
        default: break;
    }
}

void SongcoreHost::apply_rollback(const songcore::RollbackPlan& plan) {
    if (!engine_) return;
    for (int t = 0; t < 8; ++t)
        if (plan.frames[t] >= 0) engine_->clearScheduledNotesFrom(plan.frames[t], t);
}

void SongcoreHost::sync_clock() {
    if (!engine_) return;
    seq_.set_clock(engine_->getCurrentFrame());
    int sr = engine_->getSampleRate();
    if (sr > 0) sampleRate_ = sr;
    seq_.set_sample_rate(sampleRate_);
}

void SongcoreHost::before_play() {
    sync_clock();
    if (engine_) engine_->startTake();
}

int64_t SongcoreHost::after_play() {
    resync_soundfont_slots();
    flush_trace();
    // Every play verb passes here, so the metronome grid is pinned to the take's start frame.
    if (engine_) engine_->startMetronome(seq_.playback_start_frame(), frames_per_quarter());
    return seq_.playback_start_frame();
}

int64_t SongcoreHost::frames_per_quarter() const {
    return frames_per_step(project_.tempo, sampleRate_) * 4;
}

void SongcoreHost::resync_soundfont_slots() {
    if (!engine_) return;
    for (const Instrument& ins : project_.instruments)
        songcore::sync_instrument_soundfont(*engine_, ins, routing_, mediaRoots_);
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
                case NOTE_OFF_CUT: engine->scheduleKill(ev.frame, ev.track);       return;
                case NOTE_OFF_KEY: engine->scheduleKeyRelease(ev.frame, ev.track); return;
                default:           engine->scheduleNoteOff(ev.frame, ev.track);    return;
            }
        case EV_CC:
            // A cable sends literal controller numbers, never a CCA-CCD slot.
            EngineConsumer::apply_cc(*engine, ev.frame, ev.track, ev.cc.param, f32_from_bits(ev.cc.valueBits));
            return;
        default:
            return;   // program change and pitch bend have no engine form (engine_consumer.h)
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
        const int   sid   = ins.sampleId;
        const float ratio = (sid >= 0 && sid < POOL_INSTRUMENTS) ? routing_.sampleRateRatio[sid] : 1.0f;
        if (routing_.sfSlot[id] == programRouting_.sfSlot[id] && ratio == programRouting_.sampleRateRatio[id])
            continue;
        push_instrument_params(*engine_, ins, routing_, project_.tempo, sampleRate_);
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
                external_.consume(ev);
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

void SongcoreHost::preview_note_off(int64_t frame) {
    Event off{};
    off.type         = EV_NOTE_OFF;
    off.frame        = frame;
    off.track        = AudioEngine::PREVIEW_LANE;
    off.instrument   = INSTRUMENT_NONE;   // track-scoped, like every note-off on the bus
    off.noteOff.mode = NOTE_OFF_CUT;
    external_.consume(off);
}

void SongcoreHost::flush_trace() {
    if (!traceEnabled_ || traceBuf_.empty()) return;
    traceFile_.write(traceBuf_.data(), static_cast<std::streamsize>(traceBuf_.size()));
    traceFile_.flush();
    traceBuf_.clear();
}

}  // namespace songcore
