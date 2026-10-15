// Windows MIDI input (winmm). A loopback port (loopMIDI) lets the app's OUT feed its own IN.
// `MIM_DATA` is a packed DWORD that does not say how many bytes are real. ⚠️ Passing three every time
// turns one program change into two — the length comes from `midi_message_length`. No SysEx (no
// `midiInAddBuffer`). ⚠️ The callback may not printf or malloc (deadlock): it calls `deliver` only.

#include "windows/midi-in-winmm.h"

#ifdef _WIN32

namespace ptshell {

WinmmMidiIn::~WinmmMidiIn() { close(); }

int WinmmMidiIn::device_count() { return static_cast<int>(::midiInGetNumDevs()); }

std::string WinmmMidiIn::device_name(int index) {
    MIDIINCAPSA caps{};
    if (index < 0 || index >= device_count()) return std::string();
    if (::midiInGetDevCapsA(static_cast<UINT_PTR>(index), &caps, sizeof caps) != MMSYSERR_NOERROR)
        return std::string();
    return std::string(caps.szPname);
}

void CALLBACK WinmmMidiIn::midi_in_proc(HMIDIIN, UINT msg, DWORD_PTR user, DWORD_PTR p1, DWORD_PTR) {
    auto* self = reinterpret_cast<WinmmMidiIn*>(user);
    if (!self) return;

    switch (msg) {
        case MIM_DATA: {
            const uint8_t status = static_cast<uint8_t>(p1 & 0xFF);
            // A data byte as status is a driver fault (winmm already framed it): counted, not forwarded,
            // or the parser's orphan counter would blame the wire.
            if (status < 0x80) { self->note_port_error(); return; }
            const uint8_t bytes[3] = {status, static_cast<uint8_t>((p1 >> 8) & 0x7F),
                                      static_cast<uint8_t>((p1 >> 16) & 0x7F)};
            self->deliver(bytes, songcore::midi_message_length(status));
            break;
        }
        // Bad data — a marginal USB cable. Counted here, printed by the drain's report.
        case MIM_ERROR:
        case MIM_LONGERROR:
            self->note_port_error();
            break;
        default:
            // MIM_OPEN / MIM_CLOSE / MIM_LONGDATA (never) — nothing to do.
            break;
    }
}

bool WinmmMidiIn::open(int index) {
    close();
    if (index < 0 || index >= device_count()) return false;

    HMIDIIN h = nullptr;
    // `this` as instance data: the callback's way back to the sink without a global.
    if (::midiInOpen(&h, static_cast<UINT>(index), reinterpret_cast<DWORD_PTR>(&midi_in_proc),
                     reinterpret_cast<DWORD_PTR>(this), CALLBACK_FUNCTION) != MMSYSERR_NOERROR)
        return false;

    // ⚠️ Open is not running: without `midiInStart` the device is held and silent.
    if (::midiInStart(h) != MMSYSERR_NOERROR) {
        ::midiInClose(h);
        return false;
    }

    handle_    = h;
    openIndex_ = index;
    return true;
}

void WinmmMidiIn::close() {
    if (!handle_) return;
    // ⚠️ Order: stop delivery; reset, without which close answers MIDIERR_STILLPLAYING; close, which
    // waits out a callback in flight so the sink may die afterwards.
    ::midiInStop(handle_);
    ::midiInReset(handle_);
    ::midiInClose(handle_);
    handle_    = nullptr;
    openIndex_ = -1;
}

}  // namespace ptshell

#endif  // _WIN32
