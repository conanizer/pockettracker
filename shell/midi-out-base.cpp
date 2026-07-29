#include "midi-out-base.h"

#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <thread>

namespace ptshell {

namespace {
/** Lower-cased copy, for the "open the device whose name contains this" match. */
std::string lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}
}  // namespace

void MidiOutBase::trace_message(const uint8_t* data, int len, bool rejected) const {
    if (!trace_) return;
    std::printf("midi <-  ");
    for (int i = 0; i < len; ++i) std::printf("%02X ", data[i]);
    std::printf("%s\n", rejected ? " REJECTED" : "");
    std::fflush(stdout);
}

void MidiOutBase::panic_all_channels() {
    if (!is_open()) return;
    for (int ch = 0; ch < 16; ++ch) {
        const uint8_t msg[3] = {static_cast<uint8_t>(songcore::MIDI_CC | ch),
                                songcore::MIDI_CC_ALL_NOTES_OFF, 0};
        send(msg, 3);
    }
}

void MidiOutBase::test_note(int holdMs) {
    if (!is_open()) {
        std::printf("midi:    TEST skipped - no port open\n");
        return;
    }
    const int     before = errors_;
    const uint8_t on[3]  = {0x90, 60, 100};   // channel 1, C-4, velocity 100
    const uint8_t off[3] = {0x80, 60, 0};
    send(on, 3);
    // std::this_thread rather than Sleep/usleep: this file is the platform-free half, and the hold is
    // the one thing in it that would otherwise need an #ifdef.
    std::this_thread::sleep_for(std::chrono::milliseconds(holdMs < 0 ? 0 : holdMs));
    send(off, 3);
    std::printf("midi:    TEST C-4 ch1 for %d ms - %d messages sent, %d rejected by the driver\n",
                holdMs, 2, errors_ - before);
}

bool MidiOutBase::open_by_spec(const std::string& spec) {
    const int n = device_count();
    std::printf("midi:    %d output device(s)\n", n);
    for (int i = 0; i < n; ++i) std::printf("           [%d] %s\n", i, device_name(i).c_str());

    if (spec.empty() || lower(spec) == "list") {
        if (!spec.empty()) return false;
        std::printf("midi:    OUT off (set POCKETTRACKER_MIDI_OUT=<index|name fragment> to open one)\n");
        return false;
    }

    int  index   = -1;
    bool numeric = !spec.empty();
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
