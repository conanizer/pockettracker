#ifndef POCKETTRACKER_SHELL_MIDI_OUT_ALSA_H
#define POCKETTRACKER_SHELL_MIDI_OUT_ALSA_H

// The LINUX songcore::IMidiOut — ALSA rawmidi, reached through dlopen. MIDI plan phase B2b.
// See midi-out-alsa.cpp for why rawmidi and not seq, and why dlopen and not -lasound.
// Compiles to nothing off desktop/handheld Linux, so main.cpp can name the header unconditionally.

#include "alsa-rawmidi.h"
#include "midi-out-base.h"

#if defined(__linux__) && !defined(__ANDROID__)

#include <string>
#include <vector>

namespace ptshell {

// ⚠️ **`AlsaApi`, THE LOADER AND THE DEVICE WALK MOVED TO alsa-rawmidi.h AT E5**, when the INPUT
// backend turned out to need every one of them and to differ by a single integer (`STREAM_INPUT`
// instead of `STREAM_OUTPUT`). Everything this file used to say about hand-copied prototypes and about
// `tools/ptalsa` still holds and now lives there.

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
};

}  // namespace ptshell

#endif  // __linux__ && !__ANDROID__
#endif  // POCKETTRACKER_SHELL_MIDI_OUT_ALSA_H
