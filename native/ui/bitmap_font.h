#pragma once

// A bitmap font as data: packed rows and a glyph table, generated from a BDF file by
// fonts/bdf_to_header.py. Each glyph is a box placed on a baseline, so fonts of any size, width or
// aspect go through the same drawing path.

#include <cstdint>

namespace pt::ui {

struct FontGlyph {
    uint32_t cp;        // the table is sorted by code point
    uint16_t offset;    // first row in BitmapFont::rows
    uint8_t  w, h;      // the box, in font pixels
    int8_t   dx;        // box left edge from the pen
    int8_t   top;       // box top edge above the baseline
    uint8_t  advance;   // pen step
};

struct BitmapFont {
    const char*      name;
    const uint16_t*  rows;            // one per glyph row, bit 15 the leftmost pixel
    const FontGlyph* glyphs;
    int              count;
    const int16_t*   ascii;           // 128 indices into `glyphs`, −1 none; a–z folded onto A–Z when absent
    int              ascent, descent; // above and below the baseline, in font pixels
    int              defaultAdvance;  // a code point the font lacks draws blank and moves this far
    bool             hasLowercase;

    // Null when the font has no glyph for `cp`.
    constexpr const FontGlyph* find(uint32_t cp) const {
        if (cp < 128) return ascii[cp] < 0 ? nullptr : &glyphs[ascii[cp]];
        int lo = 0, hi = count - 1;
        while (lo <= hi) {
            const int mid = (lo + hi) / 2;
            if (glyphs[mid].cp == cp) return &glyphs[mid];
            if (glyphs[mid].cp < cp) lo = mid + 1;
            else                     hi = mid - 1;
        }
        return nullptr;
    }

    constexpr int advance(uint32_t cp) const {
        const FontGlyph* g = find(cp);
        return g ? g->advance : defaultAdvance;
    }
};

// The code points beyond ASCII that the UI draws. Every UI font carries them.
inline constexpr uint32_t CP_ARROW_LEFT  = 0x2190;
inline constexpr uint32_t CP_ARROW_UP    = 0x2191;
inline constexpr uint32_t CP_ARROW_RIGHT = 0x2192;
inline constexpr uint32_t CP_ARROW_DOWN  = 0x2193;
inline constexpr uint32_t CP_ELLIPSIS    = 0x2026;   // a clip marker, one column wide

}  // namespace pt::ui
