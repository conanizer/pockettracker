// pttouch — convergence D1/D3 conformance harness for the touch layout. TWO modes, two contracts,
// because the touch layout is two computations with two DIFFERENT kinds of truth behind them.
//
// The touch skin is the one piece of the Android app with no C++ twin (convergence plan §6).
//
//   `pttouch <golden>`      — the SIZES (D1). Golden-backed, and this is a strict conformance check.
//   `pttouch --positions`   — the POSITIONS (D3). Hand-written ORACLE, no golden and there cannot be
//                             one; a weaker claim, and it says so.
//
// ── SIZES (`pttouch <golden>`) ────────────────────────────────────────────────────────────────────
//
// The hit-rect LAYOUT's SIZE half is pure arithmetic and was recorded from the real Kotlin —
// `TouchLayoutMetrics`, which `VirtualControls.kt`'s four composables call — into
// `testdata/units/touch-layout.txt` during Phase B2, precisely so the port could be checked rather
// than trusted. This mode is the checker:
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
// ── POSITIONS (`pttouch --positions`) ─────────────────────────────────────────────────────────────
//
// Where each button LANDS is a Compose measure/layout result no JVM test could record — so there is
// no golden and there never can be. `touch_layout.h`'s `left_rects`/`right_rects` PORT the arrangement
// by reading the composables directly, and this mode is a HAND-WRITTEN ORACLE over them, exactly like
// `ptmapper` and `ptdispatch`: it encodes what the author believes Compose does, having read
// `VirtualControls.kt`, and asserts the STRUCTURE the arrangement must have (the D-pad is a cross,
// A/B are a diagonal, the block is centred, nothing overlaps, a tap in a rect hits that button and a
// tap in the cross-hole hits nothing) plus a handful of exact pinned coordinates so an arithmetic
// drift prints its number. It is a weaker claim than the golden modes make — the eyes on a device are
// the other half — and the header of `touch_layout.h` marks the seam. ⚠️ D3 covers the two LANDSCAPE
// boxes; PORTRAIT/PORTRAIT2 join when those modes are lit up.
//
// Build + run via the tools/ CMake project — the `d-touch-layout` and `d-touch-positions` ctests, run
// by CI on every push (see tools/CMakeLists.txt and tools/pttouch/README.md):
//   cmake -S tools -B tools/build -DCMAKE_BUILD_TYPE=Release
//   cmake --build tools/build --config Release
//   ctest --test-dir tools/build -R d-touch --output-on-failure -C Release
// Exit 0 = all green, 1 = any mismatch/assertion, 2 = a usage or bad/missing-golden error.

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

// ─── POSITIONS: the hand-written oracle (convergence D3) ──────────────────────────────────────────
//
// No golden — Compose positions are unrecordable — so this is assertions, the ptmapper/ptdispatch
// pattern. It asserts the STRUCTURE the arrangement must have and pins a few exact coordinates.

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

// ⭐ The number beside the verdict — a drift in the arithmetic prints what it became, not just "no".
void check_eq(int got, int want, const std::string& what) {
    ++g_checks;
    if (got != want) {
        ++g_fails;
        std::cerr << "FAIL: " << what << " — got " << got << ", want " << want << "\n";
    }
}

const char* bname(Button b) {
    switch (b) {
        case Button::DPAD_UP:    return "UP";
        case Button::DPAD_DOWN:  return "DOWN";
        case Button::DPAD_LEFT:  return "LEFT";
        case Button::DPAD_RIGHT: return "RIGHT";
        case Button::A:          return "A";
        case Button::B:          return "B";
        case Button::L_SHIFT:    return "L";
        case Button::R_SHIFT:    return "R";
        case Button::SELECT:     return "SELECT";
        case Button::START:      return "START";
        case Button::COUNT:      break;
    }
    return "?";
}

const touch_layout::ButtonRect* find(const touch_layout::BoxRects& box, Button b) {
    for (int i = 0; i < box.count; ++i)
        if (box.r[i].button == b) return &box.r[i];
    return nullptr;
}

bool overlaps(const touch_layout::ButtonRect& a, const touch_layout::ButtonRect& b) {
    return !(a.x + a.w <= b.x || b.x + b.w <= a.x || a.y + a.h <= b.y || b.y + b.h <= a.y);
}

