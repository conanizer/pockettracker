#include "sdl-audio-engine.h"

#include "audio-engine.h"

#include <cstdio>

namespace {

// Frames per callback. 512 @ 44.1 kHz ≈ 11.6 ms — the same order as Oboe's low-latency burst on
// device, and well under the engine's MAX_BLOCK (processLiveBlock chunks anyway, so this is a
// latency choice, never a correctness one). Handhelds may want more; that is a Phase 4 dial.
constexpr int FRAMES_PER_CALLBACK = 512;

// What we ask for. The device is free to say otherwise — see openStream.
constexpr int PREFERRED_RATE = 44100;

}  // namespace

SdlAudioEngine::SdlAudioEngine(AudioEngine* core) : core_(core) {}

SdlAudioEngine::~SdlAudioEngine() { closeStream(); }

void SDLCALL SdlAudioEngine::audioCallback(void* userdata, Uint8* out, int lenBytes) {
    auto* self = static_cast<SdlAudioEngine*>(userdata);

    // SDL hands us a byte length; the engine wants frames.
    const int numFrames = lenBytes / int(sizeof(float)) / self->channels_;

    // Pure SDL glue — the exact mirror of OboeAudioEngine::onAudioReady. processLiveBlock does
    // everything: sets flush-to-zero, CLEARS the buffer (SDL does not hand us a zeroed one), bails
    // to silence during an offline render, chunks into MAX_BLOCK processAudioBlock calls, and
    // captures the oscilloscope/spectrum/peak data.
    self->core_->processLiveBlock(reinterpret_cast<float*>(out), numFrames, self->channels_,
                                  float(self->sampleRate_));
}

bool SdlAudioEngine::openStream() {
    if (SDL_InitSubSystem(SDL_INIT_AUDIO) != 0) {
        std::fprintf(stderr, "SDL_InitSubSystem(AUDIO) failed: %s\n", SDL_GetError());
        return false;
    }

    SDL_AudioSpec want{};
    want.freq     = PREFERRED_RATE;
    want.format   = AUDIO_F32SYS;
    want.channels = 2;
    want.samples  = FRAMES_PER_CALLBACK;
    want.callback = &SdlAudioEngine::audioCallback;
    want.userdata = this;

    SDL_AudioSpec got{};

    // The device may pick its own RATE and buffer size — plenty of handhelds will not do 44100 —
    // and the engine is then TOLD what it got, exactly as Oboe reports its negotiated rate.
    //
    // FORMAT and CHANNELS are deliberately NOT negotiable. Allowing SDL to convert would silently
    // insert a format shim (and possibly a resampler) underneath the DSP, and processAudioBlock's
    // contract is stereo float — a contract the engine guards rather than assumes. Failing loudly
    // here beats sounding subtly wrong on one CFW.
    device_ = SDL_OpenAudioDevice(nullptr, 0, &want, &got,
                                  SDL_AUDIO_ALLOW_FREQUENCY_CHANGE | SDL_AUDIO_ALLOW_SAMPLES_CHANGE);
    if (device_ == 0) {
        std::fprintf(stderr, "SDL_OpenAudioDevice failed: %s\n", SDL_GetError());
        return false;
    }
    if (got.format != AUDIO_F32SYS || got.channels != 2) {
        std::fprintf(stderr, "audio device is not stereo float32 (format=0x%04X, channels=%d)\n",
                     got.format, got.channels);
        closeStream();
        return false;
    }

    // Set BEFORE the device is unpaused: SDL_OpenAudioDevice opens it paused, so the callback
    // cannot fire until the SDL_PauseAudioDevice below — no race on these two fields.
    sampleRate_ = got.freq;
    channels_   = got.channels;

    // Hand the negotiated rate to the core, which caches it for getSampleRate() and every bit of
    // pitch/tic math. Same contract as OboeAudioEngine::openStream — the core never reaches into a
    // platform stream object to ask.
    if (core_) core_->setDeviceSampleRate(sampleRate_);

    SDL_PauseAudioDevice(device_, 0);

    std::printf("audio:   %d Hz, %d ch, %d frames/callback (%.1f ms), driver=%s\n", sampleRate_,
                channels_, got.samples, 1000.0 * got.samples / sampleRate_,
                SDL_GetCurrentAudioDriver());
    return true;
}

void SdlAudioEngine::closeStream() {
    if (device_ != 0) {
        SDL_CloseAudioDevice(device_);
        device_ = 0;
    }
}

void SdlAudioEngine::resumeStream() {
    if (device_ != 0) SDL_PauseAudioDevice(device_, 0);
}
