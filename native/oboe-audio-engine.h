#pragma once
// The Android audio backend, the only Oboe-coupled unit: owns the stream and hands each callback's
// buffer to AudioEngine::processLiveBlock.
#include <oboe/Oboe.h>
#include <atomic>
#include <memory>
#include <string>

#include "audio-backend.h"

class AudioEngine;

// Built by android-main's `main()`; never a singleton.
class OboeAudioEngine : public oboe::AudioStreamDataCallback,
                        public oboe::AudioStreamErrorCallback,
                        public AudioBackend {
public:
    // Borrows the core; the owner destroys the shell first.
    explicit OboeAudioEngine(AudioEngine* core);
    ~OboeAudioEngine() override;

    // The device's own rate and burst, before openStream(). ⚠️ OpenSL ES cannot ask the device, so on
    // that path these alone keep a resampler out. Each independent; <= 0 keeps Oboe's default.
    void setPlatformDefaults(int sampleRate, int framesPerBurst);

    // While this file exists the device stays on OpenSL ES (see openStream). Empty = no guard.
    void setSlowOpenMarker(std::string path);

    // Set while a screen recording runs. ⚠️ The direct path is silent in a recording and a stream keeps
    // its path, so it is avoided while set, and `deviceLost` asks for one reopen per flip. Null = never.
    void setPlaybackCaptured(const std::atomic<bool>* captured);

    bool openStream() override;
    void closeStream() override;
    void resumeStream() override;

    // Around an offline render. ⚠️ `stop()`, not `requestPause()`: every scheduled note asks for a
    // resume, which restarts a Paused stream mid-render. `stop()` blocks until the callback is out.
    void setPaused(bool paused) override;

    // The negotiated rate.
    int sampleRate() const override { return sampleRate_; }

    // Measured when Oboe has a timestamp, else our buffer. ⚠️ Never from the audio callback — before
    // Android 11 that can stall the stream.
    OutputLatency outputLatency() const override;

    oboe::DataCallbackResult onAudioReady(
            oboe::AudioStream* audioStream,
            void* audioData,
            int32_t numFrames) override;

    // ⚠️ Raises a flag and nothing else — it runs on an Oboe thread; the frame loop reopens.
    void onErrorAfterClose(oboe::AudioStream* audioStream, oboe::Result error) override;

    bool deviceLost() const override;

private:
    AudioEngine* core;
    std::shared_ptr<oboe::AudioStream> stream;

    // Cached at openStream: the shell asks after closeStream too.
    int sampleRate_   = 0;
    int bufferFrames_ = 0;  // `outputLatency`'s fallback

    // What the platform reported (0 = nothing), for the boot line only.
    int platformRate_  = 0;
    int platformBurst_ = 0;

    std::string slowOpenMarker_;

    // The frame loop's thread only.
    const std::atomic<bool>* playbackCaptured_ = nullptr;
    bool reopenOnRecording_    = false;  // on the direct path, no recording running
    bool reopenAfterRecording_ = false;  // kept off it by one

    // Raised on Oboe's error thread; cleared when a reopen is attempted, not when it succeeds.
    std::atomic<bool> deviceLost_{false};
};
