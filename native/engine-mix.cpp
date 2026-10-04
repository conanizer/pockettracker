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

// Scale an interleaved stereo buffer by a gain moving linearly from `from` to `to`, reaching `to` on
// the last frame — the same per-sample ramp the mute gate uses.
static inline void applyGainRamp(float* buf, int frames, float from, float to) {
    for (int i = 0; i < frames; i++) {
        const float g = from + (to - from) * (float)(i + 1) / (float)frames;
        buf[i * 2]     *= g;
        buf[i * 2 + 1] *= g;
    }
}

// PSL / PBN / vibrato that ride on the note itself, set on the voice the note starts — every voice
// type, after its trigger has reset the pitch state. Both rates are already per FRAME: voice_derive.h
// scales the authored ticks and steps before the note is queued.
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
    // ⚠️ The handle is NOT tested here. It is a non-atomic pointer the UI thread can null at any
    // moment, and reading it without the slot mutex is a race whose answer may already be stale by
    // the next line. `armNote` reads it under the lock and says whether the note is worth setting up.
    SoundfontVoice& sv = sfVoices[t];
    // ⚠️ READ BEFORE `armNote`, which sets isActive unconditionally. This is the
    // question the chain setup below has to ask: is this channel's filter/EQ full of
    // a note that is still sounding? See InstrumentChain::reset's keepToneState.
    const bool wasSounding = sv.isActive;
    // This instrument's ADSR override (applied atomically inside fireArmedNote, before
    // note_on) — keyed by instrument id so de-duplicated handles stay isolated.
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
    // Per-track mono across voice types, this direction: an SF note replaces a sampler
    // note still sounding on this track with the fade a sampler note would give it.
    // The sampler trigger does the reverse. Only after armNote said yes — a dropped
    // SF note leaves the track as it was.
    for (int v = 0; v < MAX_VOICES; v++) {
        if (voices[v].trackId == t && voices[v].isActive && !voices[v].isFadingOut) {
            voices[v].startFadeOut();
        }
    }
    sv.isReleasingOnly = false;
    sv.resetPitchState();
    sv.detuneSemitones = note.detuneSemitones;  // static instrument detune (set after reset)
    sv.startDelayFrames = frame;  // start rendering at the note's exact intra-block frame
    sv.instrId = note.sampleId;

    // A TIC in the table's last row overrides the instrument tic rate —
    // one rate per FX column.
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
    sv.chain.filter.snapshotCoeffs(); // seed prev = target so first block doesn't interpolate from reset defaults
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
    // What the setters and the per-block recompute read, seeded from the instrument as
    // the sampler's trigger seeds them.
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
    // TIC00 support: continue the table where this track's previous note left off — per
    // COLUMN, since a table can be at TIC00 in FX2 and free-running in FX1. −1 = this column
    // has nothing to carry and starts wherever the trigger says.
    int savedTableRows[TABLE_LANES] = {-1, -1, -1};
    bool wasTIC00Mode = false;
    // ⚠️ …and it must be THIS table. A chain hands the note on, so the voice still sounding on
    // this track can be running a different table entirely, and its row means nothing here.
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
    // No voice left to read the row off (the previous one-shot ran out): the track's cursor still
    // holds it, or the table would restart at row 0 and how far it got would depend on the root note.
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

    // ---------------------------------------------------------------
    // VOICE ALLOCATION — mono per track + 4-step slot choice. "Steal old + allocate new" takes two
    // slots per track, which a phrase boundary across many tracks would exhaust.
    // Step 1 — fade any playing same-track voice (mono per track).
    // Step 2 — prefer a FREE slot, so the faded voice's declick tail plays out.
    // Step 3 — no free slot: recycle a same-track fading voice (trackId survives startFadeOut for this).
    // Step 4 — last resort: preempt any fading voice (other track) — a ~1 ms click beats silence.
    // ---------------------------------------------------------------

    // A note with no sample behind it must not touch the track: fading the playing voice for
    // a note that cannot sound would silence the track for nothing (a preview of an empty slot).
    const bool haveSample = note.sampleId >= 0 && note.sampleId < 256 && samples[note.sampleId];

    // Step 1: mono per track — fade whatever is still playing on this track
    for (int v = 0; haveSample && v < MAX_VOICES; v++) {
        if (voices[v].trackId == note.trackId && voices[v].isActive && !voices[v].isFadingOut) {
            voices[v].startFadeOut(DECLICK_SAMPLES, frame);   // …from the new note's own frame
        }
    }

    // Step 2: free slot
    int targetSlot = -1;
    for (int v = 0; v < MAX_VOICES; v++) {
        if (!voices[v].isActive) {
            targetSlot = v;
            break;
        }
    }

    // Step 3: pool full — recycle a same-track fading voice (cuts its tail)
    if (targetSlot == -1) {
        for (int v = 0; v < MAX_VOICES; v++) {
            if (voices[v].trackId == note.trackId && voices[v].isFadingOut) {
                targetSlot = v;
                break;
            }
        }
    }

    // Step 4: preempt any fading voice (last resort)
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
            // Per-track mono across voice types: a sampler note replaces an SF note
            // still sounding on this track. noteOff (not hardStop) so the SF release
            // plays out musically — findActiveVoiceForTrack skips releasing SF voices,
            // so mid-note params already target the new sampler voice meanwhile.
            if (note.trackId >= 0 && note.trackId < SF_VOICE_COUNT &&
                sfVoices[note.trackId].isActive && !sfVoices[note.trackId].isReleasingOnly) {
                sfVoices[note.trackId].noteOffAt(frame);
            }
            float rate = note.frequency / note.baseFrequency;

            // A TIC in the table's last row overrides the instrument tic rate —
            // one rate per FX column.
            int effectiveTicRates[TABLE_LANES];
            effectiveTicRatesFor(note.tableId, note.tableTicRate, effectiveTicRates);

            // A THO-with-note start row places every column; otherwise only a column that is
            // BOTH at TIC00 and had something to carry resumes, and the rest begin at row 0.
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
            voices[v].startDelayFrames = frame;  // start mixing at the note's exact intra-block frame

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

// One piece — frames [from, to) — of a track's rendered stereo buffer, from after the note's own gain:
// the track fader, then the voice's chain. The fader ramps across the whole block, the chain's filter
// across the piece. A voice type that renders a buffer per track calls this per piece and
// mixTrackBuffer once per block. (Audio thread only.)
template <typename V>
void AudioEngine::chainTrackPiece(V& v, int t, float* buf, const BlockMix& c, int from, int to) {
    // ⚠️ THE TRACK FADER IS APPLIED TO THE RENDERED SAMPLES, NOT TO THE TSF CHANNEL. A channel
    // volume is one value per render call, so the fader could only step at a render edge — the
    // staircase a knob turns into a tick per message. Here it is a ramp, like the note's own
    // gain before it. It stays ABOVE the chain, so an instrument's drive and filter hear the
    // faded signal.
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
        float lerp_t = (frames > 1) ? (float)(i - from + 1) / (float)frames : 1.0f;
        v.chain.filter.setInterpolatedCoeffs(lerp_t);
        v.chain.processStereo(buf[i * 2], buf[i * 2 + 1]);
    }
}

