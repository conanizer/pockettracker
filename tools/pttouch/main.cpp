// pttouch — convergence D1/D3 conformance harness for the touch-layout SIZE arithmetic.
//
// The touch skin is the one piece of the Android app with no C++ twin (convergence plan §6). Its
// hit-rect LAYOUT is pure arithmetic and was recorded from the real Kotlin — `TouchLayoutMetrics`,
// which `VirtualControls.kt`'s four composables call — into `testdata/units/touch-layout.txt` during
// Phase B2, precisely so the port could be checked rather than trusted. This tool is the checker:
//
//   * B2's TouchLayoutGoldenTest wrote each case's INPUTS and Kotlin's computed OUTPUTS to the golden
//     (ints decimal, floats as raw binary32 bits — nothing passes by being close);
//   * this re-parses each line's inputs, recomputes the right-hand side through the C++ port
//     (native/ui/touch_layout.h), and byte-compares the field string it produces.
//
// Same contract as s3-units / s5-consumer / p3-input: the golden is authoritative, this consumes it,
// and a byte that differs exits non-zero. It deliberately does NOT regenerate the file — only the
// Kotlin recorder does that (missing → generate). So a missing or empty golden is a hard ERROR here,
// not a vacuous pass: deleting the file cannot make this certify anything.
//
// ⚠️ SIZES ONLY, NOT POSITIONS — the golden's own header says why, and `touch_layout.h` repeats it:
// where each button LANDS is a Compose measure/layout result no JVM test could record. A port that
// matches every line here can still stack the D-pad wrong; that hole is closed by eyes on a device in
// a later D increment, not by this tool.
//
// Build + run via the tools/ CMake project — this is the `d-touch-layout` ctest, run by CI on every
// push (see tools/CMakeLists.txt and tools/pttouch/README.md):
//   cmake -S tools -B tools/build -DCMAKE_BUILD_TYPE=Release
//   cmake --build tools/build --config Release
//   ctest --test-dir tools/build -R d-touch-layout --output-on-failure -C Release
// Exit 0 = all green, 1 = any mismatch (or a bad/missing golden).

#include "../../native/ui/touch_layout.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>  // std::strtof
#include <cstring>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

using namespace pt::ui;

// House float format: "0x" + the 8 uppercase hex digits of the binary32 bits, matching Kotlin's
// f32(v) = "0x%08X".format(v.toRawBits()) and every other tool in this directory.
static std::string f32(float v) {
    uint32_t b;
    std::memcpy(&b, &v, sizeof b);
    char buf[16];
    std::snprintf(buf, sizeof buf, "0x%08X", b);
    return buf;
}
static std::string i(int v) { return std::to_string(v); }

// ─── the recomputed field strings, formatted EXACTLY as TouchLayoutGoldenTest emits them ─────────

static std::string rhs_left(int w, int h, float d) {
    const touch_layout::Left m = touch_layout::left(w, h, d);
    return "x=" + i(m.x) + " btn=" + i(m.button_size) +
           " lw=" + i(m.l_button_width) + " lh=" + i(m.l_button_height) +
           " selw=" + i(m.select_width) + " selh=" + i(m.select_height) +
           " small=" + i(m.small_spacer) + " large=" + i(m.large_spacer) + " med=" + i(m.medium_spacer_width) +
           " fmain=" + f32(m.main_font_sp) + " ftrig=" + f32(m.trigger_font_sp) + " fsmall=" + f32(m.small_font_sp);
}

static std::string rhs_right(int w, int h, float d) {
    const touch_layout::Right m = touch_layout::right(w, h, d);
    return "x=" + i(m.x) + " btn=" + i(m.button_size) +
           " rw=" + i(m.r_button_width) + " rh=" + i(m.r_button_height) +
           " startw=" + i(m.start_width) + " starth=" + i(m.start_height) +
           " small=" + i(m.small_spacer) + " med=" + i(m.medium_spacer) + " large=" + i(m.large_spacer) +
           " lsp=" + i(m.left_spacer) + " rsp=" + i(m.right_spacer) +
           " fmain=" + f32(m.main_font_sp) + " ftrig=" + f32(m.trigger_font_sp) + " fsmall=" + f32(m.small_font_sp);
}

static std::string rhs_portrait(int w, int h) {
    const touch_layout::Portrait m = touch_layout::portrait(w, h);
    return "x=" + i(m.x) + " boxw=" + i(m.box_width) + " boxh=" + i(m.box_height);
}

