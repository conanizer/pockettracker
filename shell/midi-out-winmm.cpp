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
#include <cctype>

namespace ptshell {

namespace {
/** Lower-cased copy, for the "open the device whose name contains this" match. */
std::string lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}
}  // namespace

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
    // POCKETTRACKER_MIDI_TRACE=1 — the console monitor. The bring-up instrument for a machine with no
    // MIDI monitor attached: without it, "the synth is silent" cannot be told from "nothing was sent",
    // and those two have completely different causes.
    if (trace_) {
        std::printf("midi <-  ");
        for (int i = 0; i < len; ++i) std::printf("%02X ", data[i]);
        std::printf("%s\n", bad ? " REJECTED" : "");
        std::fflush(stdout);
    }
}

void WinmmMidiOut::test_note(int holdMs) {
    if (!is_open()) {
        std::printf("midi:    TEST skipped - no port open\n");
        return;
    }
    const int before = errors_;
    const uint8_t on[3]  = {0x90, 60, 100};   // channel 1, C-4, velocity 100
    const uint8_t off[3] = {0x80, 60, 0};
    send(on, 3);
    ::Sleep(static_cast<DWORD>(holdMs < 0 ? 0 : holdMs));
    send(off, 3);
    std::printf("midi:    TEST C-4 ch1 for %d ms - %d messages sent, %d rejected by the driver\n",
                holdMs, 2, errors_ - before);
}

/**
 * Resolve `spec` — an index, a case-insensitive name fragment, or empty/"list" — and open it.
 *
 * Always prints the device list first. That is the console's job on a shell with no MIDI screen yet
 * (B4): without it, "nothing happened" is indistinguishable from "there was no device", which is the
 * single most common way an hour goes missing on a MIDI bring-up.
 */
bool WinmmMidiOut::open_by_spec(const std::string& spec) {
    const int n = device_count();
    std::printf("midi:    %d output device(s)\n", n);
    for (int i = 0; i < n; ++i) std::printf("           [%d] %s\n", i, device_name(i).c_str());

    if (spec.empty() || lower(spec) == "list") {
        if (!spec.empty()) return false;
        std::printf("midi:    OUT off (set POCKETTRACKER_MIDI_OUT=<index|name fragment> to open one)\n");
        return false;
    }

    int index = -1;
    bool numeric = true;
    for (char c : spec) if (c < '0' || c > '9') { numeric = false; break; }
    if (numeric) {
        index = std::atoi(spec.c_str());
    } else {
        const std::string want = lower(spec);
        for (int i = 0; i < n; ++i)
            if (lower(device_name(i)).find(want) != std::string::npos) { index = i; break; }
    }

    if (index < 0 || !open(index)) {
        std::printf("midi:    OUT '%s' NOT OPENED (no match, or the device is in use)\n", spec.c_str());
        return false;
    }
    std::printf("midi:    OUT -> [%d] %s\n", index, device_name(index).c_str());
    return true;
}

}  // namespace ptshell

#endif  // _WIN32
