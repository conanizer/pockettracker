#ifndef POCKETTRACKER_SHELL_MIDI_OUT_WINMM_H
#define POCKETTRACKER_SHELL_MIDI_OUT_WINMM_H

// Windows MIDI output (see the .cpp). Compiles to nothing elsewhere, so main.cpp can include it.

#include "midi/midi-out-base.h"

#ifdef _WIN32

#include <string>

// NOMINMAX and LEAN before <windows.h>: its min/max macros break <algorithm> with errors that look
// like they come from inside the STL.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <mmsystem.h>

namespace ptshell {

class WinmmMidiOut : public MidiOutBase {
  public:
    ~WinmmMidiOut() override;

    int         device_count() override;
    std::string device_name(int index) override;
    bool        open(int index) override;
    void        close() override;
    bool        is_open() const override { return handle_ != nullptr; }
    void        send(const uint8_t* data, int len) override;
    bool        is_builtin_synth(int index) override;

  private:
    // A SysEx (a note command) is a long message, and its buffer is the driver's until it is marked
    // done: a few take turns.
    struct LongMessage {
        MIDIHDR hdr{};
        char    data[songcore::NOTE_COMMAND_BYTES]{};
        bool    prepared = false;
    };
    bool send_long(const uint8_t* data, int len);

    HMIDIOUT    handle_ = nullptr;
    LongMessage long_[4];
};

}  // namespace ptshell

#endif  // _WIN32
#endif  // POCKETTRACKER_SHELL_MIDI_OUT_WINMM_H
