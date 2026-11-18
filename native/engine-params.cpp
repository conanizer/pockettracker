// What the control thread sets on the engine — instrument rows, programs, EQ presets, the mixer and
// the buses — and the block-top step that takes the buses in.
#include "audio-engine.h"

void AudioEngine::setInstrumentParams(int instrumentId, int start, int end, bool rev, int loop, int loopSt, int loopEn,
                                      int drv, int crsh, int dwn, int fType, int fCut, int fRes) {
    if (instrumentId < 0 || instrumentId >= 256) return;

    instrumentParams.edit(instrumentId).startPoint = start;
    instrumentParams.edit(instrumentId).endPoint = end;
    instrumentParams.edit(instrumentId).reverse = rev;
    instrumentParams.edit(instrumentId).loopMode = loop;
    instrumentParams.edit(instrumentId).loopStart = loopSt;
    instrumentParams.edit(instrumentId).loopEnd = loopEn;
    instrumentParams.edit(instrumentId).drive = drv;
    instrumentParams.edit(instrumentId).crush = crsh;
    instrumentParams.edit(instrumentId).downsample = dwn;
    instrumentParams.edit(instrumentId).filterType = fType;
    instrumentParams.edit(instrumentId).filterCut = fCut;
    instrumentParams.edit(instrumentId).filterRes = fRes;
    // Ends any exact-frame window, so the editor's audition never outlives the next ordinary push.
    instrumentParams.edit(instrumentId).startFrame = -1;
    instrumentParams.edit(instrumentId).endFrame   = -1;
    instrumentParams.publish(instrumentId);
}

void AudioEngine::setInstrumentFrameWindow(int instrumentId, int startFrame, int endFrame) {
    if (instrumentId < 0 || instrumentId >= 256) return;
    const bool armed = (startFrame >= 0 && endFrame > startFrame);
    instrumentParams.edit(instrumentId).startFrame = armed ? startFrame : -1;
    instrumentParams.edit(instrumentId).endFrame   = armed ? endFrame   : -1;
    instrumentParams.publish(instrumentId);
}

void AudioEngine::setInstrumentSendLevels(int instrId, int reverbHex, int delayHex) {
    if (instrId < 0 || instrId >= 256) return;
    instrumentParams.edit(instrId).reverbSend = reverbHex / 255.0f;
    instrumentParams.edit(instrId).delaySend  = delayHex  / 255.0f;
    instrumentParams.publish(instrId);
}

void AudioEngine::setInstrumentEqSlot(int instrId, int slot) {
    if (instrId < 0 || instrId >= 256) return;
    InstrumentParams& ip = instrumentParams.edit(instrId);
    if (slot < 0 || slot >= 128) {
        ip.eqActive = false;
    } else {
        const EqPresetBank& preset = eqPresets.staged(slot);
        bool any = false;
        for (int i = 0; i < 3; i++) {
            ip.eqBands[i] = preset.bands[i];
            if (preset.bands[i].type != EQ_OFF) any = true;
        }
        ip.eqActive = any;
    }
    instrumentParams.publish(instrId);
}

void AudioEngine::setProgram(int instrumentId, const songcore::Program& program,
                             const int64_t* markers, int count) {
    if (instrumentId < 0 || instrumentId >= songcore::PROGRAM_SLOTS) return;
    ProgramRow& row = programs.edit(instrumentId);
    const int n = (count < 0) ? 0
                : (count > songcore::PROGRAM_SLICE_MARKERS ? songcore::PROGRAM_SLICE_MARKERS : count);
    row.program = program;
    for (int i = 0; i < n; ++i) row.markers[i] = markers[i];
    row.program.sliceCount   = n;
    row.program.sliceMarkers = nullptr;   // programView points it at the audio thread's copy
    programs.publish(instrumentId);
}

// Authored hex → Hz/dB/Q, for a preset and a table morph alike, so a morph lands exactly on its ends.
static EqBandData eqBandFromHex(int type, int freqHex, int gainHex, int qHex) {
    EqBandData b;
    b.type   = type;
    b.freqHz = eqFreqHzFromHex(freqHex);
    b.gainDb = eqGainDbFromHex(gainHex);
    b.q      = eqQFromHex(qHex);
    return b;
}

void AudioEngine::applyEqBandsToModule(EqModule& eq, const EqBandsHex& bands) {
    for (int i = 0; i < 3; i++) {
        EqBandData d = eqBandFromHex(bands.type[i], bands.freq[i], bands.gain[i], bands.q[i]);
        eq.setBand(i, d.type, d.freqHz, d.gainDb, d.q);
    }
}

