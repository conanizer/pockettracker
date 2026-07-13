#pragma once

// ─── The 5×5 bitmap font ─────────────────────────────────────────────────────────────────────────
//
// A verbatim port of app/.../ui/BitmapFont5x5.kt — the same glyphs the Android app has drawn since
// it was born, converted from quinine_five.ttf. Every character is 5 px wide and 5 px tall, one byte
// per row, MSB = leftmost pixel (bit 4 .. bit 0).
//
// The Kotlin side keeps two views of this table: a Map<Char, ByteArray> and an ASCII-indexed array
// built from it (`FONT_5X5_ASCII`) for the hot draw path, with the uppercase fallback baked in. Here
// there is only the array — a 128-entry table indexed by char code, uppercase folded in at build
// time. The map view existed to be authored by hand and to hold the four non-ASCII arrows; those
// arrows get explicit names below (GLYPH_ARROW_UP etc.) and the canvas maps them itself.
//
// Font parity with Android is a HARD requirement, not a nicety: the port plan's acceptance test for
// every screen is a 1× pixel diff against an Android screenshot (linux-port-plan §4.7), and both
// sides must therefore rasterize from the identical bits.

#include <array>
#include <cstdint>

namespace pt::ui {

// One glyph: 5 rows, MSB-first (bit 4 = leftmost of the 5 columns).
using Glyph = std::array<uint8_t, 5>;

// A glyph of all-zero rows is "not mapped" — indistinguishable from a blank, which is exactly what
// an unmapped character should render as anyway. Kotlin draws an outline square for the unmapped
// case; that only ever fired for characters no screen actually uses, so it is not reproduced.
inline constexpr Glyph GLYPH_NONE{{0, 0, 0, 0, 0}};

// The arrows live outside ASCII (Kotlin keys them by the literal '↑' '↓' '←' '→'), and a `char` cannot
// index them. They are keyed by CODE POINT instead, and `Canvas::draw_text` decodes UTF-8 to find them
// — see glyph_for_codepoint below.
inline constexpr Glyph GLYPH_ARROW_UP{{0b00000, 0b00100, 0b01110, 0b11111, 0b00000}};
inline constexpr Glyph GLYPH_ARROW_DOWN{{0b00000, 0b11111, 0b01110, 0b00100, 0b00000}};
inline constexpr Glyph GLYPH_ARROW_LEFT{{0b00010, 0b00110, 0b01110, 0b00110, 0b00010}};
inline constexpr Glyph GLYPH_ARROW_RIGHT{{0b01000, 0b01100, 0b01110, 0b01100, 0b01000}};

// The four code points those glyphs answer to.
inline constexpr uint32_t CP_ARROW_LEFT  = 0x2190;
inline constexpr uint32_t CP_ARROW_UP    = 0x2191;
inline constexpr uint32_t CP_ARROW_RIGHT = 0x2192;
inline constexpr uint32_t CP_ARROW_DOWN  = 0x2193;

namespace detail {

// The authored table, in the same order as BitmapFont5x5.kt. Lowercase is NOT a separate design in
// the Kotlin font — every lowercase entry there is byte-identical to its uppercase twin — so it is
// folded in by the uppercase fallback below rather than duplicated here.
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
    {' ', {{0b00000, 0b00000, 0b00000, 0b00000, 0b00000}}},
};

// The ASCII-indexed table the draw path actually reads, built at compile time — the C++ twin of
// Kotlin's FONT_5X5_ASCII, uppercase fallback and all.
inline constexpr std::array<Glyph, 128> build_ascii() {
    std::array<Glyph, 128> out{};
    for (auto& g : out) g = GLYPH_NONE;
    for (const Entry& e : TABLE) {
        const auto code = static_cast<unsigned char>(e.c);
        if (code < 128) out[code] = e.g;
        // Fold lowercase onto the uppercase design (Kotlin: FONT_5X5[c.uppercaseChar()]).
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
 * The glyph for a Unicode code point — the ASCII table, plus the four arrows.
 *
 * Kotlin's font is a `Map<Char, ByteArray>` and so takes '→' as a key without ceremony. The C++ table
 * is ASCII-indexed for the hot path (a full screen is ~700 glyphs × 60 fps), so the arrows are a
 * separate arm rather than four holes punched in a 8192-entry array.
 */
inline constexpr const Glyph& glyph_for_codepoint(uint32_t cp) {
    if (cp < 128) return FONT_5X5_ASCII[cp];
    switch (cp) {
        case CP_ARROW_LEFT:  return GLYPH_ARROW_LEFT;
        case CP_ARROW_UP:    return GLYPH_ARROW_UP;
        case CP_ARROW_RIGHT: return GLYPH_ARROW_RIGHT;
        case CP_ARROW_DOWN:  return GLYPH_ARROW_DOWN;
        default:             return GLYPH_NONE;
    }
}

}  // namespace pt::ui
