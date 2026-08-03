// midi-in-winmm.{h,cpp} — the WINDOWS implementation of songcore::IMidiIn (MIDI plan phase E2).
//
// The first of the three input backends, and chosen first for exactly the reason B2 chose winmm for
// output even though it ships to no target device: **it is the only one testable at a desk.** With
// loopMIDI (or any DAW's virtual port) the app's own MIDI OUT can be pointed at a port the app's MIDI
// IN is listening to, and the whole of phase E can be watched — bytes, running status, the channel
// map, the routing — with no keyboard, no cable and no hardware of any kind. The two backends that DO
// ship (ALSA rawmidi, `MidiManager`) are ~100 lines each once the behaviour above them has been proven
// somewhere.
//
// ── WHAT WINMM ACTUALLY HANDS OVER, AND THE ONE TRAP IN IT ───────────────────────────────────────
//
// `MIM_DATA` is a **packed DWORD**, not a byte stream: the status byte in the low byte, then the data
// bytes. It does NOT say how many of them are real. ⚠️ Passing three every time turns one program
// change into TWO — the pad byte arrives as a data byte under running status and is a second, silent
// PC to program 0 — so the length comes from `songcore::midi_message_length`, the one copy of that
// rule, which the parser also uses.
//
// **SysEx never arrives here**, and that is a decision rather than a gap: `MIM_LONGDATA` is delivered
// only into buffers the application has added with `midiInAddBuffer`, and this adds none. The parser
// skips SysEx anyway and nothing in the app reads it; adding buffers would mean owning their lifetime
// across a close, for a feature §9 defers.
//
// ── ⚠️⚠️ THE CALLBACK RULE, AND IT IS THE WHOLE SHAPE OF THIS FILE ────────────────────────────────
//
// winmm documents its low-level MIDI callback as being allowed to call only a short list of system
// functions (`EnterCriticalSection`, `midiOutShortMsg`, `PostMessage`, `SetEvent`, the `time*` family).
// It can run in an interrupt-like context where the CRT's own locks are unavailable, so **a `printf`
// or a `malloc` in here is a deadlock, not a slow path** — which is precisely why `MidiInQueue` is a
// lock and a memcpy and why the console instrument lives in `SongcoreHost::poll()` instead of here.
// The one thing this callback does is hand at most three bytes to `MidiInBase::deliver`.

#include "midi-in-winmm.h"

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

    // ⚠️ **AN OPEN PORT IS NOT A RUNNING ONE.** `midiInOpen` alone delivers nothing at all: without
    // `midiInStart` the device is held (so no other app can have it) and silent forever, which is the
    // exact failure mode this whole file's counters exist to name — and it would read as "the keyboard
    // is broken".
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
