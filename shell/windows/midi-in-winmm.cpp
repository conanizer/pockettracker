// midi-in-winmm.{h,cpp} — the WINDOWS implementation of songcore::IMidiIn. Testable at a desk with
// a loopback port (loopMIDI): the app's MIDI OUT can feed its own MIDI IN, no hardware needed.
//
// ── WHAT WINMM HANDS OVER, AND THE ONE TRAP ──────────────────────────────────────────────────────
//
// `MIM_DATA` is a PACKED DWORD (status in the low byte) that does not say how many bytes are real.
// ⚠️ Passing three every time turns one program change into two, so the length comes from
// `songcore::midi_message_length`, the rule the parser also uses.
// SysEx never arrives: `MIM_LONGDATA` needs buffers added with `midiInAddBuffer`, and none are.
//
// ── ⚠️⚠️ THE CALLBACK RULE ────────────────────────────────────────────────────────────────────────
//
// winmm's callback may call only a short list of system functions, in a context where the CRT's
// locks are unavailable — a `printf` or `malloc` here is a DEADLOCK. It hands at most three bytes to
// `MidiInBase::deliver` and nothing more.

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
            // A data byte in the status position is a driver fault, not a stream we can resync: winmm
            // has already framed this as one message. Counted rather than forwarded, because feeding it
            // on would make the parser's orphan counter blame the wire.
            if (status < 0x80) { self->note_port_error(); return; }
            const uint8_t bytes[3] = {status, static_cast<uint8_t>((p1 >> 8) & 0x7F),
                                      static_cast<uint8_t>((p1 >> 16) & 0x7F)};
            self->deliver(bytes, songcore::midi_message_length(status));
            break;
        }
        // Invalid or incomplete data — a real symptom on a marginal USB cable, and the only place the
        // app could ever learn of it. Counted here, printed by the drain's report.
        case MIM_ERROR:
        case MIM_LONGERROR:
            self->note_port_error();
            break;
        default:
            // MIM_OPEN / MIM_CLOSE / MIM_LONGDATA (never, see the header) — nothing to do.
            break;
    }
}

bool WinmmMidiIn::open(int index) {
    close();
    if (index < 0 || index >= device_count()) return false;

    HMIDIIN h = nullptr;
    // `this` as the callback's instance data: one backend object per port, and the callback needs a way
    // back to the sink without a global.
    if (::midiInOpen(&h, static_cast<UINT>(index), reinterpret_cast<DWORD_PTR>(&midi_in_proc),
                     reinterpret_cast<DWORD_PTR>(this), CALLBACK_FUNCTION) != MMSYSERR_NOERROR)
        return false;

    // ⚠️ AN OPEN PORT IS NOT A RUNNING ONE: without `midiInStart` the device is held and silent
    // forever — which reads as "the keyboard is broken".
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
    // ⚠️ ORDER, and every step of it is load-bearing. `midiInStop` stops delivery; `midiInReset` returns
    // any pending buffers and, crucially, is what makes `midiInClose` succeed rather than answer
    // MIDIERR_STILLPLAYING; `midiInClose` then blocks until no callback is in flight, which is what
    // makes it safe for the sink to die after this returns.
    ::midiInStop(handle_);
    ::midiInReset(handle_);
    ::midiInClose(handle_);
    handle_    = nullptr;
    openIndex_ = -1;
}

}  // namespace ptshell

#endif  // _WIN32
