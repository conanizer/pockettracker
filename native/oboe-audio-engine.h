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

#include "audio-backend.h"

class AudioEngine;  // portable core — full definition pulled in by the .cpp only

// `AudioBackend` since convergence C3 — the shared shell (shell/app.cpp) reaches its device through
// those five methods and nothing else, so `shell/android-main.cpp` hands it one of these where the
// desktop hands it an `SdlAudioEngine` and app.cpp does not change a character. The interface was
// derived from SdlAudioEngine's shape and this class already matched three of it; see
// native/audio-backend.h for why the virtuals cost nothing (they are lifecycle, never the callback).
//
// ⚠️ TWO OWNERS DURING PHASES C AND D, DELIBERATELY. `jni-bridge.cpp` still constructs one of these
// for the Compose app, and `android-main.cpp` constructs a different one for the SDL app. They are
// separate instances in separate activities and never coexist; Phase E deletes the first when the
// JNI facade goes. Nothing here is a singleton and nothing here may become one.
class OboeAudioEngine : public oboe::AudioStreamDataCallback, public AudioBackend {
public:
    // Borrows the core (owned by jni-bridge, or by android-main's `main`); does not take ownership.
    // Both owners destroy the shell before the core, so no callback can run against a freed core.
    explicit OboeAudioEngine(AudioEngine* core);
    ~OboeAudioEngine() override;

    bool openStream() override;
    void closeStream() override;
    void resumeStream() override;

    /**
     * Stop (and restart) the callback around an OFFLINE RENDER.
     *
     * ⚠️ **`stop()`, NOT `requestPause()`, AND THE REASON IS NOT MERELY THAT STOP IS STRONGER.**
     * `resumeStream()` above restarts the stream whenever it finds it in `Paused` — and the engine
     * calls `requestResume()` before *every scheduled note* (audio-engine.h:99). So a paused stream
     * is one note-on away from being restarted by the engine itself, in the middle of the render this
     * call exists to protect, from a code path that has no idea a render is happening. Stopping puts
     * the stream in `Stopped`, which `resumeStream`'s check does not match, so the restart cannot
     * happen behind the render's back. Choosing pause here would have compiled, run, and reopened
     * exactly the race this method closes.
     *
     * The engine's own `isOfflineRendering` flag already makes `processLiveBlock` return silence
     * without touching `processAudioBlock` — so what this adds is the window between setting that
     * flag and the last in-flight callback returning. `stop()` blocks on the state transition, which
     * is Oboe's equivalent of `SDL_PauseAudioDevice` blocking until the callback has returned.
     */
    void setPaused(bool paused) override;

    /** The rate the device actually negotiated, not the 44100 we asked for. */
    int sampleRate() const override { return sampleRate_; }

    oboe::DataCallbackResult onAudioReady(
            oboe::AudioStream* audioStream,
            void* audioData,
            int32_t numFrames) override;

private:
    AudioEngine* core;
    std::shared_ptr<oboe::AudioStream> stream;

    // Cached at openStream, exactly as SdlAudioEngine caches its own: the shell may ask for the rate
    // after closeStream has reset the stream pointer, and reaching into a platform stream object to
    // ask is the thing the AudioBackend seam exists to stop the shell doing.
    int sampleRate_ = 0;
};
