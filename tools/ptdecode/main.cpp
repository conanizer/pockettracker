// ptdecode — convergence D2 harness for the PNG decoder (shell/image.cpp → vendored stb_image).
//
// Phase D vendors a PNG reader into the SHELL layer for the touch skin, the CRT overlays and the
// theme PNGs. A dependency vendored but never called is dead code, so this proves the decoder
// actually decodes — the same discipline C1 applied to SDL (assert it is in the .so, not merely
// that the build went green).
//
// ── The independent invariant ──────────────────────────────────────────────────────────────────
// The fixtures under testdata/images/ were produced with GDI+ (System.Drawing) — an encoder wholly
// independent of BOTH stb_image AND ptshot's hand-rolled PNG writer. PNG stores STRAIGHT (non-
// premultiplied) alpha and stb_image applies no gamma on the 8-bit path, so a decoded pixel equals
// exactly what the generator drew. The EXPECTED values here are the generator's own formulas,
// hardcoded — not read back from a golden — so a broken decoder cannot regenerate its way to green,
// and DELETING a fixture is a hard error (exit 2), never a vacuous pass. How the fixtures were made:
// testdata/images/make-image-fixtures.ps1 (the generator) and testdata/README.md §4.
//
//   * rgb_3x2.png     PNG colour type 2 (RGB, no alpha)      — geometry + channel order
//   * rgba_2x2.png    PNG colour type 6 (RGBA)               — the alpha path, varied alpha
//   * gradient_16x16  PNG colour type 6, real zlib + adaptive filters — inflate + un-filter, the
//                     paths ptshot's stored/filter-0 writer categorically CANNOT produce, so no
//                     shared corruption is possible with any stored-deflate writer.
//
// Every fixture is decoded BOTH from a file (decode_png_file) and from memory (decode_png), and the
// two must agree — the memory path is the one the D7 Android asset seam will feed via SDL_RWFromFile.
//
// It links NOTHING but shell/image.cpp + the standard library (no SDL, no pt-ui, no engine) — the
// standing proof that the decoder is as self-contained as D1 claims when it puts image decoding in
// the shell and keeps the canvas at four primitives.
//
//   cmake -S tools -B tools/build -DCMAKE_BUILD_TYPE=Release
//   cmake --build tools/build --config Release
//   ctest --test-dir tools/build -R d-image-decode --output-on-failure -C Release
// Exit 0 = all green, 1 = a wrong/failed decode, 2 = a usage or missing/unreadable-fixture error.

#include "image.h"  // shell/image.h, via the -I../shell include dir in tools/CMakeLists.txt

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

namespace {

int g_checks = 0;
int g_fails  = 0;

void check(bool cond, const std::string& what) {
    ++g_checks;
    if (!cond) {
        ++g_fails;
        std::cerr << "FAIL: " << what << "\n";
    }
}

void check_eq(int got, int want, const std::string& what) {
    ++g_checks;
    if (got != want) {
        ++g_fails;
        std::cerr << "FAIL: " << what << " (got " << got << " want " << want << ")\n";
    }
}

void check_px(std::uint32_t got, std::uint32_t want, const std::string& what) {
    ++g_checks;
    if (got != want) {
        ++g_fails;
        char buf[64];
        std::snprintf(buf, sizeof buf, "got 0x%08X want 0x%08X", got, want);
        std::cerr << "FAIL: " << what << " (" << buf << ")\n";
    }
}

// A harness error (fixture missing/unreadable, bad usage) — distinct from a decode failure, which is
// a real test FAIL. Exit 2 so that deleting the fixtures can never masquerade as a pass.
[[noreturn]] void die(const std::string& msg) {
    std::cerr << "ptdecode: " << msg << "\n";
    std::exit(2);
}

std::uint32_t argb(int a, int r, int g, int b) {
    return (static_cast<std::uint32_t>(a) << 24) | (static_cast<std::uint32_t>(r) << 16) |
           (static_cast<std::uint32_t>(g) << 8)  |  static_cast<std::uint32_t>(b);
}

std::vector<std::uint8_t> read_file(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) die("cannot open fixture: " + path);
    f.seekg(0, std::ios::end);
    const std::streamoff n = f.tellg();
    f.seekg(0, std::ios::beg);
    if (n <= 0) die("empty or unseekable fixture: " + path);
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(n));
    f.read(reinterpret_cast<char*>(bytes.data()), n);
    if (!f || f.gcount() != n) die("short read on fixture: " + path);
    return bytes;
}

