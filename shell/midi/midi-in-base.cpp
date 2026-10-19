#include "midi/midi-in-base.h"

#include <cctype>
#include <cstdio>
#include <cstdlib>

#include "songcore/device_clock.h"

namespace ptshell {

namespace {
// Lower-cased copy, for the name-fragment match.
std::string lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}
}  // namespace

void MidiInBase::deliver(const uint8_t* data, int len) {
    if (!data || len <= 0) return;
    // The arrival time, for SYNC IN: the drain turns it into the frame the bytes came on.
    const int64_t us = songcore::steady_us();
    callbacks_.fetch_add(1, std::memory_order_relaxed);
    bytes_.fetch_add(static_cast<uint64_t>(len), std::memory_order_relaxed);
    // ⚠️ Loaded once: re-reading between the null test and the call lets `set_sink(nullptr)` slip in.
    songcore::IMidiInSink* sink = sink_.load(std::memory_order_acquire);
    if (sink) sink->on_bytes_at(data, len, us);
}

std::string MidiInBase::resolve_spec(const std::string& spec) {
    const int n = device_count();
    std::printf("midi:    %d input device(s)\n", n);
    for (int i = 0; i < n; ++i) std::printf("           [%d] %s\n", i, device_name(i).c_str());

    if (spec.empty() || lower(spec) == "list") return std::string();

    int  index   = -1;
    bool numeric = true;
    for (char c : spec) if (c < '0' || c > '9') { numeric = false; break; }
    if (numeric) {
        index = std::atoi(spec.c_str());
        if (index < 0 || index >= n) index = -1;
    } else {
        const std::string want = lower(spec);
        for (int i = 0; i < n; ++i)
            if (lower(device_name(i)).find(want) != std::string::npos) { index = i; break; }
    }

    if (index < 0) {
        std::printf("midi:    IN '%s' NO MATCH (the list above is everything this machine has)\n",
                    spec.c_str());
        return std::string();
    }
    // The NAME, never the index — a port list reorders on every replug. Opened later, where the sink
    // is wired.
    const std::string name = device_name(index);
    std::printf("midi:    IN override -> [%d] %s\n", index, name.c_str());
    return name;
}

}  // namespace ptshell
