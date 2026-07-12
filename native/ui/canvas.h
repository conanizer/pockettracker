#pragma once

// ─── The canvas ──────────────────────────────────────────────────────────────────────────────────
//
// A 640×480 software framebuffer and the four primitives the entire PocketTracker UI is drawn with:
// a filled rect, a stroked rect, a run of bitmap text, and a clip rectangle. That really is all of
// it — the Android UI is one Compose `Canvas` with no widgets in it (see linux-port-plan §13), so
// porting the drawing means porting these four calls and nothing else.
//
// ─── WHY THERE IS NO `scale` PARAMETER ───────────────────────────────────────────────────────────
//
// The Kotlin renderer carries an integer `scale` through every draw call and multiplies each
// coordinate by it (`Offset((x * scale), (y * scale))`), because Compose hands it a canvas the size
// of the physical screen. Here the canvas IS the 640×480 design and the SHELL scales the finished
// frame onto the display — which is what the port plan meant by "render the 640×480 design into a
// streaming texture; present with integer scale or fit-stretch" (§4.5).
//
// So `scale` disappears from every module signature and every draw call in the port. This is the
// single biggest simplification available and it is worth being explicit about, because it makes the
// C++ modules look *different* from their Kotlin twins on precisely one axis. The design coordinates
// — the ones that matter, the ones the parity diff compares — are untouched and identical.
//
// One consequence: a "1px" border is one design pixel, drawn once. Kotlin passes `Stroke(width =
// scale)` for exactly that reason; there is nothing to pass here.

#include <cstdint>
#include <string>
#include <vector>

#include "font5x5.h"
#include "theme.h"

namespace pt::ui {

// The design resolution — 640×480 4:3, which is the PortMaster ecosystem floor (RG35xx class) and
// the reason the Android app chose it too. Anything above it integer-scales or letterboxes.
inline constexpr int DESIGN_W = 640;
inline constexpr int DESIGN_H = 480;

class Canvas {
public:
    Canvas() : px_(static_cast<size_t>(DESIGN_W) * DESIGN_H, 0xFF000000) { reset_clip(); }

    // ── The frame ────────────────────────────────────────────────────────────────────────────────

    /** Fill the whole canvas, clip ignored. The Compose surface sits on `Color.Black`; so does this. */
    void clear(Argb color = 0xFF000000);

    /** Raw ARGB (0xAARRGGBB) pixels, DESIGN_W × DESIGN_H, row-major. What the shell uploads. */
    const uint32_t* pixels() const { return px_.data(); }
    int             pitch_bytes() const { return DESIGN_W * 4; }

    // ── Primitives ───────────────────────────────────────────────────────────────────────────────

    /** Filled rect. Blends src-over when `color` has alpha < 255 (the dialog backdrops rely on it). */
    void fill_rect(int x, int y, int w, int h, Argb color);

    /** 1px outline, drawn inside the given bounds — the twin of Compose's `style = Stroke(width)`. */
    void stroke_rect(int x, int y, int w, int h, Argb color, int thickness = 1);

    /** One glyph. `font_scale` is the 5×5 cell's pixel multiplier (3 → the standard 15×15 text). */
    void draw_glyph(const Glyph& g, int x, int y, Argb color, int font_scale);
    void draw_char(char c, int x, int y, Argb color, int font_scale);

    /**
     * A run of text, left to right. `spacing` is the gap between 5×5 cells, so each character
     * advances `5 * font_scale + spacing`. Mirrors `DrawScope.drawBitmapText`.
     */
    void draw_text(const std::string& text, int x, int y, Argb color, int spacing, int font_scale);

    /**
     * Width of `text` as drawn — `n * (5 * font_scale + spacing) - spacing`, i.e. the trailing gap
     * after the last glyph is not counted. Every centred label in the Kotlin UI open-codes this
     * (`fun tw(s) = s.length * 17 - 2`); it is written once here instead.
     */
    static int text_width(const std::string& text, int spacing, int font_scale);

    // ── Clipping ─────────────────────────────────────────────────────────────────────────────────
    //
    // The layout clips the editor area to the left of the right-hand bar so that wide row highlights
    // cannot bleed into the BPM readout and the note monitor. Compose spells this `clipRect(right =
    // …)`; here a clip rect is set and restored around the editor draw.

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
