#include "skin/assets.h"

#include <SDL.h>

namespace ptshell {

std::vector<std::uint8_t> read_asset(const std::string& rel) {
    // Android: the relative path is the APK AAssetManager key, with no filesystem path to prepend.
    // Elsewhere it is anchored to the exe's directory — `SDL_RWFromFile` is `fopen`, and the working
    // directory is wherever a launcher last cd'd to.
    std::string path;
#ifdef __ANDROID__
    path = rel;
#else
    if (char* base = SDL_GetBasePath()) {
        path = std::string(base) + rel;
        SDL_free(base);
    } else {
        path = rel;  // no base path (rare): working-directory-relative rather than nothing
    }
#endif

    SDL_RWops* rw = SDL_RWFromFile(path.c_str(), "rb");
    if (rw == nullptr) return {};

    const Sint64 size = SDL_RWsize(rw);
    if (size <= 0) {  // unknown length or empty
        SDL_RWclose(rw);
        return {};
    }

    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));

    // ⚠️ Loop to EOF: on the APK path SDL_RWread is a decompressing stream and may return short.
    std::size_t got = 0;
    while (got < bytes.size()) {
        const std::size_t n = SDL_RWread(rw, bytes.data() + got, 1, bytes.size() - got);
        if (n == 0) break;  // EOF or error
        got += n;
    }
    SDL_RWclose(rw);

    if (got != bytes.size()) return {};  // a partial read is a corrupt asset
    return bytes;
}

}  // namespace ptshell
