#ifndef POCKETTRACKER_SHELL_MIDI_OUT_WINMM_H
#define POCKETTRACKER_SHELL_MIDI_OUT_WINMM_H

// The Windows songcore::IMidiOut. See midi-out-winmm.cpp for why winmm and why this backend first.
// Compiles to nothing off Windows, so main.cpp can name the header unconditionally.

#include "songcore/midi_out.h"

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

class WinmmMidiOut : public songcore::IMidiOut {
  public:
    ~WinmmMidiOut() override;

    int         device_count() override;
    std::string device_name(int index) override;
    bool        open(int index) override;
    void        close() override;
    bool        is_open() const override { return handle_ != nullptr; }
    void        send(const uint8_t* data, int len) override;

    /** Print the device list and open the one `spec` names (index, name fragment, or nothing). */
    bool open_by_spec(const std::string& spec);

    /**
     * ⚠️ How many `midiOutShortMsg` calls the DRIVER REJECTED.
     *
     * `send` cannot report failure — `IMidiOut::send` returns void, because nothing above it could do
     * anything useful with the news mid-phrase. But a send path that fails silently is
     * indistinguishable from one that works, which is the whole family of bug this project keeps
     * paying for. So the failures are counted, and the console prints the count beside the verdict.
     */
    int error_count() const { return errors_; }
    int sent_count() const { return sent_; }

    /**
     * The plan's §8.1 TEST row, reachable before the screen that will carry it exists: one C-4 on
     * channel 1, held, then released. It is the smallest thing that answers "is the cable alive?" —
     * and it deliberately does NOT go through the bus, so a failure here means the PORT, not songcore.
     */
    void test_note(int holdMs);

    /** Print every message as it leaves (POCKETTRACKER_MIDI_TRACE=1). */
    void set_trace(bool on) { trace_ = on; }

    int open_index() const { return openIndex_; }

  private:
    HMIDIOUT handle_ = nullptr;
    int      openIndex_ = -1;
    int      sent_ = 0;
    int      errors_ = 0;
    bool     trace_ = false;
};

}  // namespace ptshell

#endif  // _WIN32
#endif  // POCKETTRACKER_SHELL_MIDI_OUT_WINMM_H
