// asio-audio-engine.cpp — see the header.

#include "asio-audio-engine.h"

#ifdef _WIN32

// Before anything can pull in <windows.h>: audio-engine.h uses std::min / std::max. Not
// WIN32_LEAN_AND_MEAN — the SDK needs the COM headers it leaves out.
#ifndef NOMINMAX
#define NOMINMAX
#endif

// <cmath> before <SDL.h> — see sdl-audio-engine.h.
#include <cmath>
#include <SDL.h>
#include <SDL_syswm.h>

#include <windows.h>
#include <objbase.h>   // CoInitializeEx

#include <cstdint>
#include <cstdio>
#include <cstring>

#include "asiosys.h"
#include "asio.h"
#include "asiodrivers.h"
#include "audio-engine.h"

extern AsioDrivers* asioDrivers;
bool loadAsioDriver(char* name);

namespace {

// The instance whose driver is calling back. The callbacks carry no user pointer.
AsioAudioEngine* g_open = nullptr;
// Handed to ASIOCreateBuffers, which keeps the pointer.
ASIOCallbacks g_callbacks{};

/**
 * The SDK's driver list, created once and never destroyed.
 *
 * ⚠️ Never destroyed ON PURPOSE: its destructor calls CoUninitialize, which would take COM away from
 * SDL on this thread. COM is started here in the apartment ASIO drivers expect; if SDL already put the
 * thread in the other one, that is kept (RPC_E_CHANGED_MODE) and the drivers still load.
 */
AsioDrivers& drivers() {
    static const HRESULT co = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    (void)co;
    if (!asioDrivers) asioDrivers = new AsioDrivers();
    return *asioDrivers;
}

/** The app's window — some drivers need it as the parent of their dialogs. */
void* main_window() {
    for (Uint32 id = 1; id < 32; ++id) {
        SDL_Window* w = SDL_GetWindowFromID(id);
        if (!w) continue;
        SDL_SysWMinfo info;
        SDL_VERSION(&info.version);
        if (SDL_GetWindowWMInfo(w, &info) && info.subsystem == SDL_SYSWM_WINDOWS)
            return info.info.win.window;
    }
    return GetDesktopWindow();
}

bool supported(long type) {
    switch (type) {
        case ASIOSTInt16LSB: case ASIOSTInt24LSB: case ASIOSTInt32LSB:
        case ASIOSTFloat32LSB: case ASIOSTFloat64LSB:
        case ASIOSTInt32LSB16: case ASIOSTInt32LSB18: case ASIOSTInt32LSB20: case ASIOSTInt32LSB24:
            return true;
        default:
            return false;
    }
}

const char* type_name(long type) {
    switch (type) {
        case ASIOSTInt16LSB:   return "int16";
        case ASIOSTInt24LSB:   return "int24";
        case ASIOSTInt32LSB:   return "int32";
        case ASIOSTFloat32LSB: return "float32";
        case ASIOSTFloat64LSB: return "float64";
        case ASIOSTInt32LSB16: return "int32/16";
        case ASIOSTInt32LSB18: return "int32/18";
        case ASIOSTInt32LSB20: return "int32/20";
        case ASIOSTInt32LSB24: return "int32/24";
        default:               return "unsupported";
    }
}

// How long a running driver may go without a buffer before it counts as gone. Far above any buffer
// size, so only a driver that has really stopped trips it.
constexpr ULONGLONG kSilentMs = 1000;

inline double clamp1(float x) { return x > 1.0f ? 1.0 : (x < -1.0f ? -1.0 : double(x)); }

/** Channel `ch` of the interleaved stereo `src` into one driver buffer, in the driver's format. */
void write_channel(long type, const float* src, int ch, int frames, void* dst) {
    const auto to_int32 = [&](double scale) {
        auto* d = static_cast<int32_t*>(dst);
        for (int i = 0; i < frames; ++i) d[i] = int32_t(std::lrint(clamp1(src[i * 2 + ch]) * scale));
    };
    switch (type) {
        case ASIOSTInt16LSB: {
            auto* d = static_cast<int16_t*>(dst);
            for (int i = 0; i < frames; ++i) d[i] = int16_t(std::lrint(clamp1(src[i * 2 + ch]) * 32767.0));
            break;
        }
        case ASIOSTInt24LSB: {
            auto* d = static_cast<uint8_t*>(dst);
            for (int i = 0; i < frames; ++i) {
                const int32_t v = int32_t(std::lrint(clamp1(src[i * 2 + ch]) * 8388607.0));
                d[i * 3 + 0] = uint8_t(v);
                d[i * 3 + 1] = uint8_t(v >> 8);
                d[i * 3 + 2] = uint8_t(v >> 16);
            }
            break;
        }
        case ASIOSTInt32LSB:   to_int32(2147483647.0); break;
        case ASIOSTInt32LSB16: to_int32(32767.0);      break;
        case ASIOSTInt32LSB18: to_int32(131071.0);     break;
        case ASIOSTInt32LSB20: to_int32(524287.0);     break;
        case ASIOSTInt32LSB24: to_int32(8388607.0);    break;
        case ASIOSTFloat32LSB: {
            auto* d = static_cast<float*>(dst);
            for (int i = 0; i < frames; ++i) d[i] = src[i * 2 + ch];
            break;
        }
        case ASIOSTFloat64LSB: {
            auto* d = static_cast<double*>(dst);
            for (int i = 0; i < frames; ++i) d[i] = double(src[i * 2 + ch]);
            break;
        }
        default: break;
    }
}

int bytes_per_sample(long type) {
    switch (type) {
        case ASIOSTInt16LSB:   return 2;
        case ASIOSTInt24LSB:   return 3;
        case ASIOSTFloat64LSB: return 8;
        default:               return 4;
    }
}

/** Counts the callback in and out, so a stop can wait until it has left. */
struct InCallback {
    std::atomic<int>& n;
    explicit InCallback(std::atomic<int>& c) : n(c) { ++n; }
    ~InCallback() { --n; }
};

/** Until no callback is running, bounded — a driver that never returns must not hang the app. */
void wait_idle(const std::atomic<int>& n) {
    const ULONGLONG deadline = GetTickCount64() + 500;
    while (n.load() > 0 && GetTickCount64() < deadline) Sleep(1);
}

}  // namespace

