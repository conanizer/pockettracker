// The Android (Oboe) audio backend: the stream's lifecycle and callback; the DSP is the core's.
#include "oboe-audio-engine.h"
#include "audio-engine.h"
#include "audio-defs.h"  // LOGD/LOGE

#include <unistd.h>

#include <chrono>
#include <cstdio>

OboeAudioEngine::OboeAudioEngine(AudioEngine* core) : core(core) {}

OboeAudioEngine::~OboeAudioEngine() {
    closeStream();
}

void OboeAudioEngine::setPlatformDefaults(int sampleRate, int framesPerBurst) {
    platformRate_  = sampleRate;
    platformBurst_ = framesPerBurst;
    // Globals Oboe reads for an unspecified value — the only way to reach the OpenSL ES path.
    if (sampleRate > 0)     oboe::DefaultStreamValues::SampleRate    = sampleRate;
    if (framesPerBurst > 0) oboe::DefaultStreamValues::FramesPerBurst = framesPerBurst;
}

void OboeAudioEngine::setSlowOpenMarker(std::string path) {
    slowOpenMarker_ = std::move(path);
}

void OboeAudioEngine::setPlaybackCaptured(const std::atomic<bool>* captured) {
    playbackCaptured_ = captured;
}

bool OboeAudioEngine::deviceLost() const {
    if (deviceLost_.load(std::memory_order_relaxed)) return true;
    if (!playbackCaptured_) return false;
    return playbackCaptured_->load(std::memory_order_relaxed) ? reopenOnRecording_
                                                              : reopenAfterRecording_;
}

bool OboeAudioEngine::openStream() {
    // ⚠️ Cleared before the attempt, so a death during it is not erased.
    deviceLost_.store(false, std::memory_order_relaxed);

    // Read once, so the open and what is remembered agree.
    const bool captured = playbackCaptured_ && playbackCaptured_->load(std::memory_order_relaxed);

    oboe::AudioStreamBuilder builder;
    builder.setDataCallback(this);
    builder.setErrorCallback(this);  // carried by every attempt below
    builder.setFormat(oboe::AudioFormat::Float);
    builder.setChannelCount(oboe::ChannelCount::Stereo);

    // ⚠️ No setSampleRate: naming a rate the hardware does not run inserts a resampler and loses the
    // fast path. Every consumer reads the negotiated rate back.

    // ⚠️ The modern API first (the only way to the low-latency direct path), behind a marker file:
    // one ROM stalls startup for up to 35 s opening it. The marker is removed only after a quick open
    // AND start; left behind, the device goes straight to OpenSL ES from then on.
    const bool legacyOnly = !slowOpenMarker_.empty() && access(slowOpenMarker_.c_str(), F_OK) == 0;
    const auto t0 = std::chrono::steady_clock::now();
    oboe::Result result = oboe::Result::ErrorInternal;
    if (!legacyOnly) {
        if (!slowOpenMarker_.empty()) {
            if (FILE* f = std::fopen(slowOpenMarker_.c_str(), "w")) std::fclose(f);
        }
        // Unspecified, so Oboe avoids AAudio on Android 8.0. Exclusive is only a request.
        builder.setAudioApi(oboe::AudioApi::Unspecified);
        builder.setPerformanceMode(oboe::PerformanceMode::LowLatency);
        builder.setSharingMode(oboe::SharingMode::Exclusive);
        // ⚠️ Off the direct path by hand: Android refuses it only once the recorder has attached.
        const bool offDirect = captured && oboe::OboeExtensions::isMMapSupported();
        if (offDirect) oboe::OboeExtensions::setMMapEnabled(false);
        result = builder.openStream(stream);
        if (offDirect) oboe::OboeExtensions::setMMapEnabled(true);
    } else {
        LOGD("openStream: a past open was slow (%s) - OpenSL ES only", slowOpenMarker_.c_str());
    }

    // OpenSL ES, most capable first.
    if (result != oboe::Result::OK) {
        if (!legacyOnly) {
            LOGD("openStream: modern API failed (%s), trying OpenSL ES", oboe::convertToText(result));
        }
        builder.setAudioApi(oboe::AudioApi::OpenSLES);
        builder.setPerformanceMode(oboe::PerformanceMode::LowLatency);
        builder.setSharingMode(oboe::SharingMode::Exclusive);
        result = builder.openStream(stream);
    }
    if (result != oboe::Result::OK) {
        LOGD("openStream: OpenSLES exclusive failed (%s), trying OpenSLES shared LowLatency",
             oboe::convertToText(result));
        builder.setSharingMode(oboe::SharingMode::Shared);
        result = builder.openStream(stream);
    }
    if (result != oboe::Result::OK) {
        LOGD("openStream: OpenSLES LowLatency failed (%s), trying OpenSLES None/Shared",
             oboe::convertToText(result));
        builder.setPerformanceMode(oboe::PerformanceMode::None);
        result = builder.openStream(stream);
    }

    if (result != oboe::Result::OK) {
        LOGE("openStream: all attempts failed: %s", oboe::convertToText(result));
        return false;
    }

    // Two bursts — Oboe's own starting point; if a device crackles, this is the dial. ⚠️ Only asked:
    // the boot line reads back what was granted.
    constexpr int kBurstsPerBuffer = 2;
    const int burst = stream->getFramesPerBurst();
    if (burst > 0) {
        stream->setBufferSizeInFrames(burst * kBurstsPerBuffer);
    }

    // ⚠️ Each clears once a reopen answered it, or the loop reopens every tick. Only a stream that
    // could have the direct path reopens after a recording.
    const bool direct = oboe::OboeExtensions::isMMapUsed(stream.get());
    reopenOnRecording_    = direct && !captured;
    reopenAfterRecording_ = captured && !direct &&
                            stream->getAudioApi() == oboe::AudioApi::AAudio &&
                            oboe::OboeExtensions::isMMapSupported();

    LOGD("Stream opened: %d Hz, burst=%d, bufSz=%d, api=%s, perf=%s, sharing=%s, direct=%d, "
         "recorded=%d (platform said %d Hz / %d frames)",
         stream->getSampleRate(),
         burst,
         stream->getBufferSizeInFrames(),
         oboe::convertToText(stream->getAudioApi()),
         oboe::convertToText(stream->getPerformanceMode()),
         oboe::convertToText(stream->getSharingMode()),
         (int)direct, (int)captured,
         platformRate_, platformBurst_);

    // The negotiated rate to the core, and our own copies for sampleRate() and outputLatency().
    sampleRate_   = stream->getSampleRate();
    bufferFrames_ = stream->getBufferSizeInFrames();
    if (core) {
        core->setDeviceSampleRate(sampleRate_);
    }

    result = stream->requestStart();
    if (result != oboe::Result::OK) {
        LOGE("Failed to start: %s", oboe::convertToText(result));
        return false;
    }

    // Timed across open AND start.
    constexpr long long kSlowOpenMs = 3000;
    const long long openMs = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - t0).count();
    if (!legacyOnly && !slowOpenMarker_.empty()) {
        if (openMs < kSlowOpenMs) {
            std::remove(slowOpenMarker_.c_str());
        } else {
            LOGE("openStream: took %lld ms - OpenSL ES only from the next launch", openMs);
        }
    }

    // After the start: before any frame is presented there is no timestamp.
    const OutputLatency lat = outputLatency();
    LOGD("Stream started OK — output latency %d frames (%.1f ms), %s", lat.frames,
         sampleRate_ > 0 ? 1000.0 * lat.frames / sampleRate_ : 0.0,
         lat.measured ? "measured" : "the buffer alone; the driver's queue is not in it");
    return true;
}

