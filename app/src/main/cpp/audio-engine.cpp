// audio-engine.cpp — AudioEngine method bodies (Phase 0 file split)
// TSF API declarations only — TSF_IMPLEMENTATION lives in soundfont-voice.cpp
#include "audio-engine.h"
#include "tsf.h"

// Definition of the per-track soundfont voice array (extern declared in audio-engine.h)
SoundfontVoice sfVoices[8];

// ============================================================
// CONSTRUCTOR / DESTRUCTOR
// ============================================================

AudioEngine::AudioEngine() {
    for (int i = 0; i < 256; i++) {
        samples[i] = nullptr;
        sampleLengths[i] = 0;
        instrumentParams[i] = InstrumentParams();
    }
    globalFrameCounter = 0;

    for (int i = 0; i < WAVEFORM_SIZE; i++) {
        waveformBuffer[i] = 0.0f;
    }
    waveformIndex = 0;
    waveformDownsampleCounter = 0;
}

AudioEngine::~AudioEngine() {
    closeStream();
    for (int i = 0; i < 256; i++) {
        if (samples[i]) {
            delete[] samples[i];
        }
    }
}

// ============================================================
// STREAM MANAGEMENT
// ============================================================

bool AudioEngine::openStream() {
    // OpenSL ES does NOT trigger CCodec/C2 codec enumeration that spams 2000+ log lines
    // and blocks for up to 35 seconds on some Android ROMs (e.g. GammaCoreOS on Miyoo Flip).
    // Try OpenSL ES first; fall back to AAudio only if OpenSL ES is unavailable.

    oboe::AudioStreamBuilder builder;
    builder.setDataCallback(this);
    builder.setFormat(oboe::AudioFormat::Float);
    builder.setChannelCount(oboe::ChannelCount::Stereo);
    builder.setSampleRate(44100);

    // Attempt 1: OpenSL ES LowLatency Exclusive (best latency, no CCodec spam).
    builder.setAudioApi(oboe::AudioApi::OpenSLES);
    builder.setPerformanceMode(oboe::PerformanceMode::LowLatency);
    builder.setSharingMode(oboe::SharingMode::Exclusive);
    oboe::Result result = builder.openStream(stream);

    // Attempt 2: OpenSL ES LowLatency Shared.
    if (result != oboe::Result::OK) {
        LOGD("openStream: OpenSLES exclusive failed (%s), trying OpenSLES shared LowLatency",
             oboe::convertToText(result));
        builder.setSharingMode(oboe::SharingMode::Shared);
        result = builder.openStream(stream);
    }

    // Attempt 3: OpenSL ES None/Shared (maximum OpenSL ES compatibility).
    if (result != oboe::Result::OK) {
        LOGD("openStream: OpenSLES LowLatency failed (%s), trying OpenSLES None/Shared",
             oboe::convertToText(result));
        builder.setPerformanceMode(oboe::PerformanceMode::None);
        builder.setSharingMode(oboe::SharingMode::Shared);
        result = builder.openStream(stream);
    }

    // Attempt 4: AAudio LowLatency Exclusive (fallback; may trigger CCodec on some ROMs).
    if (result != oboe::Result::OK) {
        LOGD("openStream: OpenSLES failed (%s), falling back to AAudio LowLatency Exclusive",
             oboe::convertToText(result));
        builder.setAudioApi(oboe::AudioApi::Unspecified);
        builder.setPerformanceMode(oboe::PerformanceMode::LowLatency);
        builder.setSharingMode(oboe::SharingMode::Exclusive);
        result = builder.openStream(stream);
    }

    if (result != oboe::Result::OK) {
        LOGE("openStream: all attempts failed: %s", oboe::convertToText(result));
        return false;
    }

    LOGD("Stream opened: %d Hz, bufSz=%d, api=%s, perf=%s, sharing=%s",
         stream->getSampleRate(),
         stream->getBufferSizeInFrames(),
         oboe::convertToText(stream->getAudioApi()),
         oboe::convertToText(stream->getPerformanceMode()),
         oboe::convertToText(stream->getSharingMode()));

    result = stream->requestStart();
    if (result != oboe::Result::OK) {
        LOGE("Failed to start: %s", oboe::convertToText(result));
        return false;
    }

    LOGD("Stream started OK");
    return true;
}

void AudioEngine::closeStream() {
    if (stream) {
        stream->stop();
        stream->close();
        stream.reset();
    }
}

// ============================================================
// SAMPLE MANAGEMENT
// ============================================================

void AudioEngine::loadSample(int id, const float* data, int length) {
    if (id < 0 || id >= 256) return;

    if (samples[id]) {
        delete[] samples[id];
    }

    samples[id] = new float[length];
    for (int i = 0; i < length; i++) {
        samples[id][i] = data[i];
    }
    sampleLengths[id] = length;

    LOGD("Sample %d: %d frames", id, length);
}

void AudioEngine::clearAllSamples() {
    // Stop all active voices FIRST — they hold direct pointers to sample data.
    // Deleting samples while voices are still reading them causes use-after-free.
    for (int i = 0; i < MAX_VOICES; i++) {
        voices[i].stop();
    }
    // Clear the scheduled note/kill/param queues so buffered events don't re-trigger.
    noteQueue.clear();
    killQueue.clear();
    paramUpdateQueue.clear();

    for (int i = 0; i < 256; i++) {
        if (samples[i]) {
            delete[] samples[i];
            samples[i] = nullptr;
        }
        sampleLengths[i] = 0;
    }
    LOGD("All samples cleared");
}

void AudioEngine::setInstrumentParams(int instrumentId, int start, int end, bool rev, int loop, int loopSt,
                                      int drv, int crsh, int dwn, int fType, int fCut, int fRes) {
    if (instrumentId < 0 || instrumentId >= 256) return;

    instrumentParams[instrumentId].startPoint = start;
    instrumentParams[instrumentId].endPoint = end;
    instrumentParams[instrumentId].reverse = rev;
    instrumentParams[instrumentId].loopMode = loop;
    instrumentParams[instrumentId].loopStart = loopSt;
    instrumentParams[instrumentId].drive = drv;
    instrumentParams[instrumentId].crush = crsh;
    instrumentParams[instrumentId].downsample = dwn;
    instrumentParams[instrumentId].filterType = fType;
    instrumentParams[instrumentId].filterCut = fCut;
    instrumentParams[instrumentId].filterRes = fRes;

    LOGD("Instrument %d params: start=%d, end=%d, rev=%d, loop=%d, loopStart=%d, drive=%d, crush=%d, downsample=%d, filter=%d, cut=%d, res=%d",
         instrumentId, start, end, rev, loop, loopSt, drv, crsh, dwn, fType, fCut, fRes);
}

// ============================================================
// IMMEDIATE NOTE TRIGGER (no scheduling)
// ============================================================

void AudioEngine::triggerNote(int sampleId, int trackId, float freq, float baseFreq, float vol, float pan) {
    if (sampleId < 0 || sampleId >= 256 || !samples[sampleId]) return;

    // Resume stream if paused (prevents hum when not playing)
    resumeStream();

    // Stop track
    for (int i = 0; i < MAX_VOICES; i++) {
        if (voices[i].trackId == trackId) {
            voices[i].stop();
        }
    }

    // Find free voice
    for (int i = 0; i < MAX_VOICES; i++) {
        if (!voices[i].isActive) {
            float rate = freq / baseFreq;
            float sampleRate = stream ? (float)stream->getSampleRate() : 44100.0f;
            voices[i].trigger(samples[sampleId], sampleLengths[sampleId], trackId, rate, vol, 1.0f, pan,
                              instrumentParams[sampleId], sampleRate);
            LOGD("Note: track=%d, sampleId=%d, rate=%.3f, pan=%.2f", trackId, sampleId, rate, pan);
            return;
        }
    }
}

void AudioEngine::stopTrack(int trackId) {
    // Stop all sampler voices on this track
    for (int i = 0; i < MAX_VOICES; i++) {
        if (voices[i].trackId == trackId && voices[i].isActive) {
            voices[i].stop();
        }
    }
    // Stop any active soundfont note on this track
    if (trackId >= 0 && trackId < 8) {
        sfVoices[trackId].hardStop();
    }
}

void AudioEngine::stopAll() {
    for (int i = 0; i < MAX_VOICES; i++) {
        voices[i].stop();
    }
    // Stop all soundfont notes on all tracks
    for (int t = 0; t < 8; t++) {
        sfVoices[t].hardStop();
    }
    // Keep stream running so preview notes and future playback work immediately.
    // With all voices stopped and queue cleared, the callback outputs silence.
    LOGD("stopAll: voices and SF notes cleared, stream stays running");
}

int AudioEngine::getActiveVoiceCount() {
    int count = 0;
    for (int i = 0; i < MAX_VOICES; i++) {
        if (voices[i].isActive) {
            count++;
        }
    }
    return count;
}

void AudioEngine::getTrackActiveNotes(int* out, int trackCount) {
    for (int t = 0; t < trackCount; t++) out[t] = -1;
    for (int v = 0; v < MAX_VOICES; v++) {
        if (!voices[v].isActive) continue;
        int t = voices[v].trackId;
        if (t >= 0 && t < trackCount && out[t] == -1) {
            out[t] = voices[v].noteOctave * 12 + voices[v].notePitch;
        }
    }
}

int AudioEngine::getSampleRate() {
    if (stream) {
        return stream->getSampleRate();
    }
    return 48000; // Default fallback
}

void AudioEngine::resumeStream() {
    if (stream && stream->getState() == oboe::StreamState::Paused) {
        stream->start();
        LOGD("Stream resumed");
    }
}

// ============================================================
// CORE AUDIO PROCESSING BLOCK
// ALL audio DSP lives here. onAudioReady and renderOffline are thin wrappers.
// Rule: NEVER add audio processing logic directly to onAudioReady or renderOffline.
// ============================================================