static std::string rhs_portrait2(int w, int h, float d) {
    const touch_layout::Portrait2 m = touch_layout::portrait2(w, h, d);
    return "x=" + f32(m.x) + " cell=" + f32(m.cell_dp) +
           " pad=" + f32(m.padding_dp) + " large=" + f32(m.large_sp) + " small=" + f32(m.small_sp) +
           " sqoff=" + f32(m.sq_off_x_dp) + " wideoff=" + f32(m.wide_off_x_dp) + " offy=" + f32(m.off_y_dp) +
           " pressed=" + f32(m.pressed_dp);
}

// ─── input parsing ───────────────────────────────────────────────────────────────────────────────
// A data line is `<KIND> w=<int> h=<int> [d=<density>] => <field string>`. The part before " => " is
// the INPUT (the query); the part after is the golden's answer. We recompute the answer from the
// parsed inputs and compare — the input echo is never compared to itself.

static bool token_value(const std::string& lhs, const std::string& key, std::string& out) {
    std::istringstream is(lhs);
    std::string tok;
    while (is >> tok) {
        if (tok.rfind(key, 0) == 0) {  // starts with key (e.g. "w=")
            out = tok.substr(key.size());
            return true;
        }
    }
    return false;
}

int main(int argc, char** argv) {
    if (argc < 2) {
        std::cerr << "usage: pttouch <path/to/touch-layout.txt>\n";
        return 2;
    }
    const char* path = argv[1];
    std::ifstream in(path);
    if (!in) {
        std::cerr << "pttouch: cannot open golden '" << path << "' — it is committed, not generated "
                     "here; regenerate it from Kotlin (TouchLayoutGoldenTest), do not delete it.\n";
        return 2;
    }

    int         checked  = 0;
    int         failures = 0;
    int         lineNo   = 0;
    std::string line;
    while (std::getline(in, line)) {
        ++lineNo;
        if (!line.empty() && line.back() == '\r') line.pop_back();  // tolerate CRLF checkout
        if (line.empty() || line[0] == '#') continue;

        const size_t arrow = line.find(" => ");
        if (arrow == std::string::npos) {
            std::cerr << "pttouch: line " << lineNo << " has no ' => ': " << line << "\n";
            return 2;
        }
        const std::string lhs      = line.substr(0, arrow);
        const std::string expected = line.substr(arrow + 4);

        // KIND is the first whitespace-delimited token.
        std::istringstream ls(lhs);
        std::string        kind;
        ls >> kind;

        std::string wStr, hStr, dStr;
        if (!token_value(lhs, "w=", wStr) || !token_value(lhs, "h=", hStr)) {
            std::cerr << "pttouch: line " << lineNo << " missing w=/h=: " << line << "\n";
            return 2;
        }
        const int w = std::stoi(wStr);
        const int h = std::stoi(hStr);
        // Densities are exact in binary32 (golden guarantee), so strtof round-trips the intended bits.
        const float d = token_value(lhs, "d=", dStr) ? std::strtof(dStr.c_str(), nullptr) : 0.0f;

        std::string actual;
        if (kind == "LEFT") {
            actual = rhs_left(w, h, d);
        } else if (kind == "RIGHT") {
            actual = rhs_right(w, h, d);
        } else if (kind == "PORTRAIT") {
            actual = rhs_portrait(w, h);
        } else if (kind == "PORTRAIT2") {
            actual = rhs_portrait2(w, h, d);
        } else {
            std::cerr << "pttouch: line " << lineNo << " unknown KIND '" << kind << "'\n";
            return 2;
        }

        ++checked;
        if (actual != expected) {
            ++failures;
            std::cerr << "MISMATCH line " << lineNo << " (" << lhs << ")\n"
                      << "  expected: " << expected << "\n"
                      << "  actual:   " << actual << "\n";
        }
    }

    if (checked == 0) {
        std::cerr << "pttouch: golden '" << path << "' held no data lines — refusing to pass on an "
                     "empty check.\n";
        return 2;
    }

    std::cout << "checked " << checked << " touch-layout case(s)\n";
    if (failures == 0) {
        std::cout << "ALL GREEN\n";
        return 0;
    }
    std::cout << failures << " FAILED\n";
    return 1;
}
