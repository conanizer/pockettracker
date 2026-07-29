// midi-out-winmm.{h,cpp} — the WINDOWS implementation of songcore::IMidiOut (MIDI plan phase B2).
//
// The first of the three backends the plan names (§4.3 / §4.5): Windows = winmm, Linux = ALSA
// rawmidi via libasound, Android = a JNI up-call into MidiManager. **Nothing above `IMidiOut` is
// per-platform** — the serializer, the note lifecycle, the queue and the scaling all live in
// native/songcore/midi_out.h and are shared by all three. This file is the whole Windows half.
//
// ⚠️ WHY THIS ONE FIRST, when it ships to no target device: it is the only one testable at a desk.
// With a loopback driver (loopMIDI, or any DAW's virtual port) the entire phase-B feature — gate
// lengths, program changes, panic, the OFFSET — can be watched on a MIDI monitor without a cable, a
// handheld or a phone. The two backends that DO ship are ~100 lines each once the behaviour they
// carry has been proven somewhere.
//
// winmm rather than WinRT MIDI: `midiOutShortMsg` is three decades stable, needs no COM, no
// packaging identity and no manifest capability, and links against a DLL that is in every Windows
// install. WinRT buys BLE-MIDI, which the plan defers anyway (§9).
//
// ── THREADING ────────────────────────────────────────────────────────────────────────────────────
// `send` is called from whichever thread pumps the queue — today the frame loop, and after phase B3
// a sender thread. `midiOutShortMsg` is documented as safe to call from any thread and does not
// block (it queues into the driver), so this file needs no lock of its own. It must NOT be called
// from an audio callback, and nothing does.

#include "midi-out-winmm.h"

#ifdef _WIN32

#include <cstdio>

namespace ptshell {

WinmmMidiOut::~WinmmMidiOut() { close(); }

int WinmmMidiOut::device_count() { return static_cast<int>(::midiOutGetNumDevs()); }

std::string WinmmMidiOut::device_name(int index) {
    MIDIOUTCAPSA caps{};
    if (index < 0 || index >= device_count()) return std::string();
    if (::midiOutGetDevCapsA(static_cast<UINT_PTR>(index), &caps, sizeof caps) != MMSYSERR_NOERROR)
        return std::string();
    return std::string(caps.szPname);
}

bool WinmmMidiOut::open(int index) {
    close();
    if (index < 0 || index >= device_count()) return false;
    HMIDIOUT h = nullptr;
    if (::midiOutOpen(&h, static_cast<UINT>(index), 0, 0, CALLBACK_NULL) != MMSYSERR_NOERROR)
        return false;
    handle_ = h;
    openIndex_ = index;
    return true;
}

void WinmmMidiOut::close() {
    if (!handle_) return;
    // ⚠️ `midiOutReset` before `midiOutClose`, and it is not politeness: closing a port with notes
    // sounding leaves the DEVICE holding them, and nothing we can ever send again will stop it. Reset
    // sends an all-notes-off on all 16 channels. songcore's own panic runs one layer up and covers the
    // channels it knows about; this covers the ones it does not.
    ::midiOutReset(handle_);
    ::midiOutClose(handle_);
    handle_ = nullptr;
    openIndex_ = -1;
}

void WinmmMidiOut::send(const uint8_t* data, int len) {
    if (!handle_ || len <= 0 || len > 3) return;
    // A "short message" is the status byte in the low byte and the data bytes above it, little-endian
    // — NOT a pointer to the bytes. Unused bytes must be zero, which they are: the caller's buffer is
    // zero-filled and `len` says how much of it is real.
    DWORD msg = 0;
    for (int i = 0; i < len; ++i) msg |= static_cast<DWORD>(data[i]) << (8 * i);
    ++sent_;
    const bool bad = ::midiOutShortMsg(handle_, msg) != MMSYSERR_NOERROR;
    if (bad) ++errors_;
    trace_message(data, len, bad);
}

}  // namespace ptshell

#endif  // _WIN32