// The module was initialised by its owner; only the band params are set. A slot out of range turns
// every band off.
void AudioEngine::applyEqPresetToModule(EqModule& eq, int slot) {
    if (slot < 0 || slot >= 128) {
        for (int i = 0; i < 3; i++) eq.setBand(i, 0, 0.f, 0.f, 0.f);
        return;
    }
    const EqPresetBank& preset = eqPresets[slot];
    for (int i = 0; i < 3; i++) {
        eq.setBand(i, preset.bands[i].type, preset.bands[i].freqHz, preset.bands[i].gainDb,
                   preset.bands[i].q);
    }
}

void AudioEngine::setEqBand(int slot, int band, int type, int freqHex, int gainHex, int qHex) {
    if (slot < 0 || slot >= 128 || band < 0 || band >= 3) return;
    eqPresets.edit(slot).bands[band] = eqBandFromHex(type, freqHex, gainHex, qHex);
    // The authored bytes too — a table morph interpolates them. The only writer of either array.
    auto& h = eqPresetHex.edit(slot);
    h.type[band] = type;
    h.freq[band] = freqHex;
    h.gain[band] = gainHex;
    h.q[band]    = qHex;
    eqPresets.publish(slot);
    eqPresetHex.publish(slot);
}

// The apply* forms carry no log line, so VTR/VMV can call them on the audio thread.
void AudioEngine::applyTrackVolume(int trackId, float volume) {
    if (trackId < 0 || trackId >= 8) return;
    trackVolumes[trackId].store(volume, std::memory_order_relaxed);
}

void AudioEngine::applyMasterVolume(float volume) {
    masterVolume.store(volume, std::memory_order_relaxed);
}

void AudioEngine::setTrackVolume(int trackId, float volume) {
    if (trackId < 0 || trackId >= 8) return;
    applyTrackVolume(trackId, volume);
    LOGD("🔊 Track %d volume set to %.2f", trackId, volume);
}

void AudioEngine::setTrackMuted(int trackId, bool muted) {
    if (trackId < 0 || trackId >= 8) return;
    trackMuted[trackId].store(muted, std::memory_order_relaxed);
    // The mix ramps the gate over MUTE_GATE_SAMPLES; the voices keep running underneath.
    LOGD("🔇 Track %d %s", trackId, muted ? "muted" : "unmuted");
}

void AudioEngine::setBusMutes(bool revMuted, bool dlyMuted, bool dryBusMuted) {
    revReturnMuted.store(revMuted, std::memory_order_relaxed);
    delayReturnMuted.store(dlyMuted, std::memory_order_relaxed);
    dryMuted.store(dryBusMuted, std::memory_order_relaxed);
}

void AudioEngine::setMasterVolume(float volume) {
    applyMasterVolume(volume);
    LOGD("🔊 Master volume set to %.2f", volume);
}

void AudioEngine::setPreviewTrack(int trackId) {
    previewLaneTrack.store((trackId >= 0 && trackId < 8) ? trackId : -1, std::memory_order_relaxed);
}

void AudioEngine::setReverbParams(int feedbackHex, int dampHex, int wetHex, int sizeHex) {
    busSettings.reverbDecay = feedbackHex; busSettings.reverbDamp = dampHex;
    busSettings.reverbWet   = wetHex;      busSettings.reverbSize = sizeHex;
    prepareReverb();
    recordBus(BUS_REVERB_PARAMS);
    reverbReturnGain = wetHex / 255.0f;
}

void AudioEngine::setDelayParams(int timeOrSubdiv, int feedbackHex, bool syncMode, float bpm, int wetHex) {
    setDelayTime(timeOrSubdiv, syncMode, bpm);
    setDelayFeedbackWet(feedbackHex, wetHex);
}

// Separate halves: while a TIM drives the time, a globals push still sets FDBK and WET.
void AudioEngine::setDelayTime(int timeOrSubdiv, bool syncMode, float bpm) {
    busSettings.delayTime = timeOrSubdiv; busSettings.delaySync = syncMode; busSettings.delayBpm = bpm;
    recordBus(BUS_DELAY_TIME);
}

void AudioEngine::setDelayFeedbackWet(int feedbackHex, int wetHex) {
    busSettings.delayFeedback = feedbackHex; busSettings.delayWet = wetHex;
    recordBus(BUS_DELAY_FEEDBACK);
    delayReturnGain = wetHex / 255.0f;
}

void AudioEngine::setDelayCharacter(bool pong, int toneHex, int wobbleHex) {
    busSettings.delayPong = pong; busSettings.delayTone = toneHex; busSettings.delayWobble = wobbleHex;
    recordBus(BUS_DELAY_CHAR);
}

void AudioEngine::setDelayReverbSend(int sendHex) {
    busSettings.delayReverbSend = sendHex; busSettings.pushed = true;
    delayToReverbSend = sendHex / 255.0f;
}