AsioAudioEngine::AsioAudioEngine(AudioEngine* core) : core_(core) {}

AsioAudioEngine::~AsioAudioEngine() { closeStream(); }

std::vector<std::string> AsioAudioEngine::driver_names() {
    std::vector<std::string> names;
    AsioDrivers& d = drivers();
    const long   n = d.asioGetNumDev();
    for (long i = 0; i < n; ++i) {
        char name[MAXDRVNAMELEN] = {};
        if (d.asioGetDriverName(int(i), name, MAXDRVNAMELEN) == 0 && name[0] != 0) names.emplace_back(name);
    }
    return names;
}

bool AsioAudioEngine::fail(const std::string& why) {
    std::fprintf(stderr, "audio:   ASIO %s: %s\n", driver_.c_str(), why.c_str());
    closeStream();
    error_ = why;
    return false;
}

bool AsioAudioEngine::openStream() {
    error_.clear();
    resetRequested_ = false;
    closedSilent_   = false;
    if (g_open != nullptr && g_open != this) return fail("ANOTHER ASIO DRIVER IS OPEN");

    char name[MAXDRVNAMELEN] = {};
    std::strncpy(name, driver_.c_str(), MAXDRVNAMELEN - 1);
    drivers();
    if (!loadAsioDriver(name)) return fail("DRIVER WILL NOT LOAD");

    ASIODriverInfo info{};
    info.asioVersion = 2;
    info.sysRef      = main_window();
    if (ASIOInit(&info) != ASE_OK) {
        // ASIOInit forgets a driver that failed, so ASIOExit would not release it.
        asioDrivers->removeCurrentDriver();
        std::fprintf(stderr, "audio:   ASIO %s: init failed: %s\n", driver_.c_str(), info.errorMessage);
        return fail("DRIVER BUSY OR UNPLUGGED");
    }
    loaded_ = true;

    long ins = 0, outs = 0;
    if (ASIOGetChannels(&ins, &outs) != ASE_OK || outs < 2) return fail("NO STEREO OUTPUT");

    ASIOSampleRate current = 0;
    ASIOGetSampleRate(&current);
    if (wantRate_ > 0 && int(current) != wantRate_) {
        if (ASIOCanSampleRate(ASIOSampleRate(wantRate_)) != ASE_OK)
            return fail("NO " + std::to_string(wantRate_) + " HZ");
        if (ASIOSetSampleRate(ASIOSampleRate(wantRate_)) != ASE_OK) return fail("RATE REFUSED");
        current = wantRate_;
    }
    rate_ = int(current);
    if (rate_ <= 0) return fail("NO SAMPLE RATE");

    long minSize = 0, maxSize = 0, preferred = 0, granularity = 0;
    if (ASIOGetBufferSize(&minSize, &maxSize, &preferred, &granularity) != ASE_OK || preferred <= 0)
        return fail("NO BUFFER SIZE");
    frames_ = int(preferred);

    for (long ch = 0; ch < 2; ++ch) {
        ASIOChannelInfo ci{};
        ci.channel = ch;
        ci.isInput = ASIOFalse;
        if (ASIOGetChannelInfo(&ci) != ASE_OK) return fail("NO CHANNEL INFO");
        if (!supported(ci.type)) return fail(std::string("FORMAT ") + std::to_string(ci.type));
        if (ch == 0) sampleType_ = ci.type;
        else if (ci.type != sampleType_) return fail("MIXED FORMATS");
    }

    ASIOBufferInfo bi[2] = {};
    for (long ch = 0; ch < 2; ++ch) {
        bi[ch].isInput    = ASIOFalse;
        bi[ch].channelNum = ch;
    }
    scratch_.assign(size_t(frames_) * 2, 0.0f);
    g_callbacks.bufferSwitch         = &AsioAudioEngine::on_buffer_switch;
    g_callbacks.sampleRateDidChange  = &AsioAudioEngine::on_rate_changed;
    g_callbacks.asioMessage          = &AsioAudioEngine::on_message;
    g_callbacks.bufferSwitchTimeInfo = &AsioAudioEngine::on_buffer_switch_time_info;
    // Before the buffers exist: a driver may send messages from inside ASIOCreateBuffers.
    g_open = this;
    if (ASIOCreateBuffers(bi, 2, preferred, &g_callbacks) != ASE_OK) return fail("BUFFERS REFUSED");
    buffersMade_ = true;
    for (int ch = 0; ch < 2; ++ch)
        for (int half = 0; half < 2; ++half) {
            buffers_[ch][half] = bi[ch].buffers[half];
            if (buffers_[ch][half])
                std::memset(buffers_[ch][half], 0, size_t(frames_) * size_t(bytes_per_sample(sampleType_)));
        }

    postOutput_ = (ASIOOutputReady() == ASE_OK);
    long inLatency = 0, outLatency = 0;
    latencyFrames_ = (ASIOGetLatencies(&inLatency, &outLatency) == ASE_OK) ? int(outLatency) : 0;

    // Only differs when no rate was asked for; loaded samples are pitched for the old one.
    if (core_ && core_->getSampleRate() != rate_) core_->setDeviceSampleRate(rate_);

    paused_ = false;
    lastCallbackMs_ = GetTickCount64();
    if (ASIOStart() != ASE_OK) return fail("START REFUSED");
    running_ = true;

    std::printf("audio:   ASIO %s: %d Hz, %d frames/buffer (%.1f ms), %s, %ld outputs, output latency %d "
                "frames (%.1f ms, driver's figure)\n",
                driver_.c_str(), rate_, frames_, 1000.0 * frames_ / rate_, type_name(sampleType_), outs,
                latencyFrames_, 1000.0 * latencyFrames_ / rate_);
    std::fflush(stdout);
    return true;
}

