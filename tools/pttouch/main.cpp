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
// the other half — and the header of `touch_layout.h` marks the seam. ⚠️ Covers the two LANDSCAPE
// boxes (D3) and the PORTRAIT2 skinned grid (its columns come from Compose's WEIGHT split, so the
// oracle asserts the grid tiles and pins a clean size AND a remainder-carrying one); PORTRAIT's
// two-box split joins when that mode is lit up.
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

// PORTRAIT2 is a grid, not a centred Column, so it gets its own structure oracle: all ten buttons
// present, four equal-height rows top-anchored under the padding (NOT vertically centred — the leftover
// sits at the bottom), the D-pad/face columns aligned, and the empty grid slots and padding hitting
// nothing. Reuses the generic within/disjoint/hit-centre logic; adds the grid invariants. Read against
// `VirtualControlsPortrait2` (Row 0 = wide L/R shift; rows 1–3 = a 4-column weight grid).
void check_portrait2_structure(const touch_layout::BoxRects& box, int W, int H) {
    using touch_layout::ButtonRect;
    const char* tag = "P2";

    check_eq(box.count, 10, "P2 button count");

    // Every rect inside its box, and no two overlapping.
    for (int i = 0; i < box.count; ++i) {
        const ButtonRect& r = box.r[i];
        check(r.x >= 0 && r.y >= 0 && r.x + r.w <= W && r.y + r.h <= H,
              std::string(tag) + " " + bname(r.button) + " within box");
        for (int j = i + 1; j < box.count; ++j)
            check(!overlaps(r, box.r[j]),
                  std::string(tag) + " " + bname(r.button) + "/" + bname(box.r[j].button) + " disjoint");
    }

    // Each rect's centre hits exactly that button.
    for (int i = 0; i < box.count; ++i) {
        const ButtonRect& r = box.r[i];
        Button got{};
        check(touch_layout::hit(box, r.cx(), r.cy(), got) && got == r.button,
              std::string(tag) + " hit(centre of " + bname(r.button) + ")");
    }

    // All ten buttons present, exactly once — PORTRAIT2 shows the whole set, unlike the split boxes.
    int seen[static_cast<int>(Button::COUNT)] = {0};
    for (int i = 0; i < box.count; ++i) ++seen[static_cast<int>(box.r[i].button)];
    for (int i = 0; i < static_cast<int>(Button::COUNT); ++i)
        check_eq(seen[i], 1, std::string("P2 exactly one ") + bname(static_cast<Button>(i)));

    const ButtonRect* ls = find(box, Button::L_SHIFT);
    const ButtonRect* rs = find(box, Button::R_SHIFT);
    const ButtonRect* up = find(box, Button::DPAD_UP);
    const ButtonRect* dn = find(box, Button::DPAD_DOWN);
    const ButtonRect* lf = find(box, Button::DPAD_LEFT);
    const ButtonRect* rt = find(box, Button::DPAD_RIGHT);
    const ButtonRect* a  = find(box, Button::A);
    const ButtonRect* b  = find(box, Button::B);
    const ButtonRect* se = find(box, Button::SELECT);
    const ButtonRect* st = find(box, Button::START);
    check(ls && rs && up && dn && lf && rt && a && b && se && st, "P2 all rects present");
    if (!(ls && rs && up && dn && lf && rt && a && b && se && st)) return;

    // Four rows, each one cell tall, strictly descending and evenly spaced (the four .height(cellDp)).
    check(ls->y == rs->y, "P2 row0: L/R shift share a row");
    check(up->y == b->y && b->y == a->y, "P2 row1: UP/B/A share a row");
    check(lf->y == dn->y && dn->y == rt->y, "P2 row2: LEFT/DOWN/RIGHT share a row");
    check(se->y == st->y, "P2 row3: SEL/START share a row");
    check(ls->y < up->y && up->y < lf->y && lf->y < se->y, "P2 rows run top to bottom");
    const int cell = up->h;
    check(up->y - ls->y == cell && lf->y - up->y == cell && se->y - lf->y == cell,
          "P2 rows evenly spaced by one cell height");
    check(ls->h == cell && rs->h == cell, "P2 wide buttons are one cell tall");

    // Top-anchored, NOT centred (the LEFT/RIGHT invariant that must NOT hold here): the block sits under
    // its top padding with the larger gap at the bottom.
    check(ls->y > 0 && se->y + se->h < H, "P2 grid within box vertically");
    check(ls->y < H - (se->y + se->h), "P2 grid top-anchored (bottom leftover > top padding)");

    // Columns line up down the grid: UP/DOWN/SEL share the second quarter-column, B/RIGHT/START the third.
    check(up->x == dn->x && dn->x == se->x && up->w == dn->w && dn->w == se->w, "P2 col1 aligned (UP/DOWN/SEL)");
    check(b->x == rt->x && rt->x == st->x && b->w == rt->w && rt->w == st->w, "P2 col2 aligned (B/RIGHT/START)");
    check(lf->x == ls->x, "P2 LEFT and L-shift share the left edge");

    // Face row reads B then A left→right; the D-pad row is LEFT < DOWN < RIGHT; A is the rightmost column.
    check(b->x < a->x, "P2 face row: B left of A");
    check(lf->x < dn->x && dn->x < rt->x, "P2 dpad row: LEFT < DOWN < RIGHT");
    check(a->x + a->w >= b->x + b->w && a->x + a->w >= rt->x + rt->w, "P2 A is the rightmost column");

    // Wide top buttons: L-shift left of R-shift, R-shift starts where L-shift ends, each wider than a cell.
    check(ls->x < rs->x, "P2 L-shift left of R-shift");
    check(rs->x >= ls->x + ls->w - 2, "P2 R-shift starts where L-shift ends");
    check(ls->w > up->w && rs->w > up->w, "P2 shift buttons wider than a square cell");

    // The grid's holes: row 1 slot 0 (left of UP, above LEFT) is a Spacer, and the top padding is empty.
    Button got{};
    check(!touch_layout::hit(box, lf->x + lf->w / 2, up->cy(), got), "P2 empty slot (row1 col0) hits nothing");
    check(!touch_layout::hit(box, W / 2, ls->y / 2, got), "P2 top padding hits nothing");
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

    // ── PORTRAIT2: the skinned 4-row weight grid (convergence D) ──────────────────────────────────
    // A grid, not a centred Column, so its own structure. Two sizes: one where X is a whole number so
    // every column is exact (no weight remainder), and one where it is not (the remainder loop runs).
    {
        struct P2 { int w, h; } p2sizes[] = {{1350, 1420}, {1000, 1100}};
        for (P2 s : p2sizes) check_portrait2_structure(portrait2_rects(s.w, s.h), s.w, s.h);

        // Clean pins at 1350×1420: X = 1350/135 = 10 exactly, so pad = 15, cell = 330 and every slot is
        // a round 330 with NO weight remainder — fully hand-traceable, so the pin is independent of the
        // port's own arithmetic (not "the code compared to itself").
        {
            const BoxRects G = portrait2_rects(1350, 1420);
            const ButtonRect* ls = find(G, Button::L_SHIFT);
            const ButtonRect* rs = find(G, Button::R_SHIFT);
            const ButtonRect* up = find(G, Button::DPAD_UP);
            const ButtonRect* a  = find(G, Button::A);
            const ButtonRect* lf = find(G, Button::DPAD_LEFT);
            const ButtonRect* se = find(G, Button::SELECT);
            if (ls) { check_eq(ls->x, 15, "pin P2 LSHIFT.x"); check_eq(ls->y, 15, "pin P2 LSHIFT.y");
                      check_eq(ls->w, 660, "pin P2 LSHIFT.w"); check_eq(ls->h, 330, "pin P2 LSHIFT.h"); }
            if (rs) { check_eq(rs->x, 675, "pin P2 RSHIFT.x"); check_eq(rs->w, 660, "pin P2 RSHIFT.w"); }
            if (up) { check_eq(up->x, 345, "pin P2 UP.x"); check_eq(up->y, 345, "pin P2 UP.y");
                      check_eq(up->w, 330, "pin P2 UP.w"); }
            if (a)  { check_eq(a->x, 1005, "pin P2 A.x"); check_eq(a->y, 345, "pin P2 A.y"); }
            if (lf) { check_eq(lf->x, 15, "pin P2 LEFT.x"); check_eq(lf->y, 675, "pin P2 LEFT.y"); }
            if (se) { check_eq(se->x, 345, "pin P2 SELECT.x"); check_eq(se->y, 1005, "pin P2 SELECT.y"); }
        }

        // Messy pins at 1000×1100: X = 7.407…, so pad = 11, cell = 244, content = 978, and the 4-split
        // leaves 978 − 4·245 = −2 px that the FIRST two columns absorb → widths {244,244,245,245}. This
        // is what the clean size cannot exercise: a wrong remainder direction swaps UP.w and A.w.
        {
            const BoxRects G = portrait2_rects(1000, 1100);
            const ButtonRect* ls = find(G, Button::L_SHIFT);
            const ButtonRect* up = find(G, Button::DPAD_UP);
            const ButtonRect* a  = find(G, Button::A);
            if (ls) { check_eq(ls->x, 11, "pin P2/messy LSHIFT.x (pad)"); }
            if (up) { check_eq(up->x, 255, "pin P2/messy UP.x"); check_eq(up->w, 244, "pin P2/messy UP.w"); }
            if (a)  { check_eq(a->x, 744, "pin P2/messy A.x"); check_eq(a->w, 245, "pin P2/messy A.w"); }
        }
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
