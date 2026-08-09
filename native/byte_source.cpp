#include "byte_source.h"

#include <cstring>

#if defined(_WIN32)
#include <io.h>       // _fdopen, _close
#else
#include <unistd.h>   // close
#endif

namespace {

// ⚠️ Read on the audio thread's behalf but never from it — a sample load runs on the caller's
// thread while the audio callback is already running, so this is a plain pointer, written once at
// boot before any file is opened. Making it atomic would suggest an install is allowed to race an
// open, which it is not.
PtOpenHook g_open_hook = nullptr;

FILE* fdopen_portable(int fd, const char* mode) {
#if defined(_WIN32)
    return _fdopen(fd, mode);
#else
    return fdopen(fd, mode);
#endif
}

void close_portable(int fd) {
#if defined(_WIN32)
    _close(fd);
#else
    close(fd);
#endif
}

}  // namespace

void pt_set_open_hook(PtOpenHook hook) { g_open_hook = hook; }

PtOpenHook pt_get_open_hook() { return g_open_hook; }

bool pt_path_is_uri(const char* path) {
    if (!path) return false;
    // RFC 3986: scheme = ALPHA *( ALPHA / DIGIT / "+" / "-" / "." ), then "://".
    if (!((path[0] >= 'a' && path[0] <= 'z') || (path[0] >= 'A' && path[0] <= 'Z'))) return false;
    size_t i = 1;
    for (; path[i]; i++) {
        const char c = path[i];
        const bool schemeChar = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                                (c >= '0' && c <= '9') || c == '+' || c == '-' || c == '.';
        if (!schemeChar) break;
    }
    return i >= 1 && std::strncmp(path + i, "://", 3) == 0;
}

FILE* pt_fopen(const char* path, const char* mode) {
    if (!path || !*path || !mode) return nullptr;
    if (!pt_path_is_uri(path)) return std::fopen(path, mode);

    if (!g_open_hook) return nullptr;
    const int fd = g_open_hook(path, mode);
    if (fd < 0) return nullptr;

    FILE* f = fdopen_portable(fd, mode);
    // fdopen failing leaves the descriptor open and owned by nobody — the hook has already let go
    // of it, so this is the only place left that can close it.
    if (!f) close_portable(fd);
    return f;
}

bool pt_read_file(const char* path, std::string& out) {
    FILE* f = pt_fopen(path, "rb");
    if (!f) return false;

    out.clear();
    char buf[64 * 1024];
    size_t got;
    while ((got = std::fread(buf, 1, sizeof(buf), f)) > 0) out.append(buf, got);

    // ⚠️ A short read and a finished file are the same `fread` return, so the verdict is ferror,
    // not the byte count: nothing here knows the length up front, and a truncated project that
    // silently parses as far as it got is the failure this rules out.
    const bool ok = std::ferror(f) == 0;
    std::fclose(f);
    if (!ok) out.clear();
    return ok;
}
