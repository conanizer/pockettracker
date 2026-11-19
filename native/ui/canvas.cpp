#include "canvas.h"

#include <algorithm>

namespace pt::ui {

namespace {

// Src-over, 8-bit integer.
inline uint32_t blend(uint32_t dst, uint32_t src) {
    const uint32_t sa = (src >> 24) & 0xFF;
    if (sa == 0xFF) return src;
    if (sa == 0) return dst;
    const uint32_t ia = 255 - sa;
    const uint32_t sr = (src >> 16) & 0xFF, sg = (src >> 8) & 0xFF, sb = src & 0xFF;
    const uint32_t dr = (dst >> 16) & 0xFF, dg = (dst >> 8) & 0xFF, db = dst & 0xFF;
    // Rounded, not truncated.
    const uint32_t r = (sr * sa + dr * ia + 127) / 255;
    const uint32_t g = (sg * sa + dg * ia + 127) / 255;
    const uint32_t b = (sb * sa + db * ia + 127) / 255;
    return 0xFF000000u | (r << 16) | (g << 8) | b;
}

// One UTF-8 code point at `i`, advancing past it. A malformed byte is consumed as U+FFFD (blank).
inline uint32_t next_codepoint(const std::string& s, size_t& i) {
    const auto b0 = static_cast<unsigned char>(s[i]);
    const size_t n = s.size();

    const auto cont = [&](size_t k) {
        return i + k < n && (static_cast<unsigned char>(s[i + k]) & 0xC0) == 0x80;
    };
    const auto bits = [&](size_t k) { return static_cast<uint32_t>(s[i + k]) & 0x3F; };

    if (b0 < 0x80) { i += 1; return b0; }
    if ((b0 & 0xE0) == 0xC0 && cont(1)) {
        const uint32_t cp = ((b0 & 0x1Fu) << 6) | bits(1);
        i += 2; return cp;
    }
    if ((b0 & 0xF0) == 0xE0 && cont(1) && cont(2)) {
        const uint32_t cp = ((b0 & 0x0Fu) << 12) | (bits(1) << 6) | bits(2);
        i += 3; return cp;
    }
    if ((b0 & 0xF8) == 0xF0 && cont(1) && cont(2) && cont(3)) {
        const uint32_t cp = ((b0 & 0x07u) << 18) | (bits(1) << 12) | (bits(2) << 6) | bits(3);
        i += 4; return cp;
    }
    i += 1;
    return 0xFFFD;
}

// Spelt this way in Title case: initialisms, units, a name with capitals of its own, and the filter
// types, so LP and LD look alike.
constexpr const char* CASE_KEPT[] = {
    "ABXY", "ADSR", "AHD", "BP",  "BPM",  "CC",  "CPU", "EQ", "FX", "HP",
    "KB",   "LD",  "LFO",  "LP",  "MB",  "MIDI", "NT",  "OTT", "PPQ",       "PPQN", "RAM",
    "SoundFont", "Hz", "ms",
};

constexpr bool is_letter(char c) { return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z'); }
constexpr bool is_digit(char c) { return c >= '0' && c <= '9'; }
constexpr bool is_hex_letter(char c) { return (c >= 'A' && c <= 'F') || (c >= 'a' && c <= 'f'); }
constexpr char to_upper(char c) { return (c >= 'a' && c <= 'z') ? static_cast<char>(c - 32) : c; }
constexpr char to_lower(char c) { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c + 32) : c; }

// A word runs over letters, digits and dots, so OCTA.F is one word and C-4 is two.
constexpr bool is_word_char(char c) { return is_letter(c) || is_digit(c) || c == '.'; }

// The CASE_KEPT spelling of s[from, to), or null.
const char* case_kept(const std::string& s, size_t from, size_t to) {
    for (const char* w : CASE_KEPT) {
        size_t i = from, k = 0;
        while (i < to && w[k] != '\0' && to_upper(s[i]) == to_upper(w[k])) { ++i; ++k; }
        if (i == to && w[k] == '\0') return w;
    }
    return nullptr;
}

// The letters and dots s[from, to) in `mode`; lower case makes every one small. `whole`: they are
// the whole word, so in Title a lone letter or a button keeps its capital. `numbered`: a digit is
// before them (4TH), so Title does not capitalise.
void case_word(std::string& s, size_t from, size_t to, TextCase mode, bool whole, bool numbered) {
    if (mode != TextCase::TITLE) {
        for (size_t i = from; i < to; ++i) s[i] = mode == TextCase::UPPER ? to_upper(s[i]) : to_lower(s[i]);
        return;
    }
    if (const char* kept = case_kept(s, from, to)) {
        for (size_t i = from; i < to; ++i) s[i] = kept[i - from];
        return;
    }
    // "A" before another word is the article; elsewhere a lone letter is a name (N, CC A, A:).
    const size_t n       = s.size();
    const bool   article = to - from == 1 && to_upper(s[from]) == 'A' && to + 1 < n && s[to] == ' ' &&
                           is_letter(s[to + 1]);
    const bool   lone    = whole && to - from == 1 && !article;
    // A button joins another with + (SEL+A, R+←) or names what it does with = (A=YES). FADE+ is not one.
    const bool   button  = whole && ((to + 1 < n && ((s[to] == '+' && s[to + 1] != ' ') || s[to] == '=')) ||
                                     (from > 0 && s[from - 1] == '+'));
    bool         capital = mode == TextCase::TITLE && !numbered;   // the next letter starts a run
    for (size_t i = from; i < to; ++i) {
        if (lone || button)     s[i] = to_upper(s[i]);
        else if (s[i] == '.')   capital = mode == TextCase::TITLE;
        else if (capital)       { s[i] = to_upper(s[i]); capital = false; }
        else                    s[i] = to_lower(s[i]);
    }
}

}  // namespace

void Canvas::label(const std::string& text, int x, int y, Argb color, TextSize size) {
    if (!scrollShown_.empty() && text == scrollShown_) {   // the scrolling name, cased as a whole
        scrollShown_ = cased(text);
        scrollText_  = cased(scrollText_);
    }
    this->text(cased(text), x, y, color, size);
}

std::string Canvas::cased(const std::string& text) const {
    std::string out = text;
    for (size_t from = 0; from < out.size();) {
        if (!is_word_char(out[from])) { ++from; continue; }
        size_t next = from;
        while (next < out.size() && is_word_char(out[next])) ++next;

        // A number or a hex value (0A, 7F, 120) stays as written.
        bool hex = is_digit(out[from]);
        for (size_t i = from; hex && i < next; ++i) hex = !is_letter(out[i]) || is_hex_letter(out[i]);
        if (!hex) {
            // The letters between digits are each a word: MOD1, SUS2, 4TH, 44100Hz.
            for (size_t a = from; a < next;) {
                if (is_digit(out[a])) { ++a; continue; }
                size_t b = a;
                while (b < next && !is_digit(out[b])) ++b;
                case_word(out, a, b, case_, /*whole=*/a == from && b == next, /*numbered=*/a > from);
                a = b;
            }
        }
        from = next;
    }
    return out;
}

void Canvas::clear(Argb color) {
    std::fill(px_.begin(), px_.end(), color);
}

void Canvas::set_clip(int x, int y, int w, int h) {
    clipX_ = std::max(0, x);
    clipY_ = std::max(0, y);
    clipW_ = std::max(0, std::min(x + w, DESIGN_W) - clipX_);
    clipH_ = std::max(0, std::min(y + h, DESIGN_H) - clipY_);
}

void Canvas::reset_clip() {
    clipX_ = 0;
    clipY_ = 0;
    clipW_ = DESIGN_W;
    clipH_ = DESIGN_H;
}

void Canvas::fill_rect(int x, int y, int w, int h, Argb color) {
    if (w <= 0 || h <= 0) return;

    const int x0 = std::max(x, clipX_);
    const int y0 = std::max(y, clipY_);
    const int x1 = std::min(x + w, clipX_ + clipW_);
    const int y1 = std::min(y + h, clipY_ + clipH_);
    if (x0 >= x1 || y0 >= y1) return;

    const bool opaque = ((color >> 24) & 0xFF) == 0xFF;
    for (int py = y0; py < y1; ++py) {
        uint32_t* row = px_.data() + static_cast<size_t>(py) * DESIGN_W;
        if (opaque) {
            std::fill(row + x0, row + x1, color);
        } else {
            for (int px = x0; px < x1; ++px) row[px] = blend(row[px], color);
        }
    }
}

void Canvas::stroke_rect(int x, int y, int w, int h, Argb color, int thickness) {
    if (w <= 0 || h <= 0 || thickness <= 0) return;
    const int t = std::min(thickness, std::min(w, h));
    fill_rect(x, y, w, t, color);                  // top
    fill_rect(x, y + h - t, w, t, color);          // bottom
    fill_rect(x, y + t, t, h - 2 * t, color);      // left
    fill_rect(x + w - t, y + t, t, h - 2 * t, color);  // right
}

// Each font pixel a font_scale square. Drawn as runs and bands rather than per pixel (text is most
// of a frame's cost); the rects never overlap, so a translucent colour blends each pixel once.
void Canvas::draw_glyph(const BitmapFont& f, const FontGlyph& g, int x, int y, Argb color, int font_scale) {
    if (font_scale <= 0) return;
    const uint16_t* rows = f.rows + g.offset;
    for (int row = 0; row < g.h;) {
        const uint16_t bits = rows[row];
        if (bits == 0) { ++row; continue; }

        int last = row + 1;
        while (last < g.h && rows[last] == bits) ++last;
        const int band_y = y + row * font_scale;
        const int band_h = (last - row) * font_scale;

        for (int col = 0; col < g.w;) {
            if (!(bits & (0x8000u >> col))) { ++col; continue; }
            int end = col + 1;
            while (end < g.w && (bits & (0x8000u >> end))) ++end;
            fill_rect(x + col * font_scale, band_y, (end - col) * font_scale, band_h, color);
            col = end;
        }
        row = last;
    }
}

std::string Canvas::as_shown(const std::string& text) const {
    if (case_ == TextCase::TITLE) return text;
    std::string out = text;
    for (char& ch : out) ch = case_ == TextCase::UPPER ? to_upper(ch) : to_lower(ch);
    return out;
}

void Canvas::text(const std::string& text, int x, int y, Argb color, TextSize size) {
    if (scrollShown_.empty() || text.compare(0, scrollShown_.size(), scrollShown_) != 0) {
        text_as_typed(as_shown(text), x, y, color, size);
        return;
    }
    // The scrolling name, then whatever follows it in `text` (THEME's " >") where it always sat.
    const std::string rest = text.substr(scrollShown_.size());
    scrollShown_.clear();
    const int cx = clipX_, cy = clipY_, cw = clipW_, ch = clipH_;
    const int left = std::max(cx, x), right = std::min(cx + cw, x + scrollW_);
    set_clip(left, cy, right - left, ch);
    text_as_typed(as_shown(scrollText_), x - scrollPx_, y, color, size);
    set_clip(cx, cy, cw, ch);
    if (!rest.empty()) text_as_typed(as_shown(rest), x + scrollW_ + font_->size(size).gap, y, color, size);
}

void Canvas::text_as_typed(const std::string& text, int x, int y, Argb color, TextSize size) {
    const BitmapFont&  f        = font_->face(size);
    const UiFont::Size s        = font_->size(size);
    const int          baseline = y + s.baseline;
    int                pen      = x;
    for (size_t i = 0; i < text.size();) {
        const FontGlyph* g = f.find(next_codepoint(text, i));
        if (g == nullptr) {
            pen += f.defaultAdvance * s.scale + s.gap;
            continue;
        }
        draw_glyph(f, *g, pen + g->dx * s.scale, baseline - g->top * s.scale, color, s.scale);
        pen += g->advance * s.scale + s.gap;
    }
}

int Canvas::text_width(const std::string& raw, TextSize size) const {
    const std::string  text = as_shown(raw);
    const UiFont::Size s    = font_->size(size);
    int                w = 0;
    for (size_t i = 0; i < text.size();)
        w += font_->face(size).advance(next_codepoint(text, i)) * s.scale + s.gap;
    if (text.empty()) return 0;
    const int built_in = font_->face(size).advance('M') * s.scale - glyph_width(size);
    return w - s.gap - built_in;
}

int Canvas::glyph_count(const std::string& text) {
    int n = 0;
    for (size_t i = 0; i < text.size();) {
        next_codepoint(text, i);
        ++n;
    }
    return n;
}

size_t Canvas::glyph_offset(const std::string& text, int glyph) {
    size_t i = 0;
    for (int n = 0; n < glyph && i < text.size(); ++n) next_codepoint(text, i);
    return i;
}

std::string Canvas::clip_text(const std::string& text, int max_glyphs) {
    if (max_glyphs <= 0) return {};

    size_t i   = 0;
    size_t cut = 0;   // byte offset just past glyph `max_glyphs - 1`, where the marker goes
    int    n   = 0;

    while (i < text.size()) {
        if (n == max_glyphs - 1) cut = i;
        next_codepoint(text, i);
        if (++n > max_glyphs) return text.substr(0, cut) + "\xE2\x80\xA6";
    }
    return text;
}

void Canvas::begin_frame(long long now_ms) {
    // A name that was not drawn last frame starts from its front when it comes back.
    if (!scrollDrawn_) scrollText_.clear();
    scrollDrawn_ = false;
    scrollDueMs_ = -1;
    nowMs_       = now_ms;
    scrollShown_.clear();
}

std::string Canvas::fit_text(const std::string& text, int max_glyphs, bool scrolls) {
    constexpr long long HOLD_MS = 1000;   // at either end
    constexpr int       STEP_PX = 3;      // one pixel of the 5×5 font at its usual size
    constexpr long long STEP_MS = 60;     // 50 px a second

    if (!scrolls || max_glyphs <= 0 || glyph_count(text) <= max_glyphs) return clip_text(text, max_glyphs);
    if (scrollDrawn_ && text != scrollText_) return clip_text(text, max_glyphs);

    // The field: the name's first `max_glyphs` glyphs, so the cell behind it is as wide as ever.
    std::string shown = text.substr(0, glyph_offset(text, max_glyphs));
    const int   field = text_width(shown);
    const int   over  = text_width(text) - field;
    if (over <= 0) return shown;

    if (text != scrollText_) {
        scrollText_    = text;
        scrollStartMs_ = nowMs_;
    }
    scrollDrawn_ = true;

    // Hold at the front, STEP_PX per step, hold at the end, back to the front.
    const long long steps = (over + STEP_PX - 1) / STEP_PX;
    const long long cycle = HOLD_MS + steps * STEP_MS + HOLD_MS;
    const long long t     = (nowMs_ - scrollStartMs_) % cycle;
    int             px    = 0;
    long long       wait  = HOLD_MS - t;
    if (t >= HOLD_MS) {
        px   = static_cast<int>(std::min<long long>(over, ((t - HOLD_MS) / STEP_MS + 1) * STEP_PX));
        wait = px < over ? STEP_MS - (t - HOLD_MS) % STEP_MS : cycle - t;
    }
    scrollDueMs_ = nowMs_ + wait;
    scrollPx_    = px;
    scrollW_     = field;
    scrollShown_ = shown;
    return shown;
}

std::string Canvas::clip_text_head(const std::string& text, int max_glyphs) {
    if (max_glyphs <= 0) return {};
    const int total = glyph_count(text);
    if (total <= max_glyphs) return text;

    size_t i = 0;   // from the front: only that lands on a code-point boundary
    for (int skip = total - (max_glyphs - 1); skip > 0; --skip) next_codepoint(text, i);
    return "\xE2\x80\xA6" + text.substr(i);
}

}  // namespace pt::ui
