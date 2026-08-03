#ifndef POCKETTRACKER_SHELL_MIDI_IN_WINMM_H
#define POCKETTRACKER_SHELL_MIDI_IN_WINMM_H

// The Windows songcore::IMidiIn. See midi-in-winmm.cpp for what winmm hands over and why the callback
// is allowed to do so little. Compiles to nothing off Windows, so main.cpp can name the header
// unconditionally.
//
// Only the platform half is here: the device list, the spec resolver, the sink and the counters are
// `MidiInBase` and are shared with the ALSA and Android backends E5 adds.

#include "midi-in-base.h"

#ifdef _WIN32

#include <string>

// <windows.h> before <mmsystem.h>, and NOMINMAX/WIN32_LEAN_AND_MEAN for midi-out-winmm.h's reason:
// this header is reached from main.cpp, and windows.h's `min`/`max` macros break <algorithm> at every
// use site with an error that reads as though it were inside the STL.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <mmsystem.h>

namespace ptshell {

class WinmmMidiIn : public MidiInBase {
  public:
    ~WinmmMidiIn() override;

    int         device_count() override;
    std::string device_name(int index) override;
    bool        open(int index) override;
    void        close() override;
    bool        is_open() const override { return handle_ != nullptr; }

  private:
    // ⚠️ `CALLBACK` matters: winmm calls this with the platform's calling convention and a mismatch
    // corrupts the stack on the first message rather than failing to link.
    static void CALLBACK midi_in_proc(HMIDIIN in, UINT msg, DWORD_PTR user, DWORD_PTR p1, DWORD_PTR p2);

    HMIDIIN handle_ = nullptr;
};

}  // namespace ptshell

#endif  // _WIN32
#endif  // POCKETTRACKER_SHELL_MIDI_IN_WINMM_H
