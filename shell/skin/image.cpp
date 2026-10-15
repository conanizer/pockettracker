// The only stb_image implementation TU. The header is vendored under native/vendor/ for the licence
// guard, but only the shell includes it — pt-ui stays image-free. See native/vendor/stb_image/PT-VENDORING.md.

#include "skin/image.h"

// Every asset is a PNG.
#define STBI_ONLY_PNG
#define STB_IMAGE_IMPLEMENTATION
#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"   // vendored
#elif defined(_MSC_VER)
#pragma warning(push, 0)
#endif
#include "vendor/stb_image/stb_image.h"
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#elif defined(_MSC_VER)
#pragma warning(pop)
#endif

namespace ptshell {
namespace {

// stb_image's tight RGBA bytes (req_comp = 4; RGB arrives with A = 0xFF) → 0xAARRGGBB.
Image pack_rgba(unsigned char* rgba, int w, int h) {
    Image img;
    if (rgba == nullptr || w <= 0 || h <= 0) return img;
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