AudioBackend::OutputLatency OboeAudioEngine::outputLatency() const {
    // Live: Oboe resizes the buffer under underruns.
    if (stream) {
        const oboe::ResultWithValue<double> ms = stream->calculateLatencyMillis();
        if (ms && sampleRate_ > 0) {
            return {int(ms.value() * sampleRate_ / 1000.0 + 0.5), true};
        }
        // Expected on OpenSL ES, which has no timestamp: the floor, as SDL reports.
        return {stream->getBufferSizeInFrames(), false};
    }
    return {bufferFrames_, false};
}

void OboeAudioEngine::closeStream() {
    // No stream: a failed open or a backgrounded app must not loop.
    reopenOnRecording_    = false;
    reopenAfterRecording_ = false;
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

void OboeAudioEngine::setPaused(bool paused) {
    if (!stream) return;

    // ⚠️ stop(), not requestPause() — see the header. Blocking, so the callback is out on return.
    const oboe::Result r = paused ? stream->stop() : stream->start();
    if (r != oboe::Result::OK) {
        // Logged: rendering against a running stream would corrupt the export with no other trace.
        LOGE("setPaused(%d) failed: %s", (int)paused, oboe::convertToText(r));
    }
}

void OboeAudioEngine::onErrorAfterClose(oboe::AudioStream* /*audioStream*/, oboe::Result error) {
    deviceLost_.store(true, std::memory_order_relaxed);
    // Otherwise a repaired stream leaves no trace in the log.
    LOGE("Stream lost (%s) - the frame loop will reopen it", oboe::convertToText(error));
}

oboe::DataCallbackResult OboeAudioEngine::onAudioReady(
        oboe::AudioStream* audioStream,
        void* audioData,
        int32_t numFrames) {
    // Everything else — the render gate, sub-blocks, meters — is processLiveBlock's.
    core->processLiveBlock(static_cast<float*>(audioData), numFrames,
                           audioStream->getChannelCount(),
                           (float)audioStream->getSampleRate());
    return oboe::DataCallbackResult::Continue;
}
