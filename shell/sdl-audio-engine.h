// sdl-audio-engine.{h,cpp} — the SDL2 audio backend.
//
// The desktop/handheld twin of oboe-audio-engine: open a stereo float stream, hand each device
// buffer straight to AudioEngine::processLiveBlock(), own nothing else — all DSP and scheduling live
// in the portable core.
//
// ⚠️ NO DSP MAY EVER BE ADDED HERE. Same rule as onAudioReady: this is glue, not signal path.

#ifndef POCKETTRACKER_SDL_AUDIO_ENGINE_H
#define POCKETTRACKER_SDL_AUDIO_ENGINE_H

// <cmath> BEFORE <SDL.h>, and it is not an ordering nicety. The engine target defines
// _USE_MATH_DEFINES (PUBLIC — MSVC has no M_PI without it, and sampler-voice.h uses M_PI in a
// header), while SDL_stdinc.h defines M_PI itself behind an #ifndef. Whichever lands SECOND loses:
// pull in <cmath> first and ucrt defines it, SDL's guard then sees it and skips. The other order
// warns C4005 on every MSVC build.
#include <cmath>

#include <SDL.h>

#include "audio-backend.h"

class AudioEngine;

// The shared shell reaches its device through `AudioBackend` alone (audio-backend.h), so Android
// can hand it `OboeAudioEngine` instead with app.cpp unchanged.
class SdlAudioEngine : public AudioBackend {
  public:
    explicit SdlAudioEngine(AudioEngine* core);
    ~SdlAudioEngine() override;

    // Opens a stereo float32 device and starts it. False if the platform cannot give us stereo
    // float — processAudioBlock has a stereo-only contract, so letting SDL convert behind its back
    // would be worse than failing loudly.
    bool openStream() override;
    void closeStream() override;

    // AudioEngine::onResumeRequested — the engine asks for its stream back without knowing what a
    // stream is. On Android that unpauses Oboe; here it unpauses the SDL device.
    void resumeStream() override;

    /**
     * Stop (and restart) the callback — what an OFFLINE RENDER needs.
     *
     * ⚠️ The render drives the engine from the UI thread, the callback from SDL's. "Silent" is not
     * "absent": `renderOffline` rewrites the very buffers the callback reads. SDL_PauseAudioDevice
     * blocks until the callback has returned, so there is one writer by construction, not by timing.
     */
    void setPaused(bool paused) override;

    // The rate the device actually negotiated, not the one we asked for.
    int sampleRate() const override { return sampleRate_; }

    /**
     * Always a FLOOR, never the latency — `measured` is false here and cannot become true.
     *
     * SDL2 has no call that reports device latency: `SDL_GetQueuedAudioSize` is for queued audio and
     * says nothing about a callback device. So this reports the one term the app can see, its own
     * negotiated buffer, and leaves the driver's queue to the microphone.
     */
    OutputLatency outputLatency() const override { return {bufferFrames_, false}; }

    /**
     * Open at exactly `hz` from now on, converting if the device runs another rate; 0 negotiates.
     * For a REOPEN mid-session: loaded samples are pitched for the rate in force.
     */
    void pin_rate(int hz) { pinnedRate_ = hz; }

  private:
    static void SDLCALL audioCallback(void* userdata, Uint8* out, int lenBytes);

    AudioEngine*      core_         = nullptr;
    SDL_AudioDeviceID device_       = 0;
    int               sampleRate_   = 0;
    int               channels_     = 0;
    int               bufferFrames_ = 0;  // what the device chose, not FRAMES_PER_CALLBACK
    int               pinnedRate_   = 0;
};

#endif  // POCKETTRACKER_SDL_AUDIO_ENGINE_H