// The properties that must hold for ANY box size that fits — run against every size, so a regression
// at one resolution cannot hide behind a pin at another.
void check_box_structure(const touch_layout::BoxRects& box, int W, int H, const char* tag) {
    using touch_layout::ButtonRect;

    // Every rect inside its box, and no two overlapping.
    for (int i = 0; i < box.count; ++i) {
        const ButtonRect& r = box.r[i];
        check(r.x >= 0 && r.y >= 0 && r.x + r.w <= W && r.y + r.h <= H,
              std::string(tag) + " " + bname(r.button) + " within box");
        for (int j = i + 1; j < box.count; ++j)
            check(!overlaps(r, box.r[j]),
                  std::string(tag) + " " + bname(r.button) + "/" + bname(box.r[j].button) + " disjoint");
    }

    // The tap → button mapping: the centre of each rect hits exactly that button.
    for (int i = 0; i < box.count; ++i) {
        const ButtonRect& r = box.r[i];
        Button got{};
        const bool did = touch_layout::hit(box, r.cx(), r.cy(), got);
        check(did && got == r.button, std::string(tag) + " hit(centre of " + bname(r.button) + ")");
    }

    // A tap outside every rect hits nothing.
    Button dummy{};
    check(!touch_layout::hit(box, -1, -1, dummy), std::string(tag) + " miss above-left");
    check(!touch_layout::hit(box, W + 5, H + 5, dummy), std::string(tag) + " miss below-right");

    // The block is vertically centred: the top of the highest button and the bottom of the lowest are
    // symmetric about H/2 (the top and bottom Column spacers are equal, so the buttons are centred too).
    int minTop = H, maxBot = 0;
    for (int i = 0; i < box.count; ++i) {
        minTop = std::min(minTop, box.r[i].y);
        maxBot = std::max(maxBot, box.r[i].y + box.r[i].h);
    }
    check(std::abs((minTop + maxBot) - H) <= 2, std::string(tag) + " block vertically centred");
}

