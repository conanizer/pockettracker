// midi-out-winmm.{h,cpp} — the WINDOWS implementation of songcore::IMidiOut. Everything above
// `IMidiOut` is songcore/midi_out.h; this file is the whole Windows half. Testable at a desk with a
// loopback port (loopMIDI) and a MIDI monitor.
//
// winmm rather than WinRT MIDI: `midiOutShortMsg` is decades stable and needs no COM, packaging
// identity or manifest capability. WinRT would buy BLE-MIDI.
//
// ── THREADING ────────────────────────────────────────────────────────────────────────────────────
// `send` is called by the sender thread (and the frame loop for a panic). `midiOutShortMsg` is safe
// from any thread and does not block, so no lock here. Never from an audio callback.

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

// "Microsoft GS Wavetable Synth" is present on every Windows machine; without this AUTO would take it
// at launch and never move to the device the user plugs in.
bool WinmmMidiOut::is_builtin_synth(int index) {
    MIDIOUTCAPSA caps{};
    if (index < 0 || index >= device_count()) return false;
    if (::midiOutGetDevCapsA(static_cast<UINT_PTR>(index), &caps, sizeof caps) != MMSYSERR_NOERROR)
        return false;
    return caps.wTechnology == MOD_SWSYNTH;
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
    // ⚠️ `midiOutReset` before `midiOutClose`: it sends all-notes-off on all 16 channels, or the
    // DEVICE keeps holding notes nothing can stop (see MidiOutBase::panic_all_channels).
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
