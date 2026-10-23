#include "skin/font.h"

#include "skin/assets.h"

#include <cmath>
#include <cstdio>
#include <vector>

namespace ptshell {

namespace {

// One UTF-8 step; returns bytes consumed. A malformed lead byte passes through as Latin-1.
int utf8_next(const char* s, const char* end, std::uint32_t& cp) {
    const auto b0 = static_cast<unsigned char>(*s);
    if (b0 < 0x80) { cp = b0; return 1; }
    if ((b0 >> 5) == 0x6 && s + 1 < end) {
        cp = ((b0 & 0x1Fu) << 6) | (static_cast<unsigned char>(s[1]) & 0x3Fu);
        return 2;
    }
    if ((b0 >> 4) == 0xE && s + 2 < end) {
        cp = ((b0 & 0x0Fu) << 12) | ((static_cast<unsigned char>(s[1]) & 0x3Fu) << 6) |
             (static_cast<unsigned char>(s[2]) & 0x3Fu);
        return 3;
    }
    if ((b0 >> 3) == 0x1E && s + 3 < end) {
        cp = ((b0 & 0x07u) << 18) | ((static_cast<unsigned char>(s[1]) & 0x3Fu) << 12) |
             ((static_cast<unsigned char>(s[2]) & 0x3Fu) << 6) |
             (static_cast<unsigned char>(s[3]) & 0x3Fu);
        return 4;
    }
    cp = b0;
    return 1;
}

// Coverage → white with alpha (ARGB8888), so a colour mod tints it to any label colour.
inline std::uint32_t cov_to_argb(std::uint8_t cov) {
    return (static_cast<std::uint32_t>(cov) << 24) | 0x00FFFFFFu;
}

}  // namespace

bool Font::load(SDL_Renderer* renderer, const std::string& asset_rel, bool log) {
    unload();
    renderer_ = renderer;

    const std::vector<std::uint8_t> bytes = read_asset(asset_rel);
    if (bytes.empty()) {
        if (log) std::printf("font:    %-28s MISS (not found / unreadable)\n", asset_rel.c_str());
        ready_ = false;
        return false;
    }
    ready_ = rast_.init(bytes.data(), bytes.size());
    if (log) {
        if (ready_) std::printf("font:    %-28s %zu bytes ok\n", asset_rel.c_str(), bytes.size());
        else        std::printf("font:    %-28s PARSE FAILED (%zu bytes)\n", asset_rel.c_str(),
                                bytes.size());
    }
    return ready_;
}

void Font::unload() {
    for (auto& kv : cache_)
        if (kv.second.tex) SDL_DestroyTexture(kv.second.tex);
    cache_.clear();
    ready_    = false;
    renderer_ = nullptr;
}

std::uint64_t Font::glyph_key(std::uint32_t codepoint, float px) {
    const std::uint32_t bucket = static_cast<std::uint32_t>(std::lround(px)) & 0xFFFu;  // ≤ 4095 px
    return (static_cast<std::uint64_t>(codepoint) << 12) | bucket;
}

SDL_Texture* Font::make_texture(int w, int h, const std::uint32_t* argb) const {
    SDL_Texture* tex =
        SDL_CreateTexture(renderer_, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STATIC, w, h);
    if (tex == nullptr) return nullptr;
    SDL_UpdateTexture(tex, nullptr, argb, w * static_cast<int>(sizeof(std::uint32_t)));
    SDL_SetTextureBlendMode(tex, SDL_BLENDMODE_BLEND);
    // Linear, like the skin: a label scaled to the button must smooth.
    SDL_SetTextureScaleMode(tex, SDL_ScaleModeLinear);
    return tex;
}

const Font::GlyphTex& Font::glyph_tex(std::uint32_t codepoint, float px) {
    const std::uint64_t k  = glyph_key(codepoint, px);
    auto                it = cache_.find(k);
    if (it != cache_.end()) return it->second;

    const RasterGlyph g = rast_.glyph(codepoint, px);
    GlyphTex          t;
    t.advance = g.advance;
    t.xoff    = g.xoff;
    t.yoff    = g.yoff;
    t.w       = g.w;
    t.h       = g.h;
    if (g.w > 0 && g.h > 0 && !g.coverage.empty()) {
        std::vector<std::uint32_t> argb(g.coverage.size());
        for (std::size_t i = 0; i < g.coverage.size(); ++i) argb[i] = cov_to_argb(g.coverage[i]);
        t.tex = make_texture(g.w, g.h, argb.data());
    }
    return cache_.emplace(k, t).first->second;
}

void Font::draw_text(const std::string& text, int x_left, int y_top, float px, std::uint32_t rgb) {
    if (!ready_ || renderer_ == nullptr) return;
    const int   baseline = y_top + rast_.ascent_px(px);
    const Uint8 cr = (rgb >> 16) & 0xFF, cg = (rgb >> 8) & 0xFF, cb = rgb & 0xFF;

    int         pen = x_left;
    const char* s   = text.c_str();
    const char* end = s + text.size();
    while (s < end) {
        std::uint32_t cp = 0;
        s += utf8_next(s, end, cp);
        const GlyphTex& g = glyph_tex(cp, px);
        if (g.tex) {
            SDL_SetTextureColorMod(g.tex, cr, cg, cb);
            SDL_Rect dst{pen + g.xoff, baseline + g.yoff, g.w, g.h};
            SDL_RenderCopy(renderer_, g.tex, nullptr, &dst);
        }
        pen += g.advance;
    }
}

}  // namespace ptshell
