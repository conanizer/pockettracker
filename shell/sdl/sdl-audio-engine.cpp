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

/**
 * A monotonic nanosecond stamp, for the profiler in audioCallback.
 *
 * ⚠️ SDL's high-resolution counter, not `clock_gettime` (MSVC has none) and not SDL_GetTicks64
 * (milliseconds, where the work measured runs in tens of microseconds). Split around the division:
 * `counter * 1e9` overflows uint64 after a few weeks' uptime.
 */
uint64_t now_ns() {
    static const Uint64 freq = SDL_GetPerformanceFrequency();
    const Uint64        c    = SDL_GetPerformanceCounter();
    return (c / freq) * 1000000000ull + ((c % freq) * 1000000000ull) / freq;
}

// Frames per callback. 512 @ 48 kHz ≈ 10.7 ms. It is ABOVE the engine's PROCESS_SUBBLOCK and that is
// fine: processLiveBlock chunks to it, so this number is a latency choice and never a correctness one.
//
// ⚠️ **IT IS A REQUEST.** WASAPI hands back its own 10 ms period (480 frames at 48 kHz) for anything
// from 64 to 2048, and an ALSA dmix its own configured period (1024 on the Flip's default). Only the
// size read back in openStream is the one the callback actually runs at.
constexpr int FRAMES_PER_CALLBACK = 512;

// Asked instead where the route is known to honour a small buffer (`choose_linux_route`); 256 holds
// with headroom straight to a handheld's chip, where 128 and 64 do not.
constexpr int SMALL_FRAMES = 256;

// The rate to ask for when the platform cannot be asked what it actually runs. ⚠️ A REQUEST, never an
// assumption: `SDL_AUDIO_ALLOW_FREQUENCY_CHANGE` is set, so hardware that really runs 44.1 answers
// 44.1 — on the backends that report it.
//
// ⚠️⚠️ 48000 BECAUSE 44100 BOUGHT A SILENT CONVERSION: ALSA's plug accepts any rate, so the app was
// told 44100 while a 48 kHz dmix mixed. ⚠️ It removes a conversion, NOT latency.
//
// ⚠️⚠️ **ON WINDOWS THIS IS A FALLBACK AND NOTHING MORE** — `windows_endpoint_rate()` below. A desktop
// commonly has several outputs at different rates, and which one is in charge changes the moment a
// headset connects, so any fixed request there is right only by luck.
constexpr int PREFERRED_RATE = 48000;

#ifdef _WIN32
/**
 * What the DEFAULT output endpoint actually runs at, or 0 if Windows cannot be asked.
 *
 * ⚠️⚠️ SDL CANNOT ANSWER THIS AND NEVER REPORTS THE MISMATCH: on a differing rate `SDL_wasapi.c` sets
 * `AUTOCONVERTPCM` and overwrites the format with the request, so the boot line reports the request
 * as the hardware while a resampler runs.
 *
 * ⚠️ `eConsole` is by definition the endpoint SDL opens for a null device name — NOT enumeration
 * index 0. A device switched before `SDL_OpenAudioDevice` costs one converted session.
 */