void AudioEngine::setReverbCharacter(int preHex, int widthHex, int modHex) {
    busSettings.reverbPre = preHex; busSettings.reverbWidth = widthHex; busSettings.reverbMod = modHex;
    prepareReverb();
    recordBus(BUS_REVERB_CHAR);
}

void AudioEngine::setReverbAlgo(int algo) {
    busSettings.reverbAlgo = algo;
    prepareReverb();
    recordBus(BUS_REVERB_ALGO);
}

void AudioEngine::setReverbInputEq(int slot) {
    busSettings.reverbInputEq = slot;
    recordBus(BUS_REVERB_INEQ);
}

void AudioEngine::setDelayInputEq(int slot) {
    busSettings.delayInputEq = slot;
    recordBus(BUS_DELAY_INEQ);
}

void AudioEngine::setMasterEqSlot(int slot) {
    busSettings.masterEqSlot = slot;
    recordBus(BUS_MASTER_EQ);
}

void AudioEngine::setOttDepth(int depth) {
    busSettings.ottDepth = depth; busSettings.ottForRender = false;
    recordBus(BUS_OTT);
}

void AudioEngine::setOttDepthForRender(int depth) {
    busSettings.ottDepth = depth; busSettings.ottForRender = true;
    recordBus(BUS_OTT);
}

void AudioEngine::setMasterFx(int fx) {
    busSettings.masterFx = fx;
    recordBus(BUS_MASTER_FX);
}

void AudioEngine::setDustDepth(int depth) {
    busSettings.dustDepth = depth; busSettings.dustForRender = false;
    recordBus(BUS_DUST);
}

void AudioEngine::setDustDepthForRender(int depth) {
    busSettings.dustDepth = depth; busSettings.dustForRender = true;
    recordBus(BUS_DUST);
}

void AudioEngine::setLimiterPreGain(int depth) {
    busSettings.limiterPreGain = depth;
    recordBus(BUS_LIMITER);
}

// Control thread, before the record is published, so the block applying ALGO or SIZE finds it ready.
void AudioEngine::prepareReverb() {
    const BusSettings& s = busSettings;
    reverbSend.dragonfly.prepare(s.reverbAlgo, s.reverbDecay, s.reverbSize, s.reverbDamp, s.reverbMod,
                                 static_cast<float>(getSampleRate()));
}

// Audio thread, top of the block: every group that changed, or all after a rate change.
void AudioEngine::applyBusSettings() {
    busPublisher.read(busLive, busSeen);
    const bool all = busReplayRequested.exchange(false, std::memory_order_acquire) && busLive.pushed;
    const BusSettings& s = busLive;
    const auto due = [&](BusGroup g) {
        if (!all && s.seq[g] == busApplied[g]) return false;
        busApplied[g] = s.seq[g];
        return true;
    };
    if (due(BUS_REVERB_PARAMS)) reverbSend.setParams(s.reverbDecay, s.reverbDamp, s.reverbSize);
    if (due(BUS_REVERB_ALGO))   reverbSend.setAlgo(s.reverbAlgo);
    if (due(BUS_REVERB_CHAR))   reverbSend.setCharacter(s.reverbPre, s.reverbWidth, s.reverbMod);
    if (due(BUS_REVERB_INEQ))   applyEqPresetToModule(reverbSend.inputEq, s.reverbInputEq);
    if (due(BUS_DELAY_TIME)) {
        if (s.delaySync) delaySend.setTimeSync(s.delayTime, s.delayBpm);
        else             delaySend.setTimeFree(s.delayTime);
    }
    if (due(BUS_DELAY_FEEDBACK)) delaySend.feedback = s.delayFeedback / 255.0f;
    if (due(BUS_DELAY_CHAR))     delaySend.setCharacter(s.delayPong, s.delayTone, s.delayWobble);
    if (due(BUS_DELAY_INEQ))     applyEqPresetToModule(delaySend.inputEq, s.delayInputEq);
    if (due(BUS_MASTER_EQ))      applyEqPresetToModule(masterChain.masterEq, s.masterEqSlot);
    if (due(BUS_OTT)) {
        if (s.ottForRender) masterChain.setOttDepthForRender(s.ottDepth / 255.0f);
        else                masterChain.ott.setDepth(s.ottDepth / 255.0f);
    }
    if (due(BUS_MASTER_FX)) masterChain.setMasterFx(s.masterFx);
    if (due(BUS_DUST)) {
        if (s.dustForRender) masterChain.setDustDepthForRender(s.dustDepth / 255.0f);
        else                 masterChain.setDustDepth(s.dustDepth / 255.0f);
    }
    if (due(BUS_LIMITER)) masterChain.setLimiterPreGain(1.0f + (s.limiterPreGain / 255.0f) * 3.0f);
}
