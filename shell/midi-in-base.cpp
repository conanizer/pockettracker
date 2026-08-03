#include "midi-in-base.h"

#include <cctype>
#include <cstdio>
#include <cstdlib>

namespace ptshell {

namespace {
/** Lower-cased copy, for the "the device whose name contains this" match. */
std::string lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}
}  // namespace

void MidiInBase::deliver(const uint8_t* data, int len) {
    if (!data || len <= 0) return;
    callbacks_.fetch_add(1, std::memory_order_relaxed);
    bytes_.fetch_add(static_cast<uint64_t>(len), std::memory_order_relaxed);
    // ⚠️ Loaded ONCE into a local. Re-reading the atomic between the null test and the call is a window
    // in which `set_sink(nullptr)` lands and we call through a pointer we already checked.
    songcore::IMidiInSink* sink = sink_.load(std::memory_order_acquire);
    if (sink) sink->on_bytes(data, len);
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
    // The NAME, never the index — the setting this feeds is a name for the same reason the OUTPUT one
    // is: a port list reorders on every replug, so an index resolved today names another device
    // tomorrow. The open happens later, in the one place that also wires the sink.
    const std::string name = device_name(index);
    std::printf("midi:    IN override -> [%d] %s\n", index, name.c_str());
    return name;
}

}  // namespace ptshell
