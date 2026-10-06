#ifndef POCKETTRACKER_SHELL_MIDI_OUT_ALSA_H
#define POCKETTRACKER_SHELL_MIDI_OUT_ALSA_H

// The LINUX songcore::IMidiOut — ALSA rawmidi, reached through dlopen.
// See midi-out-alsa.cpp for why rawmidi and not seq, and why dlopen and not -lasound.
// Compiles to nothing off desktop/handheld Linux, so main.cpp can name the header unconditionally.

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

    /**
     * False when libasound.so.2 is absent or a symbol is missing — MIDI is then simply unavailable
     * and `device_count()` answers 0, which the OUTPUT row already draws as `OFF  NO PORTS`. The app
     * itself still runs, which is the whole point of dlopen over a link-time dependency.
     */
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
