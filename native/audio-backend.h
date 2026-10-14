// The audio device: SDL on desktop and handhelds (`SdlAudioEngine`), Oboe on Android
// (`OboeAudioEngine`). Lifecycle calls only — the callback goes straight to processLiveBlock.
// ⚠️ `SDL_INIT_AUDIO` is SdlAudioEngine's own, so on Android SDL audio never fights Oboe.
// ⚠️ Both implementations must guarantee the callback has finished when a pause returns.

#ifndef POCKETTRACKER_AUDIO_BACKEND_H
#define POCKETTRACKER_AUDIO_BACKEND_H

class AudioBackend {
  public:
    virtual ~AudioBackend() = default;

    // False if the platform cannot give stereo float32.
    virtual bool openStream() = 0;
    virtual void closeStream() = 0;

    // `AudioEngine::onResumeRequested`.
    virtual void resumeStream() = 0;

    // Around an offline render, so the UI thread is the engine's only driver.
    virtual void setPaused(bool paused) = 0;

    // The negotiated rate.
    virtual int sampleRate() const = 0;

    // Frames from the callback to the ear. ⚠️ `measured` false = a FLOOR (our buffer only; the driver
    // can add as much again) — only Oboe with a timestamp measures. 0 before open; kept after close.
    struct OutputLatency {
        int  frames   = 0;
        bool measured = false;
    };
    virtual OutputLatency outputLatency() const = 0;

    // Did the platform take the device away? ⚠️ False = nothing to report, not "fine": only Oboe on
    // AAudio says.
    virtual bool deviceLost() const { return false; }
};

#endif  // POCKETTRACKER_AUDIO_BACKEND_H
