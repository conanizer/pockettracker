#include "skin.h"

#include "assets.h"
#include "image.h"

#include <cstdio>

namespace ptshell {
namespace {

// One bit per art set, derived from the enumerator so the table below and `SkinArt` cannot drift apart.
constexpr uint8_t art_bit(SkinArt a) { return static_cast<uint8_t>(1u << static_cast<int>(a)); }

constexpr uint8_t kC = art_bit(SkinArt::Chrome);
constexpr uint8_t kT = art_bit(SkinArt::Transparent);

// SkinPiece → its filename and which art SETS want it, in enumerator order. Kept beside the enum so
// adding a piece is one line in each; a static_assert below makes a mismatched count a compile error
// rather than an off-by-one at run time.
//
// Membership is a MASK, not one set per file: the four generic button shapes are wanted by both Chrome
// and Transparent, which ship the same shapes in different colours under the same names, in their own
// folders. Keeping it as data means adding a set is a column of bits here and nothing else.
struct SkinFile {
    const char* name;
    uint8_t     sets;  // OR of art_bit(...) — which art sets look for this file
};

constexpr SkinFile kFiles[] = {
    {"bg_top_panel.png",           kC     },  // TopPanel
    {"bg_branding_panel.png",      kC     },  // BrandingPanel
    {"bg_button_backing.png",      kC     },  // ButtonBacking
    {"bg_screen_bezel.png",        kC     },  // ScreenBezel
    {"btn_square_normal.png",      kC | kT},  // BtnSquareNormal
    {"btn_square_pressed.png",     kC | kT},  // BtnSquarePressed
    {"btn_square_normal_dark.png", kC     },  // BtnSquareNormalDark
    {"btn_square_pressed_dark.png",kC     },  // BtnSquarePressedDark
    {"btn_wide_normal.png",        kC | kT},  // BtnWideNormal
    {"btn_wide_pressed.png",       kC | kT},  // BtnWidePressed
};
static_assert(sizeof(kFiles) / sizeof(kFiles[0]) == static_cast<int>(SkinPiece::COUNT),
              "kFiles must have one entry per SkinPiece");

const char* art_name(SkinArt a) {
    switch (a) {
        case SkinArt::Chrome:      return "chrome";
        case SkinArt::Transparent: return "transparent";
    }
    return "?";
}

}  // namespace

int Skin::load(SDL_Renderer* renderer, const std::string& theme, bool log, SkinArt art) {
    unload();

    const uint8_t want   = art_bit(art);
    int           wanted = 0;

    const std::string dir = "themes/" + theme + "/";
    for (int i = 0; i < static_cast<int>(SkinPiece::COUNT); ++i) {
        // Only the files this theme's art set wants — another set's are not missing, they were never
        // part of this skin, and reporting them as MISS would bury a real miss in noise.
        if ((kFiles[i].sets & want) == 0) continue;
        ++wanted;

        const char*       name = kFiles[i].name;
        const std::string rel  = dir + name;

        const std::vector<std::uint8_t> bytes = read_asset(rel);
        if (bytes.empty()) {
            if (log) std::printf("skin:    %-28s MISS (not found / unreadable)\n", name);
            continue;
        }

        Image img = decode_png(bytes.data(), bytes.size());
        if (!img.ok()) {
            if (log) std::printf("skin:    %-28s DECODE FAILED (%zu bytes)\n", name, bytes.size());
            continue;
        }

        // ARGB8888 is Image's 0xAARRGGBB packing byte-for-byte on a little-endian target (every target
        // here is), so the upload is a straight row copy with no channel shuffle — the same reasoning
        // sdl-video.cpp's create_texture() states for the framebuffer. STATIC, not STREAMING: a skin
        // texture is uploaded once and never touched again.
        SDL_Texture* tex = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ARGB8888,
                                             SDL_TEXTUREACCESS_STATIC, img.width, img.height);
        if (tex == nullptr) {
            if (log) std::printf("skin:    %-28s TEXTURE FAILED: %s\n", name, SDL_GetError());
            continue;
        }
        SDL_UpdateTexture(tex, nullptr, img.pixels.data(), img.width * static_cast<int>(sizeof(uint32_t)));
        // The skin composites OVER the frame and the letterbox bars, so its alpha must blend rather
        // than overwrite — the bezel and branding art have transparent regions by design, and the
        // transparent art is all alpha.
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
        if (log) std::printf("skin:    %-28s %dx%d ok\n", name, img.width, img.height);
    }

    if (log) std::printf("skin:    theme '%s' (%s) - %d/%d pieces loaded\n", theme.c_str(),
                         art_name(art), count_, wanted);
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

void Skin::draw_tinted(SDL_Renderer* renderer, SkinPiece p, const SDL_Rect& dst, uint32_t rgb) const {
    const SkinTexture& t = pieces_[static_cast<int>(p)];
    if (t.tex == nullptr) return;
    SDL_SetTextureColorMod(t.tex, static_cast<Uint8>((rgb >> 16) & 0xFF),
                           static_cast<Uint8>((rgb >> 8) & 0xFF), static_cast<Uint8>(rgb & 0xFF));
    SDL_RenderCopy(renderer, t.tex, nullptr, &dst);
    // Back to white: the mod is texture state, not draw state, so leaving it set would tint every
    // later blit of this piece — including one a caller made through plain `draw`.
    SDL_SetTextureColorMod(t.tex, 255, 255, 255);
}

}  // namespace ptshell
