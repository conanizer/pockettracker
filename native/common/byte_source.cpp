#include "byte_source.h"

#include <cstring>

#if defined(_WIN32)
#include <io.h>       // _fdopen, _close
#else
#include <unistd.h>   // close
#endif

namespace {

// Written once at boot, before any file is opened; never read from the audio thread.
PtFileHooks g_hooks;

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

void pt_set_file_hooks(const PtFileHooks& hooks) { g_hooks = hooks; }

const PtFileHooks& pt_file_hooks() { return g_hooks; }

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

    if (!g_hooks.open) return nullptr;
    const int fd = g_hooks.open(path, mode);
    if (fd < 0) return nullptr;

    FILE* f = fdopen_portable(fd, mode);
    // The hook has let go of the descriptor, so a failed fdopen must close it here.
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

    // ⚠️ `ferror`, not the byte count: a short read and the end look alike, and a truncated project
    // must not parse as far as it got.
    const bool ok = std::ferror(f) == 0;
    std::fclose(f);
    if (!ok) out.clear();
    return ok;
}

int pt_remove(const char* path) {
    if (!path || !*path) return -1;
    if (!pt_path_is_uri(path)) return std::remove(path);
    if (!g_hooks.remove) return -1;
    return g_hooks.remove(path);
}

int pt_rename(const char* from, const char* to) {
    if (!from || !*from || !to || !*to) return -1;

    const bool fromUri = pt_path_is_uri(from);
    const bool toUri   = pt_path_is_uri(to);
    if (!fromUri && !toUri) return std::rename(from, to);

    // Both callers rename `<path>.tmp` onto `<path>`; a mixed pair is a bug somewhere, not a copy.
    if (fromUri != toUri) return -1;
    if (!g_hooks.rename) return -1;
    return g_hooks.rename(from, to);
}
