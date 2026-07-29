#ifndef POCKETTRACKER_SHELL_MIDI_OUT_WINMM_H
#define POCKETTRACKER_SHELL_MIDI_OUT_WINMM_H

// The Windows songcore::IMidiOut. See midi-out-winmm.cpp for why winmm and why this backend first.
// Compiles to nothing off Windows, so main.cpp can name the header unconditionally.
//
// Only the platform half is here: the device list, the spec resolver, the TEST note, the trace and
// the counters are `MidiOutBase` and are shared with every other backend.

#include "midi-out-base.h"

#ifdef _WIN32

#include <string>

// <windows.h> before <mmsystem.h>, and NOMINMAX/WIN32_LEAN_AND_MEAN because this header is included
// by main.cpp, which also includes the standard library: windows.h's `min`/`max` macros break
// <algorithm> at every use site, and the failure reads as an error inside the STL.
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

  private:
    HMIDIOUT handle_ = nullptr;
};

}  // namespace ptshell

#endif  // _WIN32
#endif  // POCKETTRACKER_SHELL_MIDI_OUT_WINMM_H
