// One block of audio: the queue drain, the two note triggers, every voice's mix, the buses and the
// master — processAudioBlock, and processLiveBlock, the live entry every backend calls.
#include "audio-engine.h"
#include "engine-voice-ops.h"
#include "vendor/tsf/tsf.h"    // TSF API declarations only — TSF_IMPLEMENTATION lives in soundfont-voice.cpp
#include <cstdint>
#include <cstring>

namespace {
// A meter's running peak. Not fmaxf, which without -ffast-math is a libm call, per sample here;
// a NaN is ignored the same way.
inline float peak_hold(float peak, float x) {
    x = fabsf(x);
    return x > peak ? x : peak;
}
}  // namespace

// Scale an interleaved stereo buffer by a gain moving linearly from `from`, reaching `to` on the
// last frame.
static inline void applyGainRamp(float* buf, int frames, float from, float to) {
    for (int i = 0; i < frames; i++) {
        const float g = from + (to - from) * (float)(i + 1) / (float)frames;
        buf[i * 2]     *= g;
        buf[i * 2 + 1] *= g;
    }
}

// A note's own PSL / PBN / vibrato, set after its trigger has reset the pitch state. Both rates are
// already per frame.
template <typename V>
static inline void startNotePitchFx(V& v, const ScheduledNote& note) {
    if (fabsf(note.pslInitialOffset) > 0.001f && note.pslDuration > 0.0f) {
        v.pitchOffset      = note.pslInitialOffset;
        v.pitchSlideTarget = 0.0f;
        v.pitchSlideRate   = -note.pslInitialOffset / fmaxf(1.0f, note.pslDuration);
        v.pitchSliding     = true;
    }
    // Exactly 0 is "no PBN": a rate is per FRAME, so any threshold would drop a slow byte.
    if (note.pbnRate != 0.0f) {
        v.pitchSlideRate   = note.pbnRate;
        v.pitchSlideTarget = (note.pbnRate > 0) ? 127.0f : -127.0f;
        v.pitchSliding     = true;
    }
    if (note.vibratoDepth > 0.01f) {
        v.vibratoSpeed  = note.vibratoSpeed;
        v.vibratoDepth  = note.vibratoDepth;
        v.vibratoActive = true;
    }
}

// A SoundFont note onto its track's MIDI channel of the shared tsf* handle, at `frame` inside the
// current block. Audio thread only; tsf_load_memory() never runs here.
void AudioEngine::triggerSoundfontNote(const ScheduledNote& note, int frame, int64_t currentFrame,
                                       float sampleRate) {
    const int t = note.trackId;
    if (t < 0 || t >= SF_VOICE_COUNT || note.sfSlot < 0 || note.sfSlot >= MAX_SOUNDFONTS) {
        LOGT("🎹 SF DROPPED: sfSlot=%d track=%d (out of range)", note.sfSlot, note.trackId);
        return;
    }
    // ⚠️ The handle is not tested here: the UI thread can null it at any moment. `armNote` reads it
    // under the slot's lock and says whether the note is worth setting up.
    SoundfontVoice& sv = sfVoices[t];
    // ⚠️ Read before `armNote`, which sets both: is a note still sounding here, and is this the block's
    // first arm? A second one replaces the first, which never sounded.
    const bool wasSounding = sv.isActive;
    const bool firstArm    = !sv.hasArmedNote;
    // The instrument's ADSR override — by instrument id, since two instruments can share a handle.
    int eAtk = -1, eDec = -1, eSus = -1, eRel = -1;
    if (note.sampleId >= 0 && note.sampleId < 256) {
        const SfEnvOverride& eo = sfEnvOverrides[note.sampleId];
        eAtk = eo.atk; eDec = eo.dec; eSus = eo.sus; eRel = eo.rel;
    }
    if (!sv.armNote(note.sfSlot, note.midiNote, note.midiVelocity,
                    note.volume, note.pan, note.sfBank, note.sfPreset, t,
                    eAtk, eDec, eSus, eRel)) {
        LOGT("🎹 SF DROPPED: sfSlot=%d track=%d (handle not loaded)",
             note.sfSlot, note.trackId);
        return;     // …and the voice keeps whatever it was already playing
    }
    // A chord: every key on the channel, at one note's power in all (÷√n).
    int chordSemis[4] = {}, chordWait[4] = {};
    const int chordCount = std::min<int>(note.noteOn.chordCount, 4);
    for (int k = 0; k < chordCount; ++k) {
        chordSemis[k] = note.noteOn.chord[k];
        chordWait[k]  = (int)(note.noteOn.strumOrder[k] * note.noteOn.strumStep);
    }
    const float chordGain = 1.0f / sqrtf((float)sv.armChord(chordSemis, chordWait, chordCount));
    soundfonts[note.sfSlot].lastUsed.store(nextSfUseTick(), std::memory_order_relaxed);  // LRU touch
    trackOnsetFrame[t] = note.targetFrame;   // the authored frame: a late note still pairs with its VTR
    // Mono per track across voice types: an SF note fades a sampler note still sounding here, and
    // the sampler trigger does the reverse. Only after armNote said yes.
    for (int v = 0; v < MAX_VOICES; v++) {
        if (voices[v].trackId == t && voices[v].isActive && !voices[v].isFadingOut) {
            voices[v].startFadeOut();
        }
    }
    sv.isReleasingOnly = false;
    sv.resetPitchState();
    sv.detuneSemitones = note.detuneSemitones;  // after the reset
    sv.startDelayFrames = frame;
    sv.instrId = note.sampleId;

    // A TIC in the table's last row overrides the instrument's tic rate, per FX column.
    int effectiveTicRates[TABLE_LANES];
    effectiveTicRatesFor(note.tableId, note.tableTicRate, effectiveTicRates);
    // ⚠️ `armNote` has not touched the lanes: they are still the previous note's.
    int sfStartRows[TABLE_LANES];
    tableStartRows(note, wasSounding && sv.tableId >= 0 && sv.tableId == note.tableId ? sv.lanes : nullptr,
                   effectiveTicRates, sfStartRows);
    sv.resetTableState(note.tableId, effectiveTicRates,
                       note.noteOctave, note.notePitch, sfStartRows);

    // Only valid when sampleId >= 0 (phrase playback); previews pass -1.
    static const InstrumentParams kNoInstrument{};
    const bool haveInstrument = note.sampleId >= 0 && note.sampleId < 256;
    const InstrumentParams& ip = haveInstrument ? instrumentParams[note.sampleId] : kNoInstrument;
    if (haveInstrument) {
        initVoiceModSlots(sv, note.sampleId, currentFrame, sampleRate);
    } else {
        for (int m = 0; m < 4; m++) sv.voiceMods[m] = VoiceModSlot{};
    }
    // The note this one cuts finishes through the chain it had; this one starts on a clean one.
    if (firstArm) {
        sv.cutting = wasSounding;
        if (wasSounding) {
            sv.cutChain = sv.chain;
            sv.cutChain.filter.snapshotCoeffs();   // it holds where the last piece left it
        }
    }
    sv.chain.reset(sampleRate, /*sounding=*/sv.cutting);
    sv.chain.filter.setParams(ip.filterType, ip.filterCut, ip.filterRes, ip.filterDrive,
                              sampleRate);
    sv.chain.filter.snapshotCoeffs();   // or the first piece would ramp from the reset defaults
    sv.chain.drive.setDrive(ip.drive);
    sv.chain.crush.setParams(ip.crush, ip.downsample);
    if (ip.eqActive) {
        sv.chain.eq.active = true;
        for (int i = 0; i < 3; i++) {
            sv.chain.eq.bands[i].setParams(ip.eqBands[i].type, ip.eqBands[i].freqHz,
                                           ip.eqBands[i].gainDb, ip.eqBands[i].q);
        }
    }
    voiceSetSends(sv, ip.reverbSend, ip.delaySend);

    sv.params.setBase(PARAM_VOL,   note.volume * chordGain);
    sv.params.setBase(PARAM_PAN,   note.pan);
    sv.params.setBase(PARAM_PITCH, 0.0f);
    // What the setters and the per-piece recompute read.
    sv.params.setBase(PARAM_FILTER_CUT, (float)ip.filterCut);
    sv.params.setBase(PARAM_FILTER_RES, (float)ip.filterRes);
    sv.params.setBase(PARAM_DRIVE,      (float)ip.drive);
    sv.params.setBase(PARAM_CRUSH,      (float)ip.crush);
    sv.params.setBase(PARAM_DOWNSAMPLE, (float)ip.downsample);
    sv.params.resetMods();
    memset(sv.modSourceValues,  0, sizeof(sv.modSourceValues));
    memset(sv.modDestValues,    0, sizeof(sv.modDestValues));
    memset(sv.prevModDestValues,0, sizeof(sv.prevModDestValues));
    sv.modSourceValues[MOD_SRC_TABLE_VOL]  = 1.0f;
    sv.modSourceValues[MOD_SRC_PHRASE_VOL] = note.phraseVolume;
    float initVol = note.volume * chordGain * note.phraseVolume;
    sv.modDestValues[PARAM_VOL]     = initVol;
    sv.prevModDestValues[PARAM_VOL] = initVol;
    startNotePitchFx(sv, note);
    applyTableCarry(sv, note.carry, sampleRate);
    LOGT("🎹 SF FIRE: slot=%d track/ch=%d bank=%d preset=%d midi=%d vel=%d vol=%.2f",
         note.sfSlot, t, note.sfBank, note.sfPreset,
         note.midiNote, note.midiVelocity, note.volume);
}

