// ─── shell/image.cpp — the ONE stb_image implementation TU (convergence plan D2) ─────────────────
//
// ⚠️ This is the only translation unit in the whole tree that pulls in the stb_image implementation.
// The header is vendored under native/vendor/ so the licence guard (build-portmaster.sh) can see it,
// but it is SHELL-ONLY by inclusion: nothing in native/ (the engine) or native/ui/ (pt-ui) includes
// it, so the canvas keeps its four primitives and pt-ui stays image-free by construction. See
// native/vendor/stb_image/PT-VENDORING.md.

#include "image.h"

// PNG only: every skin, overlay and theme asset this shell decodes is a PNG (the world ThemeLoader.kt
// loads from). STBI_ONLY_PNG compiles the decoder + the PNG path and nothing else — no JPEG/BMP/GIF/…
// code we do not ship an asset for. Add a format here the day an asset needs it, not before.
#define STBI_ONLY_PNG
#define STB_IMAGE_IMPLEMENTATION
// Resolves via the PUBLIC native/ include root the engine exports (inherited through pt-ui). No
// include line is restated here — the same reasoning as audio-decoders.cpp reaching stb_vorbis.
#include "vendor/stb_image/stb_image.h"

namespace ptshell {
namespace {

// stb_image with req_comp = 4 hands back a tight RGBA byte stream (R,G,B,A per pixel, top-left
// origin). Pack it into the shell's 0xAARRGGBB. A 3-channel source arrives here already expanded to
// RGBA with A = 0xFF, so the output is uniform regardless of the PNG's colour type.
Image pack_rgba(unsigned char* rgba, int w, int h) {
    Image img;
    if (rgba == nullptr || w <= 0 || h <= 0) return img;  // stays !ok()
    img.width  = w;
    img.height = h;
    img.pixels.resize(static_cast<std::size_t>(w) * static_cast<std::size_t>(h));
    for (std::size_t i = 0; i < img.pixels.size(); ++i) {
        const std::uint32_t r = rgba[i * 4 + 0];
        const std::uint32_t g = rgba[i * 4 + 1];
        const std::uint32_t b = rgba[i * 4 + 2];
        const std::uint32_t a = rgba[i * 4 + 3];
        img.pixels[i] = (a << 24) | (r << 16) | (g << 8) | b;
    }
    return img;
}

}  // namespace

Image decode_png(const std::uint8_t* data, std::size_t len) {
    if (data == nullptr || len == 0) return {};
    int            w = 0, h = 0, comp = 0;
    unsigned char* rgba = stbi_load_from_memory(data, static_cast<int>(len), &w, &h, &comp, 4);
    Image          img  = pack_rgba(rgba, w, h);
    stbi_image_free(rgba);  // safe on nullptr
    return img;
}

Image decode_png_file(const std::string& path) {
    int            w = 0, h = 0, comp = 0;
    unsigned char* rgba = stbi_load(path.c_str(), &w, &h, &comp, 4);
    Image          img  = pack_rgba(rgba, w, h);
    stbi_image_free(rgba);
    return img;
}

}  // namespace ptshell