bool AsioAudioEngine::stalled() const {
    return running_ && !paused_.load() && GetTickCount64() - lastCallbackMs_.load() > kSilentMs;
}

bool AsioAudioEngine::deviceLost() const { return resetRequested_.load() || stalled(); }

void AsioAudioEngine::closeStream() {
    closedSilent_ = stalled();
    if (closedSilent_) std::printf("audio:   ASIO %s stopped calling back\n", driver_.c_str());
    if (running_) {
        ASIOStop();
        running_ = false;
    }
    wait_idle(inCallback_);
    if (buffersMade_) {
        ASIODisposeBuffers();
        buffersMade_ = false;
    }
    if (loaded_) {
        ASIOExit();
        loaded_ = false;
    }
    if (g_open == this) g_open = nullptr;
    for (auto& ch : buffers_) ch[0] = ch[1] = nullptr;
}

void AsioAudioEngine::setPaused(bool paused) {
    if (paused) {
        paused_ = true;
        if (running_) {
            ASIOStop();
            running_ = false;
        }
        wait_idle(inCallback_);
    } else {
        paused_ = false;
        lastCallbackMs_ = GetTickCount64();
        if (buffersMade_ && !running_ && ASIOStart() == ASE_OK) running_ = true;
    }
}

void AsioAudioEngine::render(long index) {
    const InCallback guard(inCallback_);
    lastCallbackMs_.store(GetTickCount64(), std::memory_order_relaxed);
    const int half = index != 0 ? 1 : 0;
    if (paused_.load() || !core_) {
        for (auto& ch : buffers_)
            if (ch[half]) std::memset(ch[half], 0, size_t(frames_) * size_t(bytes_per_sample(sampleType_)));
    } else {
        core_->processLiveBlock(scratch_.data(), frames_, 2, float(rate_));
        for (int ch = 0; ch < 2; ++ch)
            if (buffers_[ch][half]) write_channel(sampleType_, scratch_.data(), ch, frames_, buffers_[ch][half]);
    }
    if (postOutput_) ASIOOutputReady();
}