// A sampler note into the voice pool, at `frame` inside the current block. Audio thread only.
void AudioEngine::triggerSamplerNote(const ScheduledNote& note, int frame, int64_t currentFrame,
                                     float sampleRate) {
    // ⚠️ Read before the fade-out below: only a voice still running THIS table hands its rows on — a
    // chain can give the track a different one.
    const TableLane* liveLanes = nullptr;
    for (int v = 0; v < MAX_VOICES; v++) {
        if (voices[v].trackId == note.trackId && voices[v].isActive && !voices[v].isFadingOut
            && voices[v].tableId >= 0 && voices[v].tableId == note.tableId)
            liveLanes = voices[v].lanes;
    }

    // A note with no sample behind it must not touch the track (a preview of an empty slot).
    const bool haveSample = note.sampleId >= 0 && note.sampleId < 256 && samples[note.sampleId];

    // Mono per track: whatever is playing here fades from the new note's own frame.
    for (int v = 0; haveSample && v < MAX_VOICES; v++) {
        if (voices[v].trackId == note.trackId && voices[v].isActive && !voices[v].isFadingOut) {
            voices[v].startFadeOut(DECLICK_SAMPLES, frame);
        }
    }

    // The slot: a free one first, so the faded voice's tail plays out…
    int targetSlot = -1;
    for (int v = 0; v < MAX_VOICES; v++) {
        if (!voices[v].isActive) {
            targetSlot = v;
            break;
        }
    }

    // …else this track's own fading voice (trackId survives startFadeOut for this)…
    if (targetSlot == -1) {
        for (int v = 0; v < MAX_VOICES; v++) {
            if (voices[v].trackId == note.trackId && voices[v].isFadingOut) {
                targetSlot = v;
                break;
            }
        }
    }

    // …else any fading voice: a ~1 ms click beats silence.
    if (targetSlot == -1) {
        for (int v = 0; v < MAX_VOICES; v++) {
            if (voices[v].isFadingOut) {
                targetSlot = v;
                LOGT("⚠️ Voice pool tight: preempting fading slot %d for track %d", v, note.trackId);
                break;
            }
        }
    }

    if (targetSlot != -1) {
        int v = targetSlot;
        if (haveSample) {
            // Mono per track across voice types: an SF note still sounding here gets a note-off,
            // so its release plays out.
            if (note.trackId >= 0 && note.trackId < SF_VOICE_COUNT &&
                sfVoices[note.trackId].isActive && !sfVoices[note.trackId].isReleasingOnly) {
                sfVoices[note.trackId].noteOffAt(frame);
            }
            float rate = note.frequency / note.baseFrequency;
            const songcore::NoteOnPayload& on = note.noteOn;
            const int chordCount = std::min<int>(on.chordCount, 1 + Voice::CHORD_EXTRA);
            int chordSemis[1 + Voice::CHORD_EXTRA] = {};
            int strumWait[1 + Voice::CHORD_EXTRA] = {};
            for (int k = 0; k < chordCount; ++k) {
                chordSemis[k] = on.chord[k];
                strumWait[k]  = (int)(on.strumOrder[k] * on.strumStep);
            }

            // A TIC in the table's last row overrides the instrument's tic rate, per FX column.
            int effectiveTicRates[TABLE_LANES];
            effectiveTicRatesFor(note.tableId, note.tableTicRate, effectiveTicRates);

            int startRows[TABLE_LANES];
            tableStartRows(note, liveLanes, effectiveTicRates, startRows);

            // The generation BEFORE the buffers: the UI bumps it after storing them, so a
            // mismatch here can only make the mix end the voice, never read a stale pointer.
            const uint32_t gen = sampleGen[note.sampleId].load();
            voices[v].trigger(samples[note.sampleId], samplesRight[note.sampleId], sampleLengths[note.sampleId],
                              note.trackId, rate, note.baseFrequency,
                              note.volume, note.phraseVolume, note.pan, instrumentParams[note.sampleId],
                              sampleRate, note.startPointOverride, note.endPointOverride,
                              note.tableId, effectiveTicRates, note.noteOctave, note.notePitch, startRows);
            voices[v].setChord(chordSemis, chordCount, strumWait);
            voices[v].instrId = note.sampleId;
            voices[v].sampleGen = gen;
            voices[v].faderHeld = false;
            if (note.trackId >= 0 && note.trackId < SF_VOICE_COUNT) trackOnsetFrame[note.trackId] = note.targetFrame;
            voices[v].startDelayFrames = frame;

            startNotePitchFx(voices[v], note);
            initVoiceModSlots(voices[v], note.sampleId, currentFrame, sampleRate);
            applyTableCarry(voices[v], note.carry, sampleRate);

            LOGT("🎵 Triggered note at frame %lld: sample=%d, track=%d, rate=%.3f, vol=%.4f, pan=%.2f, startOverride=%d, table=%d, tic=%d, oct=%d, pitch=%d, startRow=%d",
                 (long long)currentFrame, note.sampleId, note.trackId, rate, note.volume, note.pan, note.startPointOverride,
                 note.tableId, effectiveTicRates[0], note.noteOctave, note.notePitch, startRows[0]);
        } else {
            if (note.sampleId < 0 || note.sampleId >= 256) {
                LOGT("❌ Invalid sampleId=%d for note at frame %lld", note.sampleId, (long long)currentFrame);
            } else {
                LOGT("❌ Sample %d not loaded! Note at frame %lld cannot play", note.sampleId, (long long)currentFrame);
            }
        }
    } else {
        LOGT("⚠️ No free voice (all 8 fully active) for note at frame %lld, sample=%d", (long long)currentFrame, note.sampleId);
    }
}

// Frames [from, to) of a rendered stereo buffer through a track fader and a chain: the fader ramps
// across the whole block, the chain's filter across these frames.
// ⚠️ The fader goes on the rendered samples, not on the TSF channel: a channel volume is one value per
// render call, a staircase. Above the chain, so drive and filter hear the faded signal.
static void faderThenChain(InstrumentChain& chain, float* buf, int from, int to,
                           float fStart, float fEnd, int numFrames) {
    if (fStart != 1.0f || fEnd != 1.0f) {
        for (int i = from; i < to; i++) {
            const float g = fStart + (fEnd - fStart) * (float)(i + 1) / (float)numFrames;
            buf[i * 2]     *= g;
            buf[i * 2 + 1] *= g;
        }
    }
    const int frames = to - from;
    for (int i = from; i < to; i++) {
        chain.filter.setInterpolatedCoeffs((float)(i - from + 1) / (float)frames);
        chain.processStereo(buf[i * 2], buf[i * 2 + 1]);
    }
}

// One track's chained stereo buffer onto the bus, once per block: the mute gate and the
// transport-stop ramp, the send tap, the output, the meters and scopes. Returns the buffer's peak;
// `stopFadeDone` is set when the stop ramp reached zero inside this block.
template <typename V>
float AudioEngine::mixTrackBuffer(V& v, int t, float* buf, const BlockMix& c, bool& stopFadeDone) {
    // ⚠️ The mute gate and the stop ramp ride here, per sample: below the chain, so a stop cannot
    // slam a filter still ringing, and above the send tap, so the sends are fed the faded signal
    // and not a waveform cut mid-cycle. The fader is already in the buffer, which makes a SoundFont
    // send post-fader where the sampler's is pre-fader.
    for (int i = 0; i < c.numFrames; i++) {
        const float lerp_t = (float)(i + 1) / (float)c.numFrames;
        float gate = c.gateStart[t] + (c.gateEnd[t] - c.gateStart[t]) * lerp_t;
        if (v.stopFade.moving()) {
            if (i >= v.stopFadeStartFrame) {   // a ramp dispatched mid-block waits for its frame
                gate *= v.stopFade.next();
                if (!v.stopFade.moving()) stopFadeDone = true;
            }
        } else if (stopFadeDone) {
            gate = 0.0f;   // the ramp ended inside this block; the rest of it is silence
        }
        buf[i * 2]     *= gate;
        buf[i * 2 + 1] *= gate;
    }

    if ((stemsMode == 0 || stemsMode >= 9) && (v.reverbSend.audible() || v.delaySend.audible())) {
        for (int i = 0; i < c.numFrames; i++) {
            const float rev = v.reverbSend.next(), dly = v.delaySend.next();
            revSendBufL[i] += buf[i * 2]     * rev;
            revSendBufR[i] += buf[i * 2 + 1] * rev;
            dlySendBufL[i] += buf[i * 2]     * dly;
            dlySendBufR[i] += buf[i * 2 + 1] * dly;
        }
    }

    float trackPeakL = 0.0f, trackPeakR = 0.0f;
    if (c.octaWanted) trackWasActive[t] = true;
    for (int i = 0; i < c.numFrames; i++) {
        float outL = buf[i * 2];
        float outR = buf[i * 2 + 1];
        // Pre-master: mixMaster applies the master fader and scales the meters and scopes by it.
        trackPeakL = peak_hold(trackPeakL, outL);
        trackPeakR = peak_hold(trackPeakR, outR);
        if (stemsMode == 0 || t == stemsMode - 1) {
            c.output[i * 2]     += outL;
            c.output[i * 2 + 1] += outR;
        }
        if (c.octaWanted) {
            trackWaveAccumL[t][i] += outL;
            trackWaveAccumR[t][i] += outR;
        }
        if (c.monitoredInstrId >= 0 && v.instrId == c.monitoredInstrId) {
            instrSpectrumTempL[i] += 0.5f * (outL + outR);
        }
    }
    const float trackPeak = fmaxf(trackPeakL, trackPeakR);
    if (t < 8) {  // mixer meters cover song tracks only, not the preview lane
        framePeaksPerTrackL[t] = fmaxf(framePeaksPerTrackL[t], trackPeakL);
        framePeaksPerTrackR[t] = fmaxf(framePeaksPerTrackR[t], trackPeakR);
    }
    return trackPeak;
}

// ALL audio DSP lives here: processLiveBlock (live) and renderOffline (WAV export) are thin
// wrappers, and no processing is ever added to them.
// ⚠️ The stages' order is the behaviour: each reads what the ones above it left, in `BlockMix`.
void AudioEngine::processAudioBlock(float* output, int numFrames, int channelCount, float sampleRate) {
    // Stereo only: the SoundFont pass and the buses index [i*2]. One silent block rather than a
    // write past a mono buffer.
    if (channelCount != 2) return;
    // ⚠️ The per-block scratch is PROCESS_SUBBLOCK long, and a larger block would also resolve
    // events too coarsely. Both wrappers chunk at that size.
    if (numFrames > PROCESS_SUBBLOCK) return;

    // What waitForAudioBlockBoundary() watches: while set, a SoundFont handle this block loaded may
    // still be in use (seq_cst on both sides — see the wait). On the way out, whichever return is
    // taken, the block also publishes what the screen reads of the voices.
    struct InBlock {
        AudioEngine& e;
        explicit InBlock(AudioEngine& en) : e(en) { e.audioInBlock.store(true); }
        ~InBlock() {
            e.publishVoiceView();
            e.audioBlocksDone.fetch_add(1);
            e.audioInBlock.store(false);
        }
    } inBlock(*this);

    BlockMix b;
    b.output       = output;
    b.numFrames    = numFrames;
    b.channelCount = channelCount;
    b.sampleRate   = sampleRate;

    walkMixerRamps(b);
    clearBlockScratch(b);
    takeQueuedWork(b);
    dispatchEvents(b);
    mixSamplerVoices(b);
    mixSoundfontVoices(b);
    carryFadesToNextBlock();
    captureTrackScopes(b);
    mixBuses(b);
    mixMaster(b);

    globalFrameCounter.store(b.startFrame + numFrames, std::memory_order_relaxed);
}

