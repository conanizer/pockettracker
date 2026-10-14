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
    if (fabsf(note.pbnRate) > 0.0001f) {
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
    // ⚠️ Read before `armNote`, which sets isActive: is this channel's chain still full of a
    // sounding note? See InstrumentChain::reset.
    const bool wasSounding = sv.isActive;
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
    const int sfStartRows[TABLE_LANES] = {note.tableStartRow, note.tableStartRow,
                                          note.tableStartRow};
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
    sv.chain.reset(sampleRate, /*keepToneState=*/wasSounding);
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
    sv.reverbSend = ip.reverbSend;
    sv.delaySend  = ip.delaySend;

    sv.params.setBase(PARAM_VOL,   note.volume);
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
    float initVol = note.volume * note.phraseVolume;
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
    // TIC00: the table carries on where this track's previous note left it — per column, since one
    // can be at TIC00 while another free-runs. −1 = nothing to carry.
    int savedTableRows[TABLE_LANES] = {-1, -1, -1};
    bool wasTIC00Mode = false;
    // ⚠️ Only from a voice running THIS table: a chain can hand the track a different one.
    for (int v = 0; v < MAX_VOICES; v++) {
        if (voices[v].trackId == note.trackId && voices[v].isActive && !voices[v].isFadingOut
            && voices[v].tableId >= 0 && voices[v].tableId == note.tableId) {
            for (int l = 0; l < TABLE_LANES; ++l) {
                if (voices[v].lanes[l].ticRate != 0x00) continue;
                wasTIC00Mode = true;
                savedTableRows[l] = tic00RowAfter(voices[v].lanes[l]);
                LOGT("📋 TIC00: table row %d for track %d column %d retrigger (from voice %d)",
                     savedTableRows[l], note.trackId, l + 1, v);
            }
        }
    }
    // No voice left to read the row off (a one-shot ran out): the track's cursor still holds it.
    if (!wasTIC00Mode && note.trackId >= 0 && note.trackId < SF_VOICE_COUNT &&
        note.tableId >= 0) {
        if (const Tic00Cursor* c = tic00Slot(note.trackId, note.tableId, /*create=*/false)) {
            for (int l = 0; l < TABLE_LANES; ++l) {
                if (c->ticRate[l] != 0x00 || !c->active[l]) continue;
                wasTIC00Mode = true;
                savedTableRows[l] = tic00RowAfter(c->row[l], c->lastProcessed[l]);
                LOGT("📋 TIC00: table row %d for track %d column %d retrigger (from table cursor)",
                     savedTableRows[l], note.trackId, l + 1);
            }
        }
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

            // A TIC in the table's last row overrides the instrument's tic rate, per FX column.
            int effectiveTicRates[TABLE_LANES];
            effectiveTicRatesFor(note.tableId, note.tableTicRate, effectiveTicRates);

            // A THO-with-note start row places every column; otherwise only a column that is both
            // at TIC00 and had something to carry resumes, and the rest begin at row 0.
            int startRows[TABLE_LANES] = {0, 0, 0};
            for (int l = 0; l < TABLE_LANES; ++l) {
                if (note.tableStartRow >= 0) startRows[l] = note.tableStartRow % 16;
                else if (wasTIC00Mode && effectiveTicRates[l] == 0x00 && savedTableRows[l] >= 0)
                    startRows[l] = savedTableRows[l];
            }

            // The generation BEFORE the buffers: the UI bumps it after storing them, so a
            // mismatch here can only make the mix end the voice, never read a stale pointer.
            const uint32_t gen = sampleGen[note.sampleId].load();
            voices[v].trigger(samples[note.sampleId], samplesRight[note.sampleId], sampleLengths[note.sampleId],
                              note.trackId, rate, note.baseFrequency,
                              note.volume, note.phraseVolume, note.pan, instrumentParams[note.sampleId],
                              sampleRate, note.startPointOverride, note.endPointOverride,
                              note.tableId, effectiveTicRates, note.noteOctave, note.notePitch, startRows);
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

// One piece — frames [from, to) — of a track's rendered stereo buffer: the track fader, then the
// voice's chain. The fader ramps across the whole block, the chain's filter across the piece.
template <typename V>
void AudioEngine::chainTrackPiece(V& v, int t, float* buf, const BlockMix& c, int from, int to) {
    // ⚠️ The fader goes on the rendered samples, not on the TSF channel: a channel volume is one
    // value per render call, a staircase. Above the chain, so drive and filter hear the faded signal.
    const float fStart = v.faderHeld ? v.faderHeldStart : c.trackVolStart[t];
    const float fEnd   = v.faderHeld ? v.faderHeldEnd   : c.trackVolEnd[t];
    if (v.faderInBuf) {
        v.faderInBuf = false;   // the steal pass has put it on already, old note and new apart
    } else if (fStart != 1.0f || fEnd != 1.0f) {
        for (int i = from; i < to; i++) {
            const float g = fStart + (fEnd - fStart) * (float)(i + 1) / (float)c.numFrames;
            buf[i * 2]     *= g;
            buf[i * 2 + 1] *= g;
        }
    }
    const int frames = to - from;
    for (int i = from; i < to; i++) {
        v.chain.filter.setInterpolatedCoeffs((float)(i - from + 1) / (float)frames);
        v.chain.processStereo(buf[i * 2], buf[i * 2 + 1]);
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
        if (v.stopFadeRemaining > 0) {
            if (i >= v.stopFadeStartFrame) {   // a ramp dispatched mid-block waits for its frame
                gate *= (float)v.stopFadeRemaining / (float)v.stopFadeTotal;
                if (--v.stopFadeRemaining <= 0) stopFadeDone = true;
            }
        } else if (stopFadeDone) {
            gate = 0.0f;   // the ramp ended inside this block; the rest of it is silence
        }
        buf[i * 2]     *= gate;
        buf[i * 2 + 1] *= gate;
    }

    if ((stemsMode == 0 || stemsMode >= 9) && (v.reverbSend > 0.0f || v.delaySend > 0.0f)) {
        for (int i = 0; i < c.numFrames; i++) {
            revSendBufL[i] += buf[i * 2]     * v.reverbSend;
            revSendBufR[i] += buf[i * 2 + 1] * v.reverbSend;
            dlySendBufL[i] += buf[i * 2]     * v.delaySend;
            dlySendBufR[i] += buf[i * 2 + 1] * v.delaySend;
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

// A fader's walk toward its target, at most `step` per block.
static inline float faderToward(float from, float target, float step) {
    return from < target ? fminf(target, from + step) : fmaxf(target, from - step);
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

    // ⚠️ A fader and a gate are each a PAIR, first frame and last: a jump at a block edge steps the
    // waveform. The mute is its own ramp, not folded into the fader.
    // ⚠️ Two read sites, both obligatory: the sampler's per-sample gain and the SoundFont buffer's
    // post-chain multiply. `tsf_channel_set_volume` is once per block and cannot carry either.
    // Both snap under an export: a render is the same samples every time, and a muted track starts
    // silent instead of fading out.

    // How far each may move in this block — per full swing, so a glide lasts the same time whatever
    // the block size. The fader's is also read by the VTR/VMV arms in applyParamUpdate.
    b.faderStep = (float)b.numFrames / (float)FADER_GLIDE_SAMPLES;
    const float faderStep = b.faderStep;
    {
        const float gateStep = (float)b.numFrames / (float)MUTE_GATE_SAMPLES;
        // One walk for every gate in the mixer. `gate` is the member that remembers where it got
        // to; start/end bracket this block.
        const auto walk_gate = [&](float& gate, bool muted, float& start, float& end) {
            const float target = muted ? 0.0f : 1.0f;
            if (offlineRender) gate = target;
            start = gate;
            if      (gate < target) gate = fminf(target, gate + gateStep);
            else if (gate > target) gate = fmaxf(target, gate - gateStep);
            end = gate;
        };
        const auto walk_fader = [&](float& ramp, float target, bool& songMove, float& start, float& end) {
            if (offlineRender && !songMove) ramp = target;
            start = ramp;
            end = ramp = faderToward(ramp, target, faderStep);
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
    // ⚠️ The transport-stop ramp has a deadline, and the pool is eight slots: a voice run off its
    // sample mixes one frame per block, so its fade would hold a slot for ~256 blocks and push the
    // next take's allocator into the steals that click. Past the ramp's end, ending the fading
    // voices is silent.
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
        if (LiveInputSource* live = liveInput.load(std::memory_order_acquire)) live->drainLiveInput(blockStartFrame);
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

    // ⚠️ The three batches are one timeline. A block that runs late (the first step of every take)
    // finds a note at F and its params at F+1 due on the same frame index, and params-first would
    // drop every per-voice param riding a frame behind its note (`voiceFxFrame`, scheduler.h). So
    // each queue yields to the earlier of the ones after it; `<=` keeps params, kills, notes at
    // equal frames, which a K00 on its step's frame relies on. A held-back record runs on the next
    // frame index.
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
                    b.trackVolEnd[t] = trackVolRamp[t] = faderToward(b.trackVolStart[t], upd.value, b.faderStep);
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
            b.masterVolEnd = masterVolRamp = faderToward(b.masterVolStart, upd.value, b.faderStep);
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
                storeTic00Cursor(voice);
            }
            prepareSamplerPiece(voice, to - from, b.sampleRate);
            if (editLock.owns_lock() && voice.isActive) mixSamplerPiece(voice, b, from, to);
            from = to;
        }
    }
}

// The one place a track's TIC00 cursor is written. Only the live (not fading) voice owns it, or
// the value would depend on slot order.
void AudioEngine::storeTic00Cursor(const Voice& voice) {
    bool anyTic00 = false;
    for (int l = 0; l < TABLE_LANES; ++l) anyTic00 |= (voice.lanes[l].ticRate == 0x00);
    if (anyTic00 && !voice.isFadingOut) {
        const int t = voice.trackId;
        if (t >= 0 && t < SF_VOICE_COUNT) {
            if (Tic00Cursor* c = tic00Slot(t, voice.tableId, /*create=*/true)) {
                for (int l = 0; l < TABLE_LANES; ++l) {
                    c->row[l]           = voice.lanes[l].row;
                    c->lastProcessed[l] = voice.lanes[l].lastProcessed;
                    c->ticRate[l]       = voice.lanes[l].ticRate;
                    c->active[l]        = voice.lanes[l].active;
                }
                tic00Sounding[t] = voice.tableId;   // what the TABLE screen draws
            }
        }
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
    if (voiceStepPanGlide(voice, frames)) voice.setPanGains(voice.panNow);
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
            voice.dsFadeLeft = DOWNSAMPLE_FADE_FRAMES;
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

        // LPO slides the whole loop window. ⚠️ Both bounds by the same amount: moving one changes
        // the loop's length, which is its pitch. The offset comes from the running COUNT of
        // sixteenths, clamped so the window stops at the sample's ends — never accumulated samples.
        // ⚠️ The playhead moves with the window, or a forward step would be heard a loop late.
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
    int   volGlideLeft = voice.volGlideLeft;
    float volRouteLast = voice.volRouteLast;

    // ⚠️ SF_VOICE_COUNT, not 8: the preview lane carries a real fader. Off any track both are 1.
    const int   trackId = voice.trackId;
    const bool  onTrack = trackId >= 0 && trackId < SF_VOICE_COUNT;
    const float faderFrom = !onTrack ? 1.0f : voice.faderHeld ? voice.faderHeldStart : b.trackVolStart[trackId];
    const float faderTo   = !onTrack ? 1.0f : voice.faderHeld ? voice.faderHeldEnd   : b.trackVolEnd[trackId];
    const float faderStep = faderTo - faderFrom;
    const float gateFrom  = onTrack ? b.gateStart[trackId] : 1.0f;
    const float gateStep  = onTrack ? b.gateEnd[trackId] - b.gateStart[trackId] : 0.0f;

    const int dsPrev          = voice.dsPrev;
    int       dsFadeLeft      = voice.dsFadeLeft;
    int       fadeInRemaining = voice.fadeInRemaining;

    InstrumentChain& chain = voice.chain;
    const bool chainIdle   = chain.idle();
    bool       chainRan    = false;

    // A fade dispatched at frame f waits for f. Running out of sample starts one below the loop.
    const int fadeStartFrame   = voice.fadeStartFrame;
    const int fadeOutTotal     = voice.fadeOutTotal;
    int       fadeOutRemaining = voice.fadeOutRemaining;
    bool      isFadingOut      = voice.isFadingOut;
    bool      isActive         = voice.isActive;
    bool      ranOut           = false;

    const bool   toSends    = stemsMode == 0 || stemsMode >= 9;
    const float  revLevel   = voice.reverbSend, dlyLevel = voice.delaySend;
    const bool   toReverb   = toSends && revLevel > 0.0f;
    const bool   toDelay    = toSends && dlyLevel > 0.0f;
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
        if (volGlideLeft > 0) {   // a table VOL on the sounding note: blend in, never step
            const float k = static_cast<float>(volGlideLeft) / VOL_GLIDE_FRAMES;
            volRoute = volGlideFrom * k + volRoute * (1.0f - k);
            --volGlideLeft;
        }
        volRouteLast = volRoute;
        const float trackVol  = (faderFrom + faderStep * tBlock) * (gateFrom + gateStep * tBlock);
        const float antiClick = antiClickGain(fadeInRemaining, ph);

        float procL = Playhead::read(ph.left, idx, frac, effDownsample);
        float procR = stereo ? Playhead::read(ph.right, idx, frac, effDownsample) : procL;
        if (dsFadeLeft > 0) {   // the downsample changed mid-note: the old read fades out
            const float k = static_cast<float>(dsFadeLeft--) / DOWNSAMPLE_FADE_FRAMES;
            procL = Playhead::read(ph.left, idx, frac, dsPrev) * k + procL * (1.0f - k);
            if (stereo) procR = Playhead::read(ph.right, idx, frac, dsPrev) * k + procR * (1.0f - k);
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
        const float scalar = finalVol * volRoute;
        procL *= scalar;
        procR *= scalar;

        // ⚠️ The voice's own fades (anti-click, KIL/steal) must reach the sends too, or a send
        // gets a waveform cut mid-cycle and rings the click on in its tail.
        const bool fading    = isFadingOut && i >= fadeStartFrame;
        float      voiceFade = antiClick;
        float      fo        = 1.0f;
        if (fading) {
            fo = (float)fadeOutRemaining / (float)fadeOutTotal;
            voiceFade *= fo;
            if (--fadeOutRemaining <= 0) {
                isFadingOut = false;
                isActive    = false;
            }
        }

        if (toReverb || toDelay) {
            if (toReverb) {
                revL[i] += procL * voiceFade * panL * revLevel;
                revR[i] += procR * voiceFade * panR * revLevel;
            }
            if (toDelay) {
                dlyL[i] += procL * voiceFade * panL * dlyLevel;
                dlyR[i] += procR * voiceFade * panR * dlyLevel;
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
        if (!ph.advance()) {
            ranOut = true;
            break;
        }
    }

    voice.position         = ph.position;
    voice.loopingBack      = ph.loopingBack;
    voice.volGlideLeft     = volGlideLeft;
    voice.volRouteLast     = volRouteLast;
    voice.dsFadeLeft       = dsFadeLeft;
    voice.fadeInRemaining  = fadeInRemaining;
    voice.fadeOutRemaining = fadeOutRemaining;
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
            if (sv.tableId >= 0) to = from + processTableTick(sv, from, numFrames - from, b.sampleRate);
            prepareSoundfontPiece(sv, t, from, to - from, b.sampleRate);
            if (h) {
                renderSoundfontPiece(sv, t, h, b, from, to);
                chainTrackPiece(sv, t, sfBuf, b, from, to);
            }
            from = to;
        }
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
    // A table VOL on the sounding note: this piece moves only its share of the way there. The
    // table tick keeps the pieces short meanwhile, so the steps are small ramps.
    if (sv.volGlideLeft > 0 && !sv.hasArmedNote) {
        const float share = frames >= sv.volGlideLeft ? 1.0f
                          : static_cast<float>(frames) / static_cast<float>(sv.volGlideLeft);
        sv.volGainTo    = sv.volGain + (noteVol - sv.volGain) * share;
        sv.volGlideLeft = frames >= sv.volGlideLeft ? 0 : sv.volGlideLeft - frames;
    }
    // TSF pans on its own channel. Only when modulated or gliding, so an unmodulated voice keeps
    // the pan the note or a PAN effect gave it.
    const bool panModded = fabsf(sv.params.mod[PARAM_PAN]) > 0.001f;
    const float modPan = panModded ? fmaxf(0.0f, fminf(1.0f, sv.params.get(PARAM_PAN))) : 0.0f;

    int volSlot = sv.sfSlot;
    const bool panGliding = voiceStepPanGlide(sv, frames);
    if ((panModded || panGliding) && volSlot >= 0 && volSlot < MAX_SOUNDFONTS) {
        tsf* h = soundfonts[volSlot].handle.load();
        if (h) tsf_channel_set_pan(h, t, panModded ? modPan : sv.panNow);
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

// One piece of one SoundFont voice — frames [from, to) of the block — rendered into sfBuf at the
// same frames, with the note's gain on it.
void AudioEngine::renderSoundfontPiece(SoundfontVoice& sv, int t, tsf* h, const BlockMix& b, int from, int to) {
    const int numFrames = b.numFrames;
    // The note starts at its own frame; what is before it belongs to what the track was playing.
    // An armed note's onset always lies in the voice's first piece: its table's clocks start there.
    int sfStart = from;
    if (sv.startDelayFrames > 0) {
        sfStart = std::max(from, std::min(sv.startDelayFrames, to));
        sv.startDelayFrames = 0;
    }
    if (!sv.hasArmedNote) {
        // A note-off dispatched mid-block is sent between two renders, so TSF's release begins on
        // its frame: TSF steps its envelope from a render call's first frame.
        const int offAt = sv.pendingTsfOffAt;
        if (offAt > from && offAt < to) {
            tsf_render_float_channel(h, t, sfBuf + from * 2, offAt - from, 0 /* overwrite */);
            tsf_channel_note_off(h, t, sv.pendingTsfOffNote);
            tsf_render_float_channel(h, t, sfBuf + offAt * 2, to - offAt, 0 /* overwrite */);
            sv.pendingTsfOffAt = -1;
        } else {
            // Due at or before this piece — or carried over at 0 from a block that did not
            // render this voice.
            if (offAt >= 0 && offAt <= from) {
                tsf_channel_note_off(h, t, sv.pendingTsfOffNote);
                sv.pendingTsfOffAt = -1;
            }
            tsf_render_float_channel(h, t, sfBuf + from * 2, to - from, 0 /* overwrite */);
        }
        applyGainRamp(sfBuf + from * 2, to - from, sv.volGainFrom, sv.volGainTo);
    } else {
        // ⚠️ A note that steals another is rendered in two passes. The first renders the replaced
        // note up to `fadeEnd`, past the new onset, and fades it there: the old TSF voices are cut
        // only once their last samples exist. The fade is ours, on the rendered samples — TSF
        // holds its envelope flat per 64 samples, so its own quick release is still a step.
        // `fadeEnd` is clamped to the piece, sliding the ramp earlier near its end.
        const int fadeEnd   = std::min(to, sfStart + DECLICK_SAMPLES);
        const int rampStart = std::max(from, fadeEnd - DECLICK_SAMPLES);
        const int rampLen   = fadeEnd - rampStart;
        tsf_render_float_channel(h, t, sfBuf + from * 2, fadeEnd - from, 0 /* overwrite */);
        // The old note keeps the gain it ended the last piece on — the new note's envelope has
        // already replaced the mods, and must not reach the note it cuts.
        applyGainRamp(sfBuf + from * 2, fadeEnd - from, sv.volGain, sv.volGain);
        for (int i = rampStart; i < fadeEnd; i++) {
            const float g = (float)(fadeEnd - i - 1) / (float)(rampLen > 1 ? rampLen - 1 : 1);
            sfBuf[i * 2]     *= g;
            sfBuf[i * 2 + 1] *= g;
        }
        // Now the old voices can be cut: the samples they contributed are already at zero.
        sv.fireArmedNote(h);
        // A VTR on this note's step: the old note keeps the fader it had, the new one starts on
        // the new value — so the fader goes on each half here, not on the sum in chainTrackPiece.
        const bool faderApart = sv.faderHeld && t < SF_VOICE_COUNT;
        const auto fader_at = [&](float s0, float s1, int i) {
            return s0 + (s1 - s0) * (float)(i + 1) / (float)numFrames;
        };
        if (faderApart) {
            for (int i = from; i < fadeEnd; i++) {
                const float g = fader_at(sv.faderHeldStart, sv.faderHeldEnd, i);
                sfBuf[i * 2] *= g; sfBuf[i * 2 + 1] *= g;
            }
        }
        // Rendered apart and ADDED — [sfStart, fadeEnd) still holds the tail of the fade,
        // and the two notes carry different gains across it.
        const int noteFrames = to - sfStart;
        if (noteFrames > 0) {
            tsf_render_float_channel(h, t, sfNoteBuf, noteFrames, 0 /* overwrite */);
            applyGainRamp(sfNoteBuf, noteFrames, sv.volGainFrom, sv.volGainTo);
            if (faderApart) {
                for (int i = 0; i < noteFrames; i++) {
                    const float g = fader_at(b.trackVolStart[t], b.trackVolEnd[t], sfStart + i);
                    sfNoteBuf[i * 2] *= g; sfNoteBuf[i * 2 + 1] *= g;
                }
            }
            for (int i = 0; i < noteFrames * 2; i++) sfBuf[sfStart * 2 + i] += sfNoteBuf[i];
        }
        if (faderApart) { sv.faderHeld = false; sv.faderInBuf = true; }
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

    // ⚠️ The dry gate, for soloing a send return: every send has already tapped above this
    // multiply, where eight track mutes would also silence what feeds the reverb.
    if (b.dryGateStart < 1.0f || b.dryGateEnd < 1.0f) {
        for (int i = 0; i < numFrames; i++) {
            const float lerp_t = (float)(i + 1) / (float)numFrames;
            const float g      = b.dryGateStart + (b.dryGateEnd - b.dryGateStart) * lerp_t;
            output[i * channelCount]     *= g;
            output[i * channelCount + 1] *= g;
        }
    }

    // Delay first, so its output can feed the reverb.
    delaySend.process(dlySendBufL, dlySendBufR, dlyWetL, dlyWetR, numFrames);
    const float dlyToRev = delayToReverbSend.load(std::memory_order_relaxed);
    if (dlyToRev > 0.0001f) {
        for (int i = 0; i < numFrames; i++) {
            revSendBufL[i] += dlyWetL[i] * dlyToRev;
            revSendBufR[i] += dlyWetR[i] * dlyToRev;
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
    const float rvReturn = reverbReturnGain.load(std::memory_order_relaxed);
    const float dlReturn = delayReturnGain.load(std::memory_order_relaxed);
    for (int i = 0; i < numFrames; i++) {
        // ⚠️ A return's mute rides below its module, so a muted reverb keeps building its tail.
        const float lerp_t = (float)(i + 1) / (float)numFrames;
        const float rvGate = b.revGateStart + (b.revGateEnd - b.revGateStart) * lerp_t;
        const float dlGate = b.dlyGateStart + (b.dlyGateEnd - b.dlyGateStart) * lerp_t;
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

    // The master fader: one ramped multiply over the summed bus, dry and returns. ⚠️ Here, not with
    // the per-voice gains — that would leave the returns playing over a silent mix — and before
    // masterChain, so the limiter sees a post-fader signal. The meters were filled pre-master and
    // are scaled by the block's end value, the fader on screen.
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

    // ⚠️ Chunk at PROCESS_SUBBLOCK — AUDIBLY different from a device-sized block (see the constant):
    // a device period (ALSA ~940 frames, Oboe 192-960) resolves note-ons too coarsely, and same-track
    // retriggers sharing a block exhaust the pool. renderOffline chunks the same, so live = export.
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
