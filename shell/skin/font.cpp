#include "skin/font.h"

#include "skin/assets.h"

#include <algorithm>
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

// Point to segment distance; the clamp gives round caps.
inline float dist_to_seg(float px, float py, float ax, float ay, float bx, float by) {
    const float vx = bx - ax, vy = by - ay;
    const float len2 = vx * vx + vy * vy;
    float t = len2 > 0.0f ? ((px - ax) * vx + (py - ay) * vy) / len2 : 0.0f;
    t = std::clamp(t, 0.0f, 1.0f);
    const float dx = px - (ax + t * vx), dy = py - (ay + t * vy);
    return std::sqrt(dx * dx + dy * dy);
}

// An N×N anti-aliased thin-line arrow: a shaft plus an open two-stroke head, with a 1 px coverage ramp.
void make_arrow_coverage(Arrow dir, int N, std::vector<std::uint8_t>& out) {
    out.assign(static_cast<std::size_t>(N) * N, 0);
    const float m    = N * 0.14f;              // margin from the cell edge
    const float lo   = m;
    const float hi   = N - m;
    const float mid  = N * 0.5f;
    const float half = N * 0.052f;             // half the stroke thickness
    const float wing = (N * 0.5f - m) * 0.60f; // wing reach along each axis from the apex

    // The shaft (tail → apex) and two wings from the apex.
    float sx[3], sy[3], ex[3], ey[3];
    switch (dir) {
        case Arrow::Up:
            sx[0] = mid; sy[0] = hi; ex[0] = mid;        ey[0] = lo;
            sx[1] = mid; sy[1] = lo; ex[1] = mid - wing; ey[1] = lo + wing;
            sx[2] = mid; sy[2] = lo; ex[2] = mid + wing; ey[2] = lo + wing;
            break;
        case Arrow::Down:
            sx[0] = mid; sy[0] = lo; ex[0] = mid;        ey[0] = hi;
            sx[1] = mid; sy[1] = hi; ex[1] = mid - wing; ey[1] = hi - wing;
            sx[2] = mid; sy[2] = hi; ex[2] = mid + wing; ey[2] = hi - wing;
            break;
        case Arrow::Left:
            sx[0] = hi; sy[0] = mid; ex[0] = lo;        ey[0] = mid;
            sx[1] = lo; sy[1] = mid; ex[1] = lo + wing; ey[1] = mid - wing;
            sx[2] = lo; sy[2] = mid; ex[2] = lo + wing; ey[2] = mid + wing;
            break;
        case Arrow::Right:
        default:
            sx[0] = lo; sy[0] = mid; ex[0] = hi;        ey[0] = mid;
            sx[1] = hi; sy[1] = mid; ex[1] = hi - wing; ey[1] = mid - wing;
            sx[2] = hi; sy[2] = mid; ex[2] = hi - wing; ey[2] = mid + wing;
            break;
    }

    for (int y = 0; y < N; ++y) {
        for (int x = 0; x < N; ++x) {
            const float px = x + 0.5f, py = y + 0.5f;
            float d = static_cast<float>(N) * 2.0f;
            for (int i = 0; i < 3; ++i)
                d = std::min(d, dist_to_seg(px, py, sx[i], sy[i], ex[i], ey[i]));
            const float cov = std::clamp(half + 0.5f - d, 0.0f, 1.0f);
            out[static_cast<std::size_t>(y) * N + x] = static_cast<std::uint8_t>(cov * 255.0f + 0.5f);
        }
    }
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

std::uint64_t Font::arrow_key(Arrow dir) {
    // Above any glyph key (codepoint ≤ 0x10FFFF, shifted by 12 < 2^33).
    return (static_cast<std::uint64_t>(1) << 40) | static_cast<std::uint64_t>(dir);
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

const Font::GlyphTex& Font::arrow_tex(Arrow dir) {
    const std::uint64_t k  = arrow_key(dir);
    auto                it = cache_.find(k);
    if (it != cache_.end()) return it->second;

    // Generated once per direction at 128 px; linear filtering keeps it smooth when scaled down.
    const int                 N = 128;
    std::vector<std::uint8_t> cov;
    make_arrow_coverage(dir, N, cov);
    std::vector<std::uint32_t> argb(cov.size());
    for (std::size_t i = 0; i < cov.size(); ++i) argb[i] = cov_to_argb(cov[i]);

    GlyphTex t;
    t.w   = N;
    t.h   = N;
    t.tex = make_texture(N, N, argb.data());
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

void Font::draw_arrow(Arrow dir, const SDL_Rect& box, std::uint32_t rgb) {
    if (renderer_ == nullptr || box.w <= 0 || box.h <= 0) return;
    const GlyphTex& a = arrow_tex(dir);
    if (a.tex == nullptr) return;
    const Uint8 cr = (rgb >> 16) & 0xFF, cg = (rgb >> 8) & 0xFF, cb = rgb & 0xFF;
    SDL_SetTextureColorMod(a.tex, cr, cg, cb);
    SDL_RenderCopy(renderer_, a.tex, nullptr, &box);
}

}  // namespace ptshell
