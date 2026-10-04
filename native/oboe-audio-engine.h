#pragma once
// ───────────────────────────────────────────────────────────────────────────────────────────────
// ANDROID AUDIO BACKEND — the ONLY Oboe-coupled translation unit. Owns the output stream and IS its
// oboe::AudioStreamDataCallback; each callback hands the buffer to AudioEngine::processLiveBlock,
// where all DSP and scheduling live.
// ───────────────────────────────────────────────────────────────────────────────────────────────
#include <oboe/Oboe.h>
#include <atomic>
#include <memory>
#include <string>

#include "audio-backend.h"

class AudioEngine;  // portable core — full definition pulled in by the .cpp only

// The shared shell reaches its device through `AudioBackend` alone; android-main.cpp hands it one
// of these where the desktop hands an `SdlAudioEngine`. Constructed by android-main's `main()`;
// nothing here is a singleton and nothing here may become one.
class OboeAudioEngine : public oboe::AudioStreamDataCallback,
                        public oboe::AudioStreamErrorCallback,
                        public AudioBackend {
public:
    // Borrows the core (owned by android-main's `main`); does not take ownership. The owner destroys
    // the shell before the core, so no callback can run against a freed core.
    explicit OboeAudioEngine(AudioEngine* core);
    ~OboeAudioEngine() override;

    /**
     * The device's OWN output rate and burst size, handed in before openStream().
     *
     * ⚠️⚠️ **THE OpenSL ES PATH CANNOT ASK THE DEVICE ITSELF.** AAudio opens at the native rate when
     * none is requested; OpenSL ES has no such query and falls back to Oboe's built-in guess for both
     * numbers. So on the fallback path, and on Android 8.0 where it is the first path, these two
     * values are the only thing standing between the stream and a resampler.
     *
     * Best effort, and the two are independent: a value <= 0 leaves Oboe's own default alone, so a
     * platform that answers for one and not the other still gets the half it knows.
     */
    void setPlatformDefaults(int sampleRate, int framesPerBurst);

    /**
     * A file in private storage that, while it exists, keeps this device on OpenSL ES. openStream
     * writes it before trying the modern API and deletes it once that opened quickly — see there.
     * Empty (the default) means no guard: the modern API is tried every time.
     */
    void setSlowOpenMarker(std::string path);

    bool openStream() override;
    void closeStream() override;
    void resumeStream() override;

    /**
     * Stop (and restart) the callback around an OFFLINE RENDER.
     *
     * ⚠️ `stop()`, NOT `requestPause()`: `resumeStream()` restarts a stream it finds `Paused`, and
     * the engine calls `requestResume()` before every scheduled note — so a paused stream would be
     * restarted mid-render. `Stopped` does not match that check. `stop()` blocks on the transition,
     * as `SDL_PauseAudioDevice` blocks until the callback returns; with `isOfflineRendering` already
     * silencing `processLiveBlock`, this closes the window for the last in-flight callback.
     */
    void setPaused(bool paused) override;

    /** The rate the device actually negotiated. Nothing asks for a rate any more — see openStream. */
    int sampleRate() const override { return sampleRate_; }

    /**
     * The real figure when the platform gives Oboe a timestamp, the app's own buffer when it does not.
     *
     * ⚠️ **NOT FROM THE AUDIO CALLBACK.** Oboe's own note: on Android before R, asking a running
     * stream for its timestamp from inside the data callback can stall it. Every caller here is the
     * shell, on the frame loop or on the way out.
     */
    OutputLatency outputLatency() const override;

    oboe::DataCallbackResult onAudioReady(
            oboe::AudioStream* audioStream,
            void* audioData,
            int32_t numFrames) override;

    /**
     * The stream died and Oboe has already closed it. ⚠️ **RAISES A FLAG AND NOTHING ELSE** — it runs
     * on an Oboe thread while the shell may be inside any other method here, so reopening (or even
     * dropping `stream`) would race all of them. The frame loop owns the repair; same rule as the
     * SIGTERM handler in `shell/main.cpp`.
     */
    void onErrorAfterClose(oboe::AudioStream* audioStream, oboe::Result error) override;

    bool deviceLost() const override { return deviceLost_.load(std::memory_order_relaxed); }

private:
    AudioEngine* core;
    std::shared_ptr<oboe::AudioStream> stream;

    // Cached at openStream, exactly as SdlAudioEngine caches its own: the shell may ask for the rate
    // after closeStream has reset the stream pointer, and reaching into a platform stream object to
    // ask is the thing the AudioBackend seam exists to stop the shell doing.
    int sampleRate_   = 0;
    int bufferFrames_ = 0;  // the stream's own buffer — the floor `outputLatency` falls back to

    // What the platform said it runs at, or 0 where it would not say. Kept only so the boot line can
    // print what was ASKED beside what was negotiated — the request itself goes into Oboe's global
    // defaults in setPlatformDefaults, not into the builder.
    int platformRate_  = 0;
    int platformBurst_ = 0;

    std::string slowOpenMarker_;

    // Raised on Oboe's error thread, read by the frame loop, cleared when a reopen is ATTEMPTED (see
    // openStream) rather than when one succeeds.
    std::atomic<bool> deviceLost_{false};
};