int windows_endpoint_rate() {
    // RPC_E_CHANGED_MODE means COM is already up on this thread in the other apartment — usable, but
    // not ours to shut down. Only a call that SUCCEEDED gets a matching CoUninitialize.
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

/** Where the requested rate came from — printed on the boot line, so a reading stays attributable. */
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

/**
 * The size to ask for — `POCKETTRACKER_AUDIO_FRAMES` overrides the default (a sweep aid, not a user
 * setting) and `choose_linux_route`. Rounded DOWN to a power of two (SDL's contract) and clamped to
 * 32..8192; a bad value is refused on stderr, since garbage looks like a device that ignored it.
 */
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

/**
 * The rate to ask the device for, and where that number came from.
 *
 * Three sources, in order. `POCKETTRACKER_AUDIO_RATE` wins — it is the diagnostic that forces a
 * conversion on purpose, which is the only way to measure what one costs. Then the platform's own
 * answer, where a platform has one. Then the fallback constant.
 *
 * ⚠️ ONLY WINDOWS CAN BE ASKED: SDL's ALSA backend stores a null device spec, so a probe reports zeros.
 */
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
/**
 * Pick the device and buffer size from what stands between the app and the chip (alsa-route.h).
 *
 * A sound server takes the small size and keeps volume and Bluetooth. A plain ALSA mixer chain is
 * opened at its chip directly — on a handheld build only, since that open is exclusive. Anything else
 * is left exactly as it was. An explicit AUDIODEV or POCKETTRACKER_AUDIO_FRAMES wins over all of it.
 */
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
/**
 * Put the calling (audio) thread on SCHED_FIFO, so a busy UI or system thread cannot hold its wake-up
 * back — with 2 × 256 frames that costs the whole margin in ~5 ms.
 *
 * ⚠️ NOT SDL's `SDL_THREAD_FORCE_REALTIME_TIME_CRITICAL`: that goes through rtkit over D-Bus, which a
 * handheld CFW does not run, and when it fails SDL leaves the thread at nice 0 — WORSE than the −20
 * it sets without the hint. A port runs as root, so the kernel is asked directly. Priority 10: above
 * every ordinary thread, below the kernel's interrupt threads (50), which must run to wake this one.
 */
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

    // SDL hands us a byte length; the engine wants frames.
    const int numFrames = lenBytes / int(sizeof(float)) / self->channels_;

    // ⚠️ `numFrames` and not the constant we asked for: this is the size the DEVICE chose, which through
    // a dmix is its own period whatever was requested. Relaxed atomics only — nothing here may block.
    // Off unless POCKETTRACKER_LATENCY=1, and one cached bool when it is.
    latency::audio_callback(numFrames, self->sampleRate_);

    // Pure SDL glue, the mirror of OboeAudioEngine::onAudioReady: processLiveBlock sets flush-to-zero,
    // CLEARS the buffer (SDL's is not zeroed), silences during an offline render, chunks, and
    // captures the scopes.
    //
    // ── DIAGNOSTIC (POCKETTRACKER_AUDIO_PROFILE=1): the work against the callback budget and the gap
    //    between callbacks. block > budget = compute underrun; a big gap with a fast block = preempted.
    //    One rate-limited printf a second; off by default.
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

    // The device may pick its own RATE and buffer size, and the engine is told what it got.
    // FORMAT and CHANNELS are NOT negotiable: SDL would silently insert a format shim (or resampler)
    // under the DSP, and processAudioBlock's contract is stereo float. A pinned rate is not either —
    // SDL converts rather than hand back another.
    const int allow = (pinnedRate_ > 0 ? 0 : SDL_AUDIO_ALLOW_FREQUENCY_CHANGE) | SDL_AUDIO_ALLOW_SAMPLES_CHANGE;
    device_ = SDL_OpenAudioDevice(nullptr, 0, &want, &got, allow);

#ifndef _WIN32
    // AUDIODEV points ALSA at one device (`choose_linux_route`, or the user). An `hw:` open is
    // exclusive, so if anything else holds the chip, play through the default device instead of not
    // at all. SDL reads AUDIODEV at open time, so unsetting it is enough.
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

    // Set BEFORE the device is unpaused: SDL_OpenAudioDevice opens it paused, so the callback
    // cannot fire until the SDL_PauseAudioDevice below — no race on these three fields.
    sampleRate_   = got.freq;
    channels_     = got.channels;
    bufferFrames_ = got.samples;  // ⚠️ the SIZE THE DEVICE CHOSE, not the one asked

    // Hand the negotiated rate to the core (getSampleRate(), all pitch/tic math).
    if (core_) core_->setDeviceSampleRate(sampleRate_);

    SDL_PauseAudioDevice(device_, 0);

    // Printed from the stored fields, so the line and `outputLatency()` cannot disagree. What was
    // ASKED is printed beside what was negotiated — the only way to tell a device that rounded the
    // request from one that ignored it. ⚠️ Neither half is a reading of the HARDWARE (PREFERRED_RATE).
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
    // SDL_PauseAudioDevice(dev, 1) does not return until any callback in flight has finished, so on
    // the far side of this call the engine has exactly one reader: us. See the header for why an
    // offline render needs that to be a guarantee rather than a coincidence.
    if (device_ != 0) SDL_PauseAudioDevice(device_, paused ? 1 : 0);
}
