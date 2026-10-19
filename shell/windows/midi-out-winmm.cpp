// Windows MIDI output (winmm: decades stable, no COM, packaging identity or manifest; WinRT would only
// add BLE-MIDI). `midiOutShortMsg` is thread-safe and non-blocking, so no lock. Never from an audio
// callback.

#include "windows/midi-out-winmm.h"

#ifdef _WIN32

#include <cstdio>
#include <cstring>

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

// "Microsoft GS Wavetable Synth" exists on every Windows machine; AUTO must skip it, or it never moves
// to the device the user plugs in.
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
    // ⚠️ `midiOutReset` first: all-notes-off on all channels, or the device keeps holding notes. It
    // also hands back every long message, which must be unprepared before the close.
    ::midiOutReset(handle_);
    for (LongMessage& m : long_) {
        if (m.prepared) ::midiOutUnprepareHeader(handle_, &m.hdr, sizeof m.hdr);
        m.prepared = false;
    }
    ::midiOutClose(handle_);
    handle_ = nullptr;
    openIndex_ = -1;
}

void WinmmMidiOut::send(const uint8_t* data, int len) {
    if (!handle_ || len <= 0 || len > songcore::NOTE_COMMAND_BYTES) return;
    bool bad = false;
    if (len > 3) {
        bad = !send_long(data, len);
    } else {
        // A short message packs the status in the low byte and the data above it. Unused bytes stay zero.
        DWORD msg = 0;
        for (int i = 0; i < len; ++i) msg |= static_cast<DWORD>(data[i]) << (8 * i);
        bad = ::midiOutShortMsg(handle_, msg) != MMSYSERR_NOERROR;
    }
    if (bad) ++errors_;
    trace_message(data, len, bad);
}

bool WinmmMidiOut::send_long(const uint8_t* data, int len) {
    for (LongMessage& m : long_) {
        if (m.prepared) {
            if (!(m.hdr.dwFlags & MHDR_DONE)) continue;   // still the driver's
            ::midiOutUnprepareHeader(handle_, &m.hdr, sizeof m.hdr);
            m.prepared = false;
        }
        std::memcpy(m.data, data, static_cast<size_t>(len));
        m.hdr                = MIDIHDR{};
        m.hdr.lpData         = m.data;
        m.hdr.dwBufferLength = static_cast<DWORD>(len);
        if (::midiOutPrepareHeader(handle_, &m.hdr, sizeof m.hdr) != MMSYSERR_NOERROR) return false;
        m.prepared = true;
        return ::midiOutLongMsg(handle_, &m.hdr, sizeof m.hdr) == MMSYSERR_NOERROR;
    }
    return false;   // all of them still on their way
}

}  // namespace ptshell

#endif  // _WIN32
