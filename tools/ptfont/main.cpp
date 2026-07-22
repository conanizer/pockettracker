// ─── tools/ptfont — the button-font RASTERIZER, proven on the exact .otf the shell ships (conv D) ──
//
// The twin of tools/ptdecode. It compiles shell/font_raster.cpp — the ONE translation unit that pulls
// in the vendored stb_truetype implementation — and, like ptdecode/ptmapper/pttouch, links NOTHING
// else: no SDL, no pt-ui, no engine. That is an assertion, not an economy. The rasterizer is a SHELL
// facility that must depend on stb_truetype and the standard library and nothing more, so a stray
// include reaching for SDL or the engine stops THIS target linking and names the file.
//
// ⚠️ WHY IT EXISTS, beyond "a vendored dep never called is dead code" (the C1 lesson):
// `helvetica_regular.otf` is CFF/PostScript-outline OpenType — the `OTTO` sfnt flavour, NOT the
// commoner TrueType `glyf`. stb_truetype's CFF path is real but is the half most likely to regress
// silently on a version bump, and a regression there is invisible on device (a blank or tofu label
// looks like a layout bug, not a decoder bug). So this asserts, against the real file:
//   • the file really is `OTTO` (else the test is not exercising the CFF path at all);
//   • every glyph the PORTRAIT2 button labels use produces INK and a positive advance;
//   • the arrow glyphs (↑↓←→, U+2190–2193) the D-pad shows are present.
//
// Red-drive: truncate the .otf (init fails → exit 3), or bump stb_truetype to a build without CFF
// (glyphs lose their ink → the ink assertion fails and names the character). A PASS printed over zero
// glyphs is impossible: the counts are printed beside the verdict and the gate requires them non-zero.

#include "font_raster.h"

#include <cstdint>
#include <cstdio>
#include <fstream>
#include <string>
#include <vector>

