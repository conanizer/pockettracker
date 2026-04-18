// jni-bridge.cpp — JNI EXPORT functions (Phase 0 file split)
// All JNI entry points from Kotlin/Java into the AudioEngine.
// TSF_IMPLEMENTATION lives in soundfont-voice.cpp; tsf* is opaque here (no struct member access).
#include <jni.h>
#include <climits>
#include <vector>
#include "audio-engine.h"
#include "tsf.h"  // API declarations only (tsf_close, tsf_load_filename, etc.)

// JNI requires env and thiz in every function signature, but most thin wrappers don't use them.
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunused-parameter"

static AudioEngine* engine = nullptr;

extern "C" {

JNIEXPORT jboolean JNICALL
Java_com_conanizer_pockettracker_TrackerAudioEngine_native_1create(JNIEnv *env, jobject thiz) {
    if (!engine) {
        engine = new AudioEngine();
        return engine->openStream() ? JNI_TRUE : JNI_FALSE;
    }
    return JNI_FALSE;
}

JNIEXPORT void JNICALL
Java_com_conanizer_pockettracker_TrackerAudioEngine_native_1delete(JNIEnv *env, jobject thiz) {
    delete engine;
    engine = nullptr;
}

JNIEXPORT void JNICALL
Java_com_conanizer_pockettracker_TrackerAudioEngine_native_1loadSample(
        JNIEnv *env, jobject thiz, jint id, jfloatArray data) {
    if (!engine) return;

    jsize len = env->GetArrayLength(data);
    jfloat* arr = env->GetFloatArrayElements(data, nullptr);

    engine->loadSample(id, arr, len);

    env->ReleaseFloatArrayElements(data, arr, JNI_ABORT);
}

JNIEXPORT void JNICALL
Java_com_conanizer_pockettracker_TrackerAudioEngine_native_1triggerNote(
        JNIEnv *env, jobject thiz, jint sid, jint tid, jfloat f, jfloat bf, jfloat v) {
    if (engine) {
        engine->triggerNote(sid, tid, f, bf, v);
    }
}

JNIEXPORT void JNICALL
Java_com_conanizer_pockettracker_TrackerAudioEngine_native_1stopTrack(JNIEnv *env, jobject thiz, jint tid) {
    if (engine) {
        engine->stopTrack(tid);
    }
}

JNIEXPORT void JNICALL
Java_com_conanizer_pockettracker_TrackerAudioEngine_native_1stopAll(JNIEnv *env, jobject thiz) {
    if (engine) {
        engine->stopAll();
    }
}

JNIEXPORT jint JNICALL
Java_com_conanizer_pockettracker_TrackerAudioEngine_native_1getActiveVoiceCount(JNIEnv *env, jobject thiz) {
    if (engine) {
        return engine->getActiveVoiceCount();
    }
    return 0;
}

JNIEXPORT jint JNICALL
Java_com_conanizer_pockettracker_TrackerAudioEngine_native_1getSampleRate(JNIEnv *env, jobject thiz) {
    if (engine) {
        return engine->getSampleRate();
    }
    return 48000;
}

JNIEXPORT void JNICALL
Java_com_conanizer_pockettracker_TrackerAudioEngine_native_1setInstrumentParams(
        JNIEnv *env, jobject thiz, jint instrumentId, jint start, jint end,
        jboolean reverse, jint loopMode, jint loopStart,
        jint drive, jint crush, jint downsample,
        jint filterType, jint filterCut, jint filterRes) {
    if (engine) {
        engine->setInstrumentParams(instrumentId, start, end, reverse, loopMode, loopStart,
                                    drive, crush, downsample, filterType, filterCut, filterRes);
    }
}

// ===================================
// PHASE 1: NOTE QUEUE JNI METHODS
// ===================================

JNIEXPORT jlong JNICALL
Java_com_conanizer_pockettracker_TrackerAudioEngine_native_1getCurrentFrame(JNIEnv *env, jobject thiz) {
    if (engine) {
        return (jlong)engine->getCurrentFrame();
    }
    return 0;
}

JNIEXPORT void JNICALL
Java_com_conanizer_pockettracker_TrackerAudioEngine_native_1scheduleNote(
        JNIEnv *env, jobject thiz, jlong targetFrame, jint sampleId, jint trackId,
        jfloat frequency, jfloat baseFrequency, jfloat volume) {
    if (engine) {
        engine->scheduleNote(targetFrame, sampleId, trackId, frequency, baseFrequency, volume);
    }
}

JNIEXPORT void JNICALL
Java_com_conanizer_pockettracker_TrackerAudioEngine_native_1clearScheduledNotes(JNIEnv *env, jobject thiz) {
    if (engine) {
        engine->clearScheduledNotes();
    }
}

JNIEXPORT void JNICALL
Java_com_conanizer_pockettracker_TrackerAudioEngine_native_1resumeStream(JNIEnv *env, jobject thiz) {
    if (engine) {
        engine->resumeStream();
    }
}

JNIEXPORT void JNICALL
Java_com_conanizer_pockettracker_TrackerAudioEngine_native_1getWaveform(JNIEnv *env, jobject thiz, jfloatArray outArray) {
    if (engine && outArray != nullptr) {
        jsize length = env->GetArrayLength(outArray);
        float* buffer = new float[length];

        engine->getWaveform(buffer, length);

        env->SetFloatArrayRegion(outArray, 0, length, buffer);

        delete[] buffer;
    }
}

// ═══════════════════════════════════════════════════════════════════════════
// JNI METHODS FOR OboeAudioBackend
// ═══════════════════════════════════════════════════════════════════════════

JNIEXPORT jboolean JNICALL
Java_com_conanizer_pockettracker_platform_android_OboeAudioBackend_native_1create(JNIEnv *env, jobject thiz) {
    if (!engine) {
        engine = new AudioEngine();
        return engine->openStream() ? JNI_TRUE : JNI_FALSE;
    }
    return JNI_FALSE;
}

JNIEXPORT void JNICALL
Java_com_conanizer_pockettracker_platform_android_OboeAudioBackend_native_1delete(JNIEnv *env, jobject thiz) {
    if (engine) {
        delete engine;
        engine = nullptr;
        LOGD("✅ Audio engine deleted");
    }
}

JNIEXPORT void JNICALL
Java_com_conanizer_pockettracker_platform_android_OboeAudioBackend_native_1loadSample(
        JNIEnv *env, jobject thiz, jint id, jfloatArray data) {
    if (!engine) return;

    jsize len = env->GetArrayLength(data);
    jfloat* arr = env->GetFloatArrayElements(data, nullptr);

    engine->loadSample(id, arr, len);

    env->ReleaseFloatArrayElements(data, arr, JNI_ABORT);
}

JNIEXPORT void JNICALL
Java_com_conanizer_pockettracker_platform_android_OboeAudioBackend_native_1clearAllSamples(
        JNIEnv *env, jobject thiz) {
    if (engine) {
        engine->clearAllSamples();
    }
}

JNIEXPORT jintArray JNICALL
Java_com_conanizer_pockettracker_platform_android_OboeAudioBackend_native_1getTrackActiveNotes(
        JNIEnv *env, jobject thiz) {
    jintArray result = env->NewIntArray(8);
    if (!result) return result;
    int notes[8] = { -1, -1, -1, -1, -1, -1, -1, -1 };
    if (engine) {
        engine->getTrackActiveNotes(notes, 8);
    }
    env->SetIntArrayRegion(result, 0, 8, notes);
    return result;
}

JNIEXPORT void JNICALL
Java_com_conanizer_pockettracker_platform_android_OboeAudioBackend_native_1scheduleNote(
        JNIEnv *env, jobject thiz, jlong targetFrame, jint sampleId, jint trackId,
        jfloat frequency, jfloat baseFrequency, jfloat volume, jfloat pan, jint startPointOverride) {
    if (engine) {
        engine->scheduleNote(targetFrame, sampleId, trackId, frequency, baseFrequency, volume, /*phraseVolume=*/1.0f, pan, startPointOverride);
    }
}

JNIEXPORT jlong JNICALL
Java_com_conanizer_pockettracker_platform_android_OboeAudioBackend_native_1getCurrentFrame(JNIEnv *env, jobject thiz) {
    if (engine) {
        return (jlong)engine->getCurrentFrame();
    }
    return 0;
}

JNIEXPORT void JNICALL
Java_com_conanizer_pockettracker_platform_android_OboeAudioBackend_native_1clearScheduledNotes(JNIEnv *env, jobject thiz) {
    if (engine) {
        engine->clearScheduledNotes();
    }
}

JNIEXPORT void JNICALL
Java_com_conanizer_pockettracker_platform_android_OboeAudioBackend_native_1clearScheduledNotesFrom(
        JNIEnv *env, jobject thiz, jlong fromFrame) {
    if (engine) {
        engine->clearScheduledNotesFrom((int64_t)fromFrame);
    }
}

JNIEXPORT void JNICALL
Java_com_conanizer_pockettracker_platform_android_OboeAudioBackend_native_1resumeStream(JNIEnv *env, jobject thiz) {
    if (engine) {
        engine->resumeStream();
    }
}

JNIEXPORT void JNICALL
Java_com_conanizer_pockettracker_platform_android_OboeAudioBackend_native_1stopAll(JNIEnv *env, jobject thiz) {
    if (engine) {
        engine->stopAll();
    }
}

JNIEXPORT void JNICALL
Java_com_conanizer_pockettracker_platform_android_OboeAudioBackend_native_1killTrack(JNIEnv *env, jobject thiz, jint trackId) {
    if (engine) {
        engine->stopTrack(trackId);
    }
}

JNIEXPORT void JNICALL
Java_com_conanizer_pockettracker_platform_android_OboeAudioBackend_native_1scheduleKill(JNIEnv *env, jobject thiz, jlong targetFrame, jint trackId) {
    if (engine) {
        engine->scheduleKill(targetFrame, trackId);
    }
}

JNIEXPORT jint JNICALL
Java_com_conanizer_pockettracker_platform_android_OboeAudioBackend_native_1getSampleRate(JNIEnv *env, jobject thiz) {
    if (engine) {
        return engine->getSampleRate();
    }
    return 44100;
}

JNIEXPORT void JNICALL
Java_com_conanizer_pockettracker_platform_android_OboeAudioBackend_native_1getWaveform(JNIEnv *env, jobject thiz, jfloatArray outArray) {
    if (engine && outArray != nullptr) {
        jsize length = env->GetArrayLength(outArray);
        float* buffer = new float[length];

        engine->getWaveform(buffer, length);

        env->SetFloatArrayRegion(outArray, 0, length, buffer);

        delete[] buffer;
    }
}

JNIEXPORT void JNICALL
Java_com_conanizer_pockettracker_platform_android_OboeAudioBackend_native_1setInstrumentParams(
        JNIEnv *env, jobject thiz, jint instrumentId, jint start, jint end,
        jboolean reverse, jint loopMode, jint loopStart,
        jint drive, jint crush, jint downsample,
        jint filterType, jint filterCut, jint filterRes) {
    if (engine) {
        engine->setInstrumentParams(instrumentId, start, end, reverse, loopMode, loopStart,
                                    drive, crush, downsample, filterType, filterCut, filterRes);
    }
}

// ===================================
// MIXER PEAK METER JNI METHODS
// ===================================

JNIEXPORT void JNICALL
Java_com_conanizer_pockettracker_platform_android_OboeAudioBackend_native_1getTrackPeaks(JNIEnv *env, jobject thiz, jfloatArray outArray) {
    if (engine && outArray != nullptr) {
        float buffer[8];
        engine->getTrackPeaks(buffer);
        env->SetFloatArrayRegion(outArray, 0, 8, buffer);
    }
}

JNIEXPORT void JNICALL
Java_com_conanizer_pockettracker_platform_android_OboeAudioBackend_native_1getMasterPeaks(JNIEnv *env, jobject thiz, jfloatArray outArray) {
    if (engine && outArray != nullptr) {
        float buffer[2];
        engine->getMasterPeaks(buffer);
        env->SetFloatArrayRegion(outArray, 0, 2, buffer);
    }
}

// ===================================
// OFFLINE RENDER JNI METHODS (for WAV export)
// ===================================

JNIEXPORT jfloatArray JNICALL
Java_com_conanizer_pockettracker_platform_android_OboeAudioBackend_native_1renderFrames(
        JNIEnv *env, jobject thiz, jint numFrames, jint sampleRate) {
    if (!engine) {
        return nullptr;
    }

    jfloatArray result = env->NewFloatArray(numFrames * 2);
    if (result == nullptr) {
        return nullptr;
    }

    std::vector<float> buffer(numFrames * 2);

    engine->renderOffline(numFrames, buffer.data(), sampleRate);

    env->SetFloatArrayRegion(result, 0, numFrames * 2, buffer.data());

    return result;
}

JNIEXPORT void JNICALL
Java_com_conanizer_pockettracker_platform_android_OboeAudioBackend_native_1resetFrameCounter(JNIEnv *env, jobject thiz) {
    if (engine) {
        engine->resetFrameCounter();
    }
}

JNIEXPORT jlong JNICALL
Java_com_conanizer_pockettracker_platform_android_OboeAudioBackend_native_1getFrameCounter(JNIEnv *env, jobject thiz) {
    if (engine) {
        return engine->getFrameCounter();
    }
    return 0;
}

// ===================================
// PHASE 1 BUG FIXES: DECAY AND REAL-TIME VOLUME
// ===================================

JNIEXPORT void JNICALL
Java_com_conanizer_pockettracker_platform_android_OboeAudioBackend_native_1decayPeaks(JNIEnv *env, jobject thiz) {
    if (engine) {
        engine->decayPeaks();
    }
}

JNIEXPORT void JNICALL
Java_com_conanizer_pockettracker_platform_android_OboeAudioBackend_native_1decayWaveform(JNIEnv *env, jobject thiz) {
    if (engine) {
        engine->decayWaveform();
    }
}

JNIEXPORT void JNICALL
Java_com_conanizer_pockettracker_platform_android_OboeAudioBackend_native_1setTrackVolume(JNIEnv *env, jobject thiz, jint trackId, jfloat volume) {
    if (engine) {
        engine->setTrackVolume(trackId, volume);
    }
}

JNIEXPORT void JNICALL
Java_com_conanizer_pockettracker_platform_android_OboeAudioBackend_native_1setMasterVolume(JNIEnv *env, jobject thiz, jfloat volume) {
    if (engine) {
        engine->setMasterVolume(volume);
    }
}

// ===================================
// TABLE JNI METHODS (Phase 3.5)
// ===================================

JNIEXPORT void JNICALL
Java_com_conanizer_pockettracker_platform_android_OboeAudioBackend_native_1loadTable(
        JNIEnv *env, jobject thiz, jint tableId, jbyteArray rowData) {
    if (!engine || rowData == nullptr) return;

    jsize len = env->GetArrayLength(rowData);
    if (len != 128) {
        LOGE("❌ loadTable: Invalid rowData length %d (expected 128)", len);
        return;
    }

    jbyte* data = env->GetByteArrayElements(rowData, nullptr);
    engine->loadTable(tableId, reinterpret_cast<uint8_t*>(data));
    env->ReleaseByteArrayElements(rowData, data, JNI_ABORT);
}

JNIEXPORT void JNICALL
Java_com_conanizer_pockettracker_platform_android_OboeAudioBackend_native_1scheduleNoteWithTable(
        JNIEnv *env, jobject thiz, jlong targetFrame, jint sampleId, jint trackId,
        jfloat frequency, jfloat baseFrequency, jfloat volume, jfloat phraseVolume, jfloat pan,
        jint startPointOverride, jint tableId, jint tableTicRate,
        jint noteOctave, jint notePitch,
        jfloat pslInitialOffset, jfloat pslDuration,
        jfloat pbnRate, jfloat vibratoSpeed, jfloat vibratoDepth,
        jint tableStartRow) {
    if (engine) {
        engine->scheduleNote(targetFrame, sampleId, trackId, frequency, baseFrequency,
                             volume, phraseVolume, pan, startPointOverride, tableId, tableTicRate,
                             noteOctave, notePitch,
                             pslInitialOffset, pslDuration, pbnRate, vibratoSpeed, vibratoDepth,
                             tableStartRow);
    }
}

JNIEXPORT void JNICALL
Java_com_conanizer_pockettracker_platform_android_OboeAudioBackend_native_1setVoiceTableRow(
        JNIEnv *env, jobject thiz, jint trackId, jint row) {
    if (engine) {
        engine->setVoiceTableRow(trackId, row);
    }
}

JNIEXPORT void JNICALL
Java_com_conanizer_pockettracker_platform_android_OboeAudioBackend_native_1scheduleTrackPhraseVol(
        JNIEnv *env, jobject thiz, jlong targetFrame, jint trackId, jfloat phraseVol) {
    if (engine) {
        engine->scheduleTrackPhraseVol(targetFrame, trackId, phraseVol);
    }
}

JNIEXPORT jint JNICALL
Java_com_conanizer_pockettracker_platform_android_OboeAudioBackend_native_1getVoiceTableRow(
        JNIEnv *env, jobject thiz, jint trackId) {
    if (engine) {
        return engine->getVoiceTableRow(trackId);
    }
    return -1;
}

JNIEXPORT jint JNICALL
Java_com_conanizer_pockettracker_platform_android_OboeAudioBackend_native_1getVoiceTableId(
        JNIEnv *env, jobject thiz, jint trackId) {
    if (engine) {
        return engine->getVoiceTableId(trackId);
    }
    return -1;
}

// ===================================
// PITCH MODULATION JNI METHODS (Phase 6)
// ===================================

JNIEXPORT void JNICALL
Java_com_conanizer_pockettracker_platform_android_OboeAudioBackend_native_1setPitchSlide(
        JNIEnv *env, jobject thiz, jint trackId, jfloat targetSemitones, jfloat durationTicks, jint tempo) {
    if (engine) {
        engine->setPitchSlide(trackId, targetSemitones, durationTicks, tempo);
    }
}

JNIEXPORT void JNICALL
Java_com_conanizer_pockettracker_platform_android_OboeAudioBackend_native_1setPitchBend(
        JNIEnv *env, jobject thiz, jint trackId, jfloat semitonesPerTick, jint tempo) {
    if (engine) {
        engine->setPitchBend(trackId, semitonesPerTick, tempo);
    }
}

JNIEXPORT void JNICALL
Java_com_conanizer_pockettracker_platform_android_OboeAudioBackend_native_1setVibrato(
        JNIEnv *env, jobject thiz, jint trackId, jfloat speed, jfloat depth) {
    if (engine) {
        engine->setVibrato(trackId, speed, depth);
    }
}

JNIEXPORT void JNICALL
Java_com_conanizer_pockettracker_platform_android_OboeAudioBackend_native_1clearPitchMod(
        JNIEnv *env, jobject thiz, jint trackId) {
    if (engine) {
        engine->clearPitchMod(trackId);
    }
}

JNIEXPORT void JNICALL
Java_com_conanizer_pockettracker_platform_android_OboeAudioBackend_native_1setInitialPitchOffset(
        JNIEnv *env, jobject thiz, jint trackId, jfloat semitones) {
    if (engine) {
        engine->setInitialPitchOffset(trackId, semitones);
    }
}

JNIEXPORT void JNICALL
Java_com_conanizer_pockettracker_platform_android_OboeAudioBackend_native_1setInstrumentModulation(
        JNIEnv *env, jobject thiz,
        jint sampleId, jint slotIndex, jint type, jint dest, jfloat amount,
        jint attackSamples, jint holdSamples, jint decaySamples,
        jfloat sustainLevel, jfloat lfoHz, jint oscShape, jint releaseSamples) {
    if (engine) {
        engine->setInstrumentModulation(sampleId, slotIndex, type, dest, amount,
                                        attackSamples, holdSamples, decaySamples,
                                        sustainLevel, lfoHz, oscShape, releaseSamples);
    }
}

JNIEXPORT void JNICALL
Java_com_conanizer_pockettracker_platform_android_OboeAudioBackend_native_1clearInstrumentModulation(
        JNIEnv *env, jobject thiz, jint sampleId) {
    if (engine) {
        engine->clearInstrumentModulation(sampleId);
    }
}

JNIEXPORT void JNICALL
Java_com_conanizer_pockettracker_platform_android_OboeAudioBackend_native_1triggerNoteOff(
        JNIEnv *env, jobject thiz, jint trackId) {
    if (engine) {
        engine->triggerNoteOff(trackId);
    }
}

JNIEXPORT void JNICALL
Java_com_conanizer_pockettracker_platform_android_OboeAudioBackend_native_1scheduleNoteOff(
        JNIEnv *env, jobject thiz, jlong targetFrame, jint trackId) {
    if (engine) {
        engine->scheduleNoteOff(targetFrame, trackId);
    }
}

JNIEXPORT void JNICALL
Java_com_conanizer_pockettracker_platform_android_OboeAudioBackend_native_1setOfflineRendering(
        JNIEnv *env, jobject thiz, jboolean rendering) {
    if (engine) engine->setOfflineRendering(rendering == JNI_TRUE);
}

// ===================================
// SOUNDFONT JNI FUNCTIONS
// ===================================

JNIEXPORT jint JNICALL
Java_com_conanizer_pockettracker_platform_android_OboeAudioBackend_native_1loadSoundfont(
        JNIEnv *env, jobject thiz, jint instrumentId, jstring path) {
    const char* pathStr = env->GetStringUTFChars(path, nullptr);
    if (!pathStr) return -1;

    // Find a free slot; if none, evict the slot whose instrumentId is lowest (oldest heuristic)
    int slot = -1;
    for (int i = 0; i < MAX_SOUNDFONTS; i++) {
        if (soundfonts[i].handle == nullptr) {
            slot = i;
            break;
        }
    }
    if (slot == -1) {
        // Evict slot with smallest instrumentId (oldest loaded)
        int minId = INT_MAX;
        for (int i = 0; i < MAX_SOUNDFONTS; i++) {
            if (soundfonts[i].instrumentId < minId) {
                minId = soundfonts[i].instrumentId;
                slot = i;
            }
        }
        // Free the evicted slot
        std::lock_guard<std::mutex> sfLock(soundfonts[slot].mutex);
        tsf_close(soundfonts[slot].handle);
        soundfonts[slot].handle = nullptr;
        soundfonts[slot].instrumentId = -1;
        soundfonts[slot].filePath.clear();
        // Detach any per-track voices that were using this slot
        for (int t = 0; t < 8; t++) {
            if (sfVoices[t].sfSlot == slot) sfVoices[t].detach();
        }
        LOGD("🎹 Evicted soundfont slot %d to make room for instrumentId %d", slot, (int)instrumentId);
    }

    // Load and parse the SF2 file into a single master TSF handle.
    // All 8 tracks share this handle via MIDI channels — no per-track clones needed.
    {
        std::lock_guard<std::mutex> sfLock(soundfonts[slot].mutex);
        soundfonts[slot].handle = tsf_load_filename(pathStr);
        if (!soundfonts[slot].handle) {
            LOGE("❌ Failed to parse soundfont: %s", pathStr);
            env->ReleaseStringUTFChars(path, pathStr);
            return -1;
        }
        int sampleRate = engine ? engine->getSampleRate() : 44100;
        tsf_set_output(soundfonts[slot].handle, TSF_STEREO_INTERLEAVED, sampleRate, 0.0f);
        soundfonts[slot].instrumentId = (int)instrumentId;
        soundfonts[slot].filePath = pathStr;
        LOGD("🎹 Loaded soundfont slot %d: %s (instrumentId=%d)", slot, pathStr, (int)instrumentId);
    }

    env->ReleaseStringUTFChars(path, pathStr);
    LOGD("🎹 Loaded soundfont slot %d for instrumentId %d", slot, (int)instrumentId);
    return slot;
}

JNIEXPORT void JNICALL
Java_com_conanizer_pockettracker_platform_android_OboeAudioBackend_native_1setSoundfontPreset(
        JNIEnv *env, jobject thiz, jint sfSlot, jint bank, jint preset) {
    if (sfSlot < 0 || sfSlot >= MAX_SOUNDFONTS || !soundfonts[sfSlot].handle) return;
    // Preset is set per-note via tsf_channel_set_presetnumber; this is a no-op stored in Kotlin.
    LOGD("🎹 setSoundfontPreset slot=%d bank=%d preset=%d (applied per-note)", (int)sfSlot, (int)bank, (int)preset);
}

JNIEXPORT void JNICALL
Java_com_conanizer_pockettracker_platform_android_OboeAudioBackend_native_1setSoundfontEnvelopeOverrides(
        JNIEnv *env, jobject thiz, jint sfSlot, jint bank, jint preset,
        jint atk, jint dec, jint sus, jint rel) {
    if (sfSlot < 0 || sfSlot >= MAX_SOUNDFONTS || !soundfonts[sfSlot].handle) return;
    std::lock_guard<std::mutex> sfLock(soundfonts[sfSlot].mutex);
    tsf_preset_apply_overrides(soundfonts[sfSlot].handle, (int)bank, (int)preset,
                               (int)atk, (int)dec, (int)sus, (int)rel);
    LOGD("🎹 SF envelope overrides: slot=%d bank=%d preset=%d atk=%d dec=%d sus=%d rel=%d",
         (int)sfSlot, (int)bank, (int)preset, (int)atk, (int)dec, (int)sus, (int)rel);
}

JNIEXPORT void JNICALL
Java_com_conanizer_pockettracker_platform_android_OboeAudioBackend_native_1setSoundfontFilterOverrides(
        JNIEnv *env, jobject thiz, jint sampleId, jint filterType, jint filterCut, jint filterRes) {
    if (engine) {
        engine->setInstrumentParams((int)sampleId,
            0, 255, false, 0, 0, // start, end, reverse, loop, loopSt
            0, 0, 0,             // drive, crush, downsample
            (int)filterType, (int)filterCut, (int)filterRes);
    }
}

JNIEXPORT void JNICALL
Java_com_conanizer_pockettracker_platform_android_OboeAudioBackend_native_1scheduleSoundfontNote(
        JNIEnv *env, jobject thiz, jlong frame, jint trackId, jint sfSlot,
        jint midiNote, jint velocity, jfloat vol, jfloat pan, jint bank, jint preset,
        jfloat pslInitialOffset, jfloat pslDuration, jfloat pbnRate,
        jfloat vibratoSpeed, jfloat vibratoDepth,
        jfloat phraseVol, jint sampleId,
        jint tableId, jint tableTicRate, jint noteOctave, jint notePitch, jint tableStartRow) {
    if (engine) {
        engine->scheduleSoundfontNote((int64_t)frame, (int)trackId, (int)sfSlot,
                                      (int)midiNote, (int)velocity,
                                      (float)vol, (float)pan, (int)bank, (int)preset,
                                      (float)pslInitialOffset, (float)pslDuration,
                                      (float)pbnRate, (float)vibratoSpeed, (float)vibratoDepth,
                                      (float)phraseVol, (int)sampleId,
                                      (int)tableId, (int)tableTicRate,
                                      (int)noteOctave, (int)notePitch, (int)tableStartRow);
    }
}

JNIEXPORT void JNICALL
Java_com_conanizer_pockettracker_platform_android_OboeAudioBackend_native_1unloadSoundfont(
        JNIEnv *env, jobject thiz, jint sfSlot) {
    if (sfSlot < 0 || sfSlot >= MAX_SOUNDFONTS) return;
    // Detach per-track voices using this slot BEFORE closing the master handle
    for (int t = 0; t < 8; t++) {
        if (sfVoices[t].sfSlot == (int)sfSlot) sfVoices[t].detach();
    }
    std::lock_guard<std::mutex> sfLock(soundfonts[sfSlot].mutex);
    if (soundfonts[sfSlot].handle) {
        tsf_close(soundfonts[sfSlot].handle);
        soundfonts[sfSlot].handle = nullptr;
    }
    soundfonts[sfSlot].instrumentId = -1;
    soundfonts[sfSlot].filePath.clear();
    LOGD("🎹 Unloaded soundfont slot %d", (int)sfSlot);
}

JNIEXPORT jstring JNICALL
Java_com_conanizer_pockettracker_platform_android_OboeAudioBackend_native_1getSoundfontPresetName(
        JNIEnv *env, jobject thiz, jint sfSlot, jint bank, jint preset) {
    if (sfSlot < 0 || sfSlot >= MAX_SOUNDFONTS || !soundfonts[sfSlot].handle) {
        return env->NewStringUTF("---");
    }
    std::lock_guard<std::mutex> sfLock(soundfonts[sfSlot].mutex);
    const char* name = tsf_bank_get_presetname(soundfonts[sfSlot].handle, (int)bank, (int)preset);
    return env->NewStringUTF(name ? name : "---");
}

// Returns [bank, preset_number] of the first preset in the SF2, or [-1, -1] if none loaded.
JNIEXPORT jintArray JNICALL
Java_com_conanizer_pockettracker_platform_android_OboeAudioBackend_native_1getSoundfontFirstBankPreset(
        JNIEnv *env, jobject thiz, jint sfSlot) {
    jintArray result = env->NewIntArray(2);
    jint values[2] = {-1, -1};
    if (sfSlot >= 0 && sfSlot < MAX_SOUNDFONTS && soundfonts[sfSlot].handle) {
        std::lock_guard<std::mutex> sfLock(soundfonts[sfSlot].mutex);
        int bank, preset_num;
        if (tsf_get_preset_at(soundfonts[sfSlot].handle, 0, &bank, &preset_num)) {
            values[0] = (jint)bank;
            values[1] = (jint)preset_num;
        }
    }
    env->SetIntArrayRegion(result, 0, 2, values);
    return result;
}

// Returns the total number of presets in the SF2 file, or 0 if not loaded.
JNIEXPORT jint JNICALL
Java_com_conanizer_pockettracker_platform_android_OboeAudioBackend_native_1getSoundfontPresetCount(
        JNIEnv *env, jobject thiz, jint sfSlot) {
    if (sfSlot < 0 || sfSlot >= MAX_SOUNDFONTS || !soundfonts[sfSlot].handle) return 0;
    std::lock_guard<std::mutex> sfLock(soundfonts[sfSlot].mutex);
    return (jint)tsf_get_presetcount(soundfonts[sfSlot].handle);
}

// Returns [bank, preset_number] of the preset at the given index in the SF2, or [-1, -1] on error.
JNIEXPORT jintArray JNICALL
Java_com_conanizer_pockettracker_platform_android_OboeAudioBackend_native_1getSoundfontPresetAt(
        JNIEnv *env, jobject thiz, jint sfSlot, jint index) {
    jintArray result = env->NewIntArray(2);
    jint values[2] = {-1, -1};
    if (sfSlot >= 0 && sfSlot < MAX_SOUNDFONTS && soundfonts[sfSlot].handle) {
        std::lock_guard<std::mutex> sfLock(soundfonts[sfSlot].mutex);
        int bank, preset_num;
        if (tsf_get_preset_at(soundfonts[sfSlot].handle, (int)index, &bank, &preset_num)) {
            values[0] = (jint)bank;
            values[1] = (jint)preset_num;
        }
    }
    env->SetIntArrayRegion(result, 0, 2, values);
    return result;
}

} // extern "C"

#pragma clang diagnostic pop
