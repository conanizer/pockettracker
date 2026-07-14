// sdl-audio-engine.{h,cpp} — the SDL2 audio backend.
//
// The desktop/handheld twin of oboe-audio-engine.{h,cpp}, and deliberately its mirror image: open a
// stereo float stream, hand each device buffer straight to AudioEngine::processLiveBlock(), own
// nothing else. Every sample of DSP, all scheduling, the offline-render gate, the scope/spectrum
// capture — all of it lives in the portable core. audio-engine.h's header comment named this file
// before it existed ("a future ALSA/JACK/SDL2 backend is a parallel file that calls
// processLiveBlock() the same way"), and native/CMakeLists.txt's ANDROID_PLATFORM_SOURCES describes
// the swap: the Linux port replaces exactly the Oboe/JNI shell and keeps everything above it.
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

class AudioEngine;

class SdlAudioEngine {
  public:
    explicit SdlAudioEngine(AudioEngine* core);
    ~SdlAudioEngine();

    // Opens a stereo float32 device and starts it. False if the platform cannot give us stereo
    // float — processAudioBlock has a stereo-only contract, so letting SDL convert behind its back
    // would be worse than failing loudly.
    bool openStream();
    void closeStream();

    // AudioEngine::onResumeRequested — the engine asks for its stream back without knowing what a
    // stream is. On Android that unpauses Oboe; here it unpauses the SDL device.
    void resumeStream();

    /**
     * Stop (and restart) the callback — what an OFFLINE RENDER needs (S7).
     *
     * ⚠️ The render drives the engine from the UI thread. The audio callback drives it from SDL's. A
     * paused device is the only thing that makes "one writer" true rather than merely likely: Kotlin
     * gets away with leaving Oboe open because a stopped transport means the callback reads a silent
     * engine, but "silent" is not "absent", and `renderOffline` is rewriting the very buffers it reads.
     * SDL_PauseAudioDevice blocks until the callback has returned, so after this call there is no
     * second reader — by construction, not by timing.
     */
    void setPaused(bool paused);

    // The rate the device actually negotiated, not the one we asked for.
    int sampleRate() const { return sampleRate_; }

  private:
    static void SDLCALL audioCallback(void* userdata, Uint8* out, int lenBytes);

    AudioEngine*      core_       = nullptr;
    SDL_AudioDeviceID device_     = 0;
    int               sampleRate_ = 0;
    int               channels_   = 0;
};

#endif  // POCKETTRACKER_SDL_AUDIO_ENGINE_H
