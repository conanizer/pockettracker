// audio-backend.h — the one thing the shared shell cannot decide for itself.
//
// Convergence C0.2. The frame loop, the boot sequence and the teardown are identical on every
// platform this shell will run on; the audio device is not. Desktop and handheld open an SDL device
// (`SdlAudioEngine`), and Android opens an Oboe stream (`native/oboe-audio-engine.h`) — because Oboe
// picks the lowest-latency path per device and works around known audio bugs on specific handsets,
// which on a tracker is the difference between a keypress sounding *now* and the app feeling laggy.
// That is convergence-plan §1's "SDL and Oboe are not a choice — take both", expressed as a type.
//
// ⚠️ **THIS FILE LIVES IN `native/`, NOT `shell/`, AND C3 MOVED IT THERE.** It was written in
// `shell/` because the shell was its only consumer, which was true right up until Oboe implemented
// it. The two implementations sit in different directories — `SdlAudioEngine` in `shell/`,
// `OboeAudioEngine` in `native/` — so an interface in `shell/` would have meant the engine layer
// including a header from the shell layer above it, which is the dependency edge this architecture
// spends its effort not having. `native/`'s include root is PUBLIC and every consumer already
// inherits it, so the move cost no include line anywhere: `#include "audio-backend.h"` resolves from
// both sides exactly as before. There is no SDL, no POSIX and no Oboe in this file, which is what
// makes it belong to the layer both backends can see rather than to either one of them.
//
// ⚠️ **THIS IS NOT IN THE REAL-TIME PATH, AND THAT IS WHY IT IS ALLOWED TO BE VIRTUAL.** The audio
// callback never comes through here: SDL calls `SdlAudioEngine::audioCallback` and Oboe calls
// `OboeAudioEngine::onAudioReady`, each straight into `AudioEngine::processLiveBlock`. The five
// methods below are lifecycle — open once, close once, pause around an offline render, ask the rate.
// A vtable on a handful of per-session calls costs nothing measurable; a vtable in the callback would
// have been a different question and this interface deliberately does not pose it.
//
// ⚠️ **`SDL_INIT_AUDIO` IS THE BACKEND'S BUSINESS, NOT THE SHELL'S.** `SdlAudioEngine::openStream`
// calls `SDL_InitSubSystem(SDL_INIT_AUDIO)` itself, and the shared `SDL_Init` asks only for VIDEO and
// GAMECONTROLLER. So on Android, where Oboe owns the device, the SDL audio subsystem is never
// initialised at all and the two cannot fight over it — the property convergence-plan C3 requires,
// already true today rather than something C3 has to arrange.
//
// ✅ **Both sides implement this as of C3.** The Oboe side already had three with the same shapes
// (`openStream` / `closeStream` / `resumeStream`); C3 added `setPaused` and `sampleRate` and
// inherited here. ⚠️ Read `OboeAudioEngine::setPaused` before changing either implementation — the
// two are NOT free to differ in whether they guarantee the callback has finished, and Oboe's version
// documents a second reason for its choice that the SDL one does not have.

#ifndef POCKETTRACKER_AUDIO_BACKEND_H
#define POCKETTRACKER_AUDIO_BACKEND_H

class AudioBackend {
  public:
    virtual ~AudioBackend() = default;

    /** Open the device and start it. False if the platform cannot give us stereo float32. */
    virtual bool openStream() = 0;
    virtual void closeStream() = 0;

    /** `AudioEngine::onResumeRequested` — the engine asking for its stream back. */
    virtual void resumeStream() = 0;

    /**
     * Stop (and restart) the callback around an OFFLINE RENDER.
     *
     * ⚠️ The render drives the engine from the UI thread and the callback drives it from the audio
     * thread. Pausing is what makes "one writer" true by construction rather than by timing — see
     * `SdlAudioEngine::setPaused`, which explains why leaving the device open and merely stopped (as
     * Kotlin does) is a race the app happens to win rather than a guarantee.
     */
    virtual void setPaused(bool paused) = 0;

    /** The rate the device actually negotiated, not the one we asked for. */
    virtual int sampleRate() const = 0;
};

#endif  // POCKETTRACKER_AUDIO_BACKEND_H
