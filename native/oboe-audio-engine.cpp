// oboe-audio-engine.cpp — Android (Oboe) audio backend. The only Oboe-coupled TU.
// Owns the Oboe stream lifecycle and the audio callback; all DSP/scheduling lives in the portable core.
#include "oboe-audio-engine.h"
#include "audio-engine.h"
#include "audio-defs.h"  // LOGD/LOGE/LOG_TAG (platform log shim)

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

bool OboeAudioEngine::openStream() {
    // OpenSL ES does NOT trigger CCodec/C2 codec enumeration that spams 2000+ log lines
    // and blocks for up to 35 seconds on some Android ROMs (e.g. GammaCoreOS on Miyoo Flip).
    // Try OpenSL ES first; fall back to AAudio only if OpenSL ES is unavailable.

    oboe::AudioStreamBuilder builder;
    builder.setDataCallback(this);
    builder.setFormat(oboe::AudioFormat::Float);
    builder.setChannelCount(oboe::ChannelCount::Stereo);

    // ⚠️⚠️ **NO setSampleRate, AND THAT IS THE POINT OF THIS WHOLE PATH.** Naming 44100 on hardware
    // that runs 48000 inserts a resampler, and a resampled stream commonly loses the fast mixer path
    // whichever API is underneath — which costs far more than picking the rate ever bought. Left
    // unspecified it opens at whatever `setPlatformDefaults` was told, and every consumer already
    // reads the result back: the callback passes the stream's own rate per block, and
    // `setDeviceSampleRate` below re-derives the send and master chains' coefficients from it.

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

    // ⚠️ stop(), not requestPause() — and the header explains why at length, because the difference
    // is not "stronger" but "correct": resumeStream() restarts a stream it finds Paused, and the
    // engine asks it to before every scheduled note. Stopped is the state that keeps the render's
    // exclusive access exclusive.
    //
    // The blocking forms (stop()/start(), not requestStop()/requestStart()) for the same reason
    // SDL_PauseAudioDevice is the right call on the other side: on the far side of this call there
    // must be exactly one reader of the engine, by construction rather than by timing.
    const oboe::Result r = paused ? stream->stop() : stream->start();
    if (r != oboe::Result::OK) {
        // Not fatal, and deliberately not silent. A render that proceeds against a stream which
        // failed to stop is the race this method exists to prevent, and it would otherwise present
        // as an intermittently corrupted WAV export with nothing in the log to explain it.
        LOGE("setPaused(%d) failed: %s", (int)paused, oboe::convertToText(r));
    }
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
