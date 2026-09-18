#include "sdl-audio-engine.h"

#include "audio-engine.h"
#include "latency_probe.h"

#include <cstdio>
#include <cstdint>
#include <cstdlib>

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

// Frames per callback. 512 @ 48 kHz ≈ 10.7 ms. It is ABOVE the engine's PROCESS_SUBBLOCK and that is
// fine: processLiveBlock chunks to it, so this number is a latency choice and never a correctness one.
//
// ⚠️ **IT IS A REQUEST, AND NOTHING MEASURED HAS EVER GRANTED IT** — WASAPI hands back its own 10 ms
// period (480 frames at 48 kHz) for anything from 64 to 2048, and the Flip's ALSA its own 1024. Only
// the size read back in openStream is the one the callback actually runs at.
constexpr int FRAMES_PER_CALLBACK = 512;

// The rate to ask for when the platform cannot be asked what it actually runs. ⚠️ A REQUEST, never an
// assumption: `SDL_AUDIO_ALLOW_FREQUENCY_CHANGE` is set, so hardware that really runs 44.1 answers
// 44.1 — on the backends that report it.
//
// ⚠️⚠️ **48000 BECAUSE ASKING FOR 44100 BOUGHT A SILENT CONVERSION ON EVERY DEVICE MEASURED.** The
// layer that converts is also the layer that answers questions about itself: ALSA's plug accepts any
// rate, so the app was told 44100 while the driver read 48000, and the odd 940-frame callback it
// reported was the hardware's own 1024-frame period divided by 44100/48000. Asked for 48000 the same
// device reports 1024. ⚠️ It removes a conversion and NOT latency — the buffer's duration is
// unchanged (21.32 → 21.33 ms on that device).
//
// ⚠️⚠️ **ON WINDOWS THIS IS A FALLBACK AND NOTHING MORE** — `windows_endpoint_rate()` below. A desktop
// commonly has several outputs at different rates, and which one is in charge changes the moment a
// headset connects, so any fixed request there is right only by luck.
constexpr int PREFERRED_RATE = 48000;

#ifdef _WIN32
/**
 * What the DEFAULT output endpoint actually runs at, or 0 if Windows cannot be asked.
 *
 * ⚠️⚠️ **SDL CANNOT ANSWER THIS, AND — WORSE — IT NEVER REPORTS THE MISMATCH.** When the requested
 * rate differs from the endpoint's, `SDL_wasapi.c` sets `AUTOCONVERTPCM` and then OVERWRITES the
 * format with what was asked for, so `SDL_AUDIO_ALLOW_FREQUENCY_CHANGE` can never fire and the boot
 * line reports the request back as though it were the hardware. Measured: with a 44.1 kHz headset as
 * the default output, asking 48000 gave `48000 Hz, 480 frames` with a resampler in the path, and the
 * only way to tell was asking the OS which endpoint held the process's audio session.
 *
 * ⚠️ `eConsole` is by definition the endpoint SDL opens when it is passed a null device name. It is
 * NOT an enumeration index — index 0 is whatever was listed first and need not be the default at all,
 * which is exactly how an earlier diagnostic came to print a rate unrelated to the open stream.
 *
 * ⚠️ A device switched between this call and `SDL_OpenAudioDevice` costs one converted session, which
 * is no worse than the fixed request it replaces.
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
enum class RateSource { DEVICE, FALLBACK, ENV };

const char* rate_source_text(RateSource s) {
    switch (s) {
        case RateSource::ENV:    return "env";
        case RateSource::DEVICE: return "the device";
        case RateSource::FALLBACK: break;
    }
    return "fallback";
}

/**
 * The size to ask for — `POCKETTRACKER_AUDIO_FRAMES` overrides the default.
 *
 * A diagnostic and deliberately NOT a setting: it sweeps a device for the smallest buffer that
 * device will honour, without a rebuild per size. Rounded DOWN to a power of two (SDL's contract for
 * `samples`) and clamped to 32..8192 — a bad value is refused on stderr, since a request that lands
 * as garbage looks exactly like a device that ignored it.
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
 * ⚠️ **ONLY WINDOWS CAN BE ASKED.** SDL's ALSA backend stores a null device spec, so the probe there
 * reports zeros — read off the source and confirmed on the device. The Flip's 48000 request therefore
 * stands on its own measurement rather than on anything queried at runtime.
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

}  // namespace

SdlAudioEngine::SdlAudioEngine(AudioEngine* core) : core_(core) {}

SdlAudioEngine::~SdlAudioEngine() { closeStream(); }

void SDLCALL SdlAudioEngine::audioCallback(void* userdata, Uint8* out, int lenBytes) {
    auto* self = static_cast<SdlAudioEngine*>(userdata);

    // SDL hands us a byte length; the engine wants frames.
    const int numFrames = lenBytes / int(sizeof(float)) / self->channels_;

    // ⚠️ `numFrames` and not the constant we asked for: this is the size the DEVICE chose, which on the
    // Flip is 940 where 512 was requested. Relaxed atomics only — nothing here may block. Off unless
    // POCKETTRACKER_LATENCY=1, and one cached bool when it is.
    latency::audio_callback(numFrames, self->sampleRate_);

    // Pure SDL glue — the exact mirror of OboeAudioEngine::onAudioReady. processLiveBlock does
    // everything: sets flush-to-zero, CLEARS the buffer (SDL does not hand us a zeroed one), bails
    // to silence during an offline render, chunks into PROCESS_SUBBLOCK processAudioBlock calls, and
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

    const int  askedFrames = requested_frames();
    RateSource rateSource  = RateSource::FALLBACK;
    const int  askedRate   = requested_rate(rateSource);

    SDL_AudioSpec want{};
    want.freq     = askedRate;
    want.format   = AUDIO_F32SYS;
    want.channels = 2;
    want.samples  = Uint16(askedFrames);
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
    // cannot fire until the SDL_PauseAudioDevice below — no race on these three fields.
    sampleRate_   = got.freq;
    channels_     = got.channels;
    bufferFrames_ = got.samples;  // ⚠️ the SIZE THE DEVICE CHOSE — 441 here, 940 on the Flip, 512 asked

    // Hand the negotiated rate to the core, which caches it for getSampleRate() and every bit of
    // pitch/tic math. Same contract as OboeAudioEngine::openStream — the core never reaches into a
    // platform stream object to ask.
    if (core_) core_->setDeviceSampleRate(sampleRate_);

    SDL_PauseAudioDevice(device_, 0);

    // Printed from the stored fields, not from `got`, so the line and `outputLatency()` cannot drift
    // apart and quietly disagree about what the device is doing. What was ASKED is printed beside
    // what was negotiated, never instead of it — the two differing is the normal case, and it is the
    // only way to tell a device that rounded the request from one that ignored it.
    //
    // ⚠️ Neither half is a reading of the HARDWARE. See PREFERRED_RATE.
    std::printf("audio:   %d Hz, %d ch, %d frames/callback (%.1f ms at least), asked %d Hz (%s) / %d "
                "frames, driver=%s\n",
                sampleRate_, channels_, bufferFrames_, 1000.0 * bufferFrames_ / sampleRate_,
                askedRate, rate_source_text(rateSource), askedFrames, SDL_GetCurrentAudioDriver());
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
