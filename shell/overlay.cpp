#include "overlay.h"

#include "assets.h"
#include "image.h"

#include <cstdio>

namespace ptshell {

bool ScreenOverlay::load(SDL_Renderer* renderer, int index, bool log) {
    unload();

    if (index <= 0 || index > kScreenOverlayCount) {
        if (log) std::printf("overlay: OFF (no overlay)\n");
        return false;  // OFF — nothing to draw, and not an error
    }

    const ScreenOverlayDef& def = kScreenOverlays[index - 1];

    const std::vector<std::uint8_t> bytes = read_asset(def.file);
    if (bytes.empty()) {
        if (log) std::printf("overlay: %-16s MISS (not found / unreadable)\n", def.file);
        return false;
    }

    const Image img = decode_png(bytes.data(), bytes.size());
    if (!img.ok()) {
        if (log) std::printf("overlay: %-16s DECODE FAILED (%zu bytes)\n", def.file, bytes.size());
        return false;
    }

    // ARGB8888 is Image's 0xAARRGGBB packing byte-for-byte on a little-endian target — a straight row
    // copy, exactly as skin.cpp/sdl-video.cpp state. STATIC: uploaded once, never touched again.
    SDL_Texture* tex = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ARGB8888,
                                         SDL_TEXTUREACCESS_STATIC, img.width, img.height);
    if (tex == nullptr) {
        if (log) std::printf("overlay: %-16s TEXTURE FAILED: %s\n", def.file, SDL_GetError());
        return false;
    }
    SDL_UpdateTexture(tex, nullptr, img.pixels.data(), img.width * static_cast<int>(sizeof(uint32_t)));

    // Blend over the frame (that is the whole job — a translucent filter), and set the alpha mod per
    // draw from the STRENGTH row. LINEAR filtering like the skin: the PNG is authored at one resolution
    // and stretched to the frame rect, so it must smooth under scaling — Compose drew it the same way
    // (drawImage's default FilterQuality). NEAREST is right only for the 640×480 pixel-art framebuffer,
    // whose texture lives in sdl-video.cpp and keeps its own scale mode.
    SDL_SetTextureBlendMode(tex, SDL_BLENDMODE_BLEND);
    SDL_SetTextureScaleMode(tex, SDL_ScaleModeLinear);

    tex_ = tex;
    if (log) std::printf("overlay: %-16s %dx%d ok\n", def.file, img.width, img.height);
    return true;
}

void ScreenOverlay::unload() {
    if (tex_) SDL_DestroyTexture(tex_);
    tex_ = nullptr;
}

void ScreenOverlay::draw(SDL_Renderer* renderer, const SDL_Rect& dst, int strength) const {
    if (tex_ == nullptr || strength <= 0) return;
    const Uint8 alpha = static_cast<Uint8>(strength > 255 ? 255 : strength);
    SDL_SetTextureAlphaMod(tex_, alpha);
    SDL_RenderCopy(renderer, tex_, nullptr, &dst);
}

}  // namespace ptshell
