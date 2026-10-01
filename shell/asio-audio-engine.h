// asio-audio-engine.{h,cpp} — the ASIO audio backend (Windows only; an empty TU elsewhere).
//
// The same glue as SdlAudioEngine: each driver buffer goes straight to AudioEngine::processLiveBlock,
// then is converted into the driver's sample format for the first two output channels.
//
// ⚠️ ASIO has ONE driver per process and its callbacks carry no user pointer, so only one instance may
// be open at a time. The driver's buffer size is the one set in its own control panel.
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

    /** The installed ASIO drivers, by the name each registered under. */
    static std::vector<std::string> driver_names();

    /** Which driver `openStream` loads. */
    void set_driver(const std::string& name) { driver_ = name; }
    /** The rate the driver must run at — the engine's, since loaded samples are pitched for it. */
    void set_rate(int hz) { wantRate_ = hz; }
    /** Why the last `openStream` failed, in a few words for the status line. */
    const std::string& last_error() const { return error_; }

    bool openStream() override;
    void closeStream() override;
    void resumeStream() override {}
    void setPaused(bool paused) override;
    int  sampleRate() const override { return rate_; }
    /** The driver's own output latency figure, which includes its buffer. */
    OutputLatency outputLatency() const override { return {latencyFrames_, latencyFrames_ > 0}; }
    /**
     * The driver asked to be reset (its buffer size or rate changed in its panel, or it failed), or it
     * has stopped calling back while running.
     *
     * ⚠️ The second half is not optional: a driver whose interface is unplugged can simply go quiet
     * without asking for anything, and then nothing else would ever notice.
     */
    bool deviceLost() const override;
    /** The last close found the driver silent rather than asking to be reset — its hardware is gone. */
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
    bool               loaded_        = false;   // driver loaded + ASIOInit succeeded
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
