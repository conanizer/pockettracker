#ifndef POCKETTRACKER_SHELL_MIDI_IN_WINMM_H
#define POCKETTRACKER_SHELL_MIDI_IN_WINMM_H

// Windows MIDI input (see the .cpp). Compiles to nothing elsewhere, so main.cpp can include it.

#include "midi/midi-in-base.h"

#ifdef _WIN32

#include <atomic>
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

class WinmmMidiIn : public MidiInBase {
  public:
    ~WinmmMidiIn() override;

    int         device_count() override;
    std::string device_name(int index) override;
    bool        open(int index) override;
    void        close() override;
    bool        is_open() const override { return handle_ != nullptr; }

  private:
    // ⚠️ `CALLBACK`: a calling-convention mismatch corrupts the stack on the first message, it does not
    // fail to link.
    static void CALLBACK midi_in_proc(HMIDIIN in, UINT msg, DWORD_PTR user, DWORD_PTR p1, DWORD_PTR p2);
    void shut(HMIDIIN h);

    // SysEx comes in buffers lent to the driver; each goes back for the next, except while closing.
    struct SysexBuffer {
        MIDIHDR hdr{};
        char    data[64]{};
    };

    HMIDIIN           handle_ = nullptr;
    SysexBuffer       sysex_[4];
    std::atomic<bool> closing_{false};
};

}  // namespace ptshell

#endif  // _WIN32
#endif  // POCKETTRACKER_SHELL_MIDI_IN_WINMM_H