// Decode a fixture both ways, assert it succeeded, the dimensions, and that memory == file. Returns
// the decoded image for the caller's per-pixel assertions.
ptshell::Image load_both(const std::string& dir, const std::string& name, int w, int h) {
    const std::string         path  = dir + "/" + name;
    const std::vector<std::uint8_t> bytes = read_file(path);          // exit 2 if it is not there

    const ptshell::Image mem  = ptshell::decode_png(bytes.data(), bytes.size());
    const ptshell::Image file = ptshell::decode_png_file(path);

    check(mem.ok(),  name + ": decodes from memory");
    check(file.ok(), name + ": decodes from file");
    check_eq(mem.width,  w, name + ": width");
    check_eq(mem.height, h, name + ": height");
    check(mem.pixels == file.pixels, name + ": memory and file decode identically");

    if (mem.ok()) {
        std::cout << "ok    " << name << "  " << mem.width << "x" << mem.height << "  ("
                  << mem.pixels.size() << " px, mem==file)\n";
    }
    return mem;
}

// px(img,x,y) with bounds already asserted by the width/height checks above.
std::uint32_t px(const ptshell::Image& img, int x, int y) {
    if (!img.ok() || x < 0 || y < 0 || x >= img.width || y >= img.height) return 0;
    return img.pixels[static_cast<std::size_t>(y) * img.width + x];
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::cerr << "usage: ptdecode <images-dir>\n";
        return 2;
    }
    const std::string dir = argv[1];

    // ── rgb_3x2.png : colour type 2, R=20+30x+3y G=50+40y+5x B=210-25x-15y, opaque ──────────────
    {
        const ptshell::Image img = load_both(dir, "rgb_3x2.png", 3, 2);
        for (int y = 0; y < 2; ++y)
            for (int x = 0; x < 3; ++x)
                check_px(px(img, x, y),
                         argb(255, 20 + 30 * x + 3 * y, 50 + 40 * y + 5 * x, 210 - 25 * x - 15 * y),
                         "rgb_3x2 (" + std::to_string(x) + "," + std::to_string(y) + ")");
    }

    // ── rgba_2x2.png : colour type 6, distinct RGBA with alpha in {255,170,85,0} ─────────────────
    {
        const ptshell::Image img = load_both(dir, "rgba_2x2.png", 2, 2);
        check_px(px(img, 0, 0), argb(255, 200, 10, 20),  "rgba_2x2 (0,0)");
        check_px(px(img, 1, 0), argb(170, 30, 180, 40),  "rgba_2x2 (1,0)");
        check_px(px(img, 0, 1), argb(85,  50, 60, 200),  "rgba_2x2 (0,1)");
        check_px(px(img, 1, 1), argb(0,   70, 80, 90),   "rgba_2x2 (1,1)");
    }

    // ── gradient_16x16.png : ARGB(255, x*16, y*16, (x+y)*8), every pixel — inflate + un-filter ───
    {
        const ptshell::Image img = load_both(dir, "gradient_16x16.png", 16, 16);
        for (int y = 0; y < 16; ++y)
            for (int x = 0; x < 16; ++x)
                check_px(px(img, x, y), argb(255, x * 16, y * 16, (x + y) * 8),
                         "gradient (" + std::to_string(x) + "," + std::to_string(y) + ")");
    }

    // ── Negative controls: a decode that SHOULD fail must return !ok(), not crash and not "succeed".
    //    A test whose pass is the absence of a failure cannot tell a working decoder from a broken
    //    instrument — so prove the failures fail, right here.
    {
        const std::uint8_t not_png[16] = {'n', 'o', 't', ' ', 'a', ' ', 'P', 'N',
                                          'G', 0,   1,   2,   3,   4,   5,   6};
        check(!ptshell::decode_png(not_png, sizeof not_png).ok(), "neg: non-PNG bytes rejected");
        check(!ptshell::decode_png(nullptr, 0).ok(),              "neg: null buffer rejected");
        check(!ptshell::decode_png(not_png, 0).ok(),              "neg: zero length rejected");
        check(!ptshell::decode_png_file(dir + "/does_not_exist.png").ok(),
              "neg: missing file rejected");

        // A truncated real PNG (first 24 bytes of a valid one) must fail cleanly, not decode garbage.
        const std::vector<std::uint8_t> whole = read_file(dir + "/rgb_3x2.png");
        const std::vector<std::uint8_t> cut(whole.begin(), whole.begin() + 24);
        check(!ptshell::decode_png(cut.data(), cut.size()).ok(), "neg: truncated PNG rejected");
    }

    std::cout << "checked " << g_checks << " decode assertion(s)\n";
    if (g_fails == 0) {
        std::cout << "ALL GREEN\n";
        return 0;
    }
    std::cout << g_fails << " FAILED\n";
    return 1;
}
