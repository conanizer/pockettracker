#include "sdl-audio-engine.h"

#include "audio-engine.h"

#include <cstdio>
#include <cstdint>
#include <cstdlib>

namespace {

/**
 * A monotonic nanosecond stamp, for the profiler in audioCallback.
 *
 * ⚠️ SDL's clock, not POSIX's. This was `clock_gettime(CLOCK_MONOTONIC)` — which MSVC does not have, so
 * the shell did not compile on Windows AT ALL, and nothing said so because no CI job has ever built a
 * shell for any platform (convergence plan A4; every desktop artifact to date was hand-made on one
 * Linux box). The rest of the shell tells time with SDL_GetTicks64, but that is millisecond-resolution
 * and the work measured here runs in tens of microseconds — so this takes SDL's high-resolution
 * monotonic counter instead, which is the same clock underneath on both platforms.
 *
 * Integer arithmetic, split around the division: `counter * 1e9` overflows uint64 outright on a box
 * that has been up a while (a 10 MHz counter reaches 1e13 in a few weeks, and 1e13 * 1e9 does not fit),
 * which would make the profiler print garbage on exactly the long-running session worth profiling.
 */
uint64_t now_ns() {
    static const Uint64 freq = SDL_GetPerformanceFrequency();
    const Uint64        c    = SDL_GetPerformanceCounter();
    return (c / freq) * 1000000000ull + ((c % freq) * 1000000000ull) / freq;
}

// Frames per callback. 512 @ 44.1 kHz ≈ 11.6 ms — the same order as Oboe's low-latency burst on
// device, and well under the engine's MAX_BLOCK (processLiveBlock chunks anyway, so this is a
// latency choice, never a correctness one). Handhelds may want more; that is a Phase 4 dial.
constexpr int FRAMES_PER_CALLBACK = 512;  // device rounds up to its own period (940 on the Flip) regardless

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
    //
    // ── DIAGNOSTIC (env POCKETTRACKER_AUDIO_PROFILE=1): time the real-time work against the callback
    //    budget and the inter-callback gap. block>budget => compute underrun; big gap with a fast
    //    block => the audio thread was preempted; both fine => the artifact is not the audio thread.
    //    Single audio thread, so plain static locals; one rate-limited printf/sec. Off by default.
    static const bool prof = (std::getenv("POCKETTRACKER_AUDIO_PROFILE") != nullptr);
    if (!prof) {
        self->core_->processLiveBlock(reinterpret_cast<float*>(out), numFrames, self->channels_,
                                      float(self->sampleRate_));
        return;
    }
    static uint64_t lastNs = 0, printNs = 0, maxBlk = 0, maxGap = 0, sumBlk = 0, cnt = 0, over = 0;
    uint64_t t0 = now_ns();
    if (lastNs != 0) { uint64_t g = t0 - lastNs; if (g > maxGap) maxGap = g; }
    lastNs = t0;

    self->core_->processLiveBlock(reinterpret_cast<float*>(out), numFrames, self->channels_,
                                  float(self->sampleRate_));

    uint64_t t1 = now_ns();
    uint64_t blk = t1 - t0;
    if (blk > maxBlk) maxBlk = blk;
    sumBlk += blk; ++cnt;
    uint64_t budgetNs = uint64_t(numFrames) * 1000000000ull / uint64_t(self->sampleRate_ ? self->sampleRate_ : 44100);
    if (blk > budgetNs) ++over;
    if (printNs == 0) printNs = t1;
    if (t1 - printNs >= 1000000000ull) {
        std::printf("PROF: n=%llu avgBlk=%.2fms maxBlk=%.2fms over=%llu maxGap=%.2fms budget=%.2fms frames=%d\n",
                    (unsigned long long)cnt, cnt ? double(sumBlk) / double(cnt) / 1e6 : 0.0,
                    double(maxBlk) / 1e6, (unsigned long long)over, double(maxGap) / 1e6,
                    double(budgetNs) / 1e6, numFrames);
        std::fflush(stdout);
        printNs = t1; maxBlk = 0; maxGap = 0; sumBlk = 0; cnt = 0; over = 0;
    }
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

void SdlAudioEngine::setPaused(bool paused) {
    // SDL_PauseAudioDevice(dev, 1) does not return until any callback in flight has finished, so on
    // the far side of this call the engine has exactly one reader: us. See the header for why an
    // offline render needs that to be a guarantee rather than a coincidence.
    if (device_ != 0) SDL_PauseAudioDevice(device_, paused ? 1 : 0);
}
