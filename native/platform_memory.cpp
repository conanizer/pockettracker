#include "platform_memory.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>

// A .cpp and not a header inline, for byte_source.cpp's reason plus one of its own: the Windows
// branch needs <windows.h>, whose min/max macros and ~1500 other symbols have no business being
// dragged into every translation unit that wants to know how much RAM is free.
#if defined(_WIN32)
#  define WIN32_LEAN_AND_MEAN
#  define NOMINMAX
#  include <windows.h>
#endif

namespace pt {
namespace {

#if !defined(_WIN32)
/**
 * One key out of /proc/meminfo, in bytes; -1 if absent. Values there are in kB — the unit is printed
 * on every line and is kB even on machines whose pages are not 4 kB, so it is not derived from the
 * page size.
 */
int64_t meminfo_bytes(const char* key) {
    std::FILE* f = std::fopen("/proc/meminfo", "rb");
    if (!f) return -1;

    const std::size_t keyLen = std::strlen(key);
    char line[256];
    int64_t out = -1;
    while (std::fgets(line, sizeof line, f)) {
        if (std::strncmp(line, key, keyLen) != 0 || line[keyLen] != ':') continue;
        const char* p = line + keyLen + 1;
        while (*p == ' ' || *p == '\t') ++p;
        char* end = nullptr;
        const long long kb = std::strtoll(p, &end, 10);
        if (end != p && kb >= 0) out = static_cast<int64_t>(kb) * 1024;
        break;
    }
    std::fclose(f);
    return out;
}
#endif

/**
 * ⚠️ On a 32-bit build the RAM fitted is not the ceiling — a single process cannot map more than its
 * user address space (~3 GB on armhf/Linux), and a handheld with 4 GB fitted still cannot hand one
 * allocation more than that. Clamp so the smaller of the two always binds.
 */
int64_t clamp_to_address_space(int64_t bytes) {
    if (bytes <= 0) return 0;
    if (sizeof(void*) >= 8) return bytes;
    const int64_t userAddressSpace = int64_t{3} * 1024 * 1024 * 1024;
    return std::min(bytes, userAddressSpace);
}

/**
 * `PT_MEMORY_LIMIT_MB` — pretend the machine has only this much free.
 *
 * ⚠️⚠️ **THE ONLY WAY TO REACH THE LOW-MEMORY PATHS ON A MACHINE THAT IS NOT LOW ON MEMORY, AND THEY
 * WERE UNREACHED FOR MONTHS.** Every refusal in the app hangs off this number: the soundfont
 * allocator guard and the sample decoder's room check both refuse a file only when it will not fit,
 * so on any development box the whole mechanism is a branch nothing takes. The guard was measuring
 * the wrong quantity for a soundfont's growth (see soundfont-voice.cpp) and read green throughout,
 * because no test could make it say no.
 *
 * ⚠️ It only ever makes the answer SMALLER — `std::min` — so it cannot be used to talk the app into a
 * load the machine cannot serve, and an unset or unparseable value changes nothing at all. Read on
 * every call rather than cached, because a probe that sets it after the first allocation must still
 * be obeyed.
 */
int64_t apply_memory_limit_env(int64_t bytes) {
    const char* v = std::getenv("PT_MEMORY_LIMIT_MB");
    if (!v || !*v) return bytes;
    char*           end   = nullptr;
    const long long limit = std::strtoll(v, &end, 10);
    if (end == v || limit <= 0) return bytes;
    const int64_t capped = static_cast<int64_t>(limit) * 1024 * 1024;
    return (bytes <= 0) ? capped : std::min(bytes, capped);
}

/** What the OS says, before `PT_MEMORY_LIMIT_MB` gets a chance to lower it. */
int64_t available_memory_raw() {
#if defined(_WIN32)
    MEMORYSTATUSEX st;
    st.dwLength = sizeof(st);
    if (!GlobalMemoryStatusEx(&st)) return 0;
    // ullAvailPhys, NOT ullAvailPageFile: the pagefile is swap, and swapped audio does not play.
    return clamp_to_address_space(static_cast<int64_t>(st.ullAvailPhys));
#else
    // MemAvailable is the kernel's own estimate of what a new allocation can have without swapping,
    // and is the right question. ⚠️ It arrived in Linux 3.14 — an old vendor kernel on a handheld may
    // not publish it, and returning 0 there would make every load look impossible. MemFree + Cached
    // is the pre-3.14 approximation the kernel's own documentation gives for it.
    const int64_t avail = meminfo_bytes("MemAvailable");
    if (avail >= 0) return clamp_to_address_space(avail);

    const int64_t free_   = meminfo_bytes("MemFree");
    const int64_t cached = meminfo_bytes("Cached");
    if (free_ < 0) return 0;
    return clamp_to_address_space(free_ + (cached > 0 ? cached : 0));
#endif
}

}  // namespace

int64_t available_memory_bytes() { return apply_memory_limit_env(available_memory_raw()); }

int64_t total_memory_bytes() {
#if defined(_WIN32)
    MEMORYSTATUSEX st;
    st.dwLength = sizeof(st);
    if (!GlobalMemoryStatusEx(&st)) return 0;
    return clamp_to_address_space(static_cast<int64_t>(st.ullTotalPhys));
#else
    const int64_t total = meminfo_bytes("MemTotal");
    return total > 0 ? clamp_to_address_space(total) : 0;
#endif
}

int64_t load_budget_bytes() {
    const int64_t avail = available_memory_bytes();
    const int64_t total = total_memory_bytes();
    // Unknown stays unknown and must not become zero-budget: the caller refuses on a budget it can
    // read, and refuses NOTHING on one it cannot.
    if (avail <= 0 && total <= 0) return 0;
    return std::max(avail, total / 2);
}

}  // namespace pt
