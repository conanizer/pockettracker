// oboe-audio-engine.cpp — Android (Oboe) audio backend. The only Oboe-coupled TU.
// Owns the Oboe stream lifecycle and the audio callback; all DSP/scheduling lives in the portable core.
#include "oboe-audio-engine.h"
#include "audio-engine.h"
#include "audio-defs.h"  // LOGD/LOGE/LOG_TAG (platform log shim)

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
    // Oboe reads these globals when a builder leaves a value unspecified, which is how the numbers
    // reach the OpenSL ES path — there is no per-builder way to say "the device's own".
    if (sampleRate > 0)     oboe::DefaultStreamValues::SampleRate    = sampleRate;
    if (framesPerBurst > 0) oboe::DefaultStreamValues::FramesPerBurst = framesPerBurst;
}

void OboeAudioEngine::setSlowOpenMarker(std::string path) {
    slowOpenMarker_ = std::move(path);
}

bool OboeAudioEngine::openStream() {
    // ⚠️ Cleared BEFORE the attempt, not after a successful one: a stream dying while this runs
    // raises it again, and clearing on the way out would erase that second death.
    deviceLost_.store(false, std::memory_order_relaxed);

    oboe::AudioStreamBuilder builder;
    builder.setDataCallback(this);
    builder.setErrorCallback(this);  // on the builder, so all four attempts below carry it
    builder.setFormat(oboe::AudioFormat::Float);
    builder.setChannelCount(oboe::ChannelCount::Stereo);

    // ⚠️⚠️ **NO setSampleRate, AND THAT IS THE POINT OF THIS WHOLE PATH.** Naming 44100 on hardware
    // that runs 48000 inserts a resampler, and a resampled stream commonly loses the fast mixer path
    // whichever API is underneath — which costs far more than picking the rate ever bought. Left
    // unspecified it opens at whatever `setPlatformDefaults` was told, and every consumer already
    // reads the result back: the callback passes the stream's own rate per block, and
    // `setDeviceSampleRate` below re-derives the send and master chains' coefficients from it.

    // ⚠️⚠️ **THE MODERN API FIRST, BEHIND A MARKER FILE.** It is the only route to the direct
    // (MMAP) path, which cuts the output latency to about a third where the device has it. But one
    // ROM (GammaCoreOS on the Miyoo Flip) was seen to stall startup for up to 35 s opening it. So the
    // marker is written BEFORE the attempt and removed only once the stream has opened AND started
    // quickly: a slow open, a hang, or the app killed mid-stall all leave it behind, and from then on
    // this device goes straight to OpenSL ES. A bad ROM pays the stall once.
    const bool legacyOnly = !slowOpenMarker_.empty() && access(slowOpenMarker_.c_str(), F_OK) == 0;
    const auto t0 = std::chrono::steady_clock::now();
    oboe::Result result = oboe::Result::ErrorInternal;
    if (!legacyOnly) {
        if (!slowOpenMarker_.empty()) {
            if (FILE* f = std::fopen(slowOpenMarker_.c_str(), "w")) std::fclose(f);
        }
        // Unspecified, not AAudio: on Android 8.0 Oboe knows AAudio is unreliable and picks OpenSL ES.
        // Exclusive is a request — a device without the direct path hands back a shared stream.
        builder.setAudioApi(oboe::AudioApi::Unspecified);
        builder.setPerformanceMode(oboe::PerformanceMode::LowLatency);
        builder.setSharingMode(oboe::SharingMode::Exclusive);
        result = builder.openStream(stream);
    } else {
        LOGD("openStream: a past open was slow (%s) - OpenSL ES only", slowOpenMarker_.c_str());
    }

    // OpenSL ES, most capable first. It never takes the direct path, so it is the fallback now.
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

    // One burst playing while the next is filled — the smallest buffer that is not starved, and the
    // figure Oboe's own guidance starts from. ⚠️ **ASKED, NOT SET**: the stream may round it or refuse
    // it outright (OpenSL ES largely fixes its queue at open), so the boot line below reads the result
    // back rather than repeating the request. ⭐ If a device crackles, this multiplier is the dial.
    constexpr int kBurstsPerBuffer = 2;
    const int burst = stream->getFramesPerBurst();
    if (burst > 0) {
        stream->setBufferSizeInFrames(burst * kBurstsPerBuffer);
    }

    LOGD("Stream opened: %d Hz, burst=%d, bufSz=%d, api=%s, perf=%s, sharing=%s "
         "(platform said %d Hz / %d frames)",
         stream->getSampleRate(),
         burst,
         stream->getBufferSizeInFrames(),
         oboe::convertToText(stream->getAudioApi()),
         oboe::convertToText(stream->getPerformanceMode()),
         oboe::convertToText(stream->getSharingMode()),
         platformRate_, platformBurst_);

    // Hand the negotiated device rate to the core (it caches it for getSampleRate()/pitch math), and
    // keep our own copy for AudioBackend::sampleRate() — see the header for why the shell is not
    // allowed to reach into the stream object and ask. Same for the buffer, which outputLatency()
    // falls back to.
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

    // Timed to here, not to the open: the stall was never pinned to one of the two calls.
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

    // AFTER the start, because a stream that has presented no frames has no timestamp to compute a
    // real latency from — ask before this and every device on earth reports the floor.
    const OutputLatency lat = outputLatency();
    LOGD("Stream started OK — output latency %d frames (%.1f ms), %s", lat.frames,
         sampleRate_ > 0 ? 1000.0 * lat.frames / sampleRate_ : 0.0,
         lat.measured ? "measured" : "the buffer alone; the driver's queue is not in it");
    return true;
}

