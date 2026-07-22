#include "skin.h"

#include "assets.h"
#include "image.h"

#include <cstdio>

namespace ptshell {
namespace {

// SkinPiece → filename, in enumerator order. Kept beside the enum so adding a piece is one line in
// each; a static_assert below makes a mismatched count a compile error rather than an off-by-one at
// run time.
constexpr const char* kFilenames[] = {
    "bg_top_panel.png",          // TopPanel
    "bg_branding_panel.png",     // BrandingPanel
    "bg_button_backing.png",     // ButtonBacking
    "bg_screen_bezel.png",       // ScreenBezel
    "btn_square_normal.png",     // BtnSquareNormal
    "btn_square_pressed.png",    // BtnSquarePressed
    "btn_square_normal_dark.png",   // BtnSquareNormalDark
    "btn_square_pressed_dark.png",  // BtnSquarePressedDark
    "btn_wide_normal.png",       // BtnWideNormal
    "btn_wide_pressed.png",      // BtnWidePressed
};
static_assert(sizeof(kFilenames) / sizeof(kFilenames[0]) == static_cast<int>(SkinPiece::COUNT),
              "kFilenames must have one entry per SkinPiece");

}  // namespace

int Skin::load(SDL_Renderer* renderer, const std::string& theme, bool log) {
    unload();

    const std::string dir = "themes/" + theme + "/";
    for (int i = 0; i < static_cast<int>(SkinPiece::COUNT); ++i) {
        const std::string rel = dir + kFilenames[i];

        const std::vector<std::uint8_t> bytes = read_asset(rel);
        if (bytes.empty()) {
            if (log) std::printf("skin:    %-28s MISS (not found / unreadable)\n", kFilenames[i]);
            continue;
        }

        const Image img = decode_png(bytes.data(), bytes.size());
        if (!img.ok()) {
            if (log) std::printf("skin:    %-28s DECODE FAILED (%zu bytes)\n", kFilenames[i],
                                 bytes.size());
            continue;
        }

        // ARGB8888 is Image's 0xAARRGGBB packing byte-for-byte on a little-endian target (every target
        // here is), so the upload is a straight row copy with no channel shuffle — the same reasoning
        // sdl-video.cpp's create_texture() states for the framebuffer. STATIC, not STREAMING: a skin
        // texture is uploaded once and never touched again.
        SDL_Texture* tex = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ARGB8888,
                                             SDL_TEXTUREACCESS_STATIC, img.width, img.height);
        if (tex == nullptr) {
            if (log) std::printf("skin:    %-28s TEXTURE FAILED: %s\n", kFilenames[i], SDL_GetError());
            continue;
        }
        SDL_UpdateTexture(tex, nullptr, img.pixels.data(), img.width * static_cast<int>(sizeof(uint32_t)));
        // The skin composites OVER the frame and the letterbox bars, so its alpha must blend rather
        // than overwrite — the bezel and branding art have transparent regions by design.
        SDL_SetTextureBlendMode(tex, SDL_BLENDMODE_BLEND);
        // LINEAR (bilinear) filtering, not SDL's default NEAREST: the skin is device CHROME authored at
        // one resolution and scaled to fit the band on screen, so its diagonals, curves and the cursive
        // branding must smooth under scaling — exactly what Compose's BitmapPainter (FilterQuality.Low =
        // bilinear) gave it on Android. NEAREST is right for the 640×480 pixel-art FRAMEBUFFER (that
        // texture lives in sdl-video.cpp and keeps its own scale mode); it is wrong for the chrome, whose
        // stair-stepped edges were the user-visible "ladder pixelisation" this fixes.
        SDL_SetTextureScaleMode(tex, SDL_ScaleModeLinear);

        pieces_[i] = SkinTexture{tex, img.width, img.height};
        ++count_;
        if (log) std::printf("skin:    %-28s %dx%d ok\n", kFilenames[i], img.width, img.height);
    }

    if (log) std::printf("skin:    theme '%s' — %d/%d pieces loaded\n", theme.c_str(), count_,
                         static_cast<int>(SkinPiece::COUNT));
    return count_;
}

void Skin::unload() {
    for (SkinTexture& p : pieces_) {
        if (p.tex) SDL_DestroyTexture(p.tex);
        p = SkinTexture{};
    }
    count_ = 0;
}

void Skin::draw(SDL_Renderer* renderer, SkinPiece p, const SDL_Rect& dst) const {
    const SkinTexture& t = pieces_[static_cast<int>(p)];
    if (t.tex == nullptr) return;
    SDL_RenderCopy(renderer, t.tex, nullptr, &dst);
}

}  // namespace ptshell
