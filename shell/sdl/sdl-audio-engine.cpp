#include "sdl/sdl-audio-engine.h"

#include "audio-engine.h"
#include "latency_probe.h"

#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <cstring>

#if defined(__linux__) && !defined(__ANDROID__)
#include "linux/alsa-route.h"
#endif
#if defined(__linux__) && defined(PT_HANDHELD)
#include <pthread.h>
#include <sched.h>
#endif

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#endif

namespace {

// Frames per callback, 512 @ 48 kHz ≈ 10.7 ms — a latency choice; the engine chunks to its sub-block.
// ⚠️ A request: WASAPI and an ALSA dmix answer with their own period; only the size read back counts.
constexpr int FRAMES_PER_CALLBACK = 512;

// Asked where the route honours a small buffer (`choose_linux_route`); 256 holds on a handheld's chip,
// 128 and 64 do not.
constexpr int SMALL_FRAMES = 256;

// The fallback rate, asked with SDL_AUDIO_ALLOW_FREQUENCY_CHANGE. 48000, not 44100: ALSA's plug
// accepts any rate, so 44100 bought a silent conversion into a 48 kHz dmix. On Windows the endpoint's
// own rate is asked first (`windows_endpoint_rate`).
constexpr int PREFERRED_RATE = 48000;

#ifdef _WIN32
// The default output endpoint's rate, or 0. ⚠️ SDL cannot answer this and hides a mismatch behind its
// own resampler. `eConsole` is the endpoint SDL opens for a null device name.
int windows_endpoint_rate() {
    // RPC_E_CHANGED_MODE: COM is up in the other apartment — usable, not ours to shut down.
    const HRESULT co            = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const bool    weInitialised = SUCCEEDED(co);

    int                  rate    = 0;
    IMMDeviceEnumerator* devices = nullptr;
    if (SUCCEEDED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                   __uuidof(IMMDeviceEnumerator),
                                   reinterpret_cast<void**>(&devices)))) {
        IMMDevice* endpoint = nullptr;
        if (SUCCEEDED(devices->GetDefaultAudioEndpoint(eRender, eConsole, &endpoint))) {
            IAudioClient* client = nullptr;
            if (SUCCEEDED(endpoint->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                                             reinterpret_cast<void**>(&client)))) {
                WAVEFORMATEX* mix = nullptr;
                if (SUCCEEDED(client->GetMixFormat(&mix)) && mix != nullptr) {
                    rate = int(mix->nSamplesPerSec);
                    CoTaskMemFree(mix);
                }
                client->Release();
            }
            endpoint->Release();
        }
        devices->Release();
    }

    if (weInitialised) CoUninitialize();
    return rate;
}
#endif

// Printed on the boot line, so a reading stays attributable.
enum class RateSource { DEVICE, FALLBACK, ENV, PINNED };

const char* rate_source_text(RateSource s) {
    switch (s) {
        case RateSource::ENV:    return "env";
        case RateSource::DEVICE: return "the device";
        case RateSource::PINNED: return "the engine's, kept";
        case RateSource::FALLBACK: break;
    }
    return "fallback";
}

// POCKETTRACKER_AUDIO_FRAMES (a sweep aid) or the default, rounded down to a power of two in
// 32..8192. A bad value is refused on stderr — garbage looks like a device that ignored it.
int requested_frames() {
    const char* v = std::getenv("POCKETTRACKER_AUDIO_FRAMES");
    if (v == nullptr || *v == '\0') return FRAMES_PER_CALLBACK;

    const long n = std::strtol(v, nullptr, 10);
    if (n < 32 || n > 8192) {
        std::fprintf(stderr, "POCKETTRACKER_AUDIO_FRAMES=%s out of range (32..8192), using %d\n", v,
                     FRAMES_PER_CALLBACK);
        return FRAMES_PER_CALLBACK;
    }
    int pow2 = 32;
    while (pow2 * 2 <= int(n)) pow2 *= 2;
    return pow2;
}