AudioBackend::OutputLatency OboeAudioEngine::outputLatency() const {
    // Asked live, not cached: the buffer Oboe hands out grows and shrinks under an underrun, so a
    // figure taken once at boot would go stale on exactly the device that needed watching.
    if (stream) {
        const oboe::ResultWithValue<double> ms = stream->calculateLatencyMillis();
        if (ms && sampleRate_ > 0) {
            return {int(ms.value() * sampleRate_ / 1000.0 + 0.5), true};
        }
        // ⚠️ Expected on the shipping path, not a defect: openStream takes OpenSL ES first, and only
        // AAudio implements this. The floor is then the same class of number SDL reports.
        return {stream->getBufferSizeInFrames(), false};
    }
    return {bufferFrames_, false};
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

void OboeAudioEngine::setPaused(bool paused) {
    if (!stream) return;

    // ⚠️ stop(), not requestPause() — see the header: resumeStream() restarts a Paused stream, and
    // the engine asks it to before every scheduled note. Blocking forms, so there is exactly one
    // reader of the engine on the far side of this call.
    const oboe::Result r = paused ? stream->stop() : stream->start();
    if (r != oboe::Result::OK) {
        // Not fatal, and deliberately not silent. A render that proceeds against a stream which
        // failed to stop is the race this method exists to prevent, and it would otherwise present
        // as an intermittently corrupted WAV export with nothing in the log to explain it.
        LOGE("setPaused(%d) failed: %s", (int)paused, oboe::convertToText(r));
    }
}

void OboeAudioEngine::onErrorAfterClose(oboe::AudioStream* /*audioStream*/, oboe::Result error) {
    deviceLost_.store(true, std::memory_order_relaxed);
    // The repair is silent when it works, so a stream rebuilt in 10 ms and one that never died look
    // the same in the log without this line.
    LOGE("Stream lost (%s) - the frame loop will reopen it", oboe::convertToText(error));
}

oboe::DataCallbackResult OboeAudioEngine::onAudioReady(
        oboe::AudioStream* audioStream,
        void* audioData,
        int32_t numFrames) {
    // Pure Oboe glue: forward the device buffer to the portable core. All DSP, the offline-render
    // gate, chunking to PROCESS_SUBBLOCK, and the visualizer/peak capture live in processLiveBlock.
    core->processLiveBlock(static_cast<float*>(audioData), numFrames,
                           audioStream->getChannelCount(),
                           (float)audioStream->getSampleRate());
    return oboe::DataCallbackResult::Continue;
}