namespace {

std::vector<std::uint8_t> read_file(const char* path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return {};
    return std::vector<std::uint8_t>((std::istreambuf_iterator<char>(in)),
                                     std::istreambuf_iterator<char>());
}

// One non-zero coverage byte anywhere is "has ink". A CFF path that parses but rasterizes nothing
// returns an all-zero buffer — which is the exact silent regression this whole tool exists to catch.
bool has_ink(const ptshell::RasterGlyph& g) {
    for (std::uint8_t c : g.coverage)
        if (c != 0) return true;
    return false;
}

struct Check {
    std::uint32_t cp;
    const char*   name;
};

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: ptfont <font.otf>\n");
        return 2;
    }

    const std::vector<std::uint8_t> bytes = read_file(argv[1]);
    if (bytes.size() < 4) {
        std::fprintf(stderr, "ptfont: cannot read font, or too small: %s\n", argv[1]);
        return 2;
    }

    // ── `arrows` MODE: the D-pad ARROW font (convergence D-theme, the Roboto path) ──────────────────
    //
    // The letters ("A", "L Shift", …) are Helvetica; the D-pad arrows (↑↓←→) are drawn from a SECOND
    // bundled font, because Helvetica ships no arrow glyphs (asserted below as INFORMATIONAL, and it is
    // WHY there is a second font at all). On Android those arrows came from the system SansSerif fallback
    // = Roboto; the shell bundles Roboto-Regular.ttf and blits its real glyphs through `Font::draw_text`,
    // rather than reproducing the shape. This mode GATES on exactly that: the shipped arrow font must
    // parse and its four arrow codepoints must each ink with a positive advance. A missing/wrong arrow
    // font is not fatal on device (the shell falls back to the line-drawn `draw_arrow`), but it is a
    // silent visual regression, so it is caught here on the artifact — the ptdecode/ptfont discipline.
    // Roboto is TrueType `glyf` (0x00010000), NOT OTTO, so this path deliberately does not require OTTO.
    if (argc >= 3 && std::string(argv[2]) == "arrows") {
        ptshell::FontRasterizer af;
        if (!af.init(bytes.data(), bytes.size())) {
            std::fprintf(stderr, "ptfont(arrows): FAIL — FontRasterizer::init rejected %s.\n", argv[1]);
            return 3;
        }
        const float px = 48.0f;
        const Check arrows[] = {
            {0x2191u, "UP    ↑"}, {0x2193u, "DOWN  ↓"}, {0x2190u, "LEFT  ←"}, {0x2192u, "RIGHT →"},
        };
        int  ok_count = 0;
        bool any_fail = false;
        for (const Check& c : arrows) {
            const ptshell::RasterGlyph g = af.glyph(c.cp, px);
            const bool ok = g.has_glyph && g.advance > 0 && has_ink(g);
            std::printf("  %-8s U+%04X: glyph=%d advance=%3d bitmap=%dx%d ink=%d  %s\n", c.name, c.cp,
                        g.has_glyph ? 1 : 0, g.advance, g.w, g.h, has_ink(g) ? 1 : 0, ok ? "ok" : "FAIL");
            if (ok) ++ok_count; else any_fail = true;
        }
        std::printf("\nsummary: arrows %d/4 ink+advance\n", ok_count);
        if (any_fail) {
            std::fprintf(stderr, "ptfont(arrows): FAIL — an arrow glyph is missing or blank.\n");
            return 1;
        }
        std::printf("ptfont(arrows): PASS\n");
        return 0;
    }

    // The specific-risk assertion: this MUST be a CFF OpenType file, or the test is not exercising the
    // path it was written to guard. TrueType-flavoured fonts start 0x00010000 or 'true'; CFF is 'OTTO'.
    const bool is_otto =
        bytes[0] == 'O' && bytes[1] == 'T' && bytes[2] == 'T' && bytes[3] == 'O';
    std::printf("sfnt tag: %c%c%c%c  (CFF/OTTO=%s)\n", bytes[0], bytes[1], bytes[2], bytes[3],
                is_otto ? "yes" : "NO");
    if (!is_otto) {
        std::fprintf(stderr, "ptfont: FAIL — expected a CFF (OTTO) font; the CFF path is the point.\n");
        return 3;
    }

    ptshell::FontRasterizer font;
    if (!font.init(bytes.data(), bytes.size())) {
        std::fprintf(stderr, "ptfont: FAIL — FontRasterizer::init rejected the font.\n");
        return 3;
    }

    // A size in the range the PORTRAIT2 labels actually use on a phone (x≈4..8 → 7x..11x ≈ 28..88 px).
    const float px       = 48.0f;
    const int   ascent   = font.ascent_px(px);
    const int   lineH    = font.line_height_px(px);
    std::printf("metrics @ %.0fpx: ascent=%d line_height=%d\n", px, ascent, lineH);
    if (ascent <= 0 || lineH <= 0) {
        std::fprintf(stderr, "ptfont: FAIL — non-positive vertical metrics (%d, %d).\n", ascent, lineH);
        return 3;
    }

    // Every distinct character the PORTRAIT2 labels use: "L Shift" "R Shift" "Sel" "Start" "A" "B".
    const Check ascii[] = {
        {'A', "A"}, {'B', "B"}, {'L', "L"}, {'R', "R"}, {'S', "S"}, {'s', "s"},
        {'h', "h"}, {'i', "i"}, {'f', "f"}, {'t', "t"}, {'e', "e"}, {'l', "l"},
        {'a', "a"}, {'r', "r"},
    };
    int ascii_ok = 0;
    bool ascii_fail = false;
    for (const Check& c : ascii) {
        const ptshell::RasterGlyph g = font.glyph(c.cp, px);
        const bool ok = g.has_glyph && g.advance > 0 && has_ink(g);
        std::printf("  '%s': glyph=%d advance=%3d bitmap=%dx%d ink=%d  %s\n", c.name,
                    g.has_glyph ? 1 : 0, g.advance, g.w, g.h, has_ink(g) ? 1 : 0, ok ? "ok" : "FAIL");
        if (ok) ++ascii_ok; else ascii_fail = true;
    }

    // The D-pad arrows (↑↓←→, U+2190–2193). INFORMATIONAL, NOT gated FOR THIS FONT: Helvetica has no
    // arrow glyphs, and on Android Compose drew them through SYSTEM FONT FALLBACK = Roboto. The shell now
    // bundles Roboto for exactly those four glyphs and blits them as real text; that font IS gated, by
    // `ptfont <roboto> arrows` (ctest `d-font-arrows`). So `glyph=0` here is EXPECTED and correct —
    // this font's job is the LETTERS above — and `has_ink` on a .notdef is the tofu box, which is why
    // Helvetica's own arrow codepoints must never be blitted as text.
    const Check arrows[] = {
        {0x2191u, "UP  ↑"}, {0x2193u, "DOWN ↓"}, {0x2190u, "LEFT ←"}, {0x2192u, "RIGHT →"},
    };
    int arrows_present = 0;
    for (const Check& c : arrows) {
        const ptshell::RasterGlyph g = font.glyph(c.cp, px);
        if (g.has_glyph) ++arrows_present;
        std::printf("  %-8s U+%04X: glyph=%d  (Helvetica has no arrows → shell draws these; not gated)\n",
                    c.name, c.cp, g.has_glyph ? 1 : 0);
    }

    // A blank glyph (space) must have NO ink but a POSITIVE advance — the "coverage empty, pen still
    // moves" contract font.cpp relies on to lay out multi-word labels ("L Shift").
    const ptshell::RasterGlyph sp = font.glyph(' ', px);
    const bool space_ok = sp.advance > 0 && !has_ink(sp);
    std::printf("  space: advance=%d ink=%d  %s\n", sp.advance, has_ink(sp) ? 1 : 0,
                space_ok ? "ok" : "FAIL");

    std::printf("\nsummary: ascii %d/%d, space %s, arrows present %d/4 (not gated — shell-drawn)\n",
                ascii_ok, static_cast<int>(sizeof(ascii) / sizeof(ascii[0])),
                space_ok ? "ok" : "FAIL", arrows_present);

    if (ascii_fail || !space_ok) {
        std::fprintf(stderr, "ptfont: FAIL — see the per-glyph lines above.\n");
        return 1;
    }
    std::printf("ptfont: PASS\n");
    return 0;
}