// The mixer's faders and mute gates, once per block: each one's value at the block's first and last
// frame, which the mix stages interpolate per sample. The targets are atomics the UI thread writes.
void AudioEngine::walkMixerRamps(BlockMix& b) {
    b.offlineRender = isOfflineRendering.load(std::memory_order_relaxed);
    const bool offlineRender = b.offlineRender;

    // ⚠️ Each is a PAIR (first frame, last) — the mute its own ramp, not folded into the fader — read
    // at two sites: the sampler's per-sample gain and the SoundFont buffer post-chain. An export snaps
    // them, so a muted track starts silent — all but a fader the song itself is moving.

    // How far each may move in this block — per full swing, so a glide lasts the same time whatever
    // the block size. The fader's is also read by the VTR/VMV arms in applyParamUpdate.
    b.faderStep = (float)b.numFrames / (float)FADER_GLIDE_SAMPLES;
    const float faderStep = b.faderStep;
    {
        const float gateStep = (float)b.numFrames / (float)MUTE_GATE_SAMPLES;
        // One walk for every fader, gate and return level in the mixer (glide.h). `ramp` is the member
        // that remembers where it got to; start/end bracket this block.
        const auto walk = [&](float& ramp, float target, float step, bool snap, float& start, float& end) {
            if (offlineRender && snap) ramp = target;
            start = ramp;
            end = ramp = glide_toward(ramp, target, step);
        };
        const auto walk_gate = [&](float& gate, bool muted, float& start, float& end) {
            walk(gate, muted ? 0.0f : 1.0f, gateStep, true, start, end);
        };
        const auto walk_fader = [&](float& ramp, float target, bool& songMove, float& start, float& end) {
            walk(ramp, target, faderStep, !songMove, start, end);
            if (ramp == target) songMove = false;
        };
        for (int t = 0; t < 8; t++) {
            walk_fader(trackVolRamp[t], trackVolumes[t].load(std::memory_order_relaxed), trackVolSongMove[t],
                       b.trackVolStart[t], b.trackVolEnd[t]);
            walk_gate(trackGate[t], trackMuted[t].load(std::memory_order_relaxed), b.gateStart[t], b.gateEnd[t]);
        }
        walk_gate(revReturnGate,   revReturnMuted.load(std::memory_order_relaxed),   b.revGateStart, b.revGateEnd);
        walk_gate(delayReturnGate, delayReturnMuted.load(std::memory_order_relaxed), b.dlyGateStart, b.dlyGateEnd);
        walk_gate(dryGate,         dryMuted.load(std::memory_order_relaxed),         b.dryGateStart, b.dryGateEnd);
        walk_fader(masterVolRamp, masterVolume.load(std::memory_order_relaxed), masterVolSongMove,
                   b.masterVolStart, b.masterVolEnd);
        // The returns' WET and the delay's feed into the reverb, at a fader's pace.
        walk(reverbReturnRamp,  reverbReturnGain.load(std::memory_order_relaxed),  faderStep, true,
             b.revReturnStart, b.revReturnEnd);
        walk(delayReturnRamp,   delayReturnGain.load(std::memory_order_relaxed),   faderStep, true,
             b.dlyReturnStart, b.dlyReturnEnd);
        walk(delayToReverbRamp, delayToReverbSend.load(std::memory_order_relaxed), faderStep, true,
             b.dlyToRevStart, b.dlyToRevEnd);
        for (Voice& v : voices)          if (v.faderHeld)  v.faderHeldStart  = v.faderHeldEnd;
        for (SoundfontVoice& v : sfVoices) if (v.faderHeld) v.faderHeldStart = v.faderHeldEnd;
        b.previewTrack    = previewLaneTrack.load(std::memory_order_relaxed);
    }
    // The preview lane borrows the fader and the gate of the channel the audition came from; -1
    // (no origin) keeps both at 1. ⚠️ After the walk, not in the setter: that is what makes it the
    // live fader, so a VTR or a mixer move reaches the audition.
    const int previewTrack = b.previewTrack;
    b.previewBorrows = (previewTrack >= 0 && previewTrack < 8);
    b.trackVolStart[PREVIEW_LANE] = b.previewBorrows ? b.trackVolStart[previewTrack] : 1.0f;
    b.trackVolEnd[PREVIEW_LANE]   = b.previewBorrows ? b.trackVolEnd[previewTrack]   : 1.0f;
    b.gateStart[PREVIEW_LANE]     = b.previewBorrows ? b.gateStart[previewTrack]     : 1.0f;
    b.gateEnd[PREVIEW_LANE]       = b.previewBorrows ? b.gateEnd[previewTrack]       : 1.0f;
}

// The meters, the send buses and the visualizer accumulators, zeroed for this block. The
// visualizers are skipped when nobody is watching (CAPTURE_IDLE_MS) or during an export.
void AudioEngine::clearBlockScratch(BlockMix& b) {
    for (int t = 0; t < 8; t++) { framePeaksPerTrackL[t] = 0.0f; framePeaksPerTrackR[t] = 0.0f; }
    frameSendPeakRevL = frameSendPeakRevR = frameSendPeakDelL = frameSendPeakDelR = 0.0f;

    const int64_t nowMsec       = nowMs();
    b.octaWanted     = !b.offlineRender && (nowMsec - lastTrackWaveformReadMs.load(std::memory_order_relaxed)) < CAPTURE_IDLE_MS;
    b.spectrumWanted = !b.offlineRender && (nowMsec - lastSpectrumReadMs.load(std::memory_order_relaxed))      < CAPTURE_IDLE_MS;
    const size_t frameBytes     = (size_t)b.numFrames * sizeof(float);

    memset(revSendBufL, 0, frameBytes); memset(revSendBufR, 0, frameBytes);
    memset(dlySendBufL, 0, frameBytes); memset(dlySendBufR, 0, frameBytes);

    // The OCTA accumulators (64 KB+) only while OCTA is shown.
    memset(trackWasActive, 0, sizeof(trackWasActive));
    if (b.octaWanted) {
        for (int t = 0; t < TRACK_WAVEFORM_COUNT; t++) {
            memset(trackWaveAccumL[t], 0, frameBytes);
            memset(trackWaveAccumR[t], 0, frameBytes);
        }
    }

    // One instrument's spectrum, only while the EQ screen is monitoring it.
    b.monitoredInstrId = instrSpectrumInstrId.load(std::memory_order_relaxed);
    if (b.monitoredInstrId >= 0) memset(instrSpectrumTempL, 0, frameBytes);
}

// Everything the other threads handed over since the last block: the three event queues (drained
// ONCE, one lock each, into reusable batches that dispatchEvents walks with zero locking), the
// transport's stop and restart requests, a live key's bytes, and the staged instrument and bus data.
void AudioEngine::takeQueuedWork(BlockMix& b) {
    noteBatch.clear(); killBatch.clear(); paramBatch.clear();
    b.startFrame = globalFrameCounter.load(std::memory_order_relaxed);
    const int64_t blockStartFrame = b.startFrame;
    const int64_t blockEnd = blockStartFrame + b.numFrames - 1;

    // A SoundFont freed since a voice was armed: the voice lets go before anything this block can
    // reach the slot, which may by now hold a different font.
    for (int t = 0; t < SF_VOICE_COUNT; t++) {
        SoundfontVoice& sv = sfVoices[t];
        if (sv.sfSlot >= 0 && sv.sfGen != soundfonts[sv.sfSlot].gen.load()) sv.detach();
    }

    if (stopRampRequested.exchange(false, std::memory_order_acquire)) startStopRamp(blockStartFrame);
    // Before the drain below, so the take's first note already meets the fresh state.
    if (ottRestartRequested.exchange(false, std::memory_order_acquire)) masterChain.ott.restart();
    // ⚠️ Past the stop ramp's end the fading voices are ended: one run off its sample mixes a frame per
    // block and would hold a slot for ~256 blocks, forcing the next take into clicking steals.
    {
        const int64_t rampEnd = stopRampEndFrame.load(std::memory_order_relaxed);
        if (rampEnd >= 0 && blockStartFrame >= rampEnd) {
            stopRampEndFrame.store(-1, std::memory_order_relaxed);
            for (int i = 0; i < MAX_VOICES; i++)
                if (voices[i].isFadingOut) voices[i].stop();
        }
    }
    // A live key's bytes become records here, stamped at this block's first frame, so the drain
    // below takes them. Never during an export: a key is not part of the song.
    if (!b.offlineRender) {
        if (LiveInputSource* live = liveInput.load(std::memory_order_acquire))
            live->drainLiveInput(blockStartFrame, liveClock_);
    }
    paramUpdateQueue.drainUntil(blockEnd, paramBatch);
    killQueue.drainUntil(blockEnd, killBatch);
    noteQueue.drainUntil(blockEnd, noteBatch);
    // ⚠️ After the drain, so an edit made before queuing an event is in force when the event runs;
    // before the dispatch, so a table's EQM or TIM this block lands on top. The instrument data
    // first: the bus EQs are applied from the EQ presets it carries.
    syncInstrumentData();
    applyBusSettings();
}

// The drained batches, each record at its exact frame: params, then kills, then notes.
void AudioEngine::dispatchEvents(BlockMix& b) {
    size_t paramIdx = 0, killIdx = 0, noteIdx = 0;

    // ⚠️ One timeline: each queue yields to an earlier record in the ones after it, or a late block
    // (every take's first step) would run a note's params, one frame behind it, before the note.
    // At equal frames: params, kills, notes — a K00 on its own step relies on it.
    const auto dueKill = [&] { return killIdx < killBatch.size() ? killBatch[killIdx].targetFrame : INT64_MAX; };
    const auto dueNote = [&] { return noteIdx < noteBatch.size() ? noteBatch[noteIdx].targetFrame : INT64_MAX; };

    for (int32_t frame = 0; frame < b.numFrames; frame++) {
        int64_t currentFrame = b.startFrame + frame;

        // The scheduler only enqueues: voices are written here, where the mix is the sole writer.
        while (paramIdx < paramBatch.size() && paramBatch[paramIdx].targetFrame <= currentFrame &&
               paramBatch[paramIdx].targetFrame <= std::min(dueKill(), dueNote())) {
            applyParamUpdate(paramBatch[paramIdx++], b);
        }

        while (killIdx < killBatch.size() && killBatch[killIdx].targetFrame <= currentFrame &&
               killBatch[killIdx].targetFrame <= dueNote()) {
            applyKill(killBatch[killIdx++], frame);
        }

        while (noteIdx < noteBatch.size() && noteBatch[noteIdx].targetFrame <= currentFrame) {
            ScheduledNote note = noteBatch[noteIdx++];

            // ⚠️ The instrument is chosen here, not where the note was queued: a sequencer note
            // arrives as a number and becomes a sound on this line.
            if (note.instrumentId >= 0 && !resolveScheduledNote(note)) continue;   // nothing to play

            if (note.isSoundfont) triggerSoundfontNote(note, frame, currentFrame, b.sampleRate);
            else                  triggerSamplerNote(note, frame, currentFrame, b.sampleRate);
        }
    }
}

