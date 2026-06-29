#pragma once
// ───────────────────────────────────────────────────────────────────────────────────────────────
// ANDROID AUDIO BACKEND — the ONLY Oboe-coupled translation unit.
// Owns the output stream and IS its oboe::AudioStreamDataCallback. On each callback it hands the raw
// buffer to the portable AudioEngine core via processLiveBlock(); all DSP/scheduling lives in the core
// (audio-engine.{h,cpp}), which has no Oboe/Android dependency. A Linux port adds a sibling backend
// (e.g. AlsaAudioEngine) that drives the same core the same way — the core stays untouched.
// ───────────────────────────────────────────────────────────────────────────────────────────────
#include <oboe/Oboe.h>
#include <memory>

class AudioEngine;  // portable core — full definition pulled in by the .cpp only

class OboeAudioEngine : public oboe::AudioStreamDataCallback {
public:
    // Borrows the core (owned by jni-bridge); does not take ownership. jni-bridge destroys the shell
    // before the core so no callback can run against a freed core.
    explicit OboeAudioEngine(AudioEngine* core);
    ~OboeAudioEngine();

    bool openStream();
    void closeStream();
    void resumeStream();

    oboe::DataCallbackResult onAudioReady(
            oboe::AudioStream* audioStream,
            void* audioData,
            int32_t numFrames) override;

private:
    AudioEngine* core;
    std::shared_ptr<oboe::AudioStream> stream;
};
