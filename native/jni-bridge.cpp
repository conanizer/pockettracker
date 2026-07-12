// jni-bridge.cpp — JNI EXPORT functions
// All JNI entry points from Kotlin/Java into the AudioEngine. Marshalling only: every function here
// converts arguments and forwards. Nothing that decides how the app sounds — or that a non-Android
// build would also need — belongs in this file; that is what made the SoundFont bank a portability
// bug until S6b moved it into the engine.
#include <jni.h>
#include <climits>
#include <vector>
#include "audio-engine.h"
#include "oboe-audio-engine.h"  // Android (Oboe) backend that owns the stream + drives the core

// JNI requires env and thiz in every function signature, but most thin wrappers don't use them.
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunused-parameter"

static AudioEngine* engine = nullptr;          // portable DSP core (no Oboe)
static OboeAudioEngine* oboeShell = nullptr;   // Android backend: owns the output stream + callback

// Create the portable core + its Oboe backend and start the stream. Returns true on success, false if
// an engine already exists (matches the old "create once" guard).
static bool createAudioEngine() {
    if (engine) return false;
    engine = new AudioEngine();
    oboeShell = new OboeAudioEngine(engine);
    // The core (and songcore's consumer, which must not know Oboe exists) asks for the stream through
    // this hook — the C++ equivalent of the Kotlin path's backend.resumeStream() before every note.
    engine->onResumeRequested = [] { if (oboeShell) oboeShell->resumeStream(); };
    return oboeShell->openStream();
}

// Tear down in the safe order: the shell's destructor stops/closes the stream (so no callback can run)
// and detaches the core's resume hook BEFORE the core it points at is freed. delete on null is a no-op.
static void deleteAudioEngine() {
    if (engine) engine->onResumeRequested = nullptr;   // detach before the shell it captures is freed
    delete oboeShell;
    oboeShell = nullptr;
    delete engine;
    engine = nullptr;
}

// The one handle out of this file: songcore-jni.cpp (its own TU — it must not inherit -ffast-math,
// see CMakeLists) needs the engine for the transport clock and the scheduling queues. Null until the
// audio engine is created, so every caller checks.
AudioEngine* pt_engine() { return engine; }

