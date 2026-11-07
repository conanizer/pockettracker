#ifndef POCKETTRACKER_SHELL_MIDI_SENDER_H
#define POCKETTRACKER_SHELL_MIDI_SENDER_H

// The MIDI sender thread. Messages are queued against a frame and released late-never-early by
// `ExternalConsumer::pump`; from the 60 Hz loop a note would leave up to 16.7 ms late. This pumps at
// ~1 kHz against the frame the audio device is taking (`AudioEngine::getDeviceFrame`).
// ⚠️ An SDL thread: on Android it makes JNI calls, and SDL's entry attaches it to the JVM.
// It may read the frame counter and call `pump`, nothing else. It may block in a port write, so HIGH
// priority, not TIME_CRITICAL (which would make the audio callback wait).

#include <atomic>
#include <cstdint>

struct SDL_Thread;
class AudioEngine;
namespace songcore { class SongcoreHost; }

namespace ptshell {

// Built after the port is attached, before the loop starts. Ready line at start, tick counts at stop.
class MidiSender {
  public:
    MidiSender(AudioEngine& engine, songcore::SongcoreHost& host);
    ~MidiSender();

    MidiSender(const MidiSender&)            = delete;
    MidiSender& operator=(const MidiSender&) = delete;

    // False if the thread could not be created — `poll()` then keeps pumping.
    bool start();
    void stop();

    bool running() const { return thread_ != nullptr; }

  private:
    static int thread_entry(void* self);
    void       run();

    AudioEngine&            engine_;
    songcore::SongcoreHost& host_;

    SDL_Thread*        thread_ = nullptr;
    std::atomic<bool>  quit_{false};
    std::atomic<long long> ticks_{0};
    std::atomic<long long> busyTicks_{0};
    std::atomic<long long> maxBusyGap_{0};   // µs; the thread's own worst cadence
};

}  // namespace ptshell

#endif  // POCKETTRACKER_SHELL_MIDI_SENDER_H
