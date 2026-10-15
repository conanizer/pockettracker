#ifndef POCKETTRACKER_SHELL_MIDI_OUT_ALSA_H
#define POCKETTRACKER_SHELL_MIDI_OUT_ALSA_H

// Linux MIDI output over ALSA rawmidi via dlopen (see the .cpp). Compiles to nothing elsewhere, so
// main.cpp can include it.

#include "linux/alsa-rawmidi.h"
#include "midi/midi-out-base.h"

#if defined(__linux__) && !defined(__ANDROID__)

#include <atomic>
#include <string>
#include <vector>

namespace ptshell {


class AlsaMidiOut : public MidiOutBase {
  public:
    AlsaMidiOut();
    ~AlsaMidiOut() override;

    int         device_count() override;
    std::string device_name(int index) override;
    bool        open(int index) override;
    void        close() override;
    bool        is_open() const override { return out_ != nullptr; }
    void        send(const uint8_t* data, int len) override;
    bool        broken() const override { return broken_.load(std::memory_order_relaxed); }

    // False without libasound.so.2 or a symbol: no ports, but the app still runs — the point of dlopen.
    bool available() const { return lib_ != nullptr; }

  private:
    void*                                      lib_ = nullptr;
    alsa_detail::AlsaApi                       a_{};
    std::vector<alsa_detail::RawmidiDevice>    devices_;
    void*                                      out_ = nullptr;   // snd_rawmidi_t*
    std::atomic<bool>                          broken_{false};   // set on the sender thread
};

}  // namespace ptshell

#endif  // __linux__ && !__ANDROID__
#endif  // POCKETTRACKER_SHELL_MIDI_OUT_ALSA_H
