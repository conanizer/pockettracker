// oboe-audio-engine.cpp — Android (Oboe) audio backend. The only Oboe-coupled TU.
// Owns the Oboe stream lifecycle and the audio callback; all DSP/scheduling lives in the portable core.
#include "oboe-audio-engine.h"
#include "audio-engine.h"
#include "audio-defs.h"  // LOGD/LOGE/LOG_TAG (platform log shim)

OboeAudioEngine::OboeAudioEngine(AudioEngine* core) : core(core) {}

OboeAudioEngine::~OboeAudioEngine() {
    closeStream();
}

bool OboeAudioEngine::openStream() {
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

    // Hand the negotiated device rate to the core (it caches it for getSampleRate()/pitch math).
    if (core) {
        core->setDeviceSampleRate(stream->getSampleRate());
    }

    result = stream->requestStart();
    if (result != oboe::Result::OK) {
        LOGE("Failed to start: %s", oboe::convertToText(result));
        return false;
    }

    LOGD("Stream started OK");
    return true;
}

void OboeAudioEngine::closeStream() {
    if (stream) {
        stream->stop();
        stream->close();
        stream.reset();
    }
}

void OboeAudioEngine::resumeStream() {
    if (stream && stream->getState() == oboe::StreamState::Paused) {
        stream->start();
        LOGD("Stream resumed");
    }
}

oboe::DataCallbackResult OboeAudioEngine::onAudioReady(
        oboe::AudioStream* audioStream,
        void* audioData,
        int32_t numFrames) {
    // Pure Oboe glue: forward the device buffer to the portable core. All DSP, the offline-render
    // gate, chunking to MAX_BLOCK, and the visualizer/peak capture live in processLiveBlock.
    core->processLiveBlock(static_cast<float*>(audioData), numFrames,
                           audioStream->getChannelCount(),
                           (float)audioStream->getSampleRate());
    return oboe::DataCallbackResult::Continue;
}
