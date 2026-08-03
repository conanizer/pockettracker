#ifndef POCKETTRACKER_SHELL_MIDI_IN_ALSA_H
#define POCKETTRACKER_SHELL_MIDI_IN_ALSA_H

// The LINUX songcore::IMidiIn — ALSA rawmidi, reached through dlopen. MIDI plan phase E5.
// See midi-in-alsa.cpp for the reader thread, and alsa-rawmidi.h for the loader and the device walk
// it shares with the OUTPUT backend. Compiles to nothing off desktop/handheld Linux, so midi-in.h can
// name the header unconditionally.

#include "alsa-rawmidi.h"
#include "midi-in-base.h"

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

    /** False when libasound.so.2 is absent or a symbol is missing — the INPUT row then draws NO PORTS. */
    bool available() const { return lib_ != nullptr; }

    /** Reads that came back with something other than data or -EAGAIN. Nonzero = a wire/driver fault. */
    uint64_t read_errors() const { return readErrors_.load(std::memory_order_relaxed); }
    /** True once a read failed fatally (a cable pulled): the thread has stopped and no byte can arrive. */
    bool     stopped_early() const { return dead_.load(std::memory_order_relaxed); }

  private:
    /** The reader thread's body. The ONLY thing that ever calls `deliver`. */
    void reader();

    void*                                   lib_ = nullptr;
    alsa_detail::AlsaApi                    a_{};
    std::vector<alsa_detail::RawmidiDevice> devices_;
    void*                                   in_ = nullptr;   // snd_rawmidi_t*

    // ⚠️ `thread_` is joined by `close()` BEFORE `in_` is closed, and `quit_` is what lets it be. See
    // the ordering note in the .cpp: the two are one operation and nothing may put anything between.
    std::thread           thread_;
    std::atomic<bool>     quit_{false};
    std::atomic<bool>     dead_{false};
    std::atomic<uint64_t> readErrors_{0};
};

}  // namespace ptshell

#endif  // __linux__ && !__ANDROID__
#endif  // POCKETTRACKER_SHELL_MIDI_IN_ALSA_H
