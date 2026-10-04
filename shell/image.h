// ─── shell/image.h — PNG decode, SHELL-SIDE ONLY ─────────────────────────────────────────────────
//
// The touch skin, the CRT overlays and the theme PNGs are decoded here and composited as SDL
// textures around the 640×480 frame in sdl-video's present(). The canvas never sees a pixel: pt-ui
// keeps its four primitives, and image decoding is a SHELL facility.
//
// SDL-free and engine-free (stb_image in the .cpp), so the decoder is testable without a window.
// Two entry points for the asset seam (assets.h): decode_png(bytes) for APK assets read through
// SDL_RWFromFile, decode_png_file(path) for desktop files beside the exe.

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace ptshell {

// A decoded image in the shell's pixel convention: 0xAARRGGBB per pixel, row-major, top-left origin
// — the canvas's packing, so it uploads straight to an SDL_PIXELFORMAT_ARGB8888 texture. Always four
// channels: a source with no alpha decodes fully opaque.
struct Image {
    int                   width  = 0;
    int                   height = 0;
    std::vector<uint32_t> pixels;  // width*height entries when ok(); empty on failure

    bool ok() const {
        return width > 0 && height > 0 &&
               pixels.size() == static_cast<std::size_t>(width) * static_cast<std::size_t>(height);
    }
};

// Decode a PNG held in memory. Returns an Image with ok()==false on any failure (not a PNG, corrupt,
// out of memory) — never throws, never partially fills.
Image decode_png(const std::uint8_t* data, std::size_t len);

// Decode a PNG from a file on disk. Same contract; a missing or unreadable file yields ok()==false.
Image decode_png_file(const std::string& path);

}  // namespace ptshell
