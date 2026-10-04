#pragma once

// ─── The 5×5 bitmap font ─────────────────────────────────────────────────────────────────────────
//
// Converted from quinine_five.ttf. Every glyph is 5×5 px, one byte per row, MSB = leftmost pixel
// (bit 4 .. bit 0). A 128-entry table indexed by char code, lowercase folded onto uppercase at build
// time; the non-ASCII glyphs (arrows, ellipsis) are keyed by code point.
// ⚠️ Every golden screenshot rasterizes from these exact bits.

#include <array>
#include <cstdint>

namespace pt::ui {

// One glyph: 5 rows, MSB-first (bit 4 = leftmost of the 5 columns).
using Glyph = std::array<uint8_t, 5>;

// All-zero rows = "not mapped" — and an unmapped character draws blank.
inline constexpr Glyph GLYPH_NONE{{0, 0, 0, 0, 0}};

// The arrows are outside ASCII, keyed by CODE POINT; `Canvas::draw_text` decodes UTF-8 to find them.
inline constexpr Glyph GLYPH_ARROW_UP{{0b00000, 0b00100, 0b01110, 0b11111, 0b00000}};
inline constexpr Glyph GLYPH_ARROW_DOWN{{0b00000, 0b11111, 0b01110, 0b00100, 0b00000}};
inline constexpr Glyph GLYPH_ARROW_LEFT{{0b00010, 0b00110, 0b01110, 0b00110, 0b00010}};
inline constexpr Glyph GLYPH_ARROW_RIGHT{{0b01000, 0b01100, 0b01110, 0b01100, 0b01000}};

// The four code points those glyphs answer to.
inline constexpr uint32_t CP_ARROW_LEFT  = 0x2190;
inline constexpr uint32_t CP_ARROW_UP    = 0x2191;
inline constexpr uint32_t CP_ARROW_RIGHT = 0x2192;
inline constexpr uint32_t CP_ARROW_DOWN  = 0x2193;

// ELLIPSIS: three dots in ONE column, so a "clipped here" marker costs one column rather than two.
// Keyed by U+2026 (…).
inline constexpr Glyph    GLYPH_ELLIPSIS{{0b00000, 0b00000, 0b00000, 0b00000, 0b10101}};
inline constexpr uint32_t CP_ELLIPSIS = 0x2026;

namespace detail {

// The authored table. Lowercase is not a separate design — it is folded in by the uppercase fallback.
struct Entry {
    char  c;
    Glyph g;
};

inline constexpr Entry TABLE[] = {
    // NUMBERS 0-9
    {'0', {{0b11111, 0b10001, 0b10101, 0b10001, 0b11111}}},
    {'1', {{0b01100, 0b10100, 0b00100, 0b00100, 0b11111}}},
    {'2', {{0b11110, 0b00001, 0b00110, 0b01000, 0b11111}}},
    {'3', {{0b11111, 0b00001, 0b01111, 0b00001, 0b11111}}},
    {'4', {{0b10001, 0b10001, 0b11111, 0b00001, 0b00001}}},
    {'5', {{0b11111, 0b10000, 0b11111, 0b00001, 0b11111}}},
    {'6', {{0b11111, 0b10000, 0b11111, 0b10001, 0b11111}}},
    {'7', {{0b11111, 0b00001, 0b00010, 0b00100, 0b01000}}},
    {'8', {{0b11111, 0b10001, 0b11111, 0b10001, 0b11111}}},
    {'9', {{0b11111, 0b10001, 0b11111, 0b00001, 0b11111}}},

    // UPPERCASE A-Z
    {'A', {{0b01110, 0b10001, 0b10001, 0b11111, 0b10001}}},
    {'B', {{0b11110, 0b10001, 0b11110, 0b10001, 0b11110}}},
    {'C', {{0b01111, 0b10000, 0b10000, 0b10000, 0b01111}}},
    {'D', {{0b11110, 0b10001, 0b10001, 0b10001, 0b11110}}},
    {'E', {{0b11111, 0b10000, 0b11110, 0b10000, 0b11111}}},
    {'F', {{0b11111, 0b10000, 0b11110, 0b10000, 0b10000}}},
    {'G', {{0b01111, 0b10000, 0b10111, 0b10001, 0b01111}}},
    {'H', {{0b10001, 0b10001, 0b11111, 0b10001, 0b10001}}},
    {'I', {{0b11111, 0b00100, 0b00100, 0b00100, 0b11111}}},
    {'J', {{0b00001, 0b00001, 0b10001, 0b10001, 0b01110}}},
    {'K', {{0b10001, 0b10010, 0b11100, 0b10010, 0b10001}}},
    {'L', {{0b10000, 0b10000, 0b10000, 0b10000, 0b11111}}},
    {'M', {{0b10001, 0b11011, 0b10101, 0b10101, 0b10001}}},
    {'N', {{0b10001, 0b11001, 0b10101, 0b10011, 0b10001}}},
    {'O', {{0b01110, 0b10001, 0b10001, 0b10001, 0b01110}}},
    {'P', {{0b11110, 0b10001, 0b11110, 0b10000, 0b10000}}},
    {'Q', {{0b01110, 0b10001, 0b10001, 0b10010, 0b01101}}},
    {'R', {{0b11110, 0b10001, 0b11110, 0b10010, 0b10001}}},
    {'S', {{0b01111, 0b10000, 0b01110, 0b00001, 0b11110}}},
    {'T', {{0b11111, 0b00100, 0b00100, 0b00100, 0b00100}}},
    {'U', {{0b10001, 0b10001, 0b10001, 0b10001, 0b01110}}},
    {'V', {{0b10001, 0b10001, 0b10001, 0b01010, 0b00100}}},
    {'W', {{0b10001, 0b10101, 0b10101, 0b10101, 0b01110}}},
    {'X', {{0b10001, 0b01010, 0b00100, 0b01010, 0b10001}}},
    {'Y', {{0b10001, 0b10001, 0b01110, 0b00100, 0b00100}}},
    {'Z', {{0b11111, 0b00010, 0b00100, 0b01000, 0b11111}}},

    // SPECIAL CHARACTERS
    {'_', {{0b00000, 0b00000, 0b00000, 0b00000, 0b11111}}},
    {'-', {{0b00000, 0b00000, 0b11111, 0b00000, 0b00000}}},
    {'#', {{0b01010, 0b11111, 0b01010, 0b11111, 0b01010}}},
    {'.', {{0b00000, 0b00000, 0b00000, 0b00000, 0b00100}}},
    {',', {{0b00000, 0b00000, 0b00000, 0b00100, 0b00100}}},
    {':', {{0b00000, 0b00100, 0b00000, 0b00100, 0b00000}}},
    {'/', {{0b00001, 0b00010, 0b00100, 0b01000, 0b10000}}},
    {'%', {{0b10001, 0b00010, 0b00100, 0b01000, 0b10001}}},
    {'+', {{0b00100, 0b00100, 0b11111, 0b00100, 0b00100}}},
    {'<', {{0b00010, 0b00100, 0b01000, 0b00100, 0b00010}}},
    {'>', {{0b01000, 0b00100, 0b00010, 0b00100, 0b01000}}},
    {'=', {{0b00000, 0b11111, 0b00000, 0b11111, 0b00000}}},
    {'[', {{0b00110, 0b00100, 0b00100, 0b00100, 0b00110}}},
    {'(', {{0b00010, 0b00100, 0b00100, 0b00100, 0b00010}}},
    {'!', {{0b00100, 0b00100, 0b00100, 0b00000, 0b00100}}},
    {'?', {{0b01110, 0b00010, 0b00100, 0b00000, 0b00100}}},
    {']', {{0b01100, 0b00100, 0b00100, 0b00100, 0b01100}}},
    {')', {{0b01000, 0b00100, 0b00100, 0b00100, 0b01000}}},
    {'|', {{0b00100, 0b00100, 0b00100, 0b00100, 0b00100}}},
    {'"', {{0b01010, 0b00000, 0b00000, 0b00000, 0b00000}}},
    // The "edited since it was named" marker on the SCALE screen.
    {'*', {{0b00100, 0b10101, 0b01110, 0b10101, 0b00100}}},
    {' ', {{0b00000, 0b00000, 0b00000, 0b00000, 0b00000}}},
};

// The ASCII-indexed table the draw path reads, built at compile time with the uppercase fallback.
inline constexpr std::array<Glyph, 128> build_ascii() {
    std::array<Glyph, 128> out{};
    for (auto& g : out) g = GLYPH_NONE;
    for (const Entry& e : TABLE) {
        const auto code = static_cast<unsigned char>(e.c);
        if (code < 128) out[code] = e.g;
        // Fold lowercase onto the uppercase design.
        if (e.c >= 'A' && e.c <= 'Z') out[static_cast<unsigned char>(e.c - 'A' + 'a')] = e.g;
    }
    return out;
}

}  // namespace detail

inline constexpr std::array<Glyph, 128> FONT_5X5_ASCII = detail::build_ascii();

/** The glyph for an ASCII char; blank for anything unmapped or out of range. */
inline constexpr const Glyph& glyph_for(char c) {
    const auto code = static_cast<unsigned char>(c);
    return (code < 128) ? FONT_5X5_ASCII[code] : GLYPH_NONE;
}

/**
 * The glyph for a Unicode code point — the ASCII table plus the arrows and ellipsis. The table stays
 * ASCII-indexed for the hot path (~700 glyphs a screen at 60 fps).
 */
inline constexpr const Glyph& glyph_for_codepoint(uint32_t cp) {
    if (cp < 128) return FONT_5X5_ASCII[cp];
    switch (cp) {
        case CP_ARROW_LEFT:  return GLYPH_ARROW_LEFT;
        case CP_ARROW_UP:    return GLYPH_ARROW_UP;
        case CP_ARROW_RIGHT: return GLYPH_ARROW_RIGHT;
        case CP_ARROW_DOWN:  return GLYPH_ARROW_DOWN;
        case CP_ELLIPSIS:    return GLYPH_ELLIPSIS;
        default:             return GLYPH_NONE;
    }
}

}  // namespace pt::ui
