#pragma once

// ─── The canvas ──────────────────────────────────────────────────────────────────────────────────
//
// A 640×480 software framebuffer and the four primitives the whole UI is drawn with: a filled rect, a
// stroked rect, bitmap text, and a clip rectangle. ⚠️ Four, permanently.
//
// There is no `scale` parameter: the canvas IS the 640×480 design, and the shell scales the finished
// frame onto the display. A "1px" border is one design pixel.

#include <cstdint>
#include <string>
#include <vector>

#include "font5x5.h"
#include "theme.h"

namespace pt::ui {

// The design resolution — 640×480 4:3, the PortMaster floor (RG35xx class). Anything larger
// integer-scales or letterboxes.
inline constexpr int DESIGN_W = 640;
inline constexpr int DESIGN_H = 480;

// The dim scrim every modal paints over the whole canvas. The one source of it: the shell blends the
// SAME value over the letterbox bars, so the dim is seamless across the 4:3 edge.
inline constexpr Argb MODAL_BACKDROP = 0xCC000000;

class Canvas {
public:
    Canvas() : px_(static_cast<size_t>(DESIGN_W) * DESIGN_H, 0xFF000000) { reset_clip(); }

    // ── The frame ────────────────────────────────────────────────────────────────────────────────

    /** Fill the whole canvas, clip ignored. */
    void clear(Argb color = 0xFF000000);

    /** Raw ARGB (0xAARRGGBB) pixels, DESIGN_W × DESIGN_H, row-major. What the shell uploads. */
    const uint32_t* pixels() const { return px_.data(); }
    int             pitch_bytes() const { return DESIGN_W * 4; }

    // ── Primitives ───────────────────────────────────────────────────────────────────────────────

    /** Filled rect. Blends src-over when `color` has alpha < 255 (the dialog backdrops rely on it). */
    void fill_rect(int x, int y, int w, int h, Argb color);

    /** Outline drawn inside the given bounds. */
    void stroke_rect(int x, int y, int w, int h, Argb color, int thickness = 1);

    /** One glyph. `font_scale` is the 5×5 cell's pixel multiplier (3 → the standard 15×15 text). */
    void draw_glyph(const Glyph& g, int x, int y, Argb color, int font_scale);
    void draw_char(char c, int x, int y, Argb color, int font_scale);

    /**
     * A run of text, left to right. `spacing` is the gap between 5×5 cells, so each character advances
     * `5 * font_scale + spacing`.
     * ⚠️ `text` is UTF-8 and the advance is per CODE POINT (MODS draws "→M2 AMT").
     */
    void draw_text(const std::string& text, int x, int y, Argb color, int spacing, int font_scale);

    /** Width of `text` as drawn — `n * (5 * font_scale + spacing) - spacing` (no trailing gap). */
    static int text_width(const std::string& text, int spacing, int font_scale);

    /** Glyphs (code points) in `text`. */
    static int glyph_count(const std::string& text);

    /**
     * Clip `text` to at most `max_glyphs` columns, marking a cut with U+2026 (`…`).
     * ⚠️ The marker is INSIDE the budget (`max_glyphs - 1` glyphs + `…`), so the result is never wider
     * than the column; a string that fits comes back untouched. Counted in code points, so a cut never
     * splits a UTF-8 sequence.
     */
    static std::string clip_text(const std::string& text, int max_glyphs);

    /** `clip_text`'s mirror: keep the END and mark the cut at the FRONT — for paths, whose last
     *  component is the part that means something. */
    static std::string clip_text_head(const std::string& text, int max_glyphs);

    // ── Clipping ─────────────────────────────────────────────────────────────────────────────────
    //
    // The layout clips the editor area left of the right-hand bar, so wide row highlights cannot bleed
    // into the BPM readout and note monitor.

    void set_clip(int x, int y, int w, int h);
    void reset_clip();

    /** RAII clip, so an early `return` inside a module cannot leak a clip into the next one. */
    class ClipScope {
    public:
        ClipScope(Canvas& c, int x, int y, int w, int h)
            : c_(c), cx_(c.clipX_), cy_(c.clipY_), cw_(c.clipW_), ch_(c.clipH_) {
            c.set_clip(x, y, w, h);
        }
        ~ClipScope() { c_.set_clip(cx_, cy_, cw_, ch_); }
        ClipScope(const ClipScope&)            = delete;
        ClipScope& operator=(const ClipScope&) = delete;

    private:
        Canvas& c_;
        int     cx_, cy_, cw_, ch_;
    };

private:
    void blend_px(int x, int y, Argb color);

    std::vector<uint32_t> px_;
    int                   clipX_ = 0, clipY_ = 0, clipW_ = DESIGN_W, clipH_ = DESIGN_H;
};

}  // namespace pt::ui