// One scheduled parameter update, on the voices it names or on the mixer. The fader arms also move
// this block's ramps in `b`.
void AudioEngine::applyParamUpdate(const ScheduledParamUpdate& upd, BlockMix& b) {
    const float sampleRate = b.sampleRate;
    switch (upd.action) {
        case PARAM_UPDATE_PITCH_BEND: {           // PBN on empty step
            if (IAudioVoice* pv = findActiveVoiceForTrack(upd.trackId)) pv->setPitchBendRaw(upd.value);
            break;
        }
        case PARAM_UPDATE_VIBRATO: {              // PVB/PVX on empty step
            if (IAudioVoice* pv = findActiveVoiceForTrack(upd.trackId)) pv->setVibratoRaw(upd.value, upd.value2);
            break;
        }
        case PARAM_UPDATE_WHEEL: {                // a MIDI-in bend: releasing notes bend too, as on a synth
            forEachVoiceOnTrack(upd.trackId, [&](auto& v) { v.midiWheel = upd.value; });
            break;
        }
        case PARAM_UPDATE_TABLE_ROW: {            // THO on empty step
            // ⚠️ All three columns: a THO written in a phrase belongs to none. One inside the table
            // steers the column it is typed in (processTableRow).
            forEachTrackVoice(upd.trackId, [&](auto& v) {
                for (int l = 0; l < TABLE_LANES; ++l) {
                    v.lanes[l].row = (int)upd.value % 16;
                    v.lanes[l].lastProcessed = -1;  // re-apply the row immediately
                }
            });
            break;
        }
        // A per-voice controller. ⚠️ PAN reaches only the note the track is playing; every
        // other one also reaches a SoundFont note still releasing there (forEachTrackVoice).
        case PARAM_UPDATE_VOICE_CC: {
            if (upd.sourceId == songcore::CC_PAN) {
                // Glided, as a table PAN is — a pan that jumps on a sounding note clicks.
                if (IAudioVoice* pv = findActiveVoiceForTrack(upd.trackId)) {
                    if (pv >= static_cast<IAudioVoice*>(&voices[0]) &&
                        pv <= static_cast<IAudioVoice*>(&voices[MAX_VOICES - 1]))
                        voiceGlidePan(*static_cast<Voice*>(pv), upd.value);
                    else
                        voiceGlidePan(*static_cast<SoundfontVoice*>(pv), upd.value);
                }
                break;
            }
            forEachTrackVoice(upd.trackId, [&](auto& v) { applyVoiceCc(v, upd.sourceId, upd.value, sampleRate); });
            break;
        }
        case PARAM_UPDATE_REVERSE: {              // BCK — playback direction (sampler only)
            forEachTrackVoice(upd.trackId, [&](auto& v) { voiceReverse(v, upd.value != 0.0f, upd.value2 != 0.0f); });
            break;
        }
        case PARAM_UPDATE_EQ_SLOT: {              // EQN — per-note EQ preset
            forEachTrackVoice(upd.trackId, [&](auto& v) { applyEqPresetToModule(v.chain.eq, (int)upd.value); });
            break;
        }
        case PARAM_UPDATE_MASTER_EQ: {            // EQM — master/mixer EQ preset (global)
            applyEqPresetToModule(masterChain.masterEq, (int)upd.value);   // audio thread: not the setter
            break;
        }
        // The two morph arms: the targets of the two above, the band values from the ramp. A
        // note-on resets a voice's EQ from the instrument; the next tick re-asserts the morph.
        case PARAM_UPDATE_EQ_BANDS: {             // EQN under AUS/AUF
            forEachTrackVoice(upd.trackId, [&](auto& v) { applyEqBandsToModule(v.chain.eq, upd.eqBands); });
            break;
        }
        case PARAM_UPDATE_MASTER_EQ_BANDS: {      // EQM under AUS/AUF (global)
            applyEqBandsToModule(masterChain.masterEq, upd.eqBands);
            break;
        }
        // ⚠️ Both fader arms redo this block's walk, from the block's start toward the new value:
        // the end is what the mix stages ramp to, the member is where the next block carries on
        // from. Write only the target and the glide starts a block late.
        case PARAM_UPDATE_TRACK_VOL: {            // VTR — this track's mixer fader
            if (upd.trackId >= 0 && upd.trackId < 8) {
                const int t = upd.trackId;
                trackVolumes[t].store(upd.value, std::memory_order_relaxed);
                // ⚠️ On a note's own step (it rides one frame behind the note) it does not glide:
                // the new note starts at its level, and what the note cut keeps the old fader
                // while it fades.
                const int64_t sinceOnset = upd.targetFrame - trackOnsetFrame[t];
                if (sinceOnset >= 0 && sinceOnset <= 1) {
                    for (Voice& v : voices)
                        if (v.trackId == t && v.isActive && v.isFadingOut && !v.faderHeld) {
                            v.faderHeld = true;
                            v.faderHeldStart = b.trackVolStart[t];
                            v.faderHeldEnd   = b.trackVolEnd[t];
                        }
                    SoundfontVoice& sv = sfVoices[t];
                    if (sv.isActive && (sv.isReleasingOnly || sv.hasArmedNote) && !sv.faderHeld) {
                        sv.faderHeld = true;
                        sv.faderHeldStart = b.trackVolStart[t];
                        sv.faderHeldEnd   = b.trackVolEnd[t];
                    }
                    b.trackVolStart[t] = b.trackVolEnd[t] = trackVolRamp[t] = upd.value;
                    trackVolSongMove[t] = false;
                } else {
                    b.trackVolEnd[t] = trackVolRamp[t] = glide_toward(b.trackVolStart[t], upd.value, b.faderStep);
                    trackVolSongMove[t] = true;
                }
                if (b.previewBorrows && b.previewTrack == t) {
                    b.trackVolStart[PREVIEW_LANE] = b.trackVolStart[t];
                    b.trackVolEnd[PREVIEW_LANE]   = b.trackVolEnd[t];
                }
            }
            break;
        }
        case PARAM_UPDATE_MASTER_VOL: {           // VMV — the master fader (global)
            masterVolume.store(upd.value, std::memory_order_relaxed);
            b.masterVolEnd = masterVolRamp = glide_toward(b.masterVolStart, upd.value, b.faderStep);
            masterVolSongMove = true;
            break;
        }
        case PARAM_UPDATE_DELAY_TIME: {           // TIM — the delay's echo time (global)
            delaySend.setTimeFree(filterByteOf(upd.value));
            break;
        }
        // An instrument was edited: every voice sounding on it re-reads what a trigger copied.
        // ⚠️ By instrument, not by track — one can be sounding on several at once.
        case PARAM_UPDATE_INSTRUMENT: {
            const int id = upd.instrId;
            if (id < 0 || id >= 256) break;
            const InstrumentParams& ip = instrumentParams[id];
            for (int v = 0; v < MAX_VOICES; v++)
                if (voices[v].isActive && !voices[v].isFadingOut && voices[v].instrId == id)
                    voiceReloadInstrument(voices[v], ip, sampleRate);
            for (int t = 0; t < SF_VOICE_COUNT; t++)
                if (sfVoices[t].isActive && sfVoices[t].instrId == id)
                    voiceReloadInstrument(sfVoices[t], ip, sampleRate);
            break;
        }
        default: {                                // PARAM_UPDATE_MOD_SOURCE — Vxx phraseVol
            forEachTrackVoice(upd.trackId, [&](auto& v) {
                v.modSourceValues[(ModSourceId)upd.sourceId] = upd.value;
            });
            break;
        }
    }
}

// One scheduled kill, on every voice of its track, at `frame` inside the block.
void AudioEngine::applyKill(const ScheduledKill& kill, int frame) {
    switch (kill.mode) {
        // A live key let go of: unlike an OFF, a one-shot plays out (Voice::keyRelease).
        case KILL_KEY_OFF: forEachVoiceOnTrack(kill.trackId, [&](auto& v) { voiceKeyRelease(v, frame); }); break;
        // An OFF note: each voice type runs its own release, or fades where it has none.
        case KILL_SOFT:    forEachVoiceOnTrack(kill.trackId, [&](auto& v) { v.noteOffAt(frame); });       break;
        case KILL_CUT:     forEachVoiceOnTrack(kill.trackId, [&](auto& v) { voiceCut(v, frame); });        break;
        default:           forEachVoiceOnTrack(kill.trackId, [&](auto& v) { voiceKill(v, frame); });       break;
    }
}

// ⚠️ Each sampler voice runs to the end of the block before the next starts, in pieces cut where
// its table plays a row — so a row starts on its own sample, not on a block edge.
// The edit lock is a try_lock: while a sample is being rewritten the voices advance unmixed.
void AudioEngine::mixSamplerVoices(BlockMix& b) {
    const int numFrames = b.numFrames;
    std::unique_lock<std::mutex> editLock(sampleEditMutex, std::try_to_lock);
    for (int v = 0; v < MAX_VOICES; v++) {
        Voice& voice = voices[v];
        for (int from = 0; from < numFrames && voice.isActive; ) {
            int to = numFrames;
            if (voice.tableId >= 0) {
                to = from + processTableTick(voice, from, numFrames - from, b.sampleRate);
                if (!voice.isFadingOut) storeTic00Cursor(voice.trackId, voice.tableId, voice.lanes);
            }
            prepareSamplerPiece(voice, to - from, b.sampleRate);
            if (editLock.owns_lock() && voice.isActive) mixSamplerPiece(voice, b, from, to);
            from = to;
        }
    }
}

// The one place a track's TIC00 cursor is written. ⚠️ Only from the voice that owns the track — a
// sampler voice fading under a new note would make the value depend on slot order.
void AudioEngine::storeTic00Cursor(int trackId, int tableId, const TableLane (&lanes)[TABLE_LANES]) {
    bool anyTic00 = false;
    for (int l = 0; l < TABLE_LANES; ++l) anyTic00 |= (lanes[l].ticRate == 0x00);
    if (!anyTic00 || trackId < 0 || trackId >= SF_VOICE_COUNT) return;
    if (Tic00Cursor* c = tic00Slot(trackId, tableId, /*create=*/true)) {
        for (int l = 0; l < TABLE_LANES; ++l) {
            c->row[l]           = lanes[l].row;
            c->lastProcessed[l] = lanes[l].lastProcessed;
            c->ticRate[l]       = lanes[l].ticRate;
            c->active[l]        = lanes[l].active;
        }
        tic00Sounding[trackId] = tableId;   // what the TABLE screen draws
    }
}

// Where a new note's table columns start. A THO with a note places every column; a TIC00 column goes on
// from the row after the track's last note in this table — read off `live`, the lanes of the voice
// still running it, else off the track's cursor (a voice that ran out); the rest start at the top.
void AudioEngine::tableStartRows(const ScheduledNote& note, const TableLane* live,
                                 const int (&rates)[TABLE_LANES], int (&out)[TABLE_LANES]) {
    int saved[TABLE_LANES] = {-1, -1, -1};
    bool found = false;
    for (int l = 0; live && l < TABLE_LANES; ++l) {
        if (live[l].ticRate != 0x00) continue;
        saved[l] = tic00RowAfter(live[l]);
        found = true;
    }
    if (!found && note.tableId >= 0) {
        if (const Tic00Cursor* c = tic00Slot(note.trackId, note.tableId, /*create=*/false)) {
            for (int l = 0; l < TABLE_LANES; ++l)
                if (c->ticRate[l] == 0x00 && c->active[l]) saved[l] = tic00RowAfter(c->row[l], c->lastProcessed[l]);
        }
    }
    for (int l = 0; l < TABLE_LANES; ++l) {
        if (note.tableStartRow >= 0)                 out[l] = note.tableStartRow % 16;
        else if (rates[l] == 0x00 && saved[l] >= 0)  out[l] = saved[l];
        else                                         out[l] = 0;
    }
}