// The rate to ask for: POCKETTRACKER_AUDIO_RATE (forces a conversion on purpose, to measure it), then
// the platform's answer (only Windows can be asked — SDL's ALSA backend reports zeros), then 48000.
int requested_rate(RateSource& source) {
    if (const char* v = std::getenv("POCKETTRACKER_AUDIO_RATE"); v != nullptr && *v != '\0') {
        const long n = std::strtol(v, nullptr, 10);
        if (n >= 8000 && n <= 192000) {
            source = RateSource::ENV;
            return int(n);
        }
        std::fprintf(stderr, "POCKETTRACKER_AUDIO_RATE=%s out of range (8000..192000), ignoring\n", v);
    }

#ifdef _WIN32
    if (const int hw = windows_endpoint_rate(); hw >= 8000 && hw <= 192000) {
        source = RateSource::DEVICE;
        return hw;
    }
#endif

    source = RateSource::FALLBACK;
    return PREFERRED_RATE;
}

#if defined(__linux__) && !defined(__ANDROID__)
// Device and buffer size from what stands between the app and the chip (alsa-route.h): a sound server
// gets the small size; a plain ALSA mixer chain is opened at its chip directly, on a handheld build
// only (that open is exclusive); anything else is left alone. AUDIODEV or AUDIO_FRAMES win.
void choose_linux_route(int& frames) {
    const bool framesPinned = std::getenv("POCKETTRACKER_AUDIO_FRAMES") != nullptr;
    if (const char* dev = std::getenv("AUDIODEV")) {
        std::printf("audio:   route: AUDIODEV=%s given, not inspected\n", dev);
        return;
    }

    const char* driver = SDL_GetCurrentAudioDriver();
    if (driver && (std::strcmp(driver, "pipewire") == 0 || std::strcmp(driver, "pulseaudio") == 0)) {
        if (!framesPinned) frames = SMALL_FRAMES;
        std::printf("audio:   route: SDL talks to %s itself - a sound server, asking %d frames\n", driver,
                    frames);
        return;
    }
    if (!driver || std::strcmp(driver, "alsa") != 0) return;

    const ptshell::DefaultRoute r = ptshell::inspect_default_route();
    switch (r.route) {
        case ptshell::AlsaRoute::DIRECT:
#ifdef PT_HANDHELD
            setenv("AUDIODEV", r.hw.c_str(), 1);
            if (!framesPinned) frames = SMALL_FRAMES;
            std::printf("audio:   route: %s - opening %s directly, asking %d frames\n", r.chain.c_str(),
                        r.hw.c_str(), frames);
#else
            std::printf("audio:   route: %s - left shared (not a handheld build)\n", r.chain.c_str());
#endif
            break;
        case ptshell::AlsaRoute::SERVER:
            if (!framesPinned) frames = SMALL_FRAMES;
            std::printf("audio:   route: %s - a sound server, asking %d frames\n", r.chain.c_str(), frames);
            break;
        case ptshell::AlsaRoute::LEAVE:
            std::printf("audio:   route: %s - left as is\n", r.chain.c_str());
            break;
    }
}
#endif

#if defined(__linux__) && defined(PT_HANDHELD)
// SCHED_FIFO for the audio thread, so a busy thread cannot delay its wake-up (with 2 × 256 frames
// ~5 ms is the whole margin). ⚠️ Not SDL's realtime hint: it goes through rtkit, which a handheld CFW
// lacks, and on failure leaves the thread worse off. A port runs as root. Priority 10: above ordinary
// threads, below the kernel's interrupt threads (50), which must run to wake this one.
void raise_audio_thread_priority() {
    sched_param p{};
    p.sched_priority = 10;
    const int err = pthread_setschedparam(pthread_self(), SCHED_FIFO, &p);
    if (err == 0) std::printf("audio:   callback thread on SCHED_FIFO %d\n", p.sched_priority);
    else std::printf("audio:   callback thread left as SDL set it (SCHED_FIFO refused: %s)\n", std::strerror(err));
    std::fflush(stdout);
}
#endif

}  // namespace

SdlAudioEngine::SdlAudioEngine(AudioEngine* core) : core_(core) {}

SdlAudioEngine::~SdlAudioEngine() { closeStream(); }

