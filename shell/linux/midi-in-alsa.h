#ifndef POCKETTRACKER_SHELL_MIDI_IN_ALSA_H
#define POCKETTRACKER_SHELL_MIDI_IN_ALSA_H

// Linux MIDI input over ALSA rawmidi (see the .cpp). Compiles to nothing elsewhere, so midi-in.h can
// include it unconditionally.

#include "linux/alsa-rawmidi.h"
#include "midi/midi-in-base.h"

#if defined(__linux__) && !defined(__ANDROID__)

#include <atomic>
#include <string>
#include <thread>
#include <vector>

namespace ptshell {

class AlsaMidiIn : public MidiInBase {
  public:
    AlsaMidiIn();
    ~AlsaMidiIn() override;

    int         device_count() override;
    std::string device_name(int index) override;
    bool        open(int index) override;
    void        close() override;
    bool        is_open() const override { return in_ != nullptr; }
    bool        broken() const override { return dead_.load(std::memory_order_relaxed); }

    // False without libasound.so.2 — the INPUT row draws NO PORTS.
    bool available() const { return lib_ != nullptr; }

    // Reads that returned neither data nor -EAGAIN — a wire or driver fault.
    uint64_t read_errors() const { return readErrors_.load(std::memory_order_relaxed); }
    // A read failed fatally (a cable pulled): the thread has stopped.
    bool     stopped_early() const { return dead_.load(std::memory_order_relaxed); }

  private:
    // The reader thread; the only caller of `deliver`.
    void reader();

    void*                                   lib_ = nullptr;
    alsa_detail::AlsaApi                    a_{};
    std::vector<alsa_detail::RawmidiDevice> devices_;
    void*                                   in_ = nullptr;   // snd_rawmidi_t*

    // ⚠️ `close()` joins `thread_` (signalled by `quit_`) before closing `in_` — one operation.
    std::thread           thread_;
    std::atomic<bool>     quit_{false};
    std::atomic<bool>     dead_{false};
    std::atomic<uint64_t> readErrors_{0};
};

}  // namespace ptshell

#endif  // __linux__ && !__ANDROID__
#endif  // POCKETTRACKER_SHELL_MIDI_IN_ALSA_H