// A sampler voice's modulation for the next `frames`, then its pan and filter. Each keeps the value
// it had: the mix loop ramps from that to the new one.
void AudioEngine::prepareSamplerPiece(Voice& voice, int frames, float sampleRate) {
    updateVoicePitchMod(voice, frames, sampleRate);

    for (int m = 0; m < 4; m++) voice.voiceMods[m].prevEnvValue = voice.voiceMods[m].envValue;
    updateVoiceModulation(voice, frames, sampleRate);

    voice.prevPanLeft  = voice.panLeft;
    voice.prevPanRight = voice.panRight;
    if (voiceStepPanGlide(voice, frames)) voice.setPanGains(voice.panGlide.value());
    if (fabsf(voice.params.mod[PARAM_PAN]) > 0.001f)
        voice.setPanGains(fmaxf(0.0f, fminf(1.0f, voice.params.get(PARAM_PAN))));

    voice.chain.filter.snapshotCoeffs();
    if (voice.chain.filter.enabled() &&
            (fabsf(voice.params.mod[PARAM_FILTER_CUT]) > 0.5f ||
             fabsf(voice.params.mod[PARAM_FILTER_RES]) > 0.5f)) {
        int modCut = std::max(0, std::min(255, (int)voice.params.get(PARAM_FILTER_CUT)));
        int modRes = std::max(0, std::min(255, (int)voice.params.get(PARAM_FILTER_RES)));
        voice.chain.filter.setParams(voice.chain.filter.type, modCut, modRes, voice.chain.filter.drive, sampleRate);
    }

    // A looping voice ends when its volume envelopes have: AHD/DRUM at stage 4, ADSR/TRIG at 5.
    if (voice.loopMode != 0) {
        bool hasVolMod = false, allDone = true;
        for (int m = 0; m < 4; m++) {
            const VoiceModSlot& mod = voice.voiceMods[m];
            if (mod.dest == 1 && (mod.type == 1 || mod.type == 2 || mod.type == 4 || mod.type == 5)) {
                hasVolMod = true;
                int doneStage = (mod.type == 2 || mod.type == 5) ? 5 : 4;
                if (mod.stage < doneStage) allDone = false;
            }
        }
        if (hasVolMod && allDone) voice.isActive = false;
    }
}

// One piece of one sampler voice — frames [from, to) of the block — into the output and sends.
void AudioEngine::mixSamplerPiece(Voice& voice, const BlockMix& b, int from, int to) {
    if (voice.chordExtra > 0) mixSamplerPieceAs<true>(voice, b, from, to);
    else                      mixSamplerPieceAs<false>(voice, b, from, to);
}

template <bool Chord>
void AudioEngine::mixSamplerPieceAs(Voice& voice, const BlockMix& b, int from, int to) {
    float* const output    = b.output;
    const int numFrames    = b.numFrames;
    const int channelCount = b.channelCount;

    if (!voice.sampleData) return;
    // The slot's buffers changed since this voice was triggered: its pointer may be freed.
    if (voice.sampleGen != sampleGen[voice.instrId].load(std::memory_order_relaxed)) {
        voice.stop();
        return;
    }

    voice.chain.drive.setDrive(paramWithMod(voice, PARAM_DRIVE, 255));
    voice.chain.crush.setParams(paramWithMod(voice, PARAM_CRUSH, 15), 0);   // the downsample is in the read, below
    const int effDownsample = paramWithMod(voice, PARAM_DOWNSAMPLE, 15);
    if (effDownsample != voice.dsLast) {
        if (voice.chain.started) {   // a change on the sounding note — before it, just where it starts
            voice.dsPrev     = voice.dsLast;
            voice.dsFade.start(1.0f, 0.0f, GLIDE_SAMPLES);
        }
        voice.dsLast = effDownsample;
    }
    {
        int sl = voice.sampleLength;
        // Re-derived every piece so a mod route can move them while the note rings. ⚠️ Not an
        // exact-frame window: the 0-255 pair cannot express it, so that one is carried on the voice.
        if (voice.windowStartFrame >= 0) {
            voice.actualStart = std::max(0, std::min(voice.windowStartFrame, sl - 2));
            voice.actualEnd   = std::max(voice.actualStart + 1, std::min(voice.windowEndFrame, sl - 1));
        } else {
            float rawStart   = voice.params.base[PARAM_SAMPLE_START] + voice.modDestValues[PARAM_SAMPLE_START];
            float rawEnd     = voice.params.base[PARAM_SAMPLE_END]   + voice.modDestValues[PARAM_SAMPLE_END];
            derive_sample_window(rawStart, rawEnd, sl, voice.actualStart, voice.actualEnd);
        }
        float rawLoop    = voice.params.base[PARAM_LOOP_START]   + voice.modDestValues[PARAM_LOOP_START];
        voice.actualLoopStart = std::max(voice.actualStart, std::min((int)(rawLoop * sl / 255.0f), voice.actualEnd - 1));
        voice.actualLoopEnd   = std::max(voice.actualLoopStart + 1, std::min((int)((float)voice.loopEndNorm * sl / 255.0f), voice.actualEnd));

        // LPO moves both loop bounds alike (the length is the pitch), from the running count of
        // sixteenths, clamped to the sample. ⚠️ The playhead moves too, or a step is heard a loop late.
        const int len = voice.actualLoopEnd - voice.actualLoopStart;
        int       off = 0;
        if (voice.loopSlideSixteenths != 0 && len > 0) {
            const int maxSixteenths = (int)(((int64_t)(voice.actualEnd - voice.actualLoopEnd) * 16) / len);
            const int minSixteenths = (int)(((int64_t)(voice.actualStart - voice.actualLoopStart) * 16) / len);
            voice.loopSlideSixteenths = std::max(minSixteenths,
                                                 std::min(voice.loopSlideSixteenths, maxSixteenths));
            off = (int)(((int64_t)voice.loopSlideSixteenths * len) / 16);
        }
        if (off != voice.loopSlideFrames) {
            // ⚠️ Only while the playhead is inside the loop: the intro before the first wrap and
            // a released note's tail are not positions the window owns.
            const int wasStart = voice.actualLoopStart + voice.loopSlideFrames;
            const int wasEnd   = voice.actualLoopEnd   + voice.loopSlideFrames;
            if (voice.position >= (double)wasStart && voice.position < (double)wasEnd)
                voice.position += (double)(off - voice.loopSlideFrames);
            for (int k = 0; k < voice.chordExtra; ++k)
                if (voice.chordPos[k] >= (double)wasStart && voice.chordPos[k] < (double)wasEnd)
                    voice.chordPos[k] += (double)(off - voice.loopSlideFrames);
            voice.loopSlideFrames = off;
        }
        voice.actualLoopStart += off;
        voice.actualLoopEnd   += off;
    }

    // ⚠️ After the loop bounds: oscillator mode derives its rate from the loop's length.
    float modulatedRate = getModulatedPlaybackRate(voice);

    // A note dispatched at frame f of the block does not sound before f; an onset beyond this
    // piece waits for the piece that contains it.
    int startFrame = from;
    if (voice.startDelayFrames > 0) {
        if (voice.startDelayFrames >= to) return;
        startFrame = std::max(from, voice.startDelayFrames);
        voice.startDelayFrames = 0;
    }

    // ⚠️ The loop below works on locals: its stores into the float buffers may alias any float of
    // the voice, so a field read inside it is reloaded on every sample. What it advances is written
    // back below it — leave it by `break`, never `return`.
    Playhead   ph           = playheadOf(voice, modulatedRate);
    const bool stereo       = ph.right != nullptr;
    const int  lastReadable = voice.sampleLength - 1;   // the read needs idx + 1

    // A chord's other playheads: the voice's own, at their own place and rate.
    const int chordExtra = Chord ? voice.chordExtra : 0;
    Playhead  chord[Voice::CHORD_EXTRA];
    bool      chordDone[Voice::CHORD_EXTRA];
    int       chordWait[1 + Voice::CHORD_EXTRA];
    if constexpr (Chord) chordWait[0] = voice.chordWait[0];
    for (int k = 0; k < chordExtra; ++k) {
        chordWait[k + 1]     = voice.chordWait[k + 1];
        chord[k]             = ph;
        chord[k].position    = voice.chordPos[k];
        chord[k].loopingBack = voice.chordBack[k];
        chord[k].rate        = modulatedRate * voice.chordRatio[k];
        chordDone[k]         = voice.chordDone[k];
    }
    const float chordIn = voice.chordIn, chordOut = voice.chordOut;

    // ⚠️ Two ramps, two clocks: the voice's own params ramp across this PIECE (`t`), the mixer's
    // fader and mute gate across the whole BLOCK (`tBlock`) — or a cut piece would step them.
    const int   pieceFrames = to - from;
    const float panLFrom = voice.prevPanLeft,  panLStep = voice.panLeft  - voice.prevPanLeft;
    const float panRFrom = voice.prevPanRight, panRStep = voice.panRight - voice.prevPanRight;
    const float volume   = voice.volume;
    // The mods routed to VOL, in slot order: an LFO scales the level, an envelope adds to it.
    struct VolMod { float from, step, amount; bool scales; };
    VolMod volMods[4];
    int    volModCount = 0;
    for (int m = 0; m < 4; m++) {
        const VoiceModSlot& mod = voice.voiceMods[m];
        if (mod.type == 0 || mod.stage == 0 || mod.dest != 1) continue;
        volMods[volModCount++] = {mod.prevEnvValue, mod.envValue - mod.prevEnvValue, mod.effectiveAmt, mod.type == 3};
    }
    const float volRouteFrom = voice.prevModDestValues[PARAM_VOL];
    const float volRouteStep = voice.modDestValues[PARAM_VOL] - voice.prevModDestValues[PARAM_VOL];
    const float volGlideFrom = voice.volGlideFrom;
    Ramp  volGlide     = voice.volGlide;
    float volRouteLast = voice.volRouteLast;

    // ⚠️ SF_VOICE_COUNT, not 8: the preview lane carries a real fader. Off any track both are 1.
    const int   trackId = voice.trackId;
    const bool  onTrack = trackId >= 0 && trackId < SF_VOICE_COUNT;
    const float faderFrom = !onTrack ? 1.0f : voice.faderHeld ? voice.faderHeldStart : b.trackVolStart[trackId];
    const float faderTo   = !onTrack ? 1.0f : voice.faderHeld ? voice.faderHeldEnd   : b.trackVolEnd[trackId];
    const float faderStep = faderTo - faderFrom;
    const float gateFrom  = onTrack ? b.gateStart[trackId] : 1.0f;
    const float gateStep  = onTrack ? b.gateEnd[trackId] - b.gateStart[trackId] : 0.0f;

    const int dsPrev     = voice.dsPrev;
    Ramp      dsFade     = voice.dsFade;
    Ramp      fadeIn     = voice.fadeIn;

    InstrumentChain& chain = voice.chain;
    const bool chainIdle   = chain.idle();
    bool       chainRan    = false;

    // A fade dispatched at frame f waits for f. Running out of sample starts one below the loop.
    const int fadeStartFrame = voice.fadeStartFrame;
    Ramp      fadeOut        = voice.fadeOut;
    bool      isFadingOut    = voice.isFadingOut;
    bool      isActive       = voice.isActive;
    bool      ranOut         = false;

    const bool   toSends    = stemsMode == 0 || stemsMode >= 9;
    Ramp&        revLevel   = voice.reverbSend;
    Ramp&        dlyLevel   = voice.delaySend;
    const bool   toReverb   = toSends && revLevel.audible();
    const bool   toDelay    = toSends && dlyLevel.audible();
    const bool   toOutput   = stemsMode == 0 || trackId == stemsMode - 1;
    float* const revL = revSendBufL; float* const revR = revSendBufR;
    float* const dlyL = dlySendBufL; float* const dlyR = dlySendBufR;

    const bool metered = trackId >= 0 && trackId < 8;
    float peakL = metered ? framePeaksPerTrackL[trackId] : 0.0f;
    float peakR = metered ? framePeaksPerTrackR[trackId] : 0.0f;
    // The OCTA scopes: tracks 0-7 and the preview lane, only while the screen is asking.
    const bool   scoped    = b.octaWanted && trackId >= 0 && trackId < TRACK_WAVEFORM_COUNT;
    float* const scopeL    = scoped ? trackWaveAccumL[trackId] : nullptr;
    float* const scopeR    = scoped ? trackWaveAccumR[trackId] : nullptr;
    bool         scopeLive = false;
    const bool   monitored = b.monitoredInstrId >= 0 && voice.instrId == b.monitoredInstrId;
    float* const spectrum  = instrSpectrumTempL;

    for (int i = startFrame; i < to; i++) {
        const int idx = (int)ph.position;
        // In double, then narrowed: (float)idx is inexact past 2^24 frames.
        const float frac = (float)(ph.position - (double)idx);

        if (idx < 0) {   // safety hard-stop
            isActive = false;
            break;
        }
        if (idx >= lastReadable) {   // fade rather than stop, so a resonant filter decays
            ph.position = (double)(lastReadable - 1);
            ranOut = true;
            break;
        }

        const float t      = (float)(i - from + 1) / (float)pieceFrames;   // both reach 1 on the last frame
        const float tBlock = (float)(i + 1) / (float)numFrames;
        const float panL   = panLFrom + panLStep * t;
        const float panR   = panRFrom + panRStep * t;
        float finalVol = volume;
        for (int m = 0; m < volModCount; m++) {
            const VolMod& mod    = volMods[m];
            const float   envAtI = mod.from + mod.step * t;
            finalVol = mod.scales ? fmaxf(0.0f, finalVol * (1.0f + envAtI * mod.amount))
                                  : fmaxf(0.0f, finalVol + (envAtI - 1.0f) * mod.amount);
        }
        float volRoute = volRouteFrom + volRouteStep * t;
        if (volGlide.moving()) {   // a table VOL on the sounding note: blend in, never step
            const float k = volGlide.next();
            volRoute = volGlideFrom * k + volRoute * (1.0f - k);
        }
        volRouteLast = volRoute;
        const float trackVol  = (faderFrom + faderStep * tBlock) * (gateFrom + gateStep * tBlock);
        const float antiClick = antiClickGain(fadeIn, ph);

        float procL = Playhead::read(ph.left, idx, frac, effDownsample);
        float procR = stereo ? Playhead::read(ph.right, idx, frac, effDownsample) : procL;
        const bool dsBlend = dsFade.moving();
        float      dsOld   = 0.0f;
        if (dsBlend) {   // the downsample changed mid-note: the old read fades out
            dsOld = dsFade.next();
            procL = Playhead::read(ph.left, idx, frac, dsPrev) * dsOld + procL * (1.0f - dsOld);
            if (stereo) procR = Playhead::read(ph.right, idx, frac, dsPrev) * dsOld + procR * (1.0f - dsOld);
        }
        // A strum holds the voice's own playhead where it starts until its turn.
        bool ownWaits = false;
        if constexpr (Chord) {
            ownWaits = chordWait[0] > 0;
            const float own = strumGain(chordWait[0]);
            procL *= own;
            procR *= own;
            for (int k = 0; k < chordExtra; ++k) {
                if (chordDone[k]) continue;
                const bool waits  = chordWait[k + 1] > 0;
                const float strum = strumGain(chordWait[k + 1]);
                if (waits) continue;
                Playhead& c = chord[k];
                const int cIdx = (int)c.position;
                if (cIdx < 0 || cIdx >= lastReadable) {
                    chordDone[k] = true;
                    continue;
                }
                const float cFrac = (float)(c.position - (double)cIdx);
                float g = strum;   // and a one-shot's own fade into its last frames
                if (c.oneShot) {
                    const float remaining = c.framesLeft();
                    if (remaining >= 0.0f && remaining < (float)DECLICK_SAMPLES) g *= remaining / (float)DECLICK_SAMPLES;
                }
                float cL = Playhead::read(c.left, cIdx, cFrac, effDownsample);
                if (dsBlend) cL = Playhead::read(c.left, cIdx, cFrac, dsPrev) * dsOld + cL * (1.0f - dsOld);
                procL += cL * g;
                if (stereo) {
                    float cR = Playhead::read(c.right, cIdx, cFrac, effDownsample);
                    if (dsBlend) cR = Playhead::read(c.right, cIdx, cFrac, dsPrev) * dsOld + cR * (1.0f - dsOld);
                    procR += cR * g;
                }
                if (!c.advance()) chordDone[k] = true;
            }
            procL *= chordIn;
            procR *= chordIn;
        }
        chainRan = true;
        if (!chainIdle) {
            chain.filter.setInterpolatedCoeffs(t);
            if (stereo) {   // on copies: a local whose address is taken stays in memory all loop
                float l = procL, r = procR;
                chain.processStereo(l, r);
                procL = l;
                procR = r;
            } else {
                procL = chain.processMono(procL);
            }
        }
        if (!stereo) procR = procL;   // a mono sample feeds both lanes

        // Sends are tapped below everything on the instrument and ABOVE the track fader.
        float scalar = finalVol * volRoute;
        if constexpr (Chord) scalar *= chordOut;
        procL *= scalar;
        procR *= scalar;

        // ⚠️ The voice's own fades (anti-click, KIL/steal) must reach the sends too, or a send
        // gets a waveform cut mid-cycle and rings the click on in its tail.
        const bool fading    = isFadingOut && i >= fadeStartFrame;
        float      voiceFade = antiClick;
        float      fo        = 1.0f;
        if (fading) {
            fo = fadeOut.next();
            voiceFade *= fo;
            if (!fadeOut.moving()) {
                isFadingOut = false;
                isActive    = false;
            }
        }

        if (toReverb || toDelay) {
            if (toReverb) {
                const float rev = revLevel.next();
                revL[i] += procL * voiceFade * panL * rev;
                revR[i] += procR * voiceFade * panR * rev;
            }
            if (toDelay) {
                const float dly = dlyLevel.next();
                dlyL[i] += procL * voiceFade * panL * dly;
                dlyR[i] += procR * voiceFade * panR * dly;
            }
        }

        const float globalMul = trackVol * antiClick;
        procL *= globalMul;
        procR *= globalMul;
        if (fading) {   // not an unconditional × fo: the multiplies cost more than the test
            procL *= fo;
            procR *= fo;
        }
        const float sampleL = procL * panL;
        const float sampleR = procR * panR;

        if (toOutput) {
            output[i * channelCount] += sampleL;
            output[i * channelCount + 1] += sampleR;
        }

        if (metered && !isFadingOut) {
            peakL = peak_hold(peakL, sampleL);
            peakR = peak_hold(peakR, sampleR);
        }
        if (scoped || monitored) {
            if (scoped) {
                if (!isFadingOut) scopeLive = true;
                scopeL[i] += sampleL;
                scopeR[i] += sampleR;
            }
            if (monitored) spectrum[i] += 0.5f * (sampleL + sampleR);
        }

        if (!isActive) break;
        if (ownWaits) continue;
        if (!ph.advance()) {
            ranOut = true;
            break;
        }
    }

    voice.position         = ph.position;
    voice.loopingBack      = ph.loopingBack;
    if constexpr (Chord) voice.chordWait[0] = chordWait[0];
    for (int k = 0; k < chordExtra; ++k) {
        voice.chordWait[k + 1] = chordWait[k + 1];
        voice.chordPos[k]  = chord[k].position;
        voice.chordBack[k] = chord[k].loopingBack;
        voice.chordDone[k] = chordDone[k];
    }
    voice.volGlide         = volGlide;
    voice.volRouteLast     = volRouteLast;
    voice.dsFade           = dsFade;
    voice.fadeIn           = fadeIn;
    voice.fadeOut          = fadeOut;
    voice.isFadingOut      = isFadingOut;
    voice.isActive         = isActive;
    if (ranOut) voice.startFadeOut();   // no-op if already fading
    if (chainIdle && chainRan) chain.touch();
    if (metered) {
        framePeaksPerTrackL[trackId] = peakL;
        framePeaksPerTrackR[trackId] = peakR;
    }
    if (scopeLive) trackWasActive[trackId] = true;
}

