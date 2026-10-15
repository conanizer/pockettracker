// PNG decode for the shell (skins, overlays): SDL-free, stb_image in the .cpp. The canvas never sees a
// decoded image. decode_png for asset bytes, decode_png_file for a file on disk.

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace ptshell {

// 0xAARRGGBB, row-major, top-left origin — uploads straight to ARGB8888. Always four channels.
struct Image {
    int                   width  = 0;
    int                   height = 0;
    std::vector<uint32_t> pixels;  // width*height when ok(), else empty

    bool ok() const {
        return width > 0 && height > 0 &&
               pixels.size() == static_cast<std::size_t>(width) * static_cast<std::size_t>(height);
    }
};

// ok() false on any failure; never throws, never partially fills.
Image decode_png(const std::uint8_t* data, std::size_t len);

// Same contract, from a file.
Image decode_png_file(const std::string& path);

}  // namespace ptshell
