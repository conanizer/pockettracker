#include "platform_memory.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>

// A .cpp so <windows.h> stays out of every includer.
#if defined(_WIN32)
#  define WIN32_LEAN_AND_MEAN
#  define NOMINMAX
#  include <windows.h>
#endif

namespace pt {
namespace {

#if !defined(_WIN32)
// One /proc/meminfo key in bytes (the file is always in kB); −1 if absent.
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

// A 32-bit process cannot map more than ~3 GB, whatever is fitted.
int64_t clamp_to_address_space(int64_t bytes) {
    if (bytes <= 0) return 0;
    if (sizeof(void*) >= 8) return bytes;
    const int64_t userAddressSpace = int64_t{3} * 1024 * 1024 * 1024;
    return std::min(bytes, userAddressSpace);
}

// `PT_MEMORY_LIMIT_MB`: pretend only this much is free — the only way to reach the low-memory paths
// on a machine that is not low. It only ever lowers the answer; read on every call, not cached.
int64_t apply_memory_limit_env(int64_t bytes) {
    const char* v = std::getenv("PT_MEMORY_LIMIT_MB");
    if (!v || !*v) return bytes;
    char*           end   = nullptr;
    const long long limit = std::strtoll(v, &end, 10);
    if (end == v || limit <= 0) return bytes;
    const int64_t capped = static_cast<int64_t>(limit) * 1024 * 1024;
    return (bytes <= 0) ? capped : std::min(bytes, capped);
}

// What the OS says, before `PT_MEMORY_LIMIT_MB`.
int64_t available_memory_raw() {
#if defined(_WIN32)
    MEMORYSTATUSEX st;
    st.dwLength = sizeof(st);
    if (!GlobalMemoryStatusEx(&st)) return 0;
    // Physical, not the pagefile: swapped audio does not play.
    return clamp_to_address_space(static_cast<int64_t>(st.ullAvailPhys));
#else
    // ⚠️ MemAvailable is missing before Linux 3.14 (old handheld kernels); MemFree + Cached is the
    // kernel's documented stand-in.
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
    // Unknown stays 0, which refuses nothing.
    if (avail <= 0 && total <= 0) return 0;
    return std::max(avail, total / 2);
}

}  // namespace pt
