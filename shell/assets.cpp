#include "assets.h"

#include <SDL.h>

namespace ptshell {

std::vector<std::uint8_t> read_asset(const std::string& rel) {
    // ── The one place the two worlds are told apart ──────────────────────────────────────────────
    // On Android the relative path IS the lookup key into the APK's AAssetManager, and prepending a
    // base path would break it (there is no filesystem path to prepend). Everywhere else the same
    // relative path has to be anchored to the exe's directory, or `SDL_RWFromFile` == `fopen` resolves
    // it against the CWD — which on a handheld is wherever the launch script last cd'd to, and on a
    // desktop is wherever the user double-clicked from. Neither is where the resources are.
    std::string path;
#ifdef __ANDROID__
    path = rel;
#else
    if (char* base = SDL_GetBasePath()) {
        path = std::string(base) + rel;
        SDL_free(base);
    } else {
        path = rel;  // no base path (rare) — fall back to CWD-relative rather than read nothing
    }
#endif

    SDL_RWops* rw = SDL_RWFromFile(path.c_str(), "rb");
    if (rw == nullptr) return {};

    const Sint64 size = SDL_RWsize(rw);
    if (size <= 0) {  // unknown length (<0) or empty (0) — nothing usable to decode
        SDL_RWclose(rw);
        return {};
    }

    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));

    // ⚠️ SDL_RWread may return a SHORT read, so loop to EOF: the APK path is a decompressing stream,
    // and one read that fills the buffer on desktop could truncate a large PNG on the phone.
    std::size_t got = 0;
    while (got < bytes.size()) {
        const std::size_t n = SDL_RWread(rw, bytes.data() + got, 1, bytes.size() - got);
        if (n == 0) break;  // EOF or error — SDL_RWread signals both as 0
        got += n;
    }
    SDL_RWclose(rw);

    if (got != bytes.size()) return {};  // a partial read is a corrupt asset — decode nothing
    return bytes;
}

}  // namespace ptshell
