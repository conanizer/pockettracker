// audio-backend.h — the audio device, the one thing the shared shell cannot decide for itself.
//
// Desktop and handheld open an SDL device (`SdlAudioEngine`, shell/); Android opens an Oboe stream
// (`OboeAudioEngine`, native/), which picks the lowest-latency path per device. The interface lives
// in native/ so the engine never includes from the shell above it; it has no SDL, POSIX or Oboe.
//
// ⚠️ NOT IN THE REAL-TIME PATH, which is why it may be virtual: the callback goes straight into
// `AudioEngine::processLiveBlock`. These are lifecycle calls — open, close, pause around an offline
// render, ask the rate and the latency.
//
// ⚠️ `SDL_INIT_AUDIO` is the backend's business: `SdlAudioEngine::openStream` initialises it, the
// shared `SDL_Init` does not, so on Android SDL audio is never initialised and cannot fight Oboe.
//
// ⚠️ Read `OboeAudioEngine::setPaused` before changing either implementation: both must guarantee
// the callback has finished when a pause returns.

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
     * ⚠️ The render drives the engine from the UI thread and the callback from the audio thread;
     * pausing makes "one writer" true by construction rather than by timing (see
     * `SdlAudioEngine::setPaused`).
     */
    virtual void setPaused(bool paused) = 0;

    /** The rate the device actually negotiated, not the one we asked for. */
    virtual int sampleRate() const = 0;

    /**
     * How long a frame written by the callback waits before it is heard, in frames at `sampleRate()`.
     *
     * ⚠️ **`measured` false means the number is a FLOOR, not the latency.** It is the app's own
     * buffer and nothing else: whatever the driver queues behind it is not in it, and on a handheld
     * that hidden term can be as large again. Only Oboe can ever report the real figure, and only
     * when the platform hands it a timestamp — SDL has no API for it at all. Anything that turns
     * this into a number a user reads has to say which of the two it got.
     *
     * 0 before a device is open; after `closeStream` it keeps the last device's figure, exactly as
     * `sampleRate()` does, because the shell asks both while shutting down.
     */
    struct OutputLatency {
        int  frames   = 0;
        bool measured = false;
    };
    virtual OutputLatency outputLatency() const = 0;

    /**
     * Did the platform take the device away while it was open? The shell sees nothing of that —
     * the loop runs, the transport counts, and the only symptom is silence — so the backend says.
     *
     * ⚠️ **FALSE IS "NOTHING TO REPORT", NOT "THE DEVICE IS FINE"**, which is why it is not pure.
     * Only Oboe reports it, and only on the AAudio path: OpenSL ES has no notion of
     * it (the platform re-routes an AudioTrack rather than killing it), and SDL's event for it is
     * not routed here.
     */
    virtual bool deviceLost() const { return false; }
};

#endif  // POCKETTRACKER_AUDIO_BACKEND_H