void SDLCALL SdlAudioEngine::audioCallback(void* userdata, Uint8* out, int lenBytes) {
    auto* self = static_cast<SdlAudioEngine*>(userdata);

#if defined(__linux__) && defined(PT_HANDHELD)
    static bool priorityRaised = false;   // the audio thread is the only caller
    if (!priorityRaised) {
        priorityRaised = true;
        raise_audio_thread_priority();
    }
#endif

    const int numFrames = lenBytes / int(sizeof(float)) / self->channels_;

    // ⚠️ `numFrames`, the size the device chose. Relaxed atomics only; off unless POCKETTRACKER_LATENCY=1.
    latency::audio_callback(numFrames, self->sampleRate_);

    // processLiveBlock clears the buffer (SDL's is not zeroed), sets flush-to-zero and chunks.
    // POCKETTRACKER_AUDIO_PROFILE=1: block time against the budget and the gap between callbacks, once
    // a second (block > budget = compute underrun; a big gap with a fast block = preempted).
    static const bool prof = (std::getenv("POCKETTRACKER_AUDIO_PROFILE") != nullptr);
    if (!prof) {
        self->core_->processLiveBlock(reinterpret_cast<float*>(out), numFrames, self->channels_,
                                      float(self->sampleRate_));
        return;
    }
    static uint64_t lastNs = 0, printNs = 0, maxBlk = 0, maxGap = 0, sumBlk = 0, cnt = 0, over = 0;
    uint64_t t0 = latency::now_ns();
    if (lastNs != 0) { uint64_t g = t0 - lastNs; if (g > maxGap) maxGap = g; }
    lastNs = t0;

    self->core_->processLiveBlock(reinterpret_cast<float*>(out), numFrames, self->channels_,
                                  float(self->sampleRate_));

    uint64_t t1 = latency::now_ns();
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

    int askedFrames = requested_frames();
#if defined(__linux__) && !defined(__ANDROID__)
    choose_linux_route(askedFrames);
#endif
    RateSource rateSource  = RateSource::FALLBACK;
    const int  askedRate   = pinnedRate_ > 0 ? pinnedRate_ : requested_rate(rateSource);
    if (pinnedRate_ > 0) rateSource = RateSource::PINNED;

    SDL_AudioSpec want{};
    want.freq     = askedRate;
    want.format   = AUDIO_F32SYS;
    want.channels = 2;
    want.samples  = Uint16(askedFrames);
    want.callback = &SdlAudioEngine::audioCallback;
    want.userdata = this;

    SDL_AudioSpec got{};

    // The device may pick the rate and buffer size. Format and channels are not negotiable — SDL would
    // slip a converter under the DSP — nor is a pinned rate.
    const int allow = (pinnedRate_ > 0 ? 0 : SDL_AUDIO_ALLOW_FREQUENCY_CHANGE) | SDL_AUDIO_ALLOW_SAMPLES_CHANGE;
    device_ = SDL_OpenAudioDevice(nullptr, 0, &want, &got, allow);

#ifndef _WIN32
    // An `hw:` AUDIODEV is exclusive: if something else holds the chip, play through the default
    // device instead. SDL reads AUDIODEV at open time.
    if (device_ == 0 && std::getenv("AUDIODEV") != nullptr) {
        std::fprintf(stderr, "audio:   AUDIODEV=%s failed (%s), trying the default device\n",
                     std::getenv("AUDIODEV"), SDL_GetError());
        unsetenv("AUDIODEV");
        device_ = SDL_OpenAudioDevice(nullptr, 0, &want, &got, allow);
    }
#endif

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

    // Before the unpause: the device opens paused, so no race on these fields.
    sampleRate_   = got.freq;
    channels_     = got.channels;
    bufferFrames_ = got.samples;  // ⚠️ the size the device chose

    if (core_) core_->setDeviceSampleRate(sampleRate_);

    SDL_PauseAudioDevice(device_, 0);

    // From the stored fields, so this and `outputLatency()` agree. Asked beside got: a device that
    // rounded the request vs one that ignored it.
    const char* audiodev = std::getenv("AUDIODEV");
    std::printf("audio:   %d Hz, %d ch, %d frames/callback (%.1f ms at least), asked %d Hz (%s) / %d "
                "frames, driver=%s, device=%s\n",
                sampleRate_, channels_, bufferFrames_, 1000.0 * bufferFrames_ / sampleRate_,
                askedRate, rate_source_text(rateSource), askedFrames, SDL_GetCurrentAudioDriver(),
                audiodev != nullptr ? audiodev : "default");
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
    // Pausing waits for a callback in flight, so afterwards the engine has one reader: us (see header).
    if (device_ != 0) SDL_PauseAudioDevice(device_, paused ? 1 : 0);
}