// Each SoundFont voice is rendered in pieces cut where its table plays a row, as the sampler voices
// are: TSF takes pitch and pan per render call, so a row's transpose starts on its own frame. The
// chain runs per piece; the gate, the sends and the output once across the block.
void AudioEngine::mixSoundfontVoices(BlockMix& b) {
    const int numFrames = b.numFrames;
    for (int t = 0; t < SF_VOICE_COUNT; t++) {
        SoundfontVoice& sv = sfVoices[t];
        if (!sv.isActive) continue;

        const int slot = sv.sfSlot;
        // Valid until this block ends.
        tsf* h = (slot >= 0 && slot < MAX_SOUNDFONTS) ? soundfonts[slot].handle.load() : nullptr;
        if (h) memset(sfBuf, 0, sizeof(float) * numFrames * 2);
        for (int from = 0; from < numFrames; ) {
            int to = numFrames;
            if (sv.tableId >= 0) {
                to = from + processTableTick(sv, from, numFrames - from, b.sampleRate);
                storeTic00Cursor(t, sv.tableId, sv.lanes);
            }
            prepareSoundfontPiece(sv, t, from, to - from, b.sampleRate);
            if (h) renderSoundfontPiece(sv, t, h, b, from, to);
            from = to;
        }
        sv.strumBlockEnd(numFrames);
        if (!h) continue;

        bool stopFadeDone = false;
        const float trackPeak = mixTrackBuffer(sv, t, sfBuf, b, stopFadeDone);

        // A released note is rendered until TSF goes silent — unless an ADSR/TRIG VOL release is
        // still running: that one ends the note itself (prepareSoundfontPiece).
        if (sv.isReleasingOnly && trackPeak < 0.0005f) {
            bool adsrReleasing = false;
            for (int m = 0; m < 4; m++) {
                const VoiceModSlot& mod = sv.voiceMods[m];
                if (mod.dest == 1 && (mod.type == 2 || mod.type == 5) && mod.stage == 4) {
                    adsrReleasing = true; break;
                }
            }
            if (!adsrReleasing) sv.hardStop();
        }

        // The stop ramp reached zero inside this block; its last samples are already in the bus.
        if (stopFadeDone) sv.hardStop();
    }
}

