#include "skin/skin.h"

#include "skin/assets.h"
#include "skin/image.h"

#include <cstdio>

namespace ptshell {
namespace {

// One bit per art set, derived from the enum so the two cannot drift.
constexpr uint8_t art_bit(SkinArt a) { return static_cast<uint8_t>(1u << static_cast<int>(a)); }

constexpr uint8_t kC = art_bit(SkinArt::Chrome);
constexpr uint8_t kT = art_bit(SkinArt::Transparent);

// SkinPiece → filename and the art sets that want it, in enum order (the static_assert catches a count
// mismatch). A mask: Chrome and Transparent ship the same button shapes under the same names, each in
// its own folder.
struct SkinFile {
    const char* name;
    uint8_t     sets;
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
        // Only this art set's files — another set's are not missing, and a MISS line must be real.
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

        // ARGB8888 matches Image's packing on little-endian targets; STATIC, uploaded once.
        SDL_Texture* tex = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ARGB8888,
                                             SDL_TEXTUREACCESS_STATIC, img.width, img.height);
        if (tex == nullptr) {
            if (log) std::printf("skin:    %-28s TEXTURE FAILED: %s\n", name, SDL_GetError());
            continue;
        }
        SDL_UpdateTexture(tex, nullptr, img.pixels.data(), img.width * static_cast<int>(sizeof(uint32_t)));
        // Blended: the art has transparent regions and composites over the frame.
        SDL_SetTextureBlendMode(tex, SDL_BLENDMODE_BLEND);
        // Linear, not SDL's default nearest: chrome scaled to its band must smooth.
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
    // Back to white: the mod is texture state and would tint every later blit of this piece.
    SDL_SetTextureColorMod(t.tex, 255, 255, 255);
}

}  // namespace ptshell
