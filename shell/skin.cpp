#include "skin.h"

#include "assets.h"
#include "image.h"

#include <cstdio>

namespace ptshell {
namespace {

// One bit per art set, derived from the enumerator so the table below and `SkinArt` cannot drift apart.
constexpr uint8_t art_bit(SkinArt a) { return static_cast<uint8_t>(1u << static_cast<int>(a)); }

constexpr uint8_t kC = art_bit(SkinArt::Chrome);
constexpr uint8_t kB = art_bit(SkinArt::Bitmap);
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
    {"btn_up_normal.png",          kB     },  // BmpUpNormal
    {"btn_up_pressed.png",         kB     },  // BmpUpPressed
    {"btn_down_normal.png",        kB     },  // BmpDownNormal
    {"btn_down_pressed.png",       kB     },  // BmpDownPressed
    {"btn_left_normal.png",        kB     },  // BmpLeftNormal
    {"btn_left_pressed.png",       kB     },  // BmpLeftPressed
    {"btn_right_normal.png",       kB     },  // BmpRightNormal
    {"btn_right_pressed.png",      kB     },  // BmpRightPressed
    {"btn_a_normal.png",           kB     },  // BmpANormal
    {"btn_a_pressed.png",          kB     },  // BmpAPressed
    {"btn_b_normal.png",           kB     },  // BmpBNormal
    {"btn_b_pressed.png",          kB     },  // BmpBPressed
    {"btn_sel_normal.png",         kB     },  // BmpSelNormal
    {"btn_sel_pressed.png",        kB     },  // BmpSelPressed
    {"btn_start_normal.png",       kB     },  // BmpStartNormal
    {"btn_start_pressed.png",      kB     },  // BmpStartPressed
    {"btn_lshift_normal.png",      kB     },  // BmpLShiftNormal
    {"btn_lshift_pressed.png",     kB     },  // BmpLShiftPressed
    {"btn_rshift_normal.png",      kB     },  // BmpRShiftNormal
    {"btn_rshift_pressed.png",     kB     },  // BmpRShiftPressed
};
static_assert(sizeof(kFiles) / sizeof(kFiles[0]) == static_cast<int>(SkinPiece::COUNT),
              "kFiles must have one entry per SkinPiece");

const char* art_name(SkinArt a) {
    switch (a) {
        case SkinArt::Chrome:      return "chrome";
        case SkinArt::Bitmap:      return "bitmap";
        case SkinArt::Transparent: return "transparent";
    }
    return "?";
}

// Turn the Bitmap set's two-colour OPAQUE art into white ink on transparent, so the renderer can
// colour it per frame with a texture mod and let the ground fall through to the casing behind.
//
// ⚠️ The art's own colours are thrown away deliberately: it is authored black-on-white-ink, but the
// skin takes BOTH of its colours from the live tracker theme, so the only thing worth keeping out of a
// source pixel is how much INK is on it. That is its brightness, which becomes alpha — and because the
// art is greyscale, any one channel carries it, so this reads red rather than weighting three.
void to_ink_alpha(Image& img) {
    for (uint32_t& px : img.pixels) {
        const uint32_t ink = (px >> 16) & 0xFF;              // red == luma on greyscale art
        const uint32_t a   = ((px >> 24) & 0xFF) * ink / 255;  // honour any alpha the art did carry
        px = (a << 24) | 0x00FFFFFFu;                         // flat white; the mod supplies the colour
    }
}

}  // namespace

int Skin::load(SDL_Renderer* renderer, const std::string& theme, bool log, SkinArt art) {
    unload();

    const uint8_t want   = art_bit(art);
    int           wanted = 0;

    const std::string dir = "themes/" + theme + "/";
    for (int i = 0; i < static_cast<int>(SkinPiece::COUNT); ++i) {
        // Only the files this theme's art set wants — another set's are not missing, they were never
        // part of this skin, and reporting them as MISS would bury a real miss in twenty lines of noise.
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
        // Only the Bitmap set needs converting: it is the one authored opaque. The Transparent set is
        // already flat white with an alpha channel, so touching it would be a no-op at best.
        if (art == SkinArt::Bitmap) to_ink_alpha(img);

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
        // than overwrite — the bezel and branding art have transparent regions by design, the bitmap
        // art is ALL alpha once `to_ink_alpha` has run, and the transparent art was authored that way.
        SDL_SetTextureBlendMode(tex, SDL_BLENDMODE_BLEND);
        // LINEAR (bilinear) filtering, not SDL's default NEAREST: the skin is device CHROME authored at
        // one resolution and scaled to fit the band on screen, so its diagonals, curves and the cursive
        // branding must smooth under scaling — exactly what Compose's BitmapPainter (FilterQuality.Low =
        // bilinear) gave it on Android. NEAREST is right for the 640×480 pixel-art FRAMEBUFFER (that
        // texture lives in sdl-video.cpp and keeps its own scale mode); it is wrong for the chrome, whose
        // stair-stepped edges were the user-visible "ladder pixelisation" this fixes.
        //
        // ⚠️ The Bitmap set is the other way round for the same reason: it IS pixel art, authored at
        // 64px and blown up several times over, so bilinear would dissolve exactly the hard edges that
        // make it read as bitmap. The Transparent set is back on LINEAR — anti-aliased curves authored
        // at 264px, the same kind of thing as the chrome. All three follow the one rule: match the
        // filtering to what the art is, not to where it is drawn.
        SDL_SetTextureScaleMode(tex, art == SkinArt::Bitmap ? SDL_ScaleModeNearest
                                                            : SDL_ScaleModeLinear);

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