// A SoundFont voice's modulation for the next `frames` of the block, from frame `from`: the note's
// gain, pan, filter, drive, crush and the pitch wheel, each held across the piece.
void AudioEngine::prepareSoundfontPiece(SoundfontVoice& sv, int t, int from, int frames, float sampleRate) {
    updateVoiceModulation(sv, frames, (float)sampleRate);

    // The note's gain at the end of this piece, and — for a note armed this block — at its
    // onset, where an envelope with an attack starts from zero. A finished AHD/ADSR still
    // counts: its value is 0, and skipping it would bring the note back at full volume.
    float noteVol   = sv.modDestValues[PARAM_VOL];
    float onsetVol  = noteVol;
    bool  hasVolEnv = false, volEnvDone = true;
    for (int m = 0; m < 4; m++) {
        VoiceModSlot& mod = sv.voiceMods[m];
        if (mod.type == 0 || mod.stage == 0 || mod.dest != 1) continue;
        if (mod.type == 3) {  // LFO: bipolar tremolo
            noteVol  = fmaxf(0.0f, noteVol  * (1.0f + mod.envValue * mod.effectiveAmt));
            onsetVol = fmaxf(0.0f, onsetVol * (1.0f + mod.envValue * mod.effectiveAmt));
        } else if (mod.type == 1 || mod.type == 2 || mod.type == 4 || mod.type == 5) {
            // Unipolar gain reduction. AHD/DRUM are done at stage 4, ADSR/TRIG at stage 5.
            hasVolEnv = true;
            if (mod.stage < ((mod.type == 2 || mod.type == 5) ? 5 : 4)) volEnvDone = false;
            const float onsetEnv = mod.attackSamples > 0 ? 0.0f : 1.0f;
            noteVol  = fmaxf(0.0f, noteVol  + (mod.envValue - 1.0f) * mod.effectiveAmt);
            onsetVol = fmaxf(0.0f, onsetVol + (onsetEnv     - 1.0f) * mod.effectiveAmt);
        }
    }
    sv.volGainFrom = sv.hasArmedNote ? onsetVol : sv.volGain;
    sv.volGainTo   = noteVol;
    // A table VOL on the sounding note blends in, as the sampler's does, a piece at a time. The
    // table tick keeps the pieces short meanwhile, so the steps are small ramps.
    if (sv.volGlide.moving() && !sv.hasArmedNote) {
        const float k = sv.volGlide.skip(frames);
        sv.volGainTo  = sv.volGlideFrom * k + noteVol * (1.0f - k);
    }
    // TSF pans on its own channel. Only when modulated or gliding, so an unmodulated voice keeps
    // the pan the note or a PAN effect gave it.
    const bool panModded = fabsf(sv.params.mod[PARAM_PAN]) > 0.001f;
    const float modPan = panModded ? fmaxf(0.0f, fminf(1.0f, sv.params.get(PARAM_PAN))) : 0.0f;

    int volSlot = sv.sfSlot;
    const bool panGliding = voiceStepPanGlide(sv, frames);
    if ((panModded || panGliding) && volSlot >= 0 && volSlot < MAX_SOUNDFONTS) {
        tsf* h = soundfonts[volSlot].handle.load();
        if (h) tsf_channel_set_pan(h, t, panModded ? modPan : sv.panGlide.value());
    }

    // Every VOL envelope has finished: the note ends, faded from where this piece's ramp leaves
    // it, so a partial AMT cannot end in a step. (An armed note's envelopes have only just started.)
    if (hasVolEnv && volEnvDone && !sv.hasArmedNote) sv.startStopFade(DECLICK_SAMPLES, from);

    sv.chain.filter.snapshotCoeffs();
    if (sv.chain.filter.enabled() &&
            (paramWithMod(sv, PARAM_FILTER_CUT, 255) != (int)sv.params.base[PARAM_FILTER_CUT] ||
             paramWithMod(sv, PARAM_FILTER_RES, 255) != (int)sv.params.base[PARAM_FILTER_RES])) {
        filterFromBus(sv, sv.chain.filter.type, sampleRate);
    }
    // The SF chain does its own downsampling: there is no read address here to quantize.
    sv.chain.drive.setDrive(paramWithMod(sv, PARAM_DRIVE, 255));
    sv.chain.crush.setParams(paramWithMod(sv, PARAM_CRUSH, 15), paramWithMod(sv, PARAM_DOWNSAMPLE, 15));

    sv.applyPitchMod((float)sampleRate, frames);
}

// Channel t's frames [from, to) into `buf` at the same frames, each strummed key sounded on its own
// frame: TSF starts a key at a render call's first frame.
void AudioEngine::renderSoundfontKeys(SoundfontVoice& sv, tsf* h, int t, float* buf, int from, int to) {
    while (from < to) {
        int next = to;
        for (int k = 0; k < sv.strumCount; ++k) next = std::min(next, std::max(from, sv.strumAt[k]));
        if (next == from) {
            sv.fireStrumKeys(h, from);
            continue;
        }
        tsf_render_float_channel(h, t, buf + from * 2, next - from, 0 /* overwrite */);
        from = next;
    }
}

// One piece of one SoundFont voice — frames [from, to) of the block — rendered into sfBuf at the
// same frames, with the note's gain, the track fader and the voice's chain on it.
void AudioEngine::renderSoundfontPiece(SoundfontVoice& sv, int t, tsf* h, const BlockMix& b, int from, int to) {
    const int numFrames = b.numFrames;
    // The note starts at its own frame; what is before it belongs to what the track was playing.
    // An armed note's onset always lies in the voice's first piece: its table's clocks start there.
    int sfStart = from;
    if (sv.startDelayFrames > 0) {
        sfStart = std::max(from, std::min(sv.startDelayFrames, to));
        sv.startDelayFrames = 0;
    }
    // The note sounding here keeps the fader it had when a VTR on the next note's step moved it.
    const float faderStart = sv.faderHeld ? sv.faderHeldStart : b.trackVolStart[t];
    const float faderEnd   = sv.faderHeld ? sv.faderHeldEnd   : b.trackVolEnd[t];
    if (!sv.hasArmedNote) {
        // A note-off dispatched mid-block is sent between two renders, so TSF's release begins on
        // its frame: TSF steps its envelope from a render call's first frame.
        const int offAt = sv.pendingTsfOffAt;
        if (offAt > from && offAt < to) {
            renderSoundfontKeys(sv, h, t, sfBuf, from, offAt);
            sv.keysOff(h, sv.pendingTsfOffNote);
            renderSoundfontKeys(sv, h, t, sfBuf, offAt, to);
            sv.pendingTsfOffAt = -1;
        } else {
            // Due at or before this piece — or carried over at 0 from a block that did not
            // render this voice.
            if (offAt >= 0 && offAt <= from) {
                sv.keysOff(h, sv.pendingTsfOffNote);
                sv.pendingTsfOffAt = -1;
            }
            renderSoundfontKeys(sv, h, t, sfBuf, from, to);
        }
        applyGainRamp(sfBuf + from * 2, to - from, sv.volGainFrom, sv.volGainTo);
        faderThenChain(sv.chain, sfBuf, from, to, faderStart, faderEnd, numFrames);
    } else {
        // ⚠️ A steal renders the two notes apart, each through its own fader and chain, and adds them:
        // the old one up to `fadeEnd` (past the new onset) through `cutChain`, faded at its output — TSF's
        // own release steps per 64 — and only then cut; the new one from its onset. `fadeEnd` is clamped
        // to the piece.
        const int fadeEnd   = std::min(to, sfStart + DECLICK_SAMPLES);
        const int rampStart = std::max(from, fadeEnd - DECLICK_SAMPLES);
        const int rampLen   = fadeEnd - rampStart;
        tsf_render_float_channel(h, t, sfBuf + from * 2, fadeEnd - from, 0 /* overwrite */);
        // The old note keeps the gain it ended the last piece on — the new note's envelope has
        // already replaced the mods, and must not reach the note it cuts.
        applyGainRamp(sfBuf + from * 2, fadeEnd - from, sv.volGain, sv.volGain);
        if (sv.cutting) faderThenChain(sv.cutChain, sfBuf, from, fadeEnd, faderStart, faderEnd, numFrames);
        Ramp out;
        out.start(1.0f, 0.0f, rampLen - 1);   // at zero on the last frame
        for (int i = rampStart; i < fadeEnd; i++) {
            const float g = out.next();
            sfBuf[i * 2]     *= g;
            sfBuf[i * 2 + 1] *= g;
        }
        sv.cutting   = false;
        sv.faderHeld = false;
        // Now the old voices can be cut: the samples they contributed are already at zero.
        sv.fireArmedNote(h, sfStart);
        // sfNoteBuf at the same frames as sfBuf, so the fader's ramp lines up.
        const int noteFrames = to - sfStart;
        if (noteFrames > 0) {
            float* const note = sfNoteBuf;
            renderSoundfontKeys(sv, h, t, note, sfStart, to);
            applyGainRamp(note + sfStart * 2, noteFrames, sv.volGainFrom, sv.volGainTo);
            faderThenChain(sv.chain, note, sfStart, to, b.trackVolStart[t], b.trackVolEnd[t], numFrames);
            for (int i = sfStart * 2; i < to * 2; i++) sfBuf[i] += note[i];
        }
    }
    sv.volGain = sv.volGainTo;
}

// The intra-block frames above were for THIS block. A voice the block did not mix (the sample
// edit lock was held, the handle was gone) keeps its fade or note-off and takes it from the
// next block's first frame — one block late, never one block early.
void AudioEngine::carryFadesToNextBlock() {
    for (int v = 0; v < MAX_VOICES; v++) voices[v].fadeStartFrame = 0;
    for (int t = 0; t < SF_VOICE_COUNT; t++) {
        sfVoices[t].stopFadeStartFrame = 0;
        if (sfVoices[t].pendingTsfOffAt > 0) sfVoices[t].pendingTsfOffAt = 0;
    }
}

// The OCTA scopes, only while OCTA is shown. try_lock: the UI holds this mutex while it copies the
// scopes, and one block missing from a scope is invisible.
void AudioEngine::captureTrackScopes(const BlockMix& b) {
    if (!b.octaWanted) return;
    std::unique_lock<std::mutex> lock(waveformMutex, std::try_to_lock);
    if (!lock.owns_lock()) return;
    for (int t = 0; t < TRACK_WAVEFORM_COUNT; t++) trackHasVoice[t] = trackWasActive[t];
    for (int i = 0; i < b.numFrames; i++) {
        for (int t = 0; t < TRACK_WAVEFORM_COUNT; t++) {
            // × masterVolEnd: the accumulators are filled pre-master, and OCTA shows what leaves it.
            trackWaveformBuffer[t][trackWaveformIndex] =
                (trackWaveAccumL[t][i] + trackWaveAccumR[t][i]) * 0.5f * b.masterVolEnd;
        }
        trackWaveformIndex = (trackWaveformIndex + 1) % WAVEFORM_SIZE;
    }
}