// One track's chained stereo buffer onto the bus: the mute gate and the transport-stop ramp, the send
// tap, the output, the meters and scopes — once per block, after chainTrackPiece has covered it. The
// SoundFont path's tail — a voice type that renders a buffer per track calls it too, rather than a
// copy that drifts. Returns the buffer's peak; `stopFadeDone` is set when the stop ramp reached zero
// inside this block. (Audio thread only.)
template <typename V>
float AudioEngine::mixTrackBuffer(V& v, int t, float* buf, const BlockMix& c, bool& stopFadeDone) {
    // ⚠️ THE MUTE GATE IS APPLIED HERE, and it has to be ABOVE the send tap below: the fader is
    // already in the buffer (chainTrackPiece), which makes a SoundFont send post-fader where the
    // sampler's is pre-fader, and a muted SF track has always taken its reverb and delay down with it.
    // ⚠️ THE TRANSPORT-STOP RAMP RIDES HERE, on the gate and for the gate's own reason: it has
    // to be per sample (a per-block value is the staircase both ramps exist to remove), it has
    // to sit BELOW the filter so a stop cannot slam the chain under a note still ringing
    // through it, and it has to be ABOVE the send tap so the reverb and delay are fed the
    // faded signal rather than a waveform cut off mid-cycle.
    for (int i = 0; i < c.numFrames; i++) {
        float lerp_t = (c.numFrames > 1) ? (float)(i + 1) / (float)c.numFrames : 1.0f;
        float L = buf[i * 2];
        float R = buf[i * 2 + 1];
        float gate = c.gateStart[t] + (c.gateEnd[t] - c.gateStart[t]) * lerp_t;
        if (v.stopFadeRemaining > 0) {
            if (i >= v.stopFadeStartFrame) {   // a ramp dispatched mid-block waits for its frame
                gate *= (float)v.stopFadeRemaining / (float)v.stopFadeTotal;
                if (--v.stopFadeRemaining <= 0) stopFadeDone = true;
            }
        } else if (stopFadeDone) {
            gate = 0.0f;   // the ramp ended inside this block; the rest of it is silence
        }
        buf[i * 2]     = L * gate;
        buf[i * 2 + 1] = R * gate;
    }

    // SEND TAP: the post-chain buffer into the reverb/delay buses
    if ((stemsMode == 0 || stemsMode >= 9) && (v.reverbSend > 0.0f || v.delaySend > 0.0f)) {
        for (int i = 0; i < c.numFrames; i++) {
            revSendBufL[i] += buf[i * 2]     * v.reverbSend;
            revSendBufR[i] += buf[i * 2 + 1] * v.reverbSend;
            dlySendBufL[i] += buf[i * 2]     * v.delaySend;
            dlySendBufR[i] += buf[i * 2 + 1] * v.delaySend;
        }
    }

    float trackPeakL = 0.0f, trackPeakR = 0.0f;
    if (c.octaWanted) trackWasActive[t] = true;  // OCTA capture only
    for (int i = 0; i < c.numFrames; i++) {
        float outL = buf[i * 2];
        float outR = buf[i * 2 + 1];
        // Pre-master, like the sampler path's sampleL/sampleR — the master fader is applied to the
        // summed bus in mixMaster, which also scales the meters and OCTA accumulators by it.
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

// ALL audio DSP lives here. processLiveBlock (live, via the platform backend's callback) and
// renderOffline (WAV export) are thin wrappers.
// Rule: NEVER add audio processing logic directly to processLiveBlock or renderOffline.
//
// The stages run in this order and each reads what the ones above it left; what they share is in
// `BlockMix`. ⚠️ The order is the behaviour — a stage moved is a different mix.
void AudioEngine::processAudioBlock(float* output, int numFrames, int channelCount, float sampleRate) {
    // The engine mixes STEREO ONLY: the SF render pass and the send/master chains index
    // [i*2] directly (only the sampler loop honours channelCount). Backends must open
    // stereo streams (the Oboe builder requests it; renderOffline is fixed at 2). Guard —
    // one silent block — rather than write past a mono buffer if a future backend drifts.
    if (channelCount != 2) return;
    // And the same guard for the block SIZE: every per-block member below (send buses, OCTA
    // accumulators, sfBuf) is a fixed PROCESS_SUBBLOCK array. Both wrappers chunk at that size; a
    // backend calling this directly must too. ⚠️ A larger block would also resolve events too coarsely.
    if (numFrames > PROCESS_SUBBLOCK) return;

    // What waitForAudioBlockBoundary() watches: while this is set, a SoundFont handle this block
    // loaded may still be in use. seq_cst on both sides — see the wait. On the way out, whichever
    // return is taken, the block also publishes what the screen reads of the voices.
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

// The mixer's faders and mute gates, ONCE per block: each one's value at the block's first and last
// frame, which the mix stages interpolate per sample. The targets are atomics the setters write from
// the UI thread; one block of a stale value is inaudible, and the audio thread never waits on a fader.
void AudioEngine::walkMixerRamps(BlockMix& b) {
    // ⚠️ READ BEFORE THE WALK because the mute gate needs it: an export must not FADE a muted track
    // out over its first 5.8 ms, it must start silent. Also used by the visualizer gates.
    b.offlineRender = isOfflineRendering.load(std::memory_order_relaxed);
    const bool offlineRender = b.offlineRender;

    // ⚠️ A FADER IS A PAIR, NOT A NUMBER, for the same reason the gate is: its value at the block's
    // first frame and at its last, interpolated per sample by both mix paths. A fader that jumps at a
    // block edge steps the waveform, and a knob sending 0-127 steps it ~0.8% per message — a tick per
    // message, thirty times a second. ⚠️ SNAPPED under an export, like the gate: a render must be the
    // same samples every time, and nothing in one is a gesture.
    // ⚠️ THE MUTE IS NOT FOLDED INTO THE FADER: it is a RAMP, carried as its value at the block's first
    // and last frame and interpolated per sample, as pan and the mod routes are.
    // ⚠️ TWO READ SITES, AND BOTH ARE OBLIGATORY: the sampler's per-sample gain and the SoundFont
    // buffer's post-chain multiply — the only places a track's audio exists alone. The SF path cannot
    // use `tsf_channel_set_volume`: it is once per block, the staircase this removes.

    // How far a fader may move in this block — per full swing, so the glide is the same wall-clock
    // length whatever the block size. Also read by the VTR/VMV arms in applyParamUpdate.
    b.faderStep = (float)b.numFrames / (float)FADER_GLIDE_SAMPLES;
    const float faderStep = b.faderStep;
    {
        // Per full swing, so the ramp is the same wall-clock length whatever the block size.
        const float gateStep = (float)b.numFrames / (float)MUTE_GATE_SAMPLES;
        // One walk for every gate in the mixer, so a return cannot end up ramping differently from a
        // track. `gate` is the member that remembers where it got to; start/end bracket THIS block.
        const auto walk_gate = [&](float& gate, bool muted, float& start, float& end) {
            const float target = muted ? 0.0f : 1.0f;
            if (offlineRender) gate = target;   // a mute is a STATE in an export, not a gesture
            start = gate;
            if      (gate < target) gate = fminf(target, gate + gateStep);
            else if (gate > target) gate = fmaxf(target, gate - gateStep);
            end = gate;
        };
        // The fader's walk: toward its target at FADER_GLIDE_SAMPLES per full swing, over as many
        // blocks as that takes.
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
    // The preview lane borrows the fader of the channel the audition came from — the lane is a ninth
    // voice with no fader of its own, and an instrument you can only hear at full dry level tells you
    // nothing about how it sits in the mix. -1 (no origin) keeps the neutral gain it has always had.
    //
    // ⚠️ THE ASSIGNMENT SITS AFTER THE WALK, not in the setter: reading the fader here is what makes
    // it the LIVE one, so a VTR or a mixer move lands in the audition it is aimed at.
    const int previewTrack = b.previewTrack;
    b.previewBorrows = (previewTrack >= 0 && previewTrack < 8);
    b.trackVolStart[PREVIEW_LANE] = b.previewBorrows ? b.trackVolStart[previewTrack] : 1.0f;
    b.trackVolEnd[PREVIEW_LANE]   = b.previewBorrows ? b.trackVolEnd[previewTrack]   : 1.0f;
    // …and the gate comes with it, so an audition off a muted channel stays silent.
    b.gateStart[PREVIEW_LANE]        = b.previewBorrows ? b.gateStart[previewTrack] : 1.0f;
    b.gateEnd[PREVIEW_LANE]          = b.previewBorrows ? b.gateEnd[previewTrack]   : 1.0f;
}

// The meters, the send buses and the visualizer accumulators, zeroed for this block. Only the
// [0,numFrames) slice is touched, and the visualizers are skipped when nobody is watching
// (CAPTURE_IDLE_MS) or during an offline export, where the scopes sit flat.
void AudioEngine::clearBlockScratch(BlockMix& b) {
    for (int t = 0; t < 8; t++) { framePeaksPerTrackL[t] = 0.0f; framePeaksPerTrackR[t] = 0.0f; }
    frameSendPeakRevL = frameSendPeakRevR = frameSendPeakDelL = frameSendPeakDelR = 0.0f;

    const int64_t nowMsec       = nowMs();
    b.octaWanted     = !b.offlineRender && (nowMsec - lastTrackWaveformReadMs.load(std::memory_order_relaxed)) < CAPTURE_IDLE_MS;
    b.spectrumWanted = !b.offlineRender && (nowMsec - lastSpectrumReadMs.load(std::memory_order_relaxed))      < CAPTURE_IDLE_MS;
    const size_t frameBytes     = (size_t)b.numFrames * sizeof(float);

    // Per-block scratch lives on the engine object, not the audio-thread stack (see the header).
    memset(revSendBufL, 0, frameBytes); memset(revSendBufR, 0, frameBytes);
    memset(dlySendBufL, 0, frameBytes); memset(dlySendBufR, 0, frameBytes);

    // The OCTA accumulator pair (64 KB+) is touched only when OCTA is shown. trackWasActive is reset
    // every block and read under octaWanted.
    memset(trackWasActive, 0, sizeof(trackWasActive));
    if (b.octaWanted) {
        for (int t = 0; t < TRACK_WAVEFORM_COUNT; t++) {
            memset(trackWaveAccumL[t], 0, frameBytes);
            memset(trackWaveAccumR[t], 0, frameBytes);
        }
    }

    // Per-instrument spectrum accumulator (mono sum of one instrument's voices) — only when the
    // EQ screen is monitoring an instrument.
    b.monitoredInstrId = instrSpectrumInstrId.load(std::memory_order_relaxed);
    if (b.monitoredInstrId >= 0) memset(instrSpectrumTempL, 0, frameBytes);
}

// Everything the other threads handed over since the last block: the three event queues (drained
// ONCE, one lock each, into reusable batches that dispatchEvents walks with zero locking), the
// transport's stop and restart requests, a live key's bytes, and the staged instrument and bus data.
void AudioEngine::takeQueuedWork(BlockMix& b) {
    noteBatch.clear(); killBatch.clear(); paramBatch.clear();
    // Snapshot the frame counter once (it's atomic; this thread is the only writer, so a single
    // relaxed load is enough and avoids re-loading it per frame).
    b.startFrame = globalFrameCounter.load(std::memory_order_relaxed);
    const int64_t blockStartFrame = b.startFrame;
    const int64_t blockEnd = blockStartFrame + b.numFrames - 1;

    // A SoundFont freed since a voice was armed: the voice lets go before anything this block can
    // reach the slot, which may by now hold a different font.
    for (int t = 0; t < SF_VOICE_COUNT; t++) {
        SoundfontVoice& sv = sfVoices[t];
        if (sv.sfSlot >= 0 && sv.sfGen != soundfonts[sv.sfSlot].gen.load()) sv.detach();
    }

    // ⚠️ THE TRANSPORT-STOP RAMP HAS A DEADLINE, AND THE POOL IS ONLY EIGHT SLOTS. A voice whose
    // playhead ran off its sample mixes one frame per block, so its fade would hold the slot ~256
    // blocks — enough held slots push the next take's allocator into steps 3/4, which click. By
    // this frame the ramp is over, so ending the FADING voices is silent; new notes are untouched.
    if (stopRampRequested.exchange(false, std::memory_order_acquire)) startStopRamp(blockStartFrame);
    // Before the drain below, so the take's first note already meets the fresh state.
    if (ottRestartRequested.exchange(false, std::memory_order_acquire)) masterChain.ott.restart();
    {
        const int64_t rampEnd = stopRampEndFrame.load(std::memory_order_relaxed);
        if (rampEnd >= 0 && blockStartFrame >= rampEnd) {
            stopRampEndFrame.store(-1, std::memory_order_relaxed);
            for (int i = 0; i < MAX_VOICES; i++)
                if (voices[i].isFadingOut) voices[i].stop();
        }
    }
    // A live key's bytes are turned into records HERE, stamped at this block's first frame, so the
    // drain just below picks them up and the key sounds in the block its bytes arrived in. Never
    // during an export: a render is the same samples every time, and a key is not part of the song.
    if (!b.offlineRender) {
        if (LiveInputSource* live = liveInput.load(std::memory_order_acquire)) live->drainLiveInput(blockStartFrame);
    }
    paramUpdateQueue.drainUntil(blockEnd, paramBatch);
    killQueue.drainUntil(blockEnd, killBatch);
    noteQueue.drainUntil(blockEnd, noteBatch);
    // What the control thread set is taken in AFTER the drain: an edit it made before queuing an event
    // (a knob, then the reload of the instrument) is then always in force when that event runs. And
    // before the dispatch, so a table's EQM or TIM this block lands on top of the song's setting.
    // ⚠️ The instrument data FIRST: the bus EQs are applied from the EQ presets it carries.
    syncInstrumentData();
    applyBusSettings();
}

// The drained batches, each record at its exact frame: params, then kills, then notes.
void AudioEngine::dispatchEvents(BlockMix& b) {
    size_t paramIdx = 0, killIdx = 0, noteIdx = 0;

    // ⚠️ THE THREE BATCHES ARE ONE TIMELINE, NOT THREE. Each loop takes everything due, which orders
    // them correctly only while the block keeps up. LATE (the first step of every take — a play
    // stamped at the last COMPLETED sub-block), a note at F and its params at F+1 fall due on the same
    // frame index, and params-first would DROP every per-voice param riding one frame behind its note
    // (`voiceFxFrame`, scheduler.h). So each queue yields to the earlier of the ones after it; `<=`
    // keeps the written order (params, kills, notes) at EQUAL frames, which `voiceFxFrame`'s +1 and a
    // K00 on its step's frame rely on. A held-back record runs on the next frame index.
    const auto dueKill = [&] { return killIdx < killBatch.size() ? killBatch[killIdx].targetFrame : INT64_MAX; };
    const auto dueNote = [&] { return noteIdx < noteBatch.size() ? noteBatch[noteIdx].targetFrame : INT64_MAX; };

    for (int32_t frame = 0; frame < b.numFrames; frame++) {
        int64_t currentFrame = b.startFrame + frame;

        // Apply scheduled parameter updates at their exact frame. Running here (on the audio
        // thread) is what makes live PBN/PVB/PVX/THO race-free: the look-ahead scheduler only
        // enqueues; the voices[] mutation happens here, where the mix is the sole writer.
        while (paramIdx < paramBatch.size() && paramBatch[paramIdx].targetFrame <= currentFrame &&
               paramBatch[paramIdx].targetFrame <= std::min(dueKill(), dueNote())) {
            ScheduledParamUpdate upd = paramBatch[paramIdx++];
            applyParamUpdate(upd, b);
        }

        // Process all scheduled kill events for this exact frame (BEFORE notes, and after any note
        // due EARLIER — see the timeline note above)
        while (killIdx < killBatch.size() && killBatch[killIdx].targetFrame <= currentFrame &&
               killBatch[killIdx].targetFrame <= dueNote()) {
            ScheduledKill kill = killBatch[killIdx++];
            applyKill(kill, frame);
        }

        // Trigger all notes scheduled for this exact frame
        while (noteIdx < noteBatch.size() && noteBatch[noteIdx].targetFrame <= currentFrame) {
            ScheduledNote note = noteBatch[noteIdx++];

            // ⚠️ **THE INSTRUMENT IS CHOSEN HERE, NOT WHERE THE NOTE WAS QUEUED.** A sequencer note
            // arrives as a number and becomes a sound on this line; everything below then runs on a
            // fully derived note and cannot tell the two paths apart.
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
            IAudioVoice* pv = findActiveVoiceForTrack(upd.trackId);
            if (pv) pv->setPitchBendRaw(upd.value);
            break;
        }
        case PARAM_UPDATE_VIBRATO: {              // PVB/PVX on empty step
            IAudioVoice* pv = findActiveVoiceForTrack(upd.trackId);
            if (pv) pv->setVibratoRaw(upd.value, upd.value2);
            break;
        }
        case PARAM_UPDATE_TABLE_ROW: {            // THO on empty step
            // ⚠️ ALL THREE COLUMNS. This THO is written in a PHRASE, not in the table, so it
            // belongs to no column — "put this track's table on row X" is the only thing it
            // can mean. A THO inside the table steers the column it is typed in; that one is
            // in processTableRow.
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
        // The two morph arms. Same targets as the two above, reached the same way — only the
        // source of the band values differs, so an EQN morph is subject to exactly the
        // per-voice limits an EQN is: it writes the SOUNDING voice and a note-on resets that
        // voice's EQ from the instrument, so the next tick (≤ 1/12 of a step) re-asserts it.
        case PARAM_UPDATE_EQ_BANDS: {             // EQN under AUS/AUF
            forEachTrackVoice(upd.trackId, [&](auto& v) { applyEqBandsToModule(v.chain.eq, upd.eqBands); });
            break;
        }
        case PARAM_UPDATE_MASTER_EQ_BANDS: {      // EQM under AUS/AUF (global)
            applyEqBandsToModule(masterChain.masterEq, upd.eqBands);
            break;
        }
        // ⚠️ BOTH FADER ARMS REDO THIS BLOCK'S WALK, from the block's start toward the new
        // value: the end is what the mix stages ramp to, the member is where the next
        // block carries on from. Write only the target and the glide starts a block late.
        case PARAM_UPDATE_TRACK_VOL: {            // VTR — this track's mixer fader
            if (upd.trackId >= 0 && upd.trackId < 8) {
                // The mute is a separate gate multiplied at the two read sites, so the fader
                // keeps moving under a muted track and unmuting lands wherever it has got to.
                const int t = upd.trackId;
                trackVolumes[t].store(upd.value, std::memory_order_relaxed);
                // ⚠️ ON A NOTE'S OWN STEP (it rides one frame behind the note) IT DOES NOT
                // GLIDE: the new note starts at its level, or its attack is heard sliding
                // from the last one's. What the note cut keeps the old fader while it fades.
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
        // An instrument was edited — a knob, a screen, a preset. Every voice still sounding
        // on it re-reads the parameters a trigger would have copied. ⚠️ BY INSTRUMENT, not by
        // track: the same instrument can be sounding on several at once.
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
        // A live key let go of. The three-way rule is in Voice::keyRelease; unlike a KIL,
        // a one-shot plays out.
        case KILL_KEY_OFF: forEachVoiceOnTrack(kill.trackId, [&](auto& v) { voiceKeyRelease(v, frame); }); break;
        // KIL's note-off: each voice type runs its own release, or fades where it has none.
        case KILL_SOFT:    forEachVoiceOnTrack(kill.trackId, [&](auto& v) { v.noteOffAt(frame); });       break;
        case KILL_CUT:     forEachVoiceOnTrack(kill.trackId, [&](auto& v) { voiceCut(v, frame); });        break;
        default:           forEachVoiceOnTrack(kill.trackId, [&](auto& v) { voiceKill(v, frame); });       break;
    }
}

// ⚠️⚠️ **EACH SAMPLER VOICE RUNS TO THE END OF THE BLOCK BEFORE THE NEXT STARTS, IN PIECES CUT WHERE
// ITS TABLE PLAYS A ROW.** The voice renders up to the row's frame, the row is applied, the voice's
// modulation is derived again and it renders on — so a row starts on its own sample, not on a
// block edge. A voice with no row due this block is one piece, the whole block.
//
// The sample-edit lock is a try_lock so applyRateAndBits can swap buffers safely; while it is held
// the voices still advance and are not mixed — one block of silence instead of a crash.
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

// The ONE place a track's TIC00 cursor is written, below the row logic. Only the live (not fading)
// voice owns it, or the value would depend on slot order. Written when ANY column is at TIC00,
// storing all three; a retrigger re-checks each rate.
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

// A sampler voice's modulation for the next `frames` of the block, then its pan and filter.
void AudioEngine::prepareSamplerPiece(Voice& voice, int frames, float sampleRate) {
    updateVoicePitchMod(voice, frames, sampleRate);

    // Snapshot envValues before advancing so the mix loop can interpolate
    // per-sample (eliminates block-rate staircase artifacts on short envelopes).
    for (int m = 0; m < 4; m++) voice.voiceMods[m].prevEnvValue = voice.voiceMods[m].envValue;
    updateVoiceModulation(voice, frames, sampleRate);

    // PAN modulation: snapshot before update so the mix loop can interpolate per-sample
    voice.prevPanLeft  = voice.panLeft;
    voice.prevPanRight = voice.panRight;
    if (voiceStepPanGlide(voice, frames)) {
        const float a = voice.panNow * (float)M_PI * 0.5f;
        voice.panLeft  = cosf(a);
        voice.panRight = sinf(a);
    }
    if (fabsf(voice.params.mod[PARAM_PAN]) > 0.001f) {
        float modPan = fmaxf(0.0f, fminf(1.0f, voice.params.get(PARAM_PAN)));
        float panAngle = modPan * (float)M_PI * 0.5f;
        voice.panLeft  = cosf(panAngle);
        voice.panRight = sinf(panAngle);
    }

    // FILTER modulation: snapshot then recompute coefficients when LFO/ADSR drives CUT or RES
    voice.chain.filter.snapshotCoeffs();
    if (voice.chain.filter.enabled() &&
            (fabsf(voice.params.mod[PARAM_FILTER_CUT]) > 0.5f ||
             fabsf(voice.params.mod[PARAM_FILTER_RES]) > 0.5f)) {
        int modCut = std::max(0, std::min(255, (int)voice.params.get(PARAM_FILTER_CUT)));
        int modRes = std::max(0, std::min(255, (int)voice.params.get(PARAM_FILTER_RES)));
        voice.chain.filter.setParams(voice.chain.filter.type, modCut, modRes, voice.chain.filter.drive, sampleRate);
    }

    // Auto-stop looping voice when volume envelope completes
    // AHD/DRUM done at stage 4; ADSR/TRIG done at stage 5
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

    int effDrive     = std::max(0, std::min(255, (int)(voice.params.base[PARAM_DRIVE]      + voice.modDestValues[PARAM_DRIVE])));
    int effCrush      = std::max(0, std::min(15,  (int)(voice.params.base[PARAM_CRUSH]      + voice.modDestValues[PARAM_CRUSH])));
    int effDownsample = std::max(0, std::min(15,  (int)(voice.params.base[PARAM_DOWNSAMPLE] + voice.modDestValues[PARAM_DOWNSAMPLE])));
    voice.chain.drive.setDrive(effDrive);
    voice.chain.crush.setParams(effCrush, 0);   // sampler: downsample=0, pre-interp handles it
    if (effDownsample != voice.dsLast) {
        if (voice.chain.started) {   // a change on the sounding note — before it, just where it starts
            voice.dsPrev     = voice.dsLast;
            voice.dsFadeLeft = DOWNSAMPLE_FADE_FRAMES;
        }
        voice.dsLast = effDownsample;
    }
    {
        int sl = voice.sampleLength;
        // START/END are re-derived every block so a mod route can move them while the note rings.
        // ⚠️ An exact-frame window (note-queue.h) is carried on the voice instead, because the
        // 0-255 pair cannot express it — re-deriving would widen the editor's audition to the file.
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

        // ── LPO: slide the whole window, BOTH ends by the same amount ───────────────────────
        //
        // ⚠️⚠️ BOTH BOUNDS OR NOTHING: moving one changes the loop's LENGTH, which on a looped
        // note is the pitch.
        // ⭐ The offset derives from the RUNNING TOTAL of sixteenths, never accumulated samples,
        // so sixteen steps of `01` land exactly where one step of `10` does on any length. The
        // COUNT is clamped, so the window STOPS at the sample's ends and one step back moves it.
        // ⚠️⚠️ THE PLAYHEAD MOVES WITH THE WINDOW: it keeps its position within the loop and hears
        // the new material at once. Left behind, a forward step is heard a loop late and a
        // backward one snaps to the loop start with a click. `loopSlideFrames` is the offset LAST
        // APPLIED, so the shift is a difference of two derived values.
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
            // ⚠️ ONLY WHILE THE PLAYHEAD IS INSIDE THE LOOP. Before the first wrap it is still in
            // the intro (start → loop start), and after a note-off on an ADSR voice it is running
            // the release tail with the loop abandoned; neither is a position the window owns, and
            // dragging it would walk a released note backwards into the loop it just left.
            const int wasStart = voice.actualLoopStart + voice.loopSlideFrames;
            const int wasEnd   = voice.actualLoopEnd   + voice.loopSlideFrames;
            if (voice.position >= (double)wasStart && voice.position < (double)wasEnd)
                voice.position += (double)(off - voice.loopSlideFrames);
            voice.loopSlideFrames = off;
        }
        voice.actualLoopStart += off;
        voice.actualLoopEnd   += off;
    }

    // ⚠️ AFTER the loop bounds, not before: oscillator mode's rate is derived from the loop
    // LENGTH, so reading it above would run a block behind every LPO slide and every loop edit.
    float modulatedRate = getModulatedPlaybackRate(voice);

    // Honour the intra-block trigger offset: a note dispatched at blockStart+f must not sound
    // before frame f. The fade has the same offset (fadeStartFrame, held in the loop below);
    // params stay quantized to the piece — they are ramped across it, and the ramp is the
    // anti-click. An onset beyond this piece waits for the piece that contains it.
    int startFrame = from;
    if (voice.startDelayFrames > 0) {
        if (voice.startDelayFrames >= to) return;
        startFrame = std::max(from, voice.startDelayFrames);
        voice.startDelayFrames = 0;
    }

    // ⚠️ TWO RAMPS, TWO CLOCKS. The voice's own params (pan, VOL routes, envelopes, filter) were
    // derived for THIS piece and ramp across it; the mixer's fader and mute gate were derived
    // for the whole block and must ramp across the block, or a cut piece would step them.
    const int pieceFrames = to - from;
    for (int i = startFrame; i < to; i++) {
        int idx = (int)voice.position;
        // frac computed in double THEN narrowed: (float)idx is inexact past 2^24, which
        // would corrupt frac for exactly the long samples double position exists for.
        float frac = (float)(voice.position - (double)idx);

        // Bounds check - need idx+1 for interpolation
        if (idx < 0 || idx >= voice.sampleLength - 1) {
            if (idx < 0) {
                voice.isActive = false;  // negative position: safety hard-stop
            } else {
                // At or past last interpolation point: fade out so SVF resonance decays
                voice.position = (double)(voice.sampleLength - 2);
                voice.startFadeOut();  // no-op if already fading
            }
            break;
        }

        // STEP 4 scalars (shared by mono and stereo paths)
        float t = (pieceFrames > 1) ? (float)(i - from + 1) / (float)pieceFrames : 1.0f;
        const float tBlock = (numFrames > 1) ? (float)(i + 1) / (float)numFrames : 1.0f;
        float panL = voice.prevPanLeft  + (voice.panLeft  - voice.prevPanLeft)  * t;
        float panR = voice.prevPanRight + (voice.panRight - voice.prevPanRight) * t;
        float finalVol = voice.volume;
        for (int m = 0; m < 4; m++) {
            const VoiceModSlot& mod = voice.voiceMods[m];
            if (mod.type == 0 || mod.stage == 0) continue;
            if (mod.dest == 1) {
                if (mod.type == 3) {
                    float envAtI = mod.prevEnvValue + (mod.envValue - mod.prevEnvValue) * t;
                    finalVol = fmaxf(0.0f, finalVol * (1.0f + envAtI * mod.effectiveAmt));
                } else {
                    float envAtI = mod.prevEnvValue + (mod.envValue - mod.prevEnvValue) * t;
                    finalVol = fmaxf(0.0f, finalVol + (envAtI - 1.0f) * mod.effectiveAmt);
                }
            }
        }
        float volRoute = voice.prevModDestValues[PARAM_VOL]
                       + (voice.modDestValues[PARAM_VOL] - voice.prevModDestValues[PARAM_VOL]) * t;
        if (voice.volGlideLeft > 0) {   // a table VOL on the sounding note: blend in, never step
            const float k = static_cast<float>(voice.volGlideLeft) / VOL_GLIDE_FRAMES;
            volRoute = voice.volGlideFrom * k + volRoute * (1.0f - k);
            --voice.volGlideLeft;
        }
        voice.volRouteLast = volRoute;
        // ⚠️ SF_VOICE_COUNT, not 8: the preview lane (8) carries a real fader, as on the SF path.
        // ⚠️ THE FADER AND THE MUTE GATE BOTH RIDE HERE, interpolated across the whole block on
        // `tBlock`, never the piece's `t`.
        const bool onTrack = voice.trackId >= 0 && voice.trackId < SF_VOICE_COUNT;
        const float fStart = voice.faderHeld ? voice.faderHeldStart : onTrack ? b.trackVolStart[voice.trackId] : 1.0f;
        const float fEnd   = voice.faderHeld ? voice.faderHeldEnd   : onTrack ? b.trackVolEnd[voice.trackId]   : 1.0f;
        float trackVol = onTrack
                       ? (fStart + (fEnd - fStart) * tBlock)
                         * (b.gateStart[voice.trackId]
                            + (b.gateEnd[voice.trackId] - b.gateStart[voice.trackId]) * tBlock)
                       : 1.0f;
        float antiClick = voice.antiClickFade();

        // Sample fetch + per-voice chain is the only mono/stereo difference; a mono sample
        // feeds the same value to both lanes.
        float procL, procR;
        if (voice.sampleDataRight) {
            // ── STEREO FETCH ─────────────────────────────────────────────────
            float s1L, s2L, s1R, s2R;
            if (effDownsample > 0) {
                int factor = 1 << effDownsample;
                int qi = (idx / factor) * factor;
                s1L = s2L = voice.sampleData[qi];
                s1R = s2R = voice.sampleDataRight[qi];
            } else {
                s1L = voice.sampleData[idx];       s2L = voice.sampleData[idx + 1];
                s1R = voice.sampleDataRight[idx];  s2R = voice.sampleDataRight[idx + 1];
            }
            procL = s1L + (s2L - s1L) * frac;
            procR = s1R + (s2R - s1R) * frac;
            if (voice.dsFadeLeft > 0) {
                float oldL, oldR;
                fetchDownsampled(voice, idx, frac, voice.dsPrev, oldL, oldR);
                const float k = static_cast<float>(voice.dsFadeLeft--) / DOWNSAMPLE_FADE_FRAMES;
                procL = oldL * k + procL * (1.0f - k);
                procR = oldR * k + procR * (1.0f - k);
            }
            voice.chain.filter.setInterpolatedCoeffs(t);
            voice.chain.processStereo(procL, procR);
        } else {
            // ── MONO FETCH ───────────────────────────────────────────────────
            float sample1 = voice.sampleData[idx];
            float sample2 = voice.sampleData[idx + 1];
            if (effDownsample > 0) {
                int downsampleFactor = 1 << effDownsample;
                int quantizedIdx = (idx / downsampleFactor) * downsampleFactor;
                sample1 = voice.sampleData[quantizedIdx];
                sample2 = voice.sampleData[quantizedIdx];
            }
            float processedSample = sample1 + (sample2 - sample1) * frac;
            if (voice.dsFadeLeft > 0) {
                float oldL, oldR;
                fetchDownsampled(voice, idx, frac, voice.dsPrev, oldL, oldR);
                const float k = static_cast<float>(voice.dsFadeLeft--) / DOWNSAMPLE_FADE_FRAMES;
                processedSample = oldL * k + processedSample * (1.0f - k);
            }
            voice.chain.filter.setInterpolatedCoeffs(t);
            procL = procR = voice.chain.processMono(processedSample);
        }

        // ── SHARED TAIL: sends → global gain → fade-out → pan ────────────────
        //
        // The send tap sits ABOVE the track fader: sends are PRE-FADER for the mixer and post
        // everything on the instrument (VOL, the phrase V, VOL mods), so pulling a track down
        // leaves its tails at full level. ⚠️ The MASTER fader multiplies the summed bus in
        // mixMaster, after the returns, so it carries the tails with it.
        float scalar = finalVol * volRoute;
        procL *= scalar;
        procR *= scalar;

        // ⚠️⚠️ THE VOICE'S OWN FADES MUST REACH THE SENDS, AND THE TRACK FADER MUST NOT.
        // `antiClick` and the KIL/steal fade-out are the voice's envelope — a send that missed
        // them would get a waveform cut mid-cycle and ring the click on in the tails. `trackVol`
        // stays below the tap (pre-fader sends). The fade is resolved once here and applied to the
        // dry path in its original position below. A fade dispatched at frame f waits for f.
        const bool fading = voice.isFadingOut && i >= voice.fadeStartFrame;
        float voiceFade   = antiClick;
        float fo          = 1.0f;
        if (fading) {
            fo = (float)voice.fadeOutRemaining / (float)voice.fadeOutTotal;
            voiceFade *= fo;
            if (--voice.fadeOutRemaining <= 0) {
                voice.isFadingOut = false;
                voice.isActive = false;
            }
        }

        if ((stemsMode == 0 || stemsMode >= 9) && voice.reverbSend > 0.0f) {
            revSendBufL[i] += procL * voiceFade * panL * voice.reverbSend;
            revSendBufR[i] += procR * voiceFade * panR * voice.reverbSend;
        }
        if ((stemsMode == 0 || stemsMode >= 9) && voice.delaySend > 0.0f) {
            dlySendBufL[i] += procL * voiceFade * panL * voice.delaySend;
            dlySendBufR[i] += procR * voiceFade * panR * voice.delaySend;
        }

        float globalMul = trackVol * antiClick;
        procL *= globalMul;
        procR *= globalMul;

        if (fading) {
            procL *= fo;
            procR *= fo;
        }

        float sampleL = procL * panL;
        float sampleR = procR * panR;

        if (stemsMode == 0 || voice.trackId == stemsMode - 1) {
            output[i * channelCount] += sampleL;
            output[i * channelCount + 1] += sampleR;
        }

        if (!voice.isFadingOut && voice.trackId >= 0 && voice.trackId < 8) {
            framePeaksPerTrackL[voice.trackId] = peak_hold(framePeaksPerTrackL[voice.trackId], sampleL);
            framePeaksPerTrackR[voice.trackId] = peak_hold(framePeaksPerTrackR[voice.trackId], sampleR);
        }
        // OCTA per-track capture: tracks 0-7 plus the preview lane (PREVIEW_TRACK_ID == PREVIEW_LANE).
        // Gated on octaWanted: the accumulators are only zeroed/read when OCTA is shown.
        if (b.octaWanted && voice.trackId >= 0 && voice.trackId < TRACK_WAVEFORM_COUNT) {
            if (!voice.isFadingOut) trackWasActive[voice.trackId] = true;
            trackWaveAccumL[voice.trackId][i] += sampleL;
            trackWaveAccumR[voice.trackId][i] += sampleR;
        }
        if (b.monitoredInstrId >= 0 && voice.instrId == b.monitoredInstrId) {
            instrSpectrumTempL[i] += 0.5f * (sampleL + sampleR);
        }

        if (!voice.isActive) break;

        // Active looping is bounded by LOOP END (region [loopStart, loopEnd]). Once loopReleasing
        // is set (ADSR note-off on a looping voice) the loop is abandoned: every mode runs forward
        // to actualEnd so the [loopEnd, end] tail plays out under the release envelope, then fades.
        if (voice.loopMode == LOOP_MODE_PINGPONG && !voice.loopReleasing) {
            if (voice.loopingBack) {
                voice.position -= modulatedRate;
                if (voice.position <= voice.actualLoopStart) {
                    voice.loopingBack = false;
                    voice.position = (double)voice.actualLoopStart;
                }
            } else {
                voice.position += modulatedRate;
                if (voice.position >= voice.actualLoopEnd) {
                    voice.loopingBack = true;
                    voice.position = (double)voice.actualLoopEnd;
                }
            }
        } else if (voice.reverse && !voice.loopReleasing) {
            voice.position -= modulatedRate;
            if (voice.position <= voice.actualStart) {
                if (isForwardLoopMode(voice.loopMode)) {
                    voice.position = (double)voice.actualLoopStart;
                } else {
                    voice.position = (double)voice.actualStart;
                    voice.startFadeOut();
                    break;
                }
            }
        } else {
            voice.position += modulatedRate;
            bool activeForwardLoop = (isForwardLoopMode(voice.loopMode) && !voice.loopReleasing);
            double fwdBoundary = activeForwardLoop ? (double)voice.actualLoopEnd : (double)voice.actualEnd;
            if (voice.position >= fwdBoundary) {
                if (activeForwardLoop) {
                    voice.position = (double)voice.actualLoopStart;
                } else {
                    voice.position = (double)(voice.actualEnd - 1);
                    voice.startFadeOut();
                    break;
                }
            }
        }
    } // for (int i = startFrame; i < to; i++)
}

// ⚠️⚠️ **EACH SOUNDFONT VOICE IS RENDERED IN PIECES CUT WHERE ITS TABLE PLAYS A ROW**, as the sampler
// voices are: table, modulation, then TSF up to the next row. TSF takes pitch and pan per render
// call, so a row's transpose starts on its own frame. The instrument chain runs per piece, its filter
// ramping across it; the gate, the sends and the output run once across the block.
void AudioEngine::mixSoundfontVoices(BlockMix& b) {
    const int numFrames = b.numFrames;
    // sfBuf (per-track SF render, PROCESS_SUBBLOCK frames * 2 channels) is an engine member; it is
    // memset per use below before each tsf render.
    for (int t = 0; t < SF_VOICE_COUNT; t++) {
        SoundfontVoice& sv = sfVoices[t];
        if (!sv.isActive) continue;

        const int slot = sv.sfSlot;
        tsf* h = (slot >= 0 && slot < MAX_SOUNDFONTS) ? soundfonts[slot].handle.load()  // valid until
                                                      : nullptr;                          // this block ends
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

        // Release tail: when noteOff() was called, keep rendering until TSF goes silent.
        // Suppressed while an ADSR/TRIG VOL release is active (stage 4) — TSF is still
        // generating audio for the fade; the render loop in pass 1 calls hardStop() when
        // the ADSR mod reaches stage 5.
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

        // The ramp reached zero inside this block, and its last faded samples are already summed
        // into the bus above. Ending the voice here — not where the button was pressed — is the
        // whole difference between a stop that ramps and a stop that cuts.
        if (stopFadeDone) sv.hardStop();
    }
}

// A SoundFont voice's modulation for the next `frames` of the block, from frame `from`: the note's
// gain, pan, filter, drive, crush and the pitch wheel. Everything here reaches TSF or the chain
// once per call, so it holds across the piece the caller renders next.
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
    // PAN modulation. A SoundFont voice has no panLeft/panRight gains in the mix loop — TSF
    // pans on its own channel — so the modulated value goes back through
    // tsf_channel_set_pan instead, guarded by the same |mod| > 0.001 test the sampler path
    // uses so an unmodulated voice keeps whatever pan the note or a PAN effect gave it.
    const bool panModded = fabsf(sv.params.mod[PARAM_PAN]) > 0.001f;
    const float modPan = panModded ? fmaxf(0.0f, fminf(1.0f, sv.params.get(PARAM_PAN))) : 0.0f;

    int volSlot = sv.sfSlot;
    const bool panGliding = voiceStepPanGlide(sv, frames);
    if ((panModded || panGliding) && volSlot >= 0 && volSlot < MAX_SOUNDFONTS) {
        tsf* h = soundfonts[volSlot].handle.load();
        if (h) tsf_channel_set_pan(h, t, panModded ? modPan : sv.panNow);
    }

    // Every VOL envelope has finished: the note is over, or — with AMT below FF — held at a
    // level it can no longer leave. Either way it ends, faded from where this piece's ramp
    // leaves it, so a partial AMT cannot end in a step. (An armed note's envelopes have
    // only just started, so this never ends a note before it is heard.)
    if (hasVolEnv && volEnvDone && !sv.hasArmedNote) sv.startStopFade(DECLICK_SAMPLES, from);

    // If filter mod is active, snapshot then recompute coefficients via InstrumentChain.
    sv.chain.filter.snapshotCoeffs();
    if (sv.chain.filter.enabled()) {
        const int baseCut = (int)sv.params.base[PARAM_FILTER_CUT];
        const int baseRes = (int)sv.params.base[PARAM_FILTER_RES];
        int modCut = std::max(0, std::min(255,
            (int)(sv.params.base[PARAM_FILTER_CUT] + sv.modDestValues[PARAM_FILTER_CUT])));
        int modRes = std::max(0, std::min(255,
            (int)(sv.params.base[PARAM_FILTER_RES] + sv.modDestValues[PARAM_FILTER_RES])));
        if (modCut != baseCut || modRes != baseRes) {
            sv.chain.filter.setParams(sv.chain.filter.type, modCut, modRes, sv.chain.filter.drive, sampleRate);
        }
    }
    // Drive and crush off the bus, as the sampler's mix does — except that the SF chain does
    // its own downsampling: there is no read address here to quantize.
    sv.chain.drive.setDrive(std::max(0, std::min(255,
        (int)(sv.params.base[PARAM_DRIVE] + sv.modDestValues[PARAM_DRIVE]))));
    sv.chain.crush.setParams(
        std::max(0, std::min(15, (int)(sv.params.base[PARAM_CRUSH]      + sv.modDestValues[PARAM_CRUSH]))),
        std::max(0, std::min(15, (int)(sv.params.base[PARAM_DOWNSAMPLE] + sv.modDestValues[PARAM_DOWNSAMPLE]))));

    sv.applyPitchMod((float)sampleRate, frames);
}

// One piece of one SoundFont voice — frames [from, to) of the block — rendered into sfBuf at the
// same frames, with the note's gain on it.
void AudioEngine::renderSoundfontPiece(SoundfontVoice& sv, int t, tsf* h, const BlockMix& b, int from, int to) {
    const int numFrames = b.numFrames;
    // Honour the intra-block trigger offset (see the sampler mix loop): the note starts at its
    // exact frame, and everything before it belongs to whatever this track was already playing.
    // An armed note's onset always lies in the voice's first piece: its table's clocks start there.
    int sfStart = from;
    if (sv.startDelayFrames > 0) {
        sfStart = std::max(from, std::min(sv.startDelayFrames, to));
        sv.startDelayFrames = 0;
    }
    if (!sv.hasArmedNote) {
        // A note-off dispatched mid-block is sent between two renders, so TSF's release begins on
        // its frame. TSF steps its envelope per render call (in 64-sample chunks from the call's
        // first frame), which is what makes the split land it exactly.
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
        // ⚠️⚠️ A NOTE THAT STEALS ANOTHER IS RENDERED IN TWO PASSES WITH A FADE BETWEEN THEM.
        // Pass one renders the REPLACED note up to `fadeEnd`, past the new onset, so it fades
        // rather than being cut at a block edge — the old TSF voices are killed only once their
        // last samples exist (that is what the armed note is for).
        // ⚠️ THE FADE IS OURS, NOT TSF'S: TSF holds its envelope flat per 64-sample block, so its
        // quick release is a smaller step, not none. A ramp on the rendered samples has no such
        // granularity (DECLICK_SAMPLES, as the sampler's steals). `fadeEnd` is clamped to the
        // piece, sliding the ramp earlier near its end; it is never a stub.
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

// Per-track waveform capture for OCTA visualizer — only when OCTA is being displayed.
// try_lock: the UI holds this mutex while it copies or decays the scopes, and a block that
// finds it held skips the capture rather than wait. A scope missing one block is invisible.
void AudioEngine::captureTrackScopes(const BlockMix& b) {
    if (!b.octaWanted) return;
    std::unique_lock<std::mutex> lock(waveformMutex, std::try_to_lock);
    if (!lock.owns_lock()) return;
    for (int t = 0; t < TRACK_WAVEFORM_COUNT; t++) trackHasVoice[t] = trackWasActive[t];
    for (int i = 0; i < b.numFrames; i++) {
        for (int t = 0; t < TRACK_WAVEFORM_COUNT; t++) {
            // × masterVolEnd because the accumulators are filled pre-master: OCTA shows
            // what leaves the master fader, so a master fade takes the scopes down with it.
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

    // ⚠️ It must live here: soloing a send return means "hear only what comes back from the reverb",
    // and the reverb is fed by the tracks. Eight track mutes would stop the notes (schedulers skip
    // inaudible tracks) and cut the SF sends; every send has already tapped above this multiply.
    if (b.dryGateStart < 1.0f || b.dryGateEnd < 1.0f) {
        for (int i = 0; i < numFrames; i++) {
            const float lerp_t = (numFrames > 1) ? (float)(i + 1) / (float)numFrames : 1.0f;
            const float g      = b.dryGateStart + (b.dryGateEnd - b.dryGateStart) * lerp_t;
            output[i * channelCount]     *= g;
            output[i * channelCount + 1] *= g;
        }
    }

    // SEND BUSES: delay first so its output can feed into reverb, then reverb.
    // revWet*/dlyWet* are engine members; process() fully overwrites them.
    delaySend.process(dlySendBufL, dlySendBufR, dlyWetL, dlyWetR, numFrames);
    const float dlyToRev = delayToReverbSend.load(std::memory_order_relaxed);
    if (dlyToRev > 0.0001f) {
        for (int i = 0; i < numFrames; i++) {
            revSendBufL[i] += dlyWetL[i] * dlyToRev;
            revSendBufR[i] += dlyWetR[i] * dlyToRev;
        }
    }
    reverbSend.process(revSendBufL, revSendBufR, revWetL, revWetR, numFrames);
    // Only capture when the EQ/spectrum UI is actually polling, and never block the audio
    // thread on the UI's read — try_lock and drop this block's data on contention (invisible).
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
        // ⚠️ The return's own mute rides HERE, below the module, so a muted reverb keeps building
        // its tail while it is silent — unmuting drops you back into the tail the song has been
        // feeding it, not into a reverb that starts from nothing.
        const float lerp_t = (numFrames > 1) ? (float)(i + 1) / (float)numFrames : 1.0f;
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

    // ─── THE MASTER FADER — one multiply over the summed bus, dry AND returns ────────────────────
    //
    // ⚠️ HERE, not with the per-voice gains: a fader that scaled only the dry path would leave the
    // returns playing over a silent mix. Before masterChain, so the limiter sees a post-fader signal.
    // ⚠️ IT RAMPS ACROSS THE BLOCK, like every fader here. The meters and visualiser accumulators
    // were filled pre-master and are scaled by the block's END value, the fader on screen. The
    // `!= 1.0f` skip is only an optimisation — × 1.0f is the identity.
    if (b.masterVolStart != 1.0f || b.masterVolEnd != 1.0f) {
        for (int i = 0; i < numFrames; i++) {
            const float g = b.masterVolStart
                          + (b.masterVolEnd - b.masterVolStart) * (float)(i + 1) / (float)numFrames;
            for (int c = 0; c < channelCount; c++) output[i * channelCount + c] *= g;
        }
        for (int t = 0; t < 8; t++) {
            framePeaksPerTrackL[t] *= b.masterVolEnd;
            framePeaksPerTrackR[t] *= b.masterVolEnd;
        }
        frameSendPeakRevL *= b.masterVolEnd;
        frameSendPeakRevR *= b.masterVolEnd;
        frameSendPeakDelL *= b.masterVolEnd;
        frameSendPeakDelR *= b.masterVolEnd;
    }

    // Master chain: master EQ → bus FX (OTT or DUST) → limiter
    // Stems mode bypasses EQ and bus FX; only limiter is applied.
    if (stemsMode == 0)
        masterChain.process(output, numFrames, channelCount);
    else
        masterChain.limiter.process(output, numFrames, channelCount);

    // ⚠️ AFTER the master chain, and that placement is the point: a click run through the limiter
    // would duck the whole mix on every beat, and one run through the master EQ would be coloured by
    // a setting that has nothing to do with it. It is a monitor sitting on top of the finished block.
    renderMetronome(output, numFrames, channelCount, b.sampleRate, b.startFrame, b.offlineRender);

    // Only when an instrument is being monitored (EQ screen), and never block on the UI read.
    if (b.monitoredInstrId >= 0) {
        std::unique_lock<std::mutex> lock(spectrumMutex, std::try_to_lock);
        if (lock.owns_lock()) {
            for (int i = 0; i < numFrames; i++) {
                // × masterVolEnd: the accumulator is filled pre-master (see the fader above), and
                // this curve has always been drawn post-master.
                instrSpectrumBuffer[instrSpectrumWriteIdx] = instrSpectrumTempL[i] * b.masterVolEnd;
                instrSpectrumWriteIdx = (instrSpectrumWriteIdx + 1) % SPECTRUM_SIZE;
            }
        }
    }
}

void AudioEngine::processLiveBlock(float* output, int numFrames, int channelCount, float sampleRate) {
    const auto blockStart = std::chrono::steady_clock::now();

    setFlushToZeroForCurrentThread();

    for (int i = 0; i < numFrames * channelCount; i++) {
        output[i] = 0.0f;
    }

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
    // the UI holds each one while it copies or decays, and the audio thread never waits for that. A
    // block that finds the lock held skips its capture — one block missing from a scope or a meter
    // is invisible.
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

    // Master spectrum ring — only while the spectrum visualizer or EQ screen is polling,
    // and try_lock so the audio thread never blocks on the UI's 2048-sample copy-out.
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

    // Update peak levels for mixer meters (live-only — not needed during WAV export)
    {
        std::unique_lock<std::mutex> lock(peakMutex, std::try_to_lock);
        if (lock.owns_lock()) {
            for (int t = 0; t < 8; t++) {
                trackPeaksL[t] *= PEAK_DECAY;
                trackPeaksR[t] *= PEAK_DECAY;
            }
            masterPeakL *= PEAK_DECAY;
            masterPeakR *= PEAK_DECAY;

            for (int t = 0; t < 8; t++) {
                trackPeaksL[t] = fmaxf(trackPeaksL[t], framePeaksPerTrackL[t]);
                trackPeaksR[t] = fmaxf(trackPeaksR[t], framePeaksPerTrackR[t]);
            }

            float maxL = 0.0f, maxR = 0.0f;
            for (int i = 0; i < numFrames; i++) {
                float absL = fabsf(output[i * channelCount]);
                float absR = fabsf(output[i * channelCount + 1]);
                if (absL > maxL) maxL = absL;
                if (absR > maxR) maxR = absR;
            }
            masterPeakL = fmaxf(masterPeakL, maxL);
            masterPeakR = fmaxf(masterPeakR, maxR);

            sendPeakRevL *= PEAK_DECAY; sendPeakRevR *= PEAK_DECAY;
            sendPeakDelL *= PEAK_DECAY; sendPeakDelR *= PEAK_DECAY;
            sendPeakRevL = fmaxf(sendPeakRevL, frameSendPeakRevL);
            sendPeakRevR = fmaxf(sendPeakRevR, frameSendPeakRevR);
            sendPeakDelL = fmaxf(sendPeakDelL, frameSendPeakDelL);
            sendPeakDelR = fmaxf(sendPeakDelR, frameSendPeakDelR);
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
    timingBusyNs += ns;
    timingFrames += numFrames;
    timingWorst   = std::max(timingWorst, load(ns, numFrames));
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
    return t;
}
