// ─── shell/image.h — PNG decode, SHELL-SIDE ONLY (convergence plan D2) ───────────────────────────
//
// The touch skin, the CRT overlays and the theme PNGs are decoded here and composited as SDL
// textures around the 640×480 frame in sdl-video's present() (D1). The canvas never sees a pixel of
// this: pt-ui keeps its four primitives, and image decoding is a SHELL facility, not a UI one.
//
// This header is deliberately SDL-free and engine-free — it depends on the standard library and
// (in the .cpp) the vendored stb_image, nothing else. That is what lets tools/ptdecode prove the
// decoder correct with no window and no pt-ui, exactly as ptshot proves the UI with no window.
//
// The two entry points mirror the two worlds the shell lives in (D7, the asset seam):
//   * decode_png(bytes) — Android APK assets are invisible to std::filesystem; the caller reads them
//     with SDL_RWFromFile and hands the bytes here.
//   * decode_png_file(path) — desktop skins sit beside the exe on a std::filesystem-reachable path.

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace ptshell {

// A decoded image in the shell's pixel convention: 0xAARRGGBB per pixel, row-major, top-left origin
// — the SAME ARGB packing the canvas and ptshot's PNG writer already use, so a decoded skin hands
// straight to an SDL_Texture (SDL_PIXELFORMAT_ARGB8888) with no channel shuffle. Always four
// channels: a source with no alpha decodes fully opaque (A = 0xFF).
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