extern "C" {

// ═══════════════════════════════════════════════════════════════════════════
// JNI METHODS FOR OboeAudioBackend
// ═══════════════════════════════════════════════════════════════════════════

JNIEXPORT jboolean JNICALL
Java_com_conanizer_pockettracker_platform_android_OboeAudioBackend_native_1create(JNIEnv *env, jobject thiz) {
    return createAudioEngine() ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT void JNICALL
Java_com_conanizer_pockettracker_platform_android_OboeAudioBackend_native_1delete(JNIEnv *env, jobject thiz) {
    if (engine) {
        deleteAudioEngine();
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
Java_com_conanizer_pockettracker_platform_android_OboeAudioBackend_native_1loadSampleStereo(
        JNIEnv *env, jobject thiz, jint id, jfloatArray leftData, jfloatArray rightData) {
    if (!engine) return;
    jsize len  = env->GetArrayLength(leftData);
    jfloat* L  = env->GetFloatArrayElements(leftData,  nullptr);
    jfloat* R  = env->GetFloatArrayElements(rightData, nullptr);
    engine->loadSampleStereo(id, L, R, len);
    env->ReleaseFloatArrayElements(leftData,  L, JNI_ABORT);
    env->ReleaseFloatArrayElements(rightData, R, JNI_ABORT);
}

JNIEXPORT jboolean JNICALL
Java_com_conanizer_pockettracker_platform_android_OboeAudioBackend_native_1beginSampleLoad(
        JNIEnv *env, jobject thiz, jint id, jint channels, jint estimatedFrames) {
    if (!engine) return JNI_FALSE;
    return engine->beginSampleLoad((int)id, (int)channels, (int)estimatedFrames) ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT void JNICALL
Java_com_conanizer_pockettracker_platform_android_OboeAudioBackend_native_1fillSampleChunk(
        JNIEnv *env, jobject thiz, jint id, jshortArray data, jint frameCount, jint channels) {
    if (!engine || !data) return;
    jshort* arr = env->GetShortArrayElements(data, nullptr);
    if (!arr) return;
    engine->fillSampleChunk((int)id, (const int16_t*)arr, (int)frameCount, (int)channels);
    env->ReleaseShortArrayElements(data, arr, JNI_ABORT);   // JNI_ABORT: we only read, no copy-back
}

JNIEXPORT jint JNICALL
Java_com_conanizer_pockettracker_platform_android_OboeAudioBackend_native_1finalizeSampleLoad(
        JNIEnv *env, jobject thiz, jint id) {
    if (!engine) return 0;
    return (jint)engine->finalizeSampleLoad((int)id);
}

JNIEXPORT void JNICALL
Java_com_conanizer_pockettracker_platform_android_OboeAudioBackend_native_1cancelSampleLoad(
        JNIEnv *env, jobject thiz, jint id) {
    if (!engine) return;
    engine->cancelSampleLoad((int)id);
}

JNIEXPORT jint JNICALL
Java_com_conanizer_pockettracker_platform_android_OboeAudioBackend_native_1loadSampleFromWav(
        JNIEnv *env, jobject thiz, jint id, jstring path) {
    if (!engine || !path) return 0;
    const char* pathStr = env->GetStringUTFChars(path, nullptr);
    if (!pathStr) return 0;
    int rate = engine->loadSampleFromWavFile((int)id, pathStr);
    env->ReleaseStringUTFChars(path, pathStr);
    return (jint)rate;
}

JNIEXPORT jint JNICALL
Java_com_conanizer_pockettracker_platform_android_OboeAudioBackend_native_1loadSampleFromCompressed(
        JNIEnv *env, jobject thiz, jint id, jstring path) {
    if (!engine || !path) return 0;
    const char* pathStr = env->GetStringUTFChars(path, nullptr);
    if (!pathStr) return 0;
    int rate = engine->loadSampleFromCompressed((int)id, pathStr);
    env->ReleaseStringUTFChars(path, pathStr);
    return (jint)rate;
}

JNIEXPORT jboolean JNICALL
Java_com_conanizer_pockettracker_platform_android_OboeAudioBackend_native_1hasStereoData(
        JNIEnv *env, jobject thiz, jint id) {
    if (!engine) return JNI_FALSE;
    return engine->hasStereoData(id) ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT void JNICALL
Java_com_conanizer_pockettracker_platform_android_OboeAudioBackend_native_1clearAllSamples(
        JNIEnv *env, jobject thiz) {
    if (engine) {
        engine->clearAllSamples();
    }
}

JNIEXPORT void JNICALL
Java_com_conanizer_pockettracker_platform_android_OboeAudioBackend_native_1clearSample(
        JNIEnv *env, jobject thiz, jint id) {
    if (engine) {
        engine->clearSample((int)id);
    }
}

JNIEXPORT void JNICALL
Java_com_conanizer_pockettracker_platform_android_OboeAudioBackend_native_1freeSampleUndo(
        JNIEnv *env, jobject thiz, jint id) {
    if (engine) engine->freeSampleUndo((int)id);
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
    if (oboeShell) {
        oboeShell->resumeStream();
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
        // Reused per-thread scratch instead of a per-frame new[]/delete[] — this getter is polled
        // every visualizer frame on the (single) UI thread, so the churn was pure waste on the Miyoo.
        static thread_local std::vector<float> buffer;
        if ((jsize)buffer.size() < length) buffer.resize(length);

        engine->getWaveform(buffer.data(), length);

        env->SetFloatArrayRegion(outArray, 0, length, buffer.data());
    }
}

JNIEXPORT void JNICALL
Java_com_conanizer_pockettracker_platform_android_OboeAudioBackend_native_1setInstrumentParams(
        JNIEnv *env, jobject thiz, jint instrumentId, jint start, jint end,
        jboolean reverse, jint loopMode, jint loopStart, jint loopEnd,
        jint drive, jint crush, jint downsample,
        jint filterType, jint filterCut, jint filterRes) {
    if (engine) {
        engine->setInstrumentParams(instrumentId, start, end, reverse, loopMode, loopStart, loopEnd,
                                    drive, crush, downsample, filterType, filterCut, filterRes);
    }
}

// ===================================
// MIXER PEAK METER JNI METHODS
// ===================================

JNIEXPORT void JNICALL
Java_com_conanizer_pockettracker_platform_android_OboeAudioBackend_native_1getTrackPeaks(JNIEnv *env, jobject thiz, jfloatArray outArray) {
    if (engine && outArray != nullptr) {
        float buffer[16];
        engine->getTrackPeaks(buffer);
        env->SetFloatArrayRegion(outArray, 0, 16, buffer);
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

JNIEXPORT void JNICALL
Java_com_conanizer_pockettracker_platform_android_OboeAudioBackend_native_1getSendPeaks(JNIEnv *env, jobject thiz, jfloatArray outArray) {
    if (engine && outArray != nullptr) {
        float buffer[4];
        engine->getSendPeaks(buffer);
        env->SetFloatArrayRegion(outArray, 0, 4, buffer);
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
Java_com_conanizer_pockettracker_platform_android_OboeAudioBackend_native_1getTrackWaveforms(
        JNIEnv *env, jobject thiz, jfloatArray outArray, jbooleanArray activeFlags) {
    if (!engine || outArray == nullptr || activeFlags == nullptr) return;
    jsize length = env->GetArrayLength(outArray);
    // Reused per-thread scratch instead of a per-frame new[]/delete[] (5580 floats, polled every
    // OCTA visualizer frame on the single UI thread). getTrackWaveforms writes 8 track lanes + 1
    // preview lane (TRACK_WAVEFORM_COUNT); the Kotlin array length is authoritative for the copy size.
    static thread_local std::vector<float> buf;
    if ((jsize)buf.size() < length) buf.resize(length);
    bool flags[16] = {};
    engine->getTrackWaveforms(buf.data(), flags);
    env->SetFloatArrayRegion(outArray, 0, length, buf.data());
    jsize flagLen = env->GetArrayLength(activeFlags);
    if (flagLen > 16) flagLen = 16;
    jboolean jflags[16];
    for (int i = 0; i < flagLen; i++) jflags[i] = flags[i] ? JNI_TRUE : JNI_FALSE;
    env->SetBooleanArrayRegion(activeFlags, 0, flagLen, jflags);
}

JNIEXPORT jfloatArray JNICALL
Java_com_conanizer_pockettracker_platform_android_OboeAudioBackend_native_1getSpectrumMagnitudes(JNIEnv *env, jobject thiz, jint numBins) {
    jfloatArray result = env->NewFloatArray(numBins);
    if (!engine || numBins <= 0) return result;
    // Reused per-thread scratch (waveform-getter pattern) — this is polled ~20 fps while the
    // spectrum visualizer / EQ screen is open; per-call new[]/delete[] was pure churn.
    static thread_local std::vector<float> buf;
    if ((jint)buf.size() < numBins) buf.resize(numBins);
    engine->getSpectrumMagnitudes(numBins, buf.data());
    env->SetFloatArrayRegion(result, 0, numBins, buf.data());
    return result;
}

JNIEXPORT jfloatArray JNICALL
Java_com_conanizer_pockettracker_platform_android_OboeAudioBackend_native_1getSpectrumMagnitudesForSource(JNIEnv *env, jobject thiz, jint source, jint instrId, jint numBins) {
    jfloatArray result = env->NewFloatArray(numBins);
    if (!engine || numBins <= 0) return result;
    static thread_local std::vector<float> buf;  // see getSpectrumMagnitudes
    if ((jint)buf.size() < numBins) buf.resize(numBins);
    engine->getSpectrumMagnitudesForSource(source, instrId, numBins, buf.data());
    env->SetFloatArrayRegion(result, 0, numBins, buf.data());
    return result;
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

JNIEXPORT void JNICALL
Java_com_conanizer_pockettracker_platform_android_OboeAudioBackend_native_1setOttDepth(JNIEnv *env, jobject thiz, jint depth) {
    if (engine) {
        engine->setOttDepth(depth);
    }
}

JNIEXPORT void JNICALL
Java_com_conanizer_pockettracker_platform_android_OboeAudioBackend_native_1setOttDepthForRender(JNIEnv *env, jobject thiz, jint depth) {
    if (engine) {
        engine->setOttDepthForRender(depth);
    }
}

JNIEXPORT void JNICALL
Java_com_conanizer_pockettracker_platform_android_OboeAudioBackend_native_1setMasterFx(JNIEnv *env, jobject thiz, jint fx) {
    if (engine) engine->setMasterFx(fx);
}

JNIEXPORT void JNICALL
Java_com_conanizer_pockettracker_platform_android_OboeAudioBackend_native_1setDustDepth(JNIEnv *env, jobject thiz, jint depth) {
    if (engine) engine->setDustDepth(depth);
}

JNIEXPORT void JNICALL
Java_com_conanizer_pockettracker_platform_android_OboeAudioBackend_native_1setDustDepthForRender(JNIEnv *env, jobject thiz, jint depth) {
    if (engine) engine->setDustDepthForRender(depth);
}

JNIEXPORT void JNICALL
Java_com_conanizer_pockettracker_platform_android_OboeAudioBackend_native_1setLimiterPreGain(JNIEnv *env, jobject thiz, jint depth) {
    if (engine) engine->setLimiterPreGain(depth);
}

// ===================================
// EQ / SEND / REVERB / DELAY JNI METHODS
// ===================================

JNIEXPORT void JNICALL
Java_com_conanizer_pockettracker_platform_android_OboeAudioBackend_native_1setEqBand(
        JNIEnv *env, jobject thiz,
        jint slot, jint band, jint type, jint freqHex, jint gainHex, jint qHex) {
    if (engine) engine->setEqBand(slot, band, type, freqHex, gainHex, qHex);
}

JNIEXPORT void JNICALL
Java_com_conanizer_pockettracker_platform_android_OboeAudioBackend_native_1setInstrumentEqSlot(
        JNIEnv *env, jobject thiz, jint instrId, jint slot) {
    if (engine) engine->setInstrumentEqSlot(instrId, slot);
}

JNIEXPORT void JNICALL
Java_com_conanizer_pockettracker_platform_android_OboeAudioBackend_native_1setInstrumentSendLevels(
        JNIEnv *env, jobject thiz, jint instrId, jint reverbSend, jint delaySend) {
    if (engine) engine->setInstrumentSendLevels(instrId, reverbSend, delaySend);
}

JNIEXPORT void JNICALL
Java_com_conanizer_pockettracker_platform_android_OboeAudioBackend_native_1setReverbParams(
        JNIEnv *env, jobject thiz, jint feedbackHex, jint dampHex, jint wetHex) {
    if (engine) engine->setReverbParams(feedbackHex, dampHex, wetHex);
}

JNIEXPORT void JNICALL
Java_com_conanizer_pockettracker_platform_android_OboeAudioBackend_native_1setDelayParams(
        JNIEnv *env, jobject thiz,
        jint timeOrSubdiv, jint feedbackHex, jboolean syncMode, jfloat bpm, jint wetHex) {
    if (engine) engine->setDelayParams(timeOrSubdiv, feedbackHex, syncMode, bpm, wetHex);
}

JNIEXPORT void JNICALL
Java_com_conanizer_pockettracker_platform_android_OboeAudioBackend_native_1setDelayReverbSend(
        JNIEnv *env, jobject thiz, jint sendHex) {
    if (engine) engine->setDelayReverbSend(sendHex);
}

JNIEXPORT void JNICALL
Java_com_conanizer_pockettracker_platform_android_OboeAudioBackend_native_1setReverbInputEq(
        JNIEnv *env, jobject thiz, jint slot) {
    if (engine) engine->setReverbInputEq(slot);
}

JNIEXPORT void JNICALL
Java_com_conanizer_pockettracker_platform_android_OboeAudioBackend_native_1setDelayInputEq(
        JNIEnv *env, jobject thiz, jint slot) {
    if (engine) engine->setDelayInputEq(slot);
}

JNIEXPORT void JNICALL
Java_com_conanizer_pockettracker_platform_android_OboeAudioBackend_native_1setMasterEqSlot(
        JNIEnv *env, jobject thiz, jint slot) {
    if (engine) engine->setMasterEqSlot(slot);
}

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
        jint startPointOverride, jint endPointOverride, jint tableId, jint tableTicRate,
        jint noteOctave, jint notePitch,
        jfloat pslInitialOffset, jfloat pslDuration,
        jfloat pbnRate, jfloat vibratoSpeed, jfloat vibratoDepth,
        jint tableStartRow) {
    if (engine) {
        engine->scheduleNote(targetFrame, sampleId, trackId, frequency, baseFrequency,
                             volume, phraseVolume, pan, startPointOverride, endPointOverride, tableId, tableTicRate,
                             noteOctave, notePitch,
                             pslInitialOffset, pslDuration, pbnRate, vibratoSpeed, vibratoDepth,
                             tableStartRow);
    }
}

JNIEXPORT void JNICALL
Java_com_conanizer_pockettracker_platform_android_OboeAudioBackend_native_1scheduleVoiceTableRow(
        JNIEnv *env, jobject thiz, jlong targetFrame, jint trackId, jint row) {
    if (engine) {
        engine->scheduleVoiceTableRow(targetFrame, trackId, row);
    }
}

JNIEXPORT void JNICALL
Java_com_conanizer_pockettracker_platform_android_OboeAudioBackend_native_1scheduleTrackPhraseVol(
        JNIEnv *env, jobject thiz, jlong targetFrame, jint trackId, jfloat phraseVol) {
    if (engine) {
        engine->scheduleTrackPhraseVol(targetFrame, trackId, phraseVol);
    }
}

// ── Live per-note / mixer FX (PAN / REV / DEL / BCK / EQN / EQM) ──
JNIEXPORT void JNICALL
Java_com_conanizer_pockettracker_platform_android_OboeAudioBackend_native_1scheduleVoicePan(
        JNIEnv *env, jobject thiz, jlong targetFrame, jint trackId, jfloat pan) {
    if (engine) engine->scheduleVoicePan(targetFrame, trackId, pan);
}

JNIEXPORT void JNICALL
Java_com_conanizer_pockettracker_platform_android_OboeAudioBackend_native_1scheduleVoiceReverbSend(
        JNIEnv *env, jobject thiz, jlong targetFrame, jint trackId, jfloat send) {
    if (engine) engine->scheduleVoiceReverbSend(targetFrame, trackId, send);
}

JNIEXPORT void JNICALL
Java_com_conanizer_pockettracker_platform_android_OboeAudioBackend_native_1scheduleVoiceDelaySend(
        JNIEnv *env, jobject thiz, jlong targetFrame, jint trackId, jfloat send) {
    if (engine) engine->scheduleVoiceDelaySend(targetFrame, trackId, send);
}

JNIEXPORT void JNICALL
Java_com_conanizer_pockettracker_platform_android_OboeAudioBackend_native_1scheduleVoiceReverse(
        JNIEnv *env, jobject thiz, jlong targetFrame, jint trackId, jboolean reverse, jboolean restart) {
    if (engine) engine->scheduleVoiceReverse(targetFrame, trackId, reverse, restart);
}

JNIEXPORT void JNICALL
Java_com_conanizer_pockettracker_platform_android_OboeAudioBackend_native_1scheduleVoiceEqSlot(
        JNIEnv *env, jobject thiz, jlong targetFrame, jint trackId, jint slot) {
    if (engine) engine->scheduleVoiceEqSlot(targetFrame, trackId, slot);
}

JNIEXPORT void JNICALL
Java_com_conanizer_pockettracker_platform_android_OboeAudioBackend_native_1scheduleMasterEqSlotAt(
        JNIEnv *env, jobject thiz, jlong targetFrame, jint slot) {
    if (engine) engine->scheduleMasterEqSlot(targetFrame, slot);
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

JNIEXPORT void JNICALL
Java_com_conanizer_pockettracker_platform_android_OboeAudioBackend_native_1schedulePitchBend(
        JNIEnv *env, jobject thiz, jlong targetFrame, jint trackId, jfloat semitonesPerTick, jint tempo) {
    if (engine) {
        engine->schedulePitchBend(targetFrame, trackId, semitonesPerTick, tempo);
    }
}

JNIEXPORT void JNICALL
Java_com_conanizer_pockettracker_platform_android_OboeAudioBackend_native_1scheduleVibrato(
        JNIEnv *env, jobject thiz, jlong targetFrame, jint trackId, jfloat speed, jfloat depth) {
    if (engine) {
        engine->scheduleVibrato(targetFrame, trackId, speed, depth);
    }
}

JNIEXPORT void JNICALL
Java_com_conanizer_pockettracker_platform_android_OboeAudioBackend_native_1setInstrumentModulation(
        JNIEnv *env, jobject thiz,
        jint sampleId, jint slotIndex, jint type, jint dest, jfloat amount,
        jint attackSamples, jint holdSamples, jint decaySamples,
        jfloat sustainLevel, jfloat lfoHz, jint oscShape, jint releaseSamples,
        jint lfoTrigMode) {
    if (engine) {
        engine->setInstrumentModulation(sampleId, slotIndex, type, dest, amount,
                                        attackSamples, holdSamples, decaySamples,
                                        sustainLevel, lfoHz, oscShape, releaseSamples,
                                        lfoTrigMode);
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

JNIEXPORT void JNICALL
Java_com_conanizer_pockettracker_platform_android_OboeAudioBackend_native_1setStemsMode(
        JNIEnv *env, jobject thiz, jint mode) {
    if (engine) engine->setStemsMode((int)mode);
}

JNIEXPORT void JNICALL
Java_com_conanizer_pockettracker_platform_android_OboeAudioBackend_native_1setTempo(
        JNIEnv *env, jobject thiz, jint tempo) {
    if (engine) engine->setTempo((int)tempo);
}

// ===================================
// SOUNDFONT JNI FUNCTIONS
// ===================================
// Marshalling only. The bank itself — slot de-dup, LRU eviction, tsf_load/close, the preset getters
// and their locking — is AudioEngine's (audio-engine.cpp, "SOUNDFONT BANK"), because none of it is
// platform-specific and a host build needs it too (tools/ptrender, the SDL shell).

JNIEXPORT jint JNICALL
Java_com_conanizer_pockettracker_platform_android_OboeAudioBackend_native_1loadSoundfont(
        JNIEnv *env, jobject thiz, jint instrumentId, jstring path) {
    if (!engine) return -1;
    const char* pathStr = env->GetStringUTFChars(path, nullptr);
    if (!pathStr) return -1;
    const jint slot = (jint)engine->loadSoundfont((int)instrumentId, pathStr);
    env->ReleaseStringUTFChars(path, pathStr);
    return slot;
}

JNIEXPORT void JNICALL
Java_com_conanizer_pockettracker_platform_android_OboeAudioBackend_native_1setSoundfontPreset(
        JNIEnv *env, jobject thiz, jint sfSlot, jint bank, jint preset) {
    // Preset is set per-note via tsf_channel_set_presetnumber; this is a no-op stored in Kotlin.
    LOGD("🎹 setSoundfontPreset slot=%d bank=%d preset=%d (applied per-note)", (int)sfSlot, (int)bank, (int)preset);
}

JNIEXPORT void JNICALL
Java_com_conanizer_pockettracker_platform_android_OboeAudioBackend_native_1setSoundfontEnvelopeOverrides(
        JNIEnv *env, jobject thiz, jint instrumentId,
        jint atk, jint dec, jint sus, jint rel) {
    // Store the override keyed by instrument id; triggerNote applies it atomically before note_on
    // No slot/bank/preset here — the trigger uses the note's own bank/preset,
    // so instruments sharing one de-duplicated handle stay isolated.
    if (engine) engine->setSoundfontEnvelopeOverride((int)instrumentId, (int)atk, (int)dec, (int)sus, (int)rel);
}

JNIEXPORT void JNICALL
Java_com_conanizer_pockettracker_platform_android_OboeAudioBackend_native_1setSoundfontFilterOverrides(
        JNIEnv *env, jobject thiz, jint sampleId, jint filterType, jint filterCut, jint filterRes) {
    if (engine) {
        engine->setInstrumentParams((int)sampleId,
            0, 255, false, 0, 0, 255, // start, end, reverse, loop, loopSt, loopEnd
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
        jint tableId, jint tableTicRate, jint noteOctave, jint notePitch, jint tableStartRow,
        jfloat detuneSemitones) {
    if (engine) {
        engine->scheduleSoundfontNote((int64_t)frame, (int)trackId, (int)sfSlot,
                                      (int)midiNote, (int)velocity,
                                      (float)vol, (float)pan, (int)bank, (int)preset,
                                      (float)pslInitialOffset, (float)pslDuration,
                                      (float)pbnRate, (float)vibratoSpeed, (float)vibratoDepth,
                                      (float)phraseVol, (int)sampleId,
                                      (int)tableId, (int)tableTicRate,
                                      (int)noteOctave, (int)notePitch, (int)tableStartRow,
                                      (float)detuneSemitones);
    }
}

JNIEXPORT void JNICALL
Java_com_conanizer_pockettracker_platform_android_OboeAudioBackend_native_1unloadSoundfont(
        JNIEnv *env, jobject thiz, jint sfSlot) {
    if (engine) engine->unloadSoundfont((int)sfSlot);
}

JNIEXPORT void JNICALL
Java_com_conanizer_pockettracker_platform_android_OboeAudioBackend_native_1clearAllSoundfonts(
        JNIEnv *env, jobject thiz) {
    if (engine) engine->clearAllSoundfonts();
}

JNIEXPORT jstring JNICALL
Java_com_conanizer_pockettracker_platform_android_OboeAudioBackend_native_1getSoundfontPresetName(
        JNIEnv *env, jobject thiz, jint sfSlot, jint bank, jint preset) {
    if (!engine) return env->NewStringUTF("---");
    return env->NewStringUTF(engine->getSoundfontPresetName((int)sfSlot, (int)bank, (int)preset).c_str());
}

// [bank, preset_number] of one preset, or [-1, -1] if the slot is empty / the index is out of range.
static jintArray sfPresetPair(JNIEnv* env, int sfSlot, int index) {
    jintArray result = env->NewIntArray(2);
    jint values[2] = {-1, -1};
    int bank, presetNum;
    if (engine && engine->getSoundfontPresetAt(sfSlot, index, &bank, &presetNum)) {
        values[0] = (jint)bank;
        values[1] = (jint)presetNum;
    }
    env->SetIntArrayRegion(result, 0, 2, values);
    return result;
}

// Returns [bank, preset_number] of the first preset in the SF2, or [-1, -1] if none loaded.
JNIEXPORT jintArray JNICALL
Java_com_conanizer_pockettracker_platform_android_OboeAudioBackend_native_1getSoundfontFirstBankPreset(
        JNIEnv *env, jobject thiz, jint sfSlot) {
    return sfPresetPair(env, (int)sfSlot, 0);
}

// Returns the total number of presets in the SF2 file, or 0 if not loaded.
JNIEXPORT jint JNICALL
Java_com_conanizer_pockettracker_platform_android_OboeAudioBackend_native_1getSoundfontPresetCount(
        JNIEnv *env, jobject thiz, jint sfSlot) {
    return engine ? (jint)engine->getSoundfontPresetCount((int)sfSlot) : 0;
}

// Returns [bank, preset_number] of the preset at the given index in the SF2, or [-1, -1] on error.
JNIEXPORT jintArray JNICALL
Java_com_conanizer_pockettracker_platform_android_OboeAudioBackend_native_1getSoundfontPresetAt(
        JNIEnv *env, jobject thiz, jint sfSlot, jint index) {
    return sfPresetPair(env, (int)sfSlot, (int)index);
}

// ============================================================
// SAMPLE EDITOR OPERATIONS
// ============================================================

JNIEXPORT jint JNICALL
Java_com_conanizer_pockettracker_platform_android_OboeAudioBackend_native_1getSampleLength(
        JNIEnv *env, jobject, jint id) {
    if (!engine) return 0;
    return (jint)engine->getSampleLength((int)id);
}

JNIEXPORT jfloatArray JNICALL
Java_com_conanizer_pockettracker_platform_android_OboeAudioBackend_native_1getSampleWaveform(
        JNIEnv *env, jobject, jint id, jint numBins) {
    jfloatArray result = env->NewFloatArray(numBins * 2);
    if (!engine || numBins <= 0) return result;
    jfloat* buf = env->GetFloatArrayElements(result, nullptr);
    engine->getSampleWaveform((int)id, buf, (int)numBins);
    env->ReleaseFloatArrayElements(result, buf, 0);
    return result;
}

JNIEXPORT jfloatArray JNICALL
Java_com_conanizer_pockettracker_platform_android_OboeAudioBackend_native_1getSampleWaveformRange(
        JNIEnv *env, jobject, jint id, jint startFrame, jint endFrame, jint numBins) {
    jfloatArray result = env->NewFloatArray(numBins * 2);
    if (!engine || numBins <= 0) return result;
    jfloat* buf = env->GetFloatArrayElements(result, nullptr);
    engine->getSampleWaveformRange((int)id, (int)startFrame, (int)endFrame, buf, (int)numBins);
    env->ReleaseFloatArrayElements(result, buf, 0);
    return result;
}

JNIEXPORT jfloatArray JNICALL
Java_com_conanizer_pockettracker_platform_android_OboeAudioBackend_native_1getSampleData(
        JNIEnv *env, jobject, jint id) {
    if (!engine) return env->NewFloatArray(0);
    int len = engine->getSampleLength((int)id);
    if (len <= 0) return env->NewFloatArray(0);
    jfloatArray result = env->NewFloatArray(len);
    jfloat* buf = env->GetFloatArrayElements(result, nullptr);
    engine->getSampleData((int)id, buf);
    env->ReleaseFloatArrayElements(result, buf, 0);
    return result;
}

JNIEXPORT jfloatArray JNICALL
Java_com_conanizer_pockettracker_platform_android_OboeAudioBackend_native_1getSampleDataRight(
        JNIEnv *env, jobject, jint id) {
    if (!engine) return env->NewFloatArray(0);
    int len = engine->getSampleLength((int)id);
    if (len <= 0 || !engine->hasStereoData((int)id)) return env->NewFloatArray(0);
    jfloatArray result = env->NewFloatArray(len);
    jfloat* buf = env->GetFloatArrayElements(result, nullptr);
    engine->getSampleDataRight((int)id, buf);
    env->ReleaseFloatArrayElements(result, buf, 0);
    return result;
}

JNIEXPORT jfloatArray JNICALL
Java_com_conanizer_pockettracker_platform_android_OboeAudioBackend_native_1getSampleWaveformRangeSource(
        JNIEnv *env, jobject, jint id, jint startFrame, jint endFrame, jint numBins, jint channel) {
    jfloatArray result = env->NewFloatArray(numBins * 2);
    if (!engine || numBins <= 0) return result;
    jfloat* buf = env->GetFloatArrayElements(result, nullptr);
    engine->getSampleWaveformRangeSource((int)id, (int)startFrame, (int)endFrame, buf, (int)numBins, (int)channel);
    env->ReleaseFloatArrayElements(result, buf, 0);
    return result;
}

JNIEXPORT void JNICALL
Java_com_conanizer_pockettracker_platform_android_OboeAudioBackend_native_1normalizeSample(
        JNIEnv *env, jobject, jint id, jint startFrame, jint endFrame) {
    if (engine) engine->normalizeSample((int)id, (int)startFrame, (int)endFrame);
}

JNIEXPORT void JNICALL
Java_com_conanizer_pockettracker_platform_android_OboeAudioBackend_native_1fadeInSample(
        JNIEnv *env, jobject, jint id, jint startFrame, jint endFrame) {
    if (engine) engine->fadeInSample((int)id, (int)startFrame, (int)endFrame);
}

JNIEXPORT void JNICALL
Java_com_conanizer_pockettracker_platform_android_OboeAudioBackend_native_1fadeOutSample(
        JNIEnv *env, jobject, jint id, jint startFrame, jint endFrame) {
    if (engine) engine->fadeOutSample((int)id, (int)startFrame, (int)endFrame);
}

JNIEXPORT void JNICALL
Java_com_conanizer_pockettracker_platform_android_OboeAudioBackend_native_1silenceRegion(
        JNIEnv *env, jobject, jint id, jint startFrame, jint endFrame) {
    if (engine) engine->silenceRegion((int)id, (int)startFrame, (int)endFrame);
}

JNIEXPORT void JNICALL
Java_com_conanizer_pockettracker_platform_android_OboeAudioBackend_native_1reverseSample(
        JNIEnv *env, jobject, jint id, jint startFrame, jint endFrame) {
    if (engine) engine->reverseSample((int)id, (int)startFrame, (int)endFrame);
}

JNIEXPORT void JNICALL
Java_com_conanizer_pockettracker_platform_android_OboeAudioBackend_native_1backupSample(
        JNIEnv *env, jobject, jint id) {
    if (engine) engine->backupSample((int)id);
}

JNIEXPORT void JNICALL
Java_com_conanizer_pockettracker_platform_android_OboeAudioBackend_native_1undoSample(
        JNIEnv *env, jobject, jint id) {
    if (engine) engine->undoSample((int)id);
}

JNIEXPORT void JNICALL
Java_com_conanizer_pockettracker_platform_android_OboeAudioBackend_native_1saveFxPreviewBackup(
        JNIEnv*, jobject, jint id) {
    if (engine) engine->saveFxPreviewBackup((int)id);
}

JNIEXPORT void JNICALL
Java_com_conanizer_pockettracker_platform_android_OboeAudioBackend_native_1restoreFxPreviewBackup(
        JNIEnv*, jobject) {
    if (engine) engine->restoreFxPreviewBackup();
}

JNIEXPORT jfloat JNICALL
Java_com_conanizer_pockettracker_platform_android_OboeAudioBackend_native_1getSamplePlaybackPosition(
        JNIEnv*, jobject, jint id) {
    return engine ? engine->getSamplePlaybackPosition((int)id) : -1.0f;
}

JNIEXPORT void JNICALL
Java_com_conanizer_pockettracker_platform_android_OboeAudioBackend_native_1cropSample(
        JNIEnv*, jobject, jint id, jint startFrame, jint endFrame) {
    if (engine) engine->cropSample((int)id, (int)startFrame, (int)endFrame);
}

JNIEXPORT void JNICALL
Java_com_conanizer_pockettracker_platform_android_OboeAudioBackend_native_1deleteSampleRegion(
        JNIEnv*, jobject, jint id, jint startFrame, jint endFrame) {
    if (engine) engine->deleteSampleRegion((int)id, (int)startFrame, (int)endFrame);
}

JNIEXPORT void JNICALL
Java_com_conanizer_pockettracker_platform_android_OboeAudioBackend_native_1copyRegion(
        JNIEnv*, jobject, jint id, jint startFrame, jint endFrame) {
    if (engine) engine->copyRegion((int)id, (int)startFrame, (int)endFrame);
}

JNIEXPORT void JNICALL
Java_com_conanizer_pockettracker_platform_android_OboeAudioBackend_native_1pasteRegion(
        JNIEnv*, jobject, jint id, jint insertAt) {
    if (engine) engine->pasteRegion((int)id, (int)insertAt);
}

JNIEXPORT void JNICALL
Java_com_conanizer_pockettracker_platform_android_OboeAudioBackend_native_1prepareSourcePreview(
        JNIEnv*, jobject, jint dstId, jint srcId, jint mode) {
    if (engine) engine->prepareSourcePreview((int)dstId, (int)srcId, (int)mode);
}

JNIEXPORT jint JNICALL
Java_com_conanizer_pockettracker_platform_android_OboeAudioBackend_native_1getClipboardLength(
        JNIEnv*, jobject) {
    return engine ? engine->getClipboardLength() : 0;
}

JNIEXPORT void JNICALL
Java_com_conanizer_pockettracker_platform_android_OboeAudioBackend_native_1downsampleSample(
        JNIEnv*, jobject, jint id, jint factor) {
    if (engine) engine->downsampleSample((int)id, (int)factor);
}

JNIEXPORT jint JNICALL
Java_com_conanizer_pockettracker_platform_android_OboeAudioBackend_native_1findZeroCrossing(
        JNIEnv*, jobject, jint id, jint frame, jint dir) {
    return engine ? (jint)engine->findZeroCrossing((int)id, (int)frame, (int)dir) : frame;
}

JNIEXPORT void JNICALL
Java_com_conanizer_pockettracker_platform_android_OboeAudioBackend_native_1applyRateMode(
        JNIEnv*, jobject, jint id, jint factor) {
    if (engine) engine->applyRateMode((int)id, (int)factor);
}

JNIEXPORT void JNICALL
Java_com_conanizer_pockettracker_platform_android_OboeAudioBackend_native_1pitchShiftSample(
        JNIEnv*, jobject, jint id, jfloat semitones) {
    if (engine) engine->pitchShiftSample((int)id, (float)semitones);
}

JNIEXPORT void JNICALL
Java_com_conanizer_pockettracker_platform_android_OboeAudioBackend_native_1timeStretchSample(
        JNIEnv*, jobject, jint id, jfloat ratio) {
    if (engine) engine->timeStretchSample((int)id, (float)ratio);
}

JNIEXPORT void JNICALL
Java_com_conanizer_pockettracker_platform_android_OboeAudioBackend_native_1applySampleFx(
        JNIEnv*, jobject, jint id, jint fxType, jint fxValue, jfloat sampleRate, jint limiterPreGain) {
    if (engine) engine->applySampleFx((int)id, (int)fxType, (int)fxValue, (float)sampleRate, (int)limiterPreGain);
}

JNIEXPORT jintArray JNICALL
Java_com_conanizer_pockettracker_platform_android_OboeAudioBackend_native_1detectTransients(
        JNIEnv* env, jobject, jint id, jint sensitivity, jint maxMarkers) {
    std::vector<int> buf((size_t)maxMarkers);
    int count = engine ? engine->detectTransients((int)id, (int)sensitivity, buf.data(), (int)maxMarkers) : 0;
    jintArray result = env->NewIntArray(count);
    if (result && count > 0) env->SetIntArrayRegion(result, 0, count, buf.data());
    return result;
}

} // extern "C"

#pragma clang diagnostic pop