int run_positions() {
    using namespace touch_layout;

    // Two sizes: the Xiaomi's landscape side panel (~716×1220, the device this lights up on) and a
    // smaller one, so the structure is proven to hold across scales rather than at one lucky size.
    struct Size { int w, h; } sizes[] = {{716, 1220}, {300, 760}};

    for (Size s : sizes) {
        const BoxRects L = left_rects(s.w, s.h);
        const BoxRects R = right_rects(s.w, s.h);

        // ── Membership: the two boxes partition the ten buttons, once each ──
        check_eq(L.count, 6, "LEFT button count");
        check_eq(R.count, 4, "RIGHT button count");
        int seen[static_cast<int>(Button::COUNT)] = {0};
        for (int i = 0; i < L.count; ++i) ++seen[static_cast<int>(L.r[i].button)];
        for (int i = 0; i < R.count; ++i) ++seen[static_cast<int>(R.r[i].button)];
        for (int i = 0; i < static_cast<int>(Button::COUNT); ++i)
            check_eq(seen[i], 1, std::string("exactly one ") + bname(static_cast<Button>(i)));

        // ── LEFT: the D-pad is a cross, L above it, SELECT below ──
        const ButtonRect* up   = find(L, Button::DPAD_UP);
        const ButtonRect* down = find(L, Button::DPAD_DOWN);
        const ButtonRect* lft  = find(L, Button::DPAD_LEFT);
        const ButtonRect* rgt  = find(L, Button::DPAD_RIGHT);
        const ButtonRect* lsh  = find(L, Button::L_SHIFT);
        const ButtonRect* sel  = find(L, Button::SELECT);
        check(up && down && lft && rgt && lsh && sel, "LEFT rects present");
        if (up && down && lft && rgt && lsh && sel) {
            check(lft->cy() == rgt->cy(), "cross: LEFT and RIGHT share a row");
            check(up->cy() < lft->cy() && lft->cy() < down->cy(), "cross: UP above the row above DOWN");
            check(up->cx() == down->cx(), "cross: UP and DOWN share a column");
            check(lft->cx() < up->cx() && up->cx() < rgt->cx(), "cross: UP between LEFT and RIGHT");
            check(up->cx() == (lft->cx() + rgt->cx()) / 2, "cross: UP centred over the LEFT/RIGHT gap");
            check(up->y + up->h == lft->y, "cross: UP flush against the middle row");
            check(lft->y + lft->h == down->y, "cross: middle row flush against DOWN");
            check(lsh->y + lsh->h <= up->y, "LEFT: L above the D-pad");
            check(sel->y >= down->y + down->h, "LEFT: SELECT below the D-pad");
        }

        // ── RIGHT: A and B on a diagonal (A upper-right, B lower-left), R above, START below ──
        const ButtonRect* a   = find(R, Button::A);
        const ButtonRect* b    = find(R, Button::B);
        const ButtonRect* rsh = find(R, Button::R_SHIFT);
        const ButtonRect* st  = find(R, Button::START);
        check(a && b && rsh && st, "RIGHT rects present");
        if (a && b && rsh && st) {
            check(a->cy() < b->cy(), "RIGHT: A above B");
            check(a->cx() > b->cx(), "RIGHT: A right of B");
            check(rsh->y + rsh->h <= a->y, "RIGHT: R above A");
            check(st->y >= b->y + b->h, "RIGHT: START below B");
        }

        // ── The cross-hole: a tap dead-centre of the D-pad (the medium spacer, over no button) misses ──
        if (up && lft) {
            Button got{};
            check(!hit(L, up->cx(), lft->cy(), got), "LEFT: cross-hole hits nothing");
        }

        check_box_structure(L, s.w, s.h, "LEFT");
        check_box_structure(R, s.w, s.h, "RIGHT");
    }

    // ── Exact pins at 716×1220 (the Xiaomi landscape panel), hand-derived from the Compose arrangement.
    //    These are the printed numbers: any drift in the size arithmetic OR the arrangement names itself.
    {
        const BoxRects L = left_rects(716, 1220);
        const ButtonRect* up  = find(L, Button::DPAD_UP);
        const ButtonRect* lft = find(L, Button::DPAD_LEFT);
        const ButtonRect* rgt = find(L, Button::DPAD_RIGHT);
        const ButtonRect* sel = find(L, Button::SELECT);
        if (up)  { check_eq(up->x, 253, "pin LEFT UP.x");  check_eq(up->y, 305, "pin LEFT UP.y");
                   check_eq(up->w, 210, "pin LEFT UP.w");  check_eq(up->h, 210, "pin LEFT UP.h"); }
        if (lft) { check_eq(lft->x, 43,  "pin LEFT LEFT.x"); check_eq(lft->y, 515, "pin LEFT LEFT.y"); }
        if (rgt) { check_eq(rgt->x, 463, "pin LEFT RIGHT.x"); check_eq(rgt->y, 515, "pin LEFT RIGHT.y"); }
        if (sel) { check_eq(sel->x, 421, "pin LEFT SELECT.x"); check_eq(sel->y, 977, "pin LEFT SELECT.y");
                   check_eq(sel->w, 252, "pin LEFT SELECT.w"); check_eq(sel->h, 126, "pin LEFT SELECT.h"); }

        const BoxRects R = right_rects(716, 1220);
        const ButtonRect* a  = find(R, Button::A);
        const ButtonRect* b  = find(R, Button::B);
        const ButtonRect* st = find(R, Button::START);
        if (a) { check_eq(a->x, 358, "pin RIGHT A.x"); check_eq(a->y, 410, "pin RIGHT A.y"); }
        if (b) { check_eq(b->x, 148, "pin RIGHT B.x"); check_eq(b->y, 620, "pin RIGHT B.y"); }
        if (st) { check_eq(st->x, 43, "pin RIGHT START.x"); check_eq(st->y, 977, "pin RIGHT START.y"); }
    }

    std::cout << "checked " << g_checks << " position assertion(s)\n";
    if (g_fails == 0) {
        std::cout << "ALL GREEN\n";
        return 0;
    }
    std::cout << g_fails << " FAILED\n";
    return 1;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::cerr << "usage: pttouch <path/to/touch-layout.txt>   (sizes, golden-backed)\n"
                     "       pttouch --positions                  (positions, hand-written oracle)\n";
        return 2;
    }
    if (std::string(argv[1]) == "--positions") return run_positions();

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