void AudioEngine::processAudioBlock(float* output, int numFrames, int channelCount, float sampleRate) {
    // Zero per-track peak accumulators for this block
    for (int t = 0; t < 8; t++) framePeaksPerTrack[t] = 0.0f;

    // PHASE 1: Process note queue at sample-accurate timing
    for (int32_t frame = 0; frame < numFrames; frame++) {
        int64_t currentFrame = globalFrameCounter + frame;

        // Process scheduled parameter updates (e.g. Vxx phraseVol on empty steps)
        while (paramUpdateQueue.hasUpdateAt(currentFrame)) {
            ScheduledParamUpdate upd = paramUpdateQueue.pop();
            // Apply to sampler voices
            for (int v = 0; v < MAX_VOICES; v++) {
                if (voices[v].isActive && !voices[v].isFadingOut && voices[v].trackId == upd.trackId) {
                    voices[v].modSourceValues[(ModSourceId)upd.sourceId] = upd.value;
                    break;
                }
            }
            // Apply to SF voices (Phase 5)
            if (upd.trackId >= 0 && upd.trackId < 8 && sfVoices[upd.trackId].isActive) {
                sfVoices[upd.trackId].modSourceValues[(ModSourceId)upd.sourceId] = upd.value;
            }
        }

        // Process all scheduled kill events for this exact frame (BEFORE notes)
        while (killQueue.hasKillAt(currentFrame)) {
            ScheduledKill kill = killQueue.pop();
            if (kill.softKill) {
                triggerNoteOff(kill.trackId);  // Sampler: trigger ADSR release
                // SF: noteOff (TSF handles its own release envelope internally)
                if (kill.trackId >= 0 && kill.trackId < 8) {
                    sfVoices[kill.trackId].noteOff();
                }
                LOGT("🎵 Note-off: track %d at frame %lld", kill.trackId, (long long)currentFrame);
            } else {
                for (int v = 0; v < MAX_VOICES; v++) {
                    if (voices[v].trackId == kill.trackId && voices[v].isActive) {
                        voices[v].stop();
                        LOGT("🔪 Killed track %d at frame %lld", kill.trackId, (long long)currentFrame);
                    }
                }
                // SF: soft kill so TSF's internal release envelope can play out.
                if (kill.trackId >= 0 && kill.trackId < 8) {
                    sfVoices[kill.trackId].noteOff();
                }
            }
        }

        // Trigger all notes scheduled for this exact frame
        while (noteQueue.hasNoteAt(currentFrame)) {
            ScheduledNote note = noteQueue.pop();

            // ---- SOUNDFONT PATH ----
            // Tracks use the master tsf* handle via MIDI channels (channel = trackId).
            // No per-track clone creation — tsf_load_memory() never runs on the audio thread.
            if (note.isSoundfont) {
                int t = note.trackId;
                if (t >= 0 && t < 8 &&
                    note.sfSlot >= 0 && note.sfSlot < MAX_SOUNDFONTS &&
                    soundfonts[note.sfSlot].handle != nullptr) {

                    SoundfontVoice& sv = sfVoices[t];
                    float trkVol;
                    { std::lock_guard<std::mutex> vlock(volumeMutex); trkVol = trackVolumes[t]; }
                    sv.triggerNote(note.sfSlot, note.midiNote, note.midiVelocity,
                                   note.volume, trkVol, note.pan, note.sfBank, note.sfPreset, t);
                    sv.isReleasingOnly = false;
                    sv.resetPitchState();

                    // M8-style: honour TIC effect in table's last row (same as sampler path).
                    int effectiveTicRate = note.tableTicRate;
                    if (note.tableId >= 0 && note.tableId < 256) {
                        std::lock_guard<std::mutex> lock(tableMutex);
                        if (tables[note.tableId].loaded) {
                            const TableRow& lastRow = tables[note.tableId].rows[15];
                            auto checkTic = [](uint8_t fxType, uint8_t fxValue) -> int {
                                return (fxType == FX_TIC) ? fxValue : -1;
                            };
                            int t1 = checkTic(lastRow.fx1Type, lastRow.fx1Value);
                            int t2 = checkTic(lastRow.fx2Type, lastRow.fx2Value);
                            int t3 = checkTic(lastRow.fx3Type, lastRow.fx3Value);
                            if      (t1 >= 0) effectiveTicRate = t1;
                            else if (t2 >= 0) effectiveTicRate = t2;
                            else if (t3 >= 0) effectiveTicRate = t3;
                        }
                    }
                    sv.resetTableState(note.tableId, effectiveTicRate,
                                       note.noteOctave, note.notePitch, note.tableStartRow);

                    // Copy instrument effects params and modulation slots (Phase 5 / 7).
                    // Only valid when sampleId >= 0 (phrase playback); previews pass -1.
                    if (note.sampleId >= 0 && note.sampleId < 256) {
                        sv.instrParams = instrumentParams[note.sampleId];
                        for (int m = 0; m < 4; m++) {
                            const InstrumentModSlot& src = instrumentModSlots[note.sampleId][m];
                            VoiceModSlot& dst = sv.voiceMods[m];
                            dst.type = src.type;
                            dst.dest = src.dest;
                            dst.amount = src.amount;
                            dst.attackSamples = src.attackSamples;
                            dst.holdSamples = src.holdSamples;
                            dst.decaySamples = src.decaySamples;
                            dst.sustainLevel = src.sustainLevel;
                            dst.lfoHz = src.lfoHz;
                            dst.oscShape = src.oscShape;
                            dst.lfoPhase = 0.0f;
                            dst.releaseSamples = src.releaseSamples;
                            dst.effectiveAmt = src.amount;
                            dst.effectiveRateMult = 1.0f;
                            dst.prevEnvValue = 0.0f;
                            if (src.type != 0) { dst.stage = 1; dst.envValue = 0.0f; dst.stageCounter = 0; }
                            else               { dst.stage = 0; dst.envValue = 0.0f; dst.stageCounter = 0; }
                        }
                    } else {
                        sv.instrParams = InstrumentParams{};
                        for (int m = 0; m < 4; m++) sv.voiceMods[m] = VoiceModSlot{};
                    }
                    sv.chain.reset();
                    sv.chain.filter.setParams(sv.instrParams.filterType, sv.instrParams.filterCut,
                                              sv.instrParams.filterRes, sv.instrParams.filterDrive,
                                              (int)sampleRate);
                    sv.chain.drive.setDrive(sv.instrParams.drive);
                    sv.chain.crush.setParams(sv.instrParams.crush, sv.instrParams.downsample);

                    // Initialize modulation state (Phase 5).
                    sv.params.setBase(PARAM_VOL,   note.volume);
                    sv.params.setBase(PARAM_PAN,   note.pan);
                    sv.params.setBase(PARAM_PITCH, 0.0f);
                    sv.params.resetMods();
                    memset(sv.modSourceValues,  0, sizeof(sv.modSourceValues));
                    memset(sv.modDestValues,    0, sizeof(sv.modDestValues));
                    memset(sv.prevModDestValues,0, sizeof(sv.prevModDestValues));
                    sv.modSourceValues[MOD_SRC_TABLE_VOL]  = 1.0f;
                    sv.modSourceValues[MOD_SRC_PHRASE_VOL] = note.phraseVolume;
                    float initVol = note.volume * note.phraseVolume;
                    sv.modDestValues[PARAM_VOL]     = initVol;
                    sv.prevModDestValues[PARAM_VOL] = initVol;
                    if (note.pslInitialOffset != 0.0f && note.pslDuration > 0.0f) {
                        sv.pitchOffset      = note.pslInitialOffset;
                        sv.pitchSlideTarget = 0.0f;
                        sv.pitchSlideRate   = -note.pslInitialOffset / note.pslDuration;
                        sv.pitchSliding     = true;
                    }
                    if (fabsf(note.pbnRate) > 0.0001f) {
                        sv.pitchSlideRate   = note.pbnRate;
                        sv.pitchSlideTarget = (note.pbnRate > 0) ? 127.0f : -127.0f;
                        sv.pitchSliding     = true;
                    }
                    if (note.vibratoDepth > 0.01f) {
                        sv.vibratoSpeed  = note.vibratoSpeed;
                        sv.vibratoDepth  = note.vibratoDepth;
                        sv.vibratoActive = true;
                    }
                    LOGT("🎹 SF FIRE: slot=%d track/ch=%d bank=%d preset=%d midi=%d vel=%d vol=%.2f",
                         note.sfSlot, t, note.sfBank, note.sfPreset,
                         note.midiNote, note.midiVelocity, note.volume);
                } else {
                    LOGT("🎹 SF DROPPED: sfSlot=%d track=%d (handle not loaded)", note.sfSlot, note.trackId);
                }
                continue;  // Skip voice pool processing
            }
            // ---- END SOUNDFONT PATH ----

            // TIC00 support: Check if previous voice on this track was using trigger mode
            int savedTableRow = 0;
            bool wasTIC00Mode = false;
            for (int v = 0; v < MAX_VOICES; v++) {
                if (voices[v].trackId == note.trackId && voices[v].isActive && !voices[v].isFadingOut) {
                    if (voices[v].tableTicRate == 0x00 && voices[v].tableId >= 0) {
                        wasTIC00Mode = true;
                        savedTableRow = (voices[v].tableRow + 1) % 16;
                        LOGT("📋 TIC00: Saving table row %d for track %d retrigger", savedTableRow, note.trackId);
                    }
                }
            }

            // ---------------------------------------------------------------
            // VOICE ALLOCATION — 3-step priority
            //
            // Problem: "steal old + allocate new" temporarily consumes two
            // slots per track.  When N tracks all trigger at the same frame
            // (phrase boundaries) this exhausts the 8-slot pool even with
            // only 5 active tracks.
            //
            // Step 1 — recycle fading same-track voice (0 extra slots used).
            //           trackId is preserved through startFadeOut() so we can
            //           find and reuse the slot immediately.
            // Step 2 — normal steal: fade old same-track voice, find free slot.
            // Step 3 — last resort: preempt any fading voice (other track).
            //           Produces at most a ~1ms click but prevents silence.
            // ---------------------------------------------------------------

            // Step 1: same-track fading voice → recycle directly
            int targetSlot = -1;
            for (int v = 0; v < MAX_VOICES; v++) {
                if (voices[v].trackId == note.trackId && voices[v].isFadingOut) {
                    targetSlot = v;
                    break;
                }
            }

            // Step 2: steal non-fading same-track voice, then find free slot
            if (targetSlot == -1) {
                for (int v = 0; v < MAX_VOICES; v++) {
                    if (voices[v].trackId == note.trackId && voices[v].isActive && !voices[v].isFadingOut) {
                        voices[v].startFadeOut();
                    }
                }
                for (int v = 0; v < MAX_VOICES; v++) {
                    if (!voices[v].isActive) {
                        targetSlot = v;
                        break;
                    }
                }
            }

            // Step 3: preempt any fading voice (last resort)
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
                if (note.sampleId >= 0 && note.sampleId < 256 && samples[note.sampleId]) {
                    float rate = note.frequency / note.baseFrequency;

                    // M8-style: Check if table's last row has TIC effect
                    int effectiveTicRate = note.tableTicRate;
                    if (note.tableId >= 0 && note.tableId < 256) {
                        std::lock_guard<std::mutex> lock(tableMutex);
                        if (tables[note.tableId].loaded) {
                            TableRow& lastRow = tables[note.tableId].rows[15];
                            auto checkTic = [](uint8_t fxType, uint8_t fxValue) -> int {
                                if (fxType == FX_TIC) return fxValue;
                                return -1;
                            };
                            int tic1 = checkTic(lastRow.fx1Type, lastRow.fx1Value);
                            int tic2 = checkTic(lastRow.fx2Type, lastRow.fx2Value);
                            int tic3 = checkTic(lastRow.fx3Type, lastRow.fx3Value);
                            if (tic1 >= 0) {
                                effectiveTicRate = tic1;
                                LOGT("📋 M8-style: Using TIC %02X from table %d last row", effectiveTicRate, note.tableId);
                            } else if (tic2 >= 0) {
                                effectiveTicRate = tic2;
                                LOGT("📋 M8-style: Using TIC %02X from table %d last row", effectiveTicRate, note.tableId);
                            } else if (tic3 >= 0) {
                                effectiveTicRate = tic3;
                                LOGT("📋 M8-style: Using TIC %02X from table %d last row", effectiveTicRate, note.tableId);
                            }
                        }
                    }

                    // Determine table start row
                    int startRow;
                    if (note.tableStartRow >= 0) {
                        startRow = note.tableStartRow % 16;
                    } else if (wasTIC00Mode && effectiveTicRate == 0x00) {
                        startRow = savedTableRow;
                    } else {
                        startRow = 0;
                    }

                    voices[v].trigger(samples[note.sampleId], sampleLengths[note.sampleId],
                                      note.trackId, rate, note.volume, note.phraseVolume, note.pan, instrumentParams[note.sampleId],
                                      sampleRate, note.startPointOverride,
                                      note.tableId, effectiveTicRate, note.noteOctave, note.notePitch, startRow);

                    // PSL: Set initial pitch offset and start slide to note pitch.
                    // pslDuration is already in audio frames (converted by AudioEngine.kt).
                    if (fabsf(note.pslInitialOffset) > 0.001f && note.pslDuration > 0.0f) {
                        voices[v].pitchOffset = note.pslInitialOffset;
                        float totalFrames = fmaxf(1.0f, note.pslDuration);
                        voices[v].pitchSlideTarget = 0.0f;
                        voices[v].pitchSlideRate = -note.pslInitialOffset / totalFrames;
                        voices[v].pitchSliding = true;
                        LOGT("🎵 PSL applied: offset=%.2f, duration=%.0f ticks, rate=%.6f",
                             note.pslInitialOffset, note.pslDuration, voices[v].pitchSlideRate);
                    }
                    // PBN: Set continuous pitch bend rate.
                    // pbnRate is already in semitones/frame (converted by AudioEngine.kt).
                    if (fabsf(note.pbnRate) > 0.0001f) {
                        voices[v].pitchSlideRate = note.pbnRate;
                        voices[v].pitchSlideTarget = (note.pbnRate > 0) ? 127.0f : -127.0f;
                        voices[v].pitchSliding = true;
                        LOGT("🎵 PBN applied: rate=%.4f semitones/tick", note.pbnRate);
                    }
                    // PVB/PVX: Set vibrato
                    if (note.vibratoDepth > 0.01f) {
                        voices[v].vibratoSpeed = note.vibratoSpeed;
                        voices[v].vibratoDepth = note.vibratoDepth;
                        voices[v].vibratoActive = true;
                        LOGT("🎵 Vibrato applied: speed=%.1fHz, depth=%.2f semitones",
                             note.vibratoSpeed, note.vibratoDepth);
                    }

                    // Initialize modulation state from instrument mod slots.
                    for (int m = 0; m < 4; m++) {
                        const InstrumentModSlot& src = instrumentModSlots[note.sampleId][m];
                        VoiceModSlot& dst = voices[v].voiceMods[m];
                        dst.type = src.type;
                        dst.dest = src.dest;
                        dst.amount = src.amount;
                        dst.attackSamples = src.attackSamples;
                        dst.holdSamples = src.holdSamples;
                        dst.decaySamples = src.decaySamples;
                        dst.sustainLevel = src.sustainLevel;
                        dst.lfoHz = src.lfoHz;
                        dst.oscShape = src.oscShape;
                        dst.lfoPhase = 0.0f;
                        dst.releaseSamples = src.releaseSamples;
                        dst.effectiveAmt = src.amount;
                        dst.effectiveRateMult = 1.0f;
                        dst.prevEnvValue = 0.0f;
                        if (src.type != 0) {
                            dst.stage = 1;
                            dst.envValue = 0.0f;
                            dst.stageCounter = 0;
                        } else {
                            dst.stage = 0;
                            dst.envValue = 0.0f;
                            dst.stageCounter = 0;
                        }
                    }

                    LOGT("🎵 Triggered note at frame %lld: sample=%d, track=%d, rate=%.3f, vol=%.4f, pan=%.2f, startOverride=%d, table=%d, tic=%d, oct=%d, pitch=%d, startRow=%d",
                         (long long)currentFrame, note.sampleId, note.trackId, rate, note.volume, note.pan, note.startPointOverride,
                         note.tableId, effectiveTicRate, note.noteOctave, note.notePitch, startRow);
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
    }

    // ===================================
    // TABLE PROCESSING (Phase 3.5 + Phase 4 special modes)
    // ===================================
    // Process table ticks for each active voice once per callback
    // Special TIC modes (Phase 4):
    //   TIC00 (0x00): Trigger mode - table doesn't advance automatically
    //   TIC01-FB: Standard tic rate (1 tic = 1 audio callback ~6ms)
    //   TICFC (0xFC): Octave map - row = triggered note's octave (0-9)
    //   TICFE (0xFE): Note map - row = triggered note's pitch (0-11)
    //   TICFF (0xFF): 200Hz mode - advance ~1 row per 5ms
    for (int v = 0; v < MAX_VOICES; v++) {
        Voice& voice = voices[v];
        if (!voice.isActive || voice.tableId < 0) continue;

        // Check if table is loaded
        bool tableLoaded = false;
        {
            std::lock_guard<std::mutex> lock(tableMutex);
            tableLoaded = tables[voice.tableId].loaded;
        }
        if (!tableLoaded) continue;

        // Handle special TIC modes
        bool shouldProcessRow = false;
        bool shouldAdvance = false;

        if (voice.tableTicRate == 0x00) {
            // TIC00: Trigger mode - apply row effects ONCE, don't advance automatically
            shouldProcessRow = (voice.tableRow != voice.lastProcessedRow);
            shouldAdvance = false;
        } else if (voice.tableTicRate == 0xFC || voice.tableTicRate == 0xFE) {
            // TICFC/TICFE: Static mapping modes - row is fixed, process ONCE
            shouldProcessRow = (voice.tableRow != voice.lastProcessedRow);
            shouldAdvance = false;
        } else if (voice.tableTicRate == 0xFF) {
            // TICFF: 200Hz mode - faster advancement
            voice.tic200HzAccum += numFrames;
            float samplesPerTic = sampleRate / 200.0f;
            if (voice.tic200HzAccum >= samplesPerTic) {
                voice.tic200HzAccum -= samplesPerTic;
                shouldProcessRow = true;
                shouldAdvance = true;
            }
        } else {
            // Standard tic mode (01-FB): advance every N tics
            voice.tableTicCounter++;
            if (voice.tableTicCounter >= voice.tableTicRate) {
                voice.tableTicCounter = 0;
                shouldProcessRow = true;
                shouldAdvance = true;
            }
        }

        // Process current table row if needed
        if (shouldProcessRow) {

            // Get current table row data
            TableRow row;
            {
                std::lock_guard<std::mutex> lock(tableMutex);
                row = tables[voice.tableId].rows[voice.tableRow];
            }

            // Apply transpose — write semitones to source array (Phase 2).
            // playbackRate no longer has transpose baked in; getModulatedPlaybackRate reads
            // modDestValues[PARAM_PITCH] which processRoutes accumulates from TABLE_PITCH.
            int semitones = transposeToSemitones(row.transpose);
            voice.tableTranspose = (float)semitones;  // kept for debug log
            voice.modSourceValues[MOD_SRC_TABLE_PITCH] = (float)semitones;

            // Apply volume — write to source array (Phase 2).
            // Mix loop reads modDestValues[PARAM_VOL] instead of voice.tableVolume.
            if (row.volume == 0xFF) {
                voice.tableVolume = 1.0f;  // kept for debug log
            } else {
                voice.tableVolume = row.volume / 255.0f;
            }
            voice.modSourceValues[MOD_SRC_TABLE_VOL] = voice.tableVolume;

            // Process table effects (3 effect slots per row)
            bool hopExecuted = false;
            int hopTarget = -1;

            // Helper lambda to process a single effect
            auto processEffect = [&](uint8_t fxType, uint8_t fxValue) {
                switch (fxType) {
                    case FX_KILL:
                        // K00 - Kill voice immediately
                        if (fxValue == 0x00) {
                            voice.isActive = false;
                            LOGT("📋 Table effect: KILL voice %d", v);
                        }
                        break;

                    case FX_HOP:
                        // Hxx - HOP effect (Phase 5: repeat count support)
                        // Format: HOP XY where X = repeat count, Y = target row
                        // HOP FF = stop table processing
                        // HOP 0Y = infinite loop to row Y
                        // HOP XY (X>0) = jump to row Y exactly X times, then continue
                        if (fxValue == 0xFF) {
                            // Stop table processing for this voice
                            voice.tableId = -1;
                            voice.hopTargetRow = -1;
                            voice.hopRepeatCount = 0;
                            LOGT("📋 Table HOP FF: stopped table for voice %d", v);
                        } else {
                            int repeatCount = (fxValue >> 4) & 0x0F;  // High nibble = X
                            int targetRow = fxValue & 0x0F;           // Low nibble = Y

                            if (repeatCount == 0) {
                                // HOP 0Y = Infinite loop to row Y
                                hopExecuted = true;
                                hopTarget = targetRow;
                                LOGT("📋 Table HOP %02X: infinite loop to row %d, voice %d", fxValue, targetRow, v);
                            } else {
                                // HOP XY (X>0) = Jump X times, then continue
                                if (voice.hopTargetRow == -1 || voice.hopTargetRow != targetRow) {
                                    voice.hopRepeatCount = repeatCount;
                                    voice.hopTargetRow = targetRow;
                                    LOGT("📋 Table HOP %02X: initialized counter=%d, target=%d, voice %d",
                                         fxValue, repeatCount, targetRow, v);
                                }

                                if (voice.hopRepeatCount > 0) {
                                    voice.hopRepeatCount--;
                                    hopExecuted = true;
                                    hopTarget = targetRow;
                                    LOGT("📋 Table HOP: jump to row %d, %d jumps remaining, voice %d",
                                         targetRow, voice.hopRepeatCount, v);
                                } else {
                                    // Counter exhausted, don't jump, reset state and continue normally
                                    voice.hopTargetRow = -1;
                                    LOGT("📋 Table HOP: counter exhausted, continuing past row, voice %d", v);
                                }
                            }
                        }
                        break;

                    case FX_VOLUME:
                        // Vxx - Set volume (overrides volume column)
                        voice.tableVolume = fxValue / 255.0f;
                        voice.modSourceValues[MOD_SRC_TABLE_VOL] = voice.tableVolume;
                        break;

                    case FX_OFFSET:
                        // Oxx - Change sample position (relative to current)
                        if (voice.sampleLength > 0) {
                            float normalizedPos = fxValue / 255.0f;
                            voice.position = normalizedPos * (voice.sampleLength - 1);
                        }
                        break;

                    case FX_TIC:
                        // Txx - Set table tick rate (tics per row advance)
                        if (fxValue >= 0x01 && fxValue <= 0xFB) {
                            voice.tableTicRate = fxValue;
                            voice.tableTicCounter = 0;
                            LOGT("📋 Table effect: TIC %02X - set tick rate to %d", fxValue, fxValue);
                        }
                        break;

                    case FX_THO:
                        // THO 0X - Table hop to row X (simple unconditional jump)
                        hopExecuted = true;
                        hopTarget = fxValue & 0x0F;
                        LOGT("📋 Table THO %02X: hop to row %d, voice %d", fxValue, hopTarget, v);
                        break;

                    default:
                        // Unknown or unimplemented effect - ignore
                        break;
                }
            };

            // Process all 3 effect slots
            processEffect(row.fx1Type, row.fx1Value);
            processEffect(row.fx2Type, row.fx2Value);
            processEffect(row.fx3Type, row.fx3Value);

            // Mark this row as processed (before any jumps change tableRow)
            int processedRow = voice.tableRow;
            voice.lastProcessedRow = processedRow;

            // Handle row advancement
            if (hopExecuted && hopTarget >= 0) {
                voice.tableRow = hopTarget % 16;
                LOGT("📋 Table HOP: voice %d jumped to row %d", v, voice.tableRow);
            } else if (shouldAdvance) {
                voice.tableRow = (voice.tableRow + 1) % 16;
            }

            if (shouldAdvance && voice.tableRow == 0) {
                LOGT("📋 Table %d loop: voice=%d, transpose=%.0f, vol=%.2f",
                     voice.tableId, v, voice.tableTranspose, voice.tableVolume);
            }
        }
    }

    // ===================================
    // TABLE PROCESSING — SF VOICES
    // ===================================
    // Mirrors the sampler table loop above.  Effects that write to modSourceValues[]
    // (transpose, volume) are automatically picked up by updateVoiceModulation / applyPitchMod.
    // Sampler-only effects (FX_OFFSET) are silently skipped.
    for (int t = 0; t < 8; t++) {
        SoundfontVoice& sv = sfVoices[t];
        if (!sv.isActive || sv.tableId < 0) continue;

        bool tableLoaded = false;
        {
            std::lock_guard<std::mutex> lock(tableMutex);
            tableLoaded = tables[sv.tableId].loaded;
        }
        if (!tableLoaded) continue;

        bool shouldProcessRow = false;
        bool shouldAdvance    = false;

        if (sv.tableTicRate == 0x00) {
            shouldProcessRow = (sv.tableRow != sv.lastProcessedRow);
            shouldAdvance    = false;
        } else if (sv.tableTicRate == 0xFC || sv.tableTicRate == 0xFE) {
            shouldProcessRow = (sv.tableRow != sv.lastProcessedRow);
            shouldAdvance    = false;
        } else if (sv.tableTicRate == 0xFF) {
            sv.tic200HzAccum += numFrames;
            float samplesPerTic = sampleRate / 200.0f;
            if (sv.tic200HzAccum >= samplesPerTic) {
                sv.tic200HzAccum -= samplesPerTic;
                shouldProcessRow = true;
                shouldAdvance    = true;
            }
        } else {
            sv.tableTicCounter++;
            if (sv.tableTicCounter >= sv.tableTicRate) {
                sv.tableTicCounter = 0;
                shouldProcessRow   = true;
                shouldAdvance      = true;
            }
        }

        if (shouldProcessRow) {
            TableRow row;
            {
                std::lock_guard<std::mutex> lock(tableMutex);
                row = tables[sv.tableId].rows[sv.tableRow];
            }

            int semitones = transposeToSemitones(row.transpose);
            sv.tableTranspose = (float)semitones;
            sv.modSourceValues[MOD_SRC_TABLE_PITCH] = (float)semitones;

            if (row.volume == 0xFF) {
                sv.tableVolume = 1.0f;
            } else {
                sv.tableVolume = row.volume / 255.0f;
            }
            sv.modSourceValues[MOD_SRC_TABLE_VOL] = sv.tableVolume;

            bool hopExecuted = false;
            int  hopTarget   = -1;

            auto processEffect = [&](uint8_t fxType, uint8_t fxValue) {
                switch (fxType) {
                    case FX_KILL:
                        if (fxValue == 0x00) sv.noteOff();
                        break;
                    case FX_HOP:
                        if (fxValue == 0xFF) {
                            sv.tableId      = -1;
                            sv.hopTargetRow = -1;
                            sv.hopRepeatCount = 0;
                        } else {
                            int repeatCount = (fxValue >> 4) & 0x0F;
                            int targetRow   =  fxValue       & 0x0F;
                            if (repeatCount == 0) {
                                hopExecuted = true; hopTarget = targetRow;
                            } else {
                                if (sv.hopTargetRow == -1 || sv.hopTargetRow != targetRow) {
                                    sv.hopRepeatCount = repeatCount;
                                    sv.hopTargetRow   = targetRow;
                                }
                                if (sv.hopRepeatCount > 0) {
                                    sv.hopRepeatCount--;
                                    hopExecuted = true; hopTarget = targetRow;
                                } else {
                                    sv.hopTargetRow = -1;
                                }
                            }
                        }
                        break;
                    case FX_VOLUME:
                        sv.tableVolume = fxValue / 255.0f;
                        sv.modSourceValues[MOD_SRC_TABLE_VOL] = sv.tableVolume;
                        break;
                    case FX_TIC:
                        if (fxValue >= 0x01 && fxValue <= 0xFB) {
                            sv.tableTicRate    = fxValue;
                            sv.tableTicCounter = 0;
                        }
                        break;
                    case FX_THO:
                        hopExecuted = true; hopTarget = fxValue & 0x0F;
                        break;
                    default:
                        break;  // FX_OFFSET, FX_RETRIGGER, FX_ARP not applicable to SF
                }
            };

            processEffect(row.fx1Type, row.fx1Value);
            processEffect(row.fx2Type, row.fx2Value);
            processEffect(row.fx3Type, row.fx3Value);

            sv.lastProcessedRow = sv.tableRow;

            if (hopExecuted && hopTarget >= 0) {
                sv.tableRow = hopTarget % 16;
            } else if (shouldAdvance) {
                sv.tableRow = (sv.tableRow + 1) % 16;
            }
        }
    }

    // ===================================
    // PITCH MODULATION PROCESSING (Phase 6)
    // ===================================
    for (int v = 0; v < MAX_VOICES; v++) {
        Voice& voice = voices[v];
        if (!voice.isActive) continue;
        updateVoicePitchMod(voice, numFrames, sampleRate);
    }

    // ===================================
    // MODULATION PROCESSING (Phase 4 — AHD/ADSR/LFO)
    // ===================================
    // Snapshot envValues before advancing so the mix loop can interpolate
    // per-sample (eliminates block-rate staircase artifacts on short envelopes).
    for (int v = 0; v < MAX_VOICES; v++) {
        if (!voices[v].isActive) continue;
        for (int m = 0; m < 4; m++)
            voices[v].voiceMods[m].prevEnvValue = voices[v].voiceMods[m].envValue;
    }
    for (int v = 0; v < MAX_VOICES; v++) {
        if (!voices[v].isActive) continue;
        updateVoiceModulation(voices[v], numFrames, sampleRate);
    }

    // Apply per-voice PAN and FILTER modulation (once per block)
    for (int v = 0; v < MAX_VOICES; v++) {
        Voice& voice = voices[v];
        if (!voice.isActive) continue;

        // PAN modulation: recalculate stereo gains from ParamBus (base=basePan, mod=env/LFO)
        if (fabsf(voice.params.mod[PARAM_PAN]) > 0.001f) {
            float modPan = fmaxf(0.0f, fminf(1.0f, voice.params.get(PARAM_PAN)));
            float panAngle = modPan * (float)M_PI * 0.5f;
            voice.panLeft = cosf(panAngle);
            voice.panRight = sinf(panAngle);
        }

        // FILTER modulation: recompute coefficients when LFO/ADSR drives CUT or RES
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

    // Mix voices
    for (int v = 0; v < MAX_VOICES; v++) {
        Voice& voice = voices[v];
        if (!voice.isActive || !voice.sampleData) continue;

        // Get modulated playback rate (includes pitch slide + vibrato)
        float modulatedRate = getModulatedPlaybackRate(voice);

        // Effective distortion and sample-bound params (base + mod, clamped)
        int effDrive      = std::max(0, std::min(255, (int)(voice.params.base[PARAM_DRIVE]      + voice.modDestValues[PARAM_DRIVE])));
        int effCrush      = std::max(0, std::min(15,  (int)(voice.params.base[PARAM_CRUSH]      + voice.modDestValues[PARAM_CRUSH])));
        int effDownsample = std::max(0, std::min(15,  (int)(voice.params.base[PARAM_DOWNSAMPLE] + voice.modDestValues[PARAM_DOWNSAMPLE])));
        // Push effective values into the chain so processMono() picks them up this block.
        voice.chain.drive.setDrive(effDrive);
        voice.chain.crush.setParams(effCrush, 0);   // sampler: downsample=0, pre-interp handles it
        {
            float rawStart   = voice.params.base[PARAM_SAMPLE_START] + voice.modDestValues[PARAM_SAMPLE_START];
            float rawEnd     = voice.params.base[PARAM_SAMPLE_END]   + voice.modDestValues[PARAM_SAMPLE_END];
            float rawLoop    = voice.params.base[PARAM_LOOP_START]   + voice.modDestValues[PARAM_LOOP_START];
            int sl = voice.sampleLength;
            voice.actualStart     = std::max(0,             std::min((int)(rawStart * sl / 255.0f), sl - 2));
            voice.actualEnd       = std::max(voice.actualStart + 1, std::min((int)(rawEnd * sl / 255.0f), sl - 1));
            voice.actualLoopStart = std::max(voice.actualStart, std::min((int)(rawLoop * sl / 255.0f), voice.actualEnd));
        }

        for (int i = 0; i < numFrames; i++) {
            int idx = (int)voice.position;
            float frac = voice.position - (float)idx;

            // Bounds check - need idx+1 for interpolation
            if (idx < 0 || idx >= voice.sampleLength - 1) {
                // Handle edge case: exactly at last sample
                if (idx == voice.sampleLength - 1 && frac == 0.0f) {
                    float sample = voice.sampleData[idx] * voice.volume;
                    float sampleL = sample * voice.panLeft;
                    float sampleR = sample * voice.panRight;
                    output[i * channelCount] += sampleL;
                    output[i * channelCount + 1] += sampleR;
                    if (voice.trackId >= 0 && voice.trackId < 8) {
                        float peakLevel = fmaxf(fabsf(sampleL), fabsf(sampleR));
                        framePeaksPerTrack[voice.trackId] = fmaxf(framePeaksPerTrack[voice.trackId], peakLevel);
                    }
                }
                voice.isActive = false;
                break;
            }

            // ===================================
            // SIGNAL CHAIN: Downsample → Crush → Interpolate → Drive → Volume
            // ===================================

            float sample1 = voice.sampleData[idx];
            float sample2 = voice.sampleData[idx + 1];

            // STEP 1: DOWNSAMPLE (sample rate reduction via sample-and-hold)
            if (effDownsample > 0) {
                int downsampleFactor = 1 << effDownsample;
                int quantizedIdx = (idx / downsampleFactor) * downsampleFactor;
                sample1 = voice.sampleData[quantizedIdx];
                sample2 = voice.sampleData[quantizedIdx];
            }

            // STEP 2: LINEAR INTERPOLATION (pitch shifting)
            float processedSample = sample1 + (sample2 - sample1) * frac;

            // STEP 3: Crush → Drive → Filter via InstrumentChain (post-interpolation)
            processedSample = voice.chain.processMono(processedSample);

            // STEP 4: Apply volume after effects, with modulation (Phase 4)
            float t = (numFrames > 1) ? (float)(i + 1) / (float)numFrames : 1.0f;
            float finalVol = voice.volume;
            for (int m = 0; m < 4; m++) {
                const VoiceModSlot& mod = voice.voiceMods[m];
                if (mod.type == 0 || mod.stage == 0) continue;
                if (mod.dest == 1) { // VOL destination
                    if (mod.type == 3) {
                        // LFO: bipolar tremolo — interpolate for smooth low-rate modulation
                        float envAtI = mod.prevEnvValue + (mod.envValue - mod.prevEnvValue) * t;
                        finalVol = fmaxf(0.0f, finalVol * (1.0f + envAtI * mod.effectiveAmt));
                    } else {
                        // AHD/DRUM/ADSR/TRIG: only interpolate on decay (falling envelope)
                        float envAtI = (mod.envValue < mod.prevEnvValue)
                            ? mod.prevEnvValue + (mod.envValue - mod.prevEnvValue) * t
                            : mod.envValue;
                        finalVol = fmaxf(0.0f, finalVol + (envAtI - 1.0f) * mod.effectiveAmt);
                    }
                }
                // PITCH dest: accumulated into voice.params.mod[PARAM_PITCH] by updateVoiceModulation
            }
            // modDestValues[PARAM_VOL] = TABLE_VOL × phraseVol × instrVol (processRoutes, once/block).
            // Interpolate per-sample using prevModDestValues to avoid clicks on block-boundary changes
            // (e.g. Vxx on empty step, table row volume changes).
            float volRoute = voice.prevModDestValues[PARAM_VOL]
                           + (voice.modDestValues[PARAM_VOL] - voice.prevModDestValues[PARAM_VOL]) * t;
            float sample = processedSample * finalVol * volRoute;

            // STEP 7: Apply real-time track and master volume
            float trackVol, masterVol;
            {
                std::lock_guard<std::mutex> lock(volumeMutex);
                trackVol = (voice.trackId >= 0 && voice.trackId < 8) ? trackVolumes[voice.trackId] : 1.0f;
                masterVol = masterVolume;
            }
            sample = sample * trackVol * masterVol;

            // STEP 8: Anti-click fades (fade-in + end-of-sample fade-out)
            sample *= voice.antiClickFade();

            // STEP 8b: Voice-steal fade-out multiplier
            if (voice.isFadingOut) {
                sample *= (float)voice.fadeOutRemaining / (float)DECLICK_SAMPLES;
                if (--voice.fadeOutRemaining <= 0) {
                    voice.isFadingOut = false;
                    voice.isActive = false;
                }
            }

            // Apply pan and write to stereo channels
            float sampleL = sample * voice.panLeft;
            float sampleR = sample * voice.panRight;
            output[i * channelCount] += sampleL;
            output[i * channelCount + 1] += sampleR;

            // Track peak for metering
            if (!voice.isFadingOut && voice.trackId >= 0 && voice.trackId < 8) {
                float peakLevel = fmaxf(fabsf(sampleL), fabsf(sampleR));
                framePeaksPerTrack[voice.trackId] = fmaxf(framePeaksPerTrack[voice.trackId], peakLevel);
            }

            if (!voice.isActive) break;

            // Update position based on playback mode
            if (voice.loopMode == 2) {
                // Ping-pong loop
                if (voice.loopingBack) {
                    voice.position -= modulatedRate;
                    if (voice.position <= voice.actualLoopStart) {
                        voice.loopingBack = false;
                        voice.position = (float)voice.actualLoopStart;
                    }
                } else {
                    voice.position += modulatedRate;
                    if (voice.position >= voice.actualEnd) {
                        voice.loopingBack = true;
                        voice.position = (float)voice.actualEnd;
                    }
                }
            } else if (voice.reverse) {
                // Reverse playback
                voice.position -= modulatedRate;
                if (voice.position <= voice.actualStart) {
                    if (voice.loopMode == 1) {
                        voice.position = (float)voice.actualLoopStart;
                    } else {
                        voice.isActive = false;
                        break;
                    }
                }
            } else {
                // Forward playback
                voice.position += modulatedRate;
                if (voice.position >= voice.actualEnd) {
                    if (voice.loopMode == 1) {
                        voice.position = (float)voice.actualLoopStart;
                    } else {
                        voice.isActive = false;
                        break;
                    }
                }
            }
        }
    }

    // ===================================
    // SOUNDFONT RENDERING — per-track channel renders (Phase 6)
    // ===================================
    {
        float sfBuf[2048];  // 1024 frames * 2 channels — safe for any Oboe buffer size
        float masterVol;
        { std::lock_guard<std::mutex> vlock(volumeMutex); masterVol = masterVolume; }

        // 1. Advance modulation state machines and apply volume/pitch (audio thread, no lock needed).
        for (int t = 0; t < 8; t++) {
            SoundfontVoice& sv = sfVoices[t];
            if (!sv.isActive) continue;

            // Run the shared modulation engine (envelopes, LFOs, routes) — same as sampler path.
            updateVoiceModulation(sv, numFrames, (float)sampleRate);

            // Apply modulated volume: modDestValues[PARAM_VOL] = instrVol × phraseVol × tableVol.
            // LFO/AHD VOL (dest=1) are block-rate for SF — apply directly to channel volume.
            float noteVol = sv.modDestValues[PARAM_VOL];
            for (int m = 0; m < 4; m++) {
                VoiceModSlot& mod = sv.voiceMods[m];
                if (mod.type == 0 || mod.stage == 0 || mod.dest != 1) continue;
                // Skip completed mods — don't silence the channel during TSF's release tail.
                // AHD/DRUM done at stage 4, ADSR/TRIG done at stage 5.
                if ((mod.type == 1 || mod.type == 4) && mod.stage == 4) continue;
                if ((mod.type == 2 || mod.type == 5) && mod.stage == 5) continue;
                if (mod.type == 3) {  // LFO: bipolar tremolo
                    noteVol = fmaxf(0.0f, noteVol * (1.0f + mod.envValue * mod.effectiveAmt));
                } else {  // AHD/DRUM/ADSR/TRIG: unipolar gain reduction
                    noteVol = fmaxf(0.0f, noteVol + (mod.envValue - 1.0f) * mod.effectiveAmt);
                }
            }
            // Audio-thread path: no mutex needed (consistent with triggerNote / applyPitchMod).
            if (sv.sfSlot >= 0) {
                float trkVol;
                { std::lock_guard<std::mutex> vlock(volumeMutex); trkVol = trackVolumes[t]; }
                tsf* h = soundfonts[sv.sfSlot].handle;
                if (h) tsf_channel_set_volume(h, t, noteVol * trkVol);
            }

            // When releasing with ADSR/TRIG VOL mods: stop as soon as all have finished
            // so the channel volume doesn't jump back to the base level after release.
            // (Without this, the mod would be skipped at stage 5, making the channel loud
            // again for one block before TSF silence detection fires.)
            if (sv.isReleasingOnly) {
                bool hasAdsrVolMod  = false;
                bool allAdsrVolDone = true;
                for (int m = 0; m < 4; m++) {
                    const VoiceModSlot& mod = sv.voiceMods[m];
                    if (mod.dest == 1 && (mod.type == 2 || mod.type == 5) && mod.stage > 0) {
                        hasAdsrVolMod = true;
                        if (mod.stage < 5) allAdsrVolDone = false;
                    }
                }
                if (hasAdsrVolMod && allAdsrVolDone) {
                    sv.hardStop();
                    continue;
                }
            }

            // If filter mod is active, recompute coefficients via InstrumentChain.
            if (sv.chain.filter.enabled()) {
                int modCut = std::max(0, std::min(255,
                    (int)(sv.instrParams.filterCut + sv.modDestValues[PARAM_FILTER_CUT])));
                int modRes = std::max(0, std::min(255,
                    (int)(sv.instrParams.filterRes + sv.modDestValues[PARAM_FILTER_RES])));
                if (modCut != sv.instrParams.filterCut || modRes != sv.instrParams.filterRes) {
                    sv.chain.filter.setParams(sv.chain.filter.type, modCut, modRes, sv.chain.filter.drive, (int)sampleRate);
                }
            }

            // Advance pitch slide/vibrato and write MIDI pitch wheel.
            // applyPitchMod now also adds modDestValues[PARAM_PITCH] (LFO/AHD → PITCH routes).
            sv.applyPitchMod((float)sampleRate, numFrames);
        }

        // 2. Render each active track into its own per-channel buffer, then mix.
        // tsf_render_float_channel() filters by MIDI channel (= trackId), so each track
        // gets an independent buffer — enabling per-track effects in Phase 7.
        for (int t = 0; t < 8; t++) {
            SoundfontVoice& sv = sfVoices[t];
            if (!sv.isActive || sv.sfSlot < 0) continue;
            tsf* h = soundfonts[sv.sfSlot].handle;
            if (!h) continue;

            memset(sfBuf, 0, sizeof(float) * numFrames * 2);
            {
                std::lock_guard<std::mutex> sfLock(soundfonts[sv.sfSlot].mutex);
                tsf_render_float_channel(h, t, sfBuf, numFrames, 0 /* overwrite */);
            }

            // Phase 7: apply instrument effects to this track's buffer before mixing.
            // Signal chain via InstrumentChain: Downsample+Crush → Drive → Filter.
            // Downsample and crush are both handled by BitcrushModule (Decimator).
            for (int i = 0; i < numFrames; i++) {
                float L = sfBuf[i * 2];
                float R = sfBuf[i * 2 + 1];
                sv.chain.processStereo(L, R);
                sfBuf[i * 2]     = L;
                sfBuf[i * 2 + 1] = R;
            }

            float trackPeak = 0.0f;
            for (int i = 0; i < numFrames * 2; i++) {
                trackPeak = fmaxf(trackPeak, fabsf(sfBuf[i]));
                output[i] += sfBuf[i] * masterVol;
            }
            framePeaksPerTrack[t] = fmaxf(framePeaksPerTrack[t], trackPeak);

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
        }
    }

    // Master chain (stub — modules added here in future, limiter moves here eventually)
    masterChain.process(output, numFrames, channelCount);

    // Brickwall limiter at -0.1 dBFS
    {
        constexpr float LIMITER_THRESHOLD = 0.98855f;
        for (int i = 0; i < numFrames; i++) {
            output[i * channelCount]     = fmaxf(-LIMITER_THRESHOLD, fminf(LIMITER_THRESHOLD, output[i * channelCount]));
            output[i * channelCount + 1] = fmaxf(-LIMITER_THRESHOLD, fminf(LIMITER_THRESHOLD, output[i * channelCount + 1]));
        }
    }

    globalFrameCounter += numFrames;
}

// ============================================================
// LIVE AUDIO CALLBACK (thin wrapper — no DSP here!)
// ============================================================

oboe::DataCallbackResult AudioEngine::onAudioReady(
        oboe::AudioStream *audioStream,
        void *audioData,
        int32_t numFrames) {

    // Set flush-to-zero mode once at audio thread start.
    static std::once_flag ftzFlag;
    std::call_once(ftzFlag, []() {
#if defined(__aarch64__)
        uint64_t fpcr;
        asm volatile("mrs %0, fpcr" : "=r"(fpcr));
        fpcr |= (1ULL << 24);  // FZ bit
        asm volatile("msr fpcr, %0" : : "r"(fpcr));
#elif defined(__arm__)
        uint32_t fpscr;
        asm volatile("vmrs %0, fpscr" : "=r"(fpscr));
        fpscr |= (1U << 24);  // FZ bit
        asm volatile("vmsr fpscr, %0" : : "r"(fpscr));
#endif
    });

    float *output = static_cast<float*>(audioData);
    int channelCount = audioStream->getChannelCount();

    // Silence output
    for (int i = 0; i < numFrames * channelCount; i++) {
        output[i] = 0.0f;
    }

    // During offline WAV render: output silence and let renderOffline process the queue.
    if (isOfflineRendering.load()) {
        return oboe::DataCallbackResult::Continue;
    }

    float sampleRate = (float)audioStream->getSampleRate();
    processAudioBlock(output, numFrames, channelCount, sampleRate);

    // Capture waveform for oscilloscope (left channel only, with downsampling)
    {
        std::lock_guard<std::mutex> lock(waveformMutex);
        for (int i = 0; i < numFrames; i++) {
            waveformDownsampleCounter++;
            if (waveformDownsampleCounter >= WAVEFORM_DOWNSAMPLE) {
                waveformBuffer[waveformIndex] = output[i * channelCount];
                waveformIndex = (waveformIndex + 1) % WAVEFORM_SIZE;
                waveformDownsampleCounter = 0;
            }
        }
    }

    // Update peak levels for mixer meters (live-only — not needed during WAV export)
    {
        std::lock_guard<std::mutex> lock(peakMutex);

        for (int t = 0; t < 8; t++) {
            trackPeaks[t] *= PEAK_DECAY;
        }
        masterPeakL *= PEAK_DECAY;
        masterPeakR *= PEAK_DECAY;

        for (int t = 0; t < 8; t++) {
            trackPeaks[t] = fmaxf(trackPeaks[t], framePeaksPerTrack[t]);
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
    }

    return oboe::DataCallbackResult::Continue;
}

// ============================================================
// NOTE SCHEDULING
// ============================================================

int64_t AudioEngine::getCurrentFrame() {
    return globalFrameCounter;
}

void AudioEngine::scheduleNote(int64_t targetFrame, int sampleId, int trackId,
                               float frequency, float baseFrequency, float volume, float phraseVolume, float pan,
                               int startPointOverride, int tableId, int tableTicRate,
                               int noteOctave, int notePitch,
                               float pslInitialOffset, float pslDuration,
                               float pbnRate, float vibratoSpeed, float vibratoDepth,
                               int tableStartRow) {
    ScheduledNote note = {
            .targetFrame = targetFrame,
            .sampleId = sampleId,
            .trackId = trackId,
            .frequency = frequency,
            .baseFrequency = baseFrequency,
            .volume = volume,
            .phraseVolume = phraseVolume,
            .pan = pan,
            .startPointOverride = startPointOverride,
            .tableId = tableId,
            .tableTicRate = tableTicRate,
            .noteOctave = noteOctave,
            .notePitch = notePitch,
            .pslInitialOffset = pslInitialOffset,
            .pslDuration = pslDuration,
            .pbnRate = pbnRate,
            .vibratoSpeed = vibratoSpeed,
            .vibratoDepth = vibratoDepth,
            .tableStartRow = tableStartRow
    };
    noteQueue.schedule(note);
}

void AudioEngine::scheduleSoundfontNote(int64_t targetFrame, int trackId, int sfSlot,
                                        int midiNote, int midiVelocity, float vol, float pan,
                                        int bank, int preset,
                                        float pslInitialOffset, float pslDuration,
                                        float pbnRate, float vibratoSpeed, float vibratoDepth,
                                        float phraseVol, int sampleId,
                                        int tableId, int tableTicRate,
                                        int noteOctave, int notePitch, int tableStartRow) {
    ScheduledNote note{};
    note.targetFrame      = targetFrame;
    note.trackId          = trackId;
    note.isSoundfont      = true;
    note.sfSlot           = sfSlot;
    note.midiNote         = midiNote;
    note.midiVelocity     = midiVelocity;
    note.volume           = vol;
    note.phraseVolume     = phraseVol;
    note.pan              = pan;
    note.sfBank           = bank;
    note.sfPreset         = preset;
    note.sampleId         = sampleId;
    note.frequency        = 440.0f;
    note.baseFrequency    = 440.0f;
    note.startPointOverride = -1;
    note.tableId          = tableId;
    note.tableTicRate     = tableTicRate;
    note.noteOctave       = noteOctave;
    note.notePitch        = notePitch;
    note.pslInitialOffset = pslInitialOffset;
    note.pslDuration      = pslDuration;
    note.pbnRate          = pbnRate;
    note.vibratoSpeed     = vibratoSpeed;
    note.vibratoDepth     = vibratoDepth;
    note.tableStartRow    = tableStartRow;
    noteQueue.schedule(note);
}

void AudioEngine::scheduleKill(int64_t targetFrame, int trackId) {
    ScheduledKill kill = {
            .targetFrame = targetFrame,
            .trackId = trackId
    };
    killQueue.schedule(kill);
}

void AudioEngine::scheduleNoteOff(int64_t targetFrame, int trackId) {
    ScheduledKill kill = {
            .targetFrame = targetFrame,
            .trackId = trackId,
            .softKill = true
    };
    killQueue.schedule(kill);
}

void AudioEngine::clearScheduledNotes() {
    noteQueue.clear();
    killQueue.clear();
    paramUpdateQueue.clear();
}

void AudioEngine::clearScheduledNotesFrom(int64_t fromFrame) {
    noteQueue.clearFrom(fromFrame);
    killQueue.clearFrom(fromFrame);
    paramUpdateQueue.clearFrom(fromFrame);
}

// ============================================================
// TABLE METHODS
// ============================================================

void AudioEngine::loadTable(int tableId, const uint8_t* rowData) {
    if (tableId < 0 || tableId >= 256) return;

    std::lock_guard<std::mutex> lock(tableMutex);
    Table& table = tables[tableId];

    for (int row = 0; row < 16; row++) {
        int offset = row * 8;
        table.rows[row].transpose = (int8_t)rowData[offset + 0];
        table.rows[row].volume = rowData[offset + 1];
        table.rows[row].fx1Type = rowData[offset + 2];
        table.rows[row].fx1Value = rowData[offset + 3];
        table.rows[row].fx2Type = rowData[offset + 4];
        table.rows[row].fx2Value = rowData[offset + 5];
        table.rows[row].fx3Type = rowData[offset + 6];
        table.rows[row].fx3Value = rowData[offset + 7];
    }
    table.loaded = true;

    LOGD("📋 Loaded table %d", tableId);
}

int AudioEngine::getVoiceTableRow(int trackId) {
    for (int v = 0; v < MAX_VOICES; v++) {
        if (voices[v].isActive && voices[v].trackId == trackId) {
            return voices[v].tableRow;
        }
    }
    // SF voices are indexed directly by trackId
    if (trackId >= 0 && trackId < 8) {
        const SoundfontVoice& sv = sfVoices[trackId];
        if (sv.isActive && sv.tableId >= 0) return sv.tableRow;
    }
    return -1;
}

int AudioEngine::getVoiceTableId(int trackId) {
    for (int v = 0; v < MAX_VOICES; v++) {
        if (voices[v].isActive && voices[v].trackId == trackId) {
            return voices[v].tableId;
        }
    }
    // SF voices are indexed directly by trackId
    if (trackId >= 0 && trackId < 8) {
        const SoundfontVoice& sv = sfVoices[trackId];
        if (sv.isActive) return sv.tableId;
    }
    return -1;
}

void AudioEngine::setVoiceTableRow(int trackId, int row) {
    for (int v = 0; v < MAX_VOICES; v++) {
        if (voices[v].isActive && voices[v].trackId == trackId) {
            voices[v].tableRow = row % 16;
            voices[v].lastProcessedRow = -1;
            LOGD("📋 THO: Set voice %d (track %d) table row to %d", v, trackId, voices[v].tableRow);
            return;
        }
    }
    LOGD("📋 THO: No active voice on track %d, ignoring", trackId);
}

void AudioEngine::scheduleTrackPhraseVol(int64_t targetFrame, int trackId, float phraseVol) {
    paramUpdateQueue.schedule({ targetFrame, trackId, (int)MOD_SRC_PHRASE_VOL, phraseVol });
}

// ============================================================
// WAVEFORM / METERS
// ============================================================

void AudioEngine::getWaveform(float* outBuffer, int bufferSize) {
    std::lock_guard<std::mutex> lock(waveformMutex);
    for (int i = 0; i < bufferSize && i < WAVEFORM_SIZE; i++) {
        int readIndex = (waveformIndex + i) % WAVEFORM_SIZE;
        outBuffer[i] = waveformBuffer[readIndex];
    }
}

void AudioEngine::getTrackPeaks(float* outBuffer) {
    std::lock_guard<std::mutex> lock(peakMutex);
    for (int i = 0; i < 8; i++) {
        outBuffer[i] = trackPeaks[i];
    }
}

void AudioEngine::getMasterPeaks(float* outBuffer) {
    std::lock_guard<std::mutex> lock(peakMutex);
    outBuffer[0] = masterPeakL;
    outBuffer[1] = masterPeakR;
}

void AudioEngine::decayPeaks() {
    std::lock_guard<std::mutex> lock(peakMutex);
    const float MANUAL_DECAY = 0.92f;

    for (int t = 0; t < 8; t++) {
        trackPeaks[t] *= MANUAL_DECAY;
        if (trackPeaks[t] < 0.001f) trackPeaks[t] = 0.0f;
    }
    masterPeakL *= MANUAL_DECAY;
    masterPeakR *= MANUAL_DECAY;
    if (masterPeakL < 0.001f) masterPeakL = 0.0f;
    if (masterPeakR < 0.001f) masterPeakR = 0.0f;
}

void AudioEngine::decayWaveform() {
    std::lock_guard<std::mutex> lock(waveformMutex);
    const float WAVEFORM_DECAY = 0.90f;

    for (int i = 0; i < WAVEFORM_SIZE; i++) {
        waveformBuffer[i] *= WAVEFORM_DECAY;
        if (fabsf(waveformBuffer[i]) < 0.001f) waveformBuffer[i] = 0.0f;
    }
}

// ============================================================
// VOLUME CONTROL
// ============================================================

void AudioEngine::setTrackVolume(int trackId, float volume) {
    if (trackId < 0 || trackId >= 8) return;
    { std::lock_guard<std::mutex> lock(volumeMutex); trackVolumes[trackId] = volume; }
    SoundfontVoice& sv = sfVoices[trackId];
    sv.trackVolume = volume;
    if (sv.isActive && sv.sfSlot >= 0 && sv.sfSlot < MAX_SOUNDFONTS) {
        std::lock_guard<std::mutex> sfLock(soundfonts[sv.sfSlot].mutex);
        tsf* h = soundfonts[sv.sfSlot].handle;
        if (h) tsf_channel_set_volume(h, trackId, sv.noteVolume * volume);
    }
    LOGD("🔊 Track %d volume set to %.2f", trackId, volume);
}

void AudioEngine::setMasterVolume(float volume) {
    std::lock_guard<std::mutex> lock(volumeMutex);
    masterVolume = volume;
    LOGD("🔊 Master volume set to %.2f", volume);
}

// ============================================================
// PITCH MODULATION METHODS
// ============================================================

IAudioVoice* AudioEngine::findActiveVoiceForTrack(int trackId) {
    if (trackId >= 0 && trackId < 8 && sfVoices[trackId].isActive) {
        return &sfVoices[trackId];
    }
    for (int v = 0; v < MAX_VOICES; v++) {
        if (voices[v].isActive && voices[v].trackId == trackId) {
            return &voices[v];
        }
    }
    return nullptr;
}

void AudioEngine::setPitchSlide(int trackId, float targetSemitones, float durationTicks, int tempo) {
    IAudioVoice* v = findActiveVoiceForTrack(trackId);
    if (!v) return;
    float sr = stream ? (float)stream->getSampleRate() : 44100.0f;
    float framesPerTic = sr / (tempo / 60.0f * 4.0f * 12.0f);
    float totalFrames = fmaxf(1.0f, framesPerTic * durationTicks);
    v->setPitchSlideRaw(targetSemitones, totalFrames);
    LOGD("🎵 Pitch slide: track=%d, to=%.2f over %.0f frames", trackId, targetSemitones, totalFrames);
}

void AudioEngine::setPitchBend(int trackId, float semitonesPerStep, int tempo) {
    IAudioVoice* v = findActiveVoiceForTrack(trackId);
    if (!v) return;
    if (fabsf(semitonesPerStep) < 0.0001f) {
        v->setPitchBendRaw(0.0f);
        LOGD("🎵 Pitch bend stopped: track=%d", trackId);
    } else {
        float sr = stream ? (float)stream->getSampleRate() : 44100.0f;
        float framesPerStep = sr / (tempo / 60.0f * 4.0f * 12.0f) * 12.0f;
        float ratePerFrame = semitonesPerStep / framesPerStep;
        v->setPitchBendRaw(ratePerFrame);
        LOGD("🎵 Pitch bend: track=%d, rate=%.4f semitones/step", trackId, semitonesPerStep);
    }
}

void AudioEngine::setVibrato(int trackId, float speed, float depth) {
    IAudioVoice* v = findActiveVoiceForTrack(trackId);
    if (!v) return;
    v->setVibratoRaw(speed, depth);
    if (depth < 0.01f) {
        LOGD("🎵 Vibrato stopped: track=%d", trackId);
    } else {
        LOGD("🎵 Vibrato: track=%d, speed=%.1fHz, depth=%.2f semitones", trackId, speed, depth);
    }
}

void AudioEngine::clearPitchMod(int trackId) {
    IAudioVoice* v = findActiveVoiceForTrack(trackId);
    if (!v) return;
    v->clearPitchMod();
    LOGD("🎵 Pitch mod cleared: track=%d", trackId);
}

void AudioEngine::setInitialPitchOffset(int trackId, float semitones) {
    IAudioVoice* v = findActiveVoiceForTrack(trackId);
    if (!v) return;
    v->setInitialPitchOffset(semitones);
    LOGD("🎵 Pitch offset set: track=%d, offset=%.2f semitones", trackId, semitones);
}

// ============================================================
// MODULATION METHODS
// ============================================================

void AudioEngine::setInstrumentModulation(int sampleId, int slotIndex,
                                          int type, int dest, float amount,
                                          int attackSamples, int holdSamples, int decaySamples,
                                          float sustainLevel, float lfoHz, int oscShape,
                                          int releaseSamples) {
    if (sampleId < 0 || sampleId >= 256 || slotIndex < 0 || slotIndex >= 4) return;
    InstrumentModSlot& slot = instrumentModSlots[sampleId][slotIndex];
    slot.type = type;
    slot.dest = dest;
    slot.amount = amount;
    slot.attackSamples = attackSamples;
    slot.holdSamples = holdSamples;
    slot.decaySamples = decaySamples;
    slot.sustainLevel = sustainLevel;
    slot.lfoHz = lfoHz;
    slot.oscShape = oscShape;
    slot.releaseSamples = releaseSamples;
}

void AudioEngine::triggerNoteOff(int trackId) {
    for (int v = 0; v < MAX_VOICES; v++) {
        if (!voices[v].isActive || voices[v].trackId != trackId) continue;
        bool hasRelease = false;
        for (int m = 0; m < 4; m++) {
            VoiceModSlot& mod = voices[v].voiceMods[m];
            if (mod.dest == 1 && (mod.type == 2 || mod.type == 5)) {
                if (mod.stage >= 1 && mod.stage <= 3 && mod.releaseSamples > 0) {
                    mod.stage = 4;
                    mod.stageCounter = 0;
                    hasRelease = true;
                } else if (mod.stage == 4) {
                    hasRelease = true;
                }
            }
        }
        if (!hasRelease) {
            voices[v].stop();
        }
    }
}

void AudioEngine::clearInstrumentModulation(int sampleId) {
    if (sampleId < 0 || sampleId >= 256) return;
    for (int m = 0; m < 4; m++) {
        instrumentModSlots[sampleId][m] = InstrumentModSlot();
    }
}

void AudioEngine::updateVoiceModulation(IAudioVoice& voice, int numFrames, float sampleRate) {
    // Snapshot previous dest values for sub-block interpolation (future use).
    memcpy(voice.prevModDestValues, voice.modDestValues, sizeof(float) * PARAM_COUNT);

    // Clear dynamic source slots (ENV0-3, LFO0-3) before each block.
    // Static sources (VELOCITY/KEYTRACK/RANDOM) keep their note-on values.
    // Sequencer sources (TABLE_VOL/PITCH/PITCH_SLIDE/VIBRATO) are written by their state machines
    // earlier in processAudioBlock and must NOT be cleared here.
    for (int m = 0; m < 4; m++) {
        voice.modSourceValues[MOD_SRC_ENV0 + m] = 0.0f;
        voice.modSourceValues[MOD_SRC_LFO0 + m] = 0.0f;
    }
    voice.modSourceValues[MOD_SRC_NONE] = 0.0f;  // Always 0 — required for via=NONE paths.

    float sr = sampleRate;

    // ── Phase 4.4: Mod-to-mod routing ──────────────────────────────────────
    // AMT is additive: modulator contributes +norm*src.amount to the target's base amount.
    // Example: LFO AMT=50 + AHD→MOD_AMT AMT=20 → LFO effective amt sweeps 50→70→50.
    // RATE remains multiplicative (frequency scaling is naturally exponential).
    {
        float amtOffset[4] = {0.0f, 0.0f, 0.0f, 0.0f};
        float rateMult[4]  = {1.0f, 1.0f, 1.0f, 1.0f};
        for (int m = 0; m < 4; m++) {
            const VoiceModSlot& src = voice.voiceMods[m];
            if (src.type == 0 || src.stage == 0) continue;
            if (src.dest != 8 && src.dest != 9 && src.dest != 10) continue;
            int target = (m + 1) % 4;
            float norm = (src.type == 3) ? (src.envValue * 0.5f + 0.5f)
                                          : fmaxf(0.0f, src.envValue);
            if (src.dest == 8 || src.dest == 10) amtOffset[target] += norm * src.amount;
            if (src.dest == 9 || src.dest == 10) {
                float rateScale = fminf(2.0f, norm * src.amount * 2.0f);
                rateMult[target] *= fmaxf(0.05f, rateScale);
            }
        }
        for (int m = 0; m < 4; m++) {
            voice.voiceMods[m].effectiveAmt      = fminf(1.0f, voice.voiceMods[m].amount + amtOffset[m]);
            voice.voiceMods[m].effectiveRateMult = rateMult[m];
        }
    }

    for (int m = 0; m < 4; m++) {
        VoiceModSlot& mod = voice.voiceMods[m];
        if (mod.type == 0 || mod.stage == 0) continue;

        if (mod.type == 1 || mod.type == 4) {
            // ── AHD / DRUM: Attack → Hold → Decay ──
            if (mod.stage == 4) continue;
            mod.stageCounter += numFrames;
            float rMult = fmaxf(0.01f, mod.effectiveRateMult);
            int effAttack = mod.attackSamples > 0 ? (int)fmaxf(1.0f, mod.attackSamples / rMult) : 0;
            int effHold   = mod.holdSamples   > 0 ? (int)fmaxf(1.0f, mod.holdSamples   / rMult) : 0;
            int effDecay  = mod.decaySamples  > 0 ? (int)fmaxf(1.0f, mod.decaySamples  / rMult) : 0;
            switch (mod.stage) {
                case 1: // Attack: ramp 0 → 1
                    if (effAttack > 0) {
                        mod.envValue = fminf(1.0f, (float)mod.stageCounter / effAttack);
                        if (mod.stageCounter >= effAttack) {
                            mod.envValue = 1.0f;
                            mod.stage = 2; mod.stageCounter = 0;
                        }
                    } else {
                        mod.envValue = 1.0f;
                        mod.stage = 2; mod.stageCounter = 0;
                    }
                    break;
                case 2: // Hold: stay at 1
                    mod.envValue = 1.0f;
                    if (effHold == 0 || mod.stageCounter >= effHold) {
                        mod.stage = 3; mod.stageCounter = 0;
                    }
                    break;
                case 3: // Decay: ramp 1 → 0
                    if (effDecay > 0) {
                        mod.envValue = fmaxf(0.0f, 1.0f - (float)mod.stageCounter / effDecay);
                        if (mod.stageCounter >= effDecay) {
                            mod.envValue = 0.0f;
                            mod.stage = 4;
                        }
                    } else {
                        mod.envValue = 0.0f;
                        mod.stage = 4;
                    }
                    break;
            }

        } else if (mod.type == 2 || mod.type == 5) {
            // ── ADSR / TRIG: Attack → Decay → Sustain → Release ──
            if (mod.stage == 5) continue;
            mod.stageCounter += numFrames;
            float rMult    = fmaxf(0.01f, mod.effectiveRateMult);
            int effAttack  = mod.attackSamples  > 0 ? (int)fmaxf(1.0f, mod.attackSamples  / rMult) : 0;
            int effDecay   = mod.decaySamples   > 0 ? (int)fmaxf(1.0f, mod.decaySamples   / rMult) : 0;
            int effRelease = mod.releaseSamples > 0 ? (int)fmaxf(1.0f, mod.releaseSamples / rMult) : 0;
            switch (mod.stage) {
                case 1: // Attack: ramp 0 → 1
                    if (effAttack > 0) {
                        mod.envValue = fminf(1.0f, (float)mod.stageCounter / effAttack);
                        if (mod.stageCounter >= effAttack) {
                            mod.envValue = 1.0f;
                            mod.stage = 2; mod.stageCounter = 0;
                        }
                    } else {
                        mod.envValue = 1.0f;
                        mod.stage = 2; mod.stageCounter = 0;
                    }
                    break;
                case 2: // Decay: ramp 1 → sustainLevel
                    if (effDecay > 0) {
                        float t = fminf(1.0f, (float)mod.stageCounter / effDecay);
                        mod.envValue = 1.0f - t * (1.0f - mod.sustainLevel);
                        if (mod.stageCounter >= effDecay) {
                            mod.envValue = mod.sustainLevel;
                            mod.stage = 3; mod.stageCounter = 0;
                        }
                    } else {
                        mod.envValue = mod.sustainLevel;
                        mod.stage = 3; mod.stageCounter = 0;
                    }
                    break;
                case 3: // Sustain: hold at sustainLevel (until triggerNoteOff)
                    mod.envValue = mod.sustainLevel;
                    break;
                case 4: // Release: ramp sustainLevel → 0
                    if (effRelease > 0) {
                        mod.envValue = fmaxf(0.0f,
                            mod.sustainLevel * (1.0f - (float)mod.stageCounter / effRelease));
                        if (mod.stageCounter >= effRelease) {
                            mod.envValue = 0.0f;
                            mod.stage = 5;
                        }
                    } else {
                        mod.envValue = 0.0f;
                        mod.stage = 5;
                    }
                    break;
            }

        } else if (mod.type == 3) {
            // ── LFO: phase-based oscillator ──
            float effHz       = mod.lfoHz * mod.effectiveRateMult;
            float phaseAdvance = 2.0f * (float)M_PI * effHz / sr * numFrames;
            mod.lfoPhase += phaseAdvance;
            while (mod.lfoPhase >= 2.0f * (float)M_PI) mod.lfoPhase -= 2.0f * (float)M_PI;

            float norm = mod.lfoPhase / (2.0f * (float)M_PI);
            switch (mod.oscShape) {
                case 0: // TRI
                    if      (norm < 0.25f) mod.envValue = norm * 4.0f;
                    else if (norm < 0.75f) mod.envValue = 1.0f - (norm - 0.25f) * 4.0f;
                    else                   mod.envValue = (norm - 1.0f) * 4.0f;
                    break;
                case 1: // SIN
                    mod.envValue = sinf(mod.lfoPhase);
                    break;
                case 2: // RMP+ (sawtooth rising)
                    mod.envValue = norm * 2.0f - 1.0f;
                    break;
                case 3: // RMP- (sawtooth falling)
                    mod.envValue = 1.0f - norm * 2.0f;
                    break;
                case 6: // SQU+
                    mod.envValue = (norm < 0.5f) ? 1.0f : -1.0f;
                    break;
                case 7: // SQU-
                    mod.envValue = (norm < 0.5f) ? -1.0f : 1.0f;
                    break;
                default: // EXP+/EXP-/RND/DRNK — fall back to SIN
                    mod.envValue = sinf(mod.lfoPhase);
                    break;
            }
        } else if (mod.type == 6) {
            // ── SCALAR: constant output, no state advance ──
            // mod.amount (0.0–1.0) is the fixed value set at note-on.
            // stage=1 is set at trigger so this branch is reached every block.
            mod.envValue = mod.amount;
        }

        // Write envValue to source array.
        // VOL (dest=1) is handled per-sample in the mix loop via prevEnvValue — not routed here.
        // MOD_* (dest≥7) are handled by the mod-to-mod system above — not routed here.
        // SCALAR (type=6) reuses the LFO slot (same as LFO) — it's a degenerate LFO that never oscillates.
        ModSourceId srcId = (mod.type == 3 || mod.type == 6)
            ? (ModSourceId)(MOD_SRC_LFO0 + m)
            : (ModSourceId)(MOD_SRC_ENV0 + m);
        voice.modSourceValues[srcId] = mod.envValue;
    }

    // ── Build routes and call processRoutes ─────────────────────────────────
    // Capacity: 4 user routes + 4 fixed sequencer routes = 8 total.
    // User routes: from VoiceModSlot configs (rebuilt each block because effectiveAmt changes).
    // Fixed routes: always-on connections from sequencer sources (table/pitch/vibrato).
    // VOL (dest=1) and MOD_* (dest≥7) destinations are excluded from user routes.
    ModRoute routes[8];
    int routeCount = 0;

    // User routes (from instrument mod matrix)
    for (int m = 0; m < 4; m++) {
        const VoiceModSlot& mod = voice.voiceMods[m];
        if (mod.type == 0) continue;
        if (mod.dest == 0 || mod.dest == 1) continue;  // NONE or VOL (per-sample path)
        if (mod.dest >= 7) continue;                    // STA / MOD_AMT / MOD_RATE / MOD_BOTH

        ModSourceId srcId = (mod.type == 3 || mod.type == 6)
            ? (ModSourceId)(MOD_SRC_LFO0 + m)
            : (ModSourceId)(MOD_SRC_ENV0 + m);
        ParamId destId;
        float   scale;
        switch (mod.dest) {
            case 2: destId = PARAM_PAN;        scale = mod.effectiveAmt * 0.5f;   break;
            case 3: destId = PARAM_PITCH;      scale = mod.effectiveAmt * 12.0f;  break;
            case 4: destId = PARAM_PITCH;      scale = mod.effectiveAmt * 1.0f;   break;
            case 5: destId = PARAM_FILTER_CUT; scale = mod.effectiveAmt * 255.0f; break;
            case 6: destId = PARAM_FILTER_RES; scale = mod.effectiveAmt * 255.0f; break;
            default: continue;
        }
        routes[routeCount++] = { srcId, destId, scale, MOD_SRC_NONE, 0.0f };
    }

    // Fixed routes (always-on; fire whenever source is non-zero)
    // TABLE_PITCH and PITCH_SLIDE both add semitones; VIBRATO adds ±depth semitones.
    // TABLE_VOL multiplies note volume (processRoutes writes modDestValues[PARAM_VOL];
    //   mix loop reads it instead of voice.tableVolume).
    routes[routeCount++] = { MOD_SRC_TABLE_PITCH, PARAM_PITCH, 1.0f, MOD_SRC_NONE,        0.0f };
    routes[routeCount++] = { MOD_SRC_PITCH_SLIDE, PARAM_PITCH, 1.0f, MOD_SRC_NONE,        0.0f };
    routes[routeCount++] = { MOD_SRC_VIBRATO,     PARAM_PITCH, 1.0f, MOD_SRC_NONE,        0.0f };
    // VOL: TABLE_VOL × PHRASE_VOL × instrVol (via-based multiplication, Surge XT pattern)
    // instrVol = params.base[PARAM_VOL] (set at trigger from instrument volume field)
    // phraseVol = modSourceValues[MOD_SRC_PHRASE_VOL] (set at trigger from phrase step volume)
    // tableVol  = modSourceValues[MOD_SRC_TABLE_VOL]  (updated each block by table engine)
    routes[routeCount++] = { MOD_SRC_TABLE_VOL,   PARAM_VOL,
                              voice.params.base[PARAM_VOL],   // depth = instrVol
                              MOD_SRC_PHRASE_VOL, 1.0f };     // via = phraseVol (full multiplication)

    processRoutes(voice.modSourceValues, voice.modDestValues, routes, routeCount);

    // Bridge: copy modDestValues into params.mod[] so existing mix-loop reads of
    // params.get(PARAM_PAN/PITCH/FILTER_CUT/FILTER_RES) continue to work unchanged.
    // Phase 2+ will migrate the mix loop to read modDestValues[] directly.
    for (int p = 0; p < PARAM_COUNT; p++) {
        voice.params.mod[p] = voice.modDestValues[p];
    }
}

void AudioEngine::updateVoicePitchMod(Voice& voice, int numFrames, float sampleRate) {
    // Process pitch slide (PSL / PBN)
    if (voice.pitchSliding) {
        float delta = voice.pitchSlideTarget - voice.pitchOffset;
        float totalDelta = voice.pitchSlideRate * numFrames;
        if (fabsf(totalDelta) >= fabsf(delta)) {
            voice.pitchOffset = voice.pitchSlideTarget;
            if (fabsf(voice.pitchSlideTarget) < 100.0f) {
                voice.pitchSliding = false;
            }
        } else {
            voice.pitchOffset += totalDelta;
        }
    }
    // Write pitch slide output to source array (picked up by processRoutes in updateVoiceModulation).
    voice.modSourceValues[MOD_SRC_PITCH_SLIDE] = voice.pitchOffset;

    // Process vibrato LFO (PVB / PVX)
    if (voice.vibratoActive) {
        float phaseIncrement = (2.0f * (float)M_PI * voice.vibratoSpeed / sampleRate) * numFrames;
        voice.vibratoPhase += phaseIncrement;
        while (voice.vibratoPhase >= 2.0f * (float)M_PI) {
            voice.vibratoPhase -= 2.0f * (float)M_PI;
        }
    }
    // Write vibrato output to source array. 0.0f when inactive.
    voice.modSourceValues[MOD_SRC_VIBRATO] = voice.vibratoActive
        ? sinf(voice.vibratoPhase) * voice.vibratoDepth
        : 0.0f;
}

float AudioEngine::getModulatedPlaybackRate(Voice& voice) {
    // modDestValues[PARAM_PITCH] accumulates all pitch sources via processRoutes:
    //   TABLE_PITCH (table row transpose) + PITCH_SLIDE (PSL/PBN) +
    //   VIBRATO (PVB/PVX) + user mod slots targeting PARAM_PITCH.
    // voice.playbackRate = basePlaybackRate (no transpose baked in after Phase 2),
    //   or arpeggio-adjusted rate from setMidiNote().
    float rateMod = powf(2.0f, voice.modDestValues[PARAM_PITCH] / 12.0f);
    return voice.playbackRate * rateMod;
}

// ============================================================
// OFFLINE RENDER
// ============================================================

void AudioEngine::renderOffline(int numFrames, float* output, int sampleRate) {
    for (int i = 0; i < numFrames * 2; i++) output[i] = 0.0f;

    const int BLOCK_SIZE = 256;
    int rendered = 0;
    while (rendered < numFrames) {
        int chunk = std::min(BLOCK_SIZE, numFrames - rendered);
        processAudioBlock(output + rendered * 2, chunk, 2, (float)sampleRate);
        rendered += chunk;
    }
}

void AudioEngine::resetFrameCounter() {
    globalFrameCounter = 0;
}

int64_t AudioEngine::getFrameCounter() {
    return globalFrameCounter;
}

void AudioEngine::setOfflineRendering(bool offline) {
    isOfflineRendering.store(offline);
    LOGD("🎬 Offline rendering: %s", offline ? "ON" : "OFF");
}