// The dry sum's gate, then the send buses and their returns onto the output.
void AudioEngine::mixBuses(const BlockMix& b) {
    float* const output    = b.output;
    const int numFrames    = b.numFrames;
    const int channelCount = b.channelCount;
    // Each level here ramps across the block from where the last one ended (walkMixerRamps).
    const auto at = [numFrames](float start, float end, int i) {
        return start + (end - start) * ((float)(i + 1) / (float)numFrames);
    };

    // ⚠️ The dry gate, for soloing a send return: every send has already tapped above this
    // multiply, where eight track mutes would also silence what feeds the reverb.
    if (b.dryGateStart < 1.0f || b.dryGateEnd < 1.0f) {
        for (int i = 0; i < numFrames; i++) {
            const float g = at(b.dryGateStart, b.dryGateEnd, i);
            output[i * channelCount]     *= g;
            output[i * channelCount + 1] *= g;
        }
    }

    // Delay first, so its output can feed the reverb.
    delaySend.process(dlySendBufL, dlySendBufR, dlyWetL, dlyWetR, numFrames);
    if (b.dlyToRevStart > 0.0f || b.dlyToRevEnd > 0.0f) {
        for (int i = 0; i < numFrames; i++) {
            const float feed = at(b.dlyToRevStart, b.dlyToRevEnd, i);
            revSendBufL[i] += dlyWetL[i] * feed;
            revSendBufR[i] += dlyWetR[i] * feed;
        }
    }
    reverbSend.process(revSendBufL, revSendBufR, revWetL, revWetR, numFrames);
    // Only while the spectrum UI is polling; try_lock, and a block that finds it held is dropped.
    if (b.spectrumWanted) {
        std::unique_lock<std::mutex> lock(spectrumMutex, std::try_to_lock);
        if (lock.owns_lock()) {
            for (int i = 0; i < numFrames; i++) {
                delaySpectrumBuffer[delaySpectrumWriteIdx] = 0.5f * (dlyWetL[i] + dlyWetR[i]);
                delaySpectrumWriteIdx = (delaySpectrumWriteIdx + 1) % SPECTRUM_SIZE;
                reverbSpectrumBuffer[reverbSpectrumWriteIdx] = 0.5f * (revWetL[i] + revWetR[i]);
                reverbSpectrumWriteIdx = (reverbSpectrumWriteIdx + 1) % SPECTRUM_SIZE;
            }
        }
    }
    for (int i = 0; i < numFrames; i++) {
        // ⚠️ A return's mute rides below its module, so a muted reverb keeps building its tail.
        const float rvReturn = at(b.revReturnStart, b.revReturnEnd, i);
        const float dlReturn = at(b.dlyReturnStart, b.dlyReturnEnd, i);
        const float rvGate   = at(b.revGateStart, b.revGateEnd, i);
        const float dlGate   = at(b.dlyGateStart, b.dlyGateEnd, i);
        float rv  = revWetL[i] * rvReturn * rvGate;
        float rvR = revWetR[i] * rvReturn * rvGate;
        float dl  = dlyWetL[i] * dlReturn * dlGate;
        float dlR = dlyWetR[i] * dlReturn * dlGate;
        if (stemsMode == 0) {
            output[i * channelCount]     += rv + dl;
            output[i * channelCount + 1] += rvR + dlR;
        } else if (stemsMode == 9) {
            output[i * channelCount]     += rv;
            output[i * channelCount + 1] += rvR;
        } else if (stemsMode == 10) {
            output[i * channelCount]     += dl;
            output[i * channelCount + 1] += dlR;
        }
        // modes 1-8: no send returns (dry track stems)
        frameSendPeakRevL = peak_hold(frameSendPeakRevL, rv);
        frameSendPeakRevR = peak_hold(frameSendPeakRevR, rvR);
        frameSendPeakDelL = peak_hold(frameSendPeakDelL, dl);
        frameSendPeakDelR = peak_hold(frameSendPeakDelR, dlR);
    }
}

// The master fader, the master chain, the metronome on top, and the monitored instrument's spectrum.
void AudioEngine::mixMaster(const BlockMix& b) {
    float* const output    = b.output;
    const int numFrames    = b.numFrames;
    const int channelCount = b.channelCount;

    // The master fader over dry and returns alike, before the limiter. The meters were filled
    // pre-master and are scaled by the block's end value.
    if (b.masterVolStart != 1.0f || b.masterVolEnd != 1.0f) {
        applyGainRamp(output, numFrames, b.masterVolStart, b.masterVolEnd);
        for (int t = 0; t < 8; t++) {
            framePeaksPerTrackL[t] *= b.masterVolEnd;
            framePeaksPerTrackR[t] *= b.masterVolEnd;
        }
        frameSendPeakRevL *= b.masterVolEnd;
        frameSendPeakRevR *= b.masterVolEnd;
        frameSendPeakDelL *= b.masterVolEnd;
        frameSendPeakDelR *= b.masterVolEnd;
    }

    // Master EQ → bus FX → limiter; a stems render takes the limiter only.
    if (stemsMode == 0)
        masterChain.process(output, numFrames, channelCount);
    else
        masterChain.limiter.process(output, numFrames, channelCount);

    // ⚠️ After the master chain: a click through the limiter would duck the whole mix on every beat.
    renderMetronome(output, numFrames, channelCount, b.sampleRate, b.startFrame, b.offlineRender);

    // Only when an instrument is being monitored (EQ screen), and never block on the UI read.
    if (b.monitoredInstrId >= 0) {
        std::unique_lock<std::mutex> lock(spectrumMutex, std::try_to_lock);
        if (lock.owns_lock()) {
            for (int i = 0; i < numFrames; i++) {
                // × masterVolEnd: filled pre-master, drawn post-master.
                instrSpectrumBuffer[instrSpectrumWriteIdx] = instrSpectrumTempL[i] * b.masterVolEnd;
                instrSpectrumWriteIdx = (instrSpectrumWriteIdx + 1) % SPECTRUM_SIZE;
            }
        }
    }
}

void AudioEngine::processLiveBlock(float* output, int numFrames, int channelCount, float sampleRate) {
    const auto blockStart = std::chrono::steady_clock::now();

    setFlushToZeroForCurrentThread();

    std::fill_n(output, numFrames * channelCount, 0.0f);

    // During offline WAV render: output silence and let renderOffline process the queue. The keys
    // pressed meanwhile are dropped, not saved up — see setLiveInput.
    if (isOfflineRendering.load()) {
        if (LiveInputSource* live = liveInput.load(std::memory_order_acquire)) live->discardLiveInput();
        return;
    }

    // One anchor for the whole callback, on the device's line: its sub-blocks run faster than real time.
    const int64_t startUs = std::chrono::duration_cast<std::chrono::microseconds>(
                                blockStart.time_since_epoch()).count();
    deviceClock_.on_callback(globalFrameCounter.load(std::memory_order_relaxed), startUs, numFrames,
                             static_cast<int>(sampleRate));
    liveClock_.us         = startUs;
    liveClock_.frame      = getDeviceFrame(startUs);
    liveClock_.sampleRate = static_cast<int>(sampleRate);

    // ⚠️ Chunked at PROCESS_SUBBLOCK, as renderOffline is: a device period resolves notes too coarsely.
    int processed = 0;
    while (processed < numFrames) {
        int chunk = std::min((int)numFrames - processed, PROCESS_SUBBLOCK);
        processAudioBlock(output + processed * channelCount, chunk, channelCount, sampleRate);
        processed += chunk;
    }

    // The master scope and the meters below take their mutex with try_lock, as the spectrum does:
    // the audio thread never waits for the UI, and one block missing from either is invisible.
    {
        std::unique_lock<std::mutex> lock(waveformMutex, std::try_to_lock);
        if (lock.owns_lock()) {
            for (int i = 0; i < numFrames; i++) {
                waveformDownsampleCounter++;
                if (waveformDownsampleCounter >= WAVEFORM_DOWNSAMPLE) {
                    waveformBuffer[waveformIndex] = output[i * channelCount];
                    waveformIndex = (waveformIndex + 1) % WAVEFORM_SIZE;
                    waveformDownsampleCounter = 0;
                }
            }
        }
    }

    // The master spectrum, only while a screen is polling it.
    if ((nowMs() - lastSpectrumReadMs.load(std::memory_order_relaxed)) < CAPTURE_IDLE_MS) {
        std::unique_lock<std::mutex> lock(spectrumMutex, std::try_to_lock);
        if (lock.owns_lock()) {
            for (int i = 0; i < numFrames; i++) {
                spectrumBuffer[spectrumWriteIdx] = channelCount > 1
                    ? 0.5f * (output[i * channelCount] + output[i * channelCount + 1])
                    : output[i * channelCount];
                spectrumWriteIdx = (spectrumWriteIdx + 1) % SPECTRUM_SIZE;
            }
        }
    }

    // The mixer's meters: each decays, then takes this block's peak if that is higher.
    {
        std::unique_lock<std::mutex> lock(peakMutex, std::try_to_lock);
        if (lock.owns_lock()) {
            const auto meter = [](float& held, float blockPeak) { held = fmaxf(held * PEAK_DECAY, blockPeak); };
            for (int t = 0; t < 8; t++) {
                meter(trackPeaksL[t], framePeaksPerTrackL[t]);
                meter(trackPeaksR[t], framePeaksPerTrackR[t]);
            }
            float maxL = 0.0f, maxR = 0.0f;
            for (int i = 0; i < numFrames; i++) {
                maxL = peak_hold(maxL, output[i * channelCount]);
                maxR = peak_hold(maxR, output[i * channelCount + 1]);
            }
            meter(masterPeakL, maxL);
            meter(masterPeakR, maxR);
            meter(sendPeakRevL, frameSendPeakRevL);
            meter(sendPeakRevR, frameSendPeakRevR);
            meter(sendPeakDelL, frameSendPeakDelL);
            meter(sendPeakDelR, frameSendPeakDelR);
        }
    }

    recordBlockTiming(blockStart, numFrames, sampleRate);
}

void AudioEngine::recordBlockTiming(std::chrono::steady_clock::time_point start, int numFrames,
                                    float sampleRate) {
    if (numFrames <= 0 || sampleRate <= 0.0f) return;
    const int64_t ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                           std::chrono::steady_clock::now() - start).count();
    // A load is busy time over sound time, in tenths of a percent.
    const auto load = [sampleRate](int64_t busyNs, int64_t frames) {
        return (int)std::min<double>(busyNs * (double)sampleRate / (frames * 1e6), 1e6);
    };
    const int blockLoad = load(ns, numFrames);
    timingBusyNs += ns;
    timingFrames += numFrames;
    timingWorst   = std::max(timingWorst, blockLoad);
    if (blockLoad >= 1000) timingLateBlocks.fetch_add(1, std::memory_order_relaxed);
    if (timingFrames < (int64_t)sampleRate) return;

    timingPubFrames.store(numFrames, std::memory_order_relaxed);
    timingPubRate.store((int)sampleRate, std::memory_order_relaxed);
    timingPubMean.store(load(timingBusyNs, timingFrames), std::memory_order_relaxed);
    timingPubWorst.store(timingWorst, std::memory_order_relaxed);
    timingBusyNs = 0;
    timingFrames = 0;
    timingWorst  = 0;
}

AudioEngine::BlockTiming AudioEngine::getBlockTiming() const {
    BlockTiming t;
    t.blockFrames = timingPubFrames.load(std::memory_order_relaxed);
    t.sampleRate  = timingPubRate.load(std::memory_order_relaxed);
    t.meanLoad    = timingPubMean.load(std::memory_order_relaxed);
    t.worstLoad   = timingPubWorst.load(std::memory_order_relaxed);
    t.lateBlocks  = timingLateBlocks.load(std::memory_order_relaxed);
    return t;
}
