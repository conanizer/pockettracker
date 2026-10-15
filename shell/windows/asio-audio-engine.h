// The ASIO audio backend (Windows; an empty TU elsewhere): each driver buffer goes to
// processLiveBlock, then into the driver's sample format on the first two outputs. ⚠️ One driver per
// process and no user pointer in the callbacks — one open instance at a time. The buffer size is the
// driver's own control-panel setting.
#ifndef POCKETTRACKER_ASIO_AUDIO_ENGINE_H
#define POCKETTRACKER_ASIO_AUDIO_ENGINE_H

#ifdef _WIN32

#include <atomic>
#include <string>
#include <vector>

#include "audio-backend.h"

class AudioEngine;
struct ASIOTime;

class AsioAudioEngine : public AudioBackend {
  public:
    explicit AsioAudioEngine(AudioEngine* core);
    ~AsioAudioEngine() override;

    // The installed drivers, by registered name.
    static std::vector<std::string> driver_names();

    void set_driver(const std::string& name) { driver_ = name; }
    // The engine's rate — loaded samples are pitched for it.
    void set_rate(int hz) { wantRate_ = hz; }
    // Why the last `openStream` failed, in a few words for the status line.
    const std::string& last_error() const { return error_; }

    bool openStream() override;
    void closeStream() override;
    void resumeStream() override {}
    void setPaused(bool paused) override;
    int  sampleRate() const override { return rate_; }
    // The driver's own figure, buffer included.
    OutputLatency outputLatency() const override { return {latencyFrames_, latencyFrames_ > 0}; }
    // The driver asked for a reset (panel change, failure) or went quiet while running. ⚠️ The second
    // half matters: an unplugged interface can go silent without asking for anything.
    bool deviceLost() const override;
    // The last close found the driver silent rather than asking — its hardware is gone.
    bool closed_silent() const { return closedSilent_; }

  private:
    bool fail(const std::string& why);
    bool stalled() const;
    void render(long index);

    static void on_buffer_switch(long index, long directProcess);
    static void on_rate_changed(double rate);
    static long on_message(long selector, long value, void* message, double* opt);
    static ASIOTime* on_buffer_switch_time_info(ASIOTime* params, long index, long directProcess);

    AudioEngine*       core_ = nullptr;
    std::string        driver_;
    std::string        error_;
    int                wantRate_      = 0;
    int                rate_          = 0;
    int                frames_        = 0;
    int                latencyFrames_ = 0;
    long               sampleType_    = 0;
    bool               postOutput_    = false;
    bool               loaded_        = false;   // loaded and ASIOInit succeeded
    bool               buffersMade_   = false;
    bool               running_       = false;
    bool               closedSilent_  = false;
    void*              buffers_[2][2] = {};      // [channel][half]
    std::vector<float> scratch_;                 // interleaved stereo, frames_ × 2
    std::atomic<bool>  paused_{false};
    std::atomic<int>   inCallback_{0};
    std::atomic<bool>  resetRequested_{false};
    std::atomic<unsigned long long> lastCallbackMs_{0};   // GetTickCount64 at the last buffer
};

#endif  // _WIN32
#endif  // POCKETTRACKER_ASIO_AUDIO_ENGINE_H