void AsioAudioEngine::on_buffer_switch(long index, long /*directProcess*/) {
    if (g_open) g_open->render(index);
}

ASIOTime* AsioAudioEngine::on_buffer_switch_time_info(ASIOTime* /*params*/, long index, long /*directProcess*/) {
    if (g_open) g_open->render(index);
    return nullptr;
}

void AsioAudioEngine::on_rate_changed(double rate) {
    if (g_open && int(rate) != g_open->rate_) g_open->resetRequested_ = true;
}

long AsioAudioEngine::on_message(long selector, long value, void* /*message*/, double* /*opt*/) {
    switch (selector) {
        case kAsioSelectorSupported:
            return (value == kAsioResetRequest || value == kAsioEngineVersion ||
                    value == kAsioResyncRequest || value == kAsioLatenciesChanged ||
                    value == kAsioSupportsTimeInfo || value == kAsioSupportsTimeCode)
                       ? 1 : 0;
        // A reset cannot happen on the driver's thread: flag it, and the app reopens from its loop.
        case kAsioResetRequest:
            if (g_open) g_open->resetRequested_ = true;
            return 1;
        case kAsioResyncRequest:    return 1;
        case kAsioLatenciesChanged: return 1;
        case kAsioEngineVersion:    return 2;
        case kAsioSupportsTimeInfo: return 0;   // the plain bufferSwitch is enough
        case kAsioSupportsTimeCode: return 0;
        default:                    return 0;
    }
}

#endif  // _WIN32
