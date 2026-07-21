#include "sdl-touch.h"

#include "sdl-input.h"

#include "ui/font5x5.h"

#include <algorithm>
#include <cstdio>

namespace tl = pt::ui::touch_layout;
using pt::ui::Glyph;
using pt::ui::glyph_for_codepoint;

namespace {

// The panels only appear where the letterbox bar is at least this wide — narrower than a finger-sized
// box and there is nothing usable to draw. The value is DeviceAdapter.MIN_BUTTON_PANEL_PX.
constexpr int MIN_PANEL_PX = 150;

// The three fixed panel colours, lifted from VirtualControls.kt: the button in its two states, and the
// 0xFF1a1a1a the Column fills its box with. RGB only — the panels are always opaque.
constexpr uint32_t BTN_NORMAL  = 0x3D5A80;  // VirtualControls.kt BTN_NORMAL
constexpr uint32_t BTN_PRESSED = 0x98C1D9;  // VirtualControls.kt BTN_PRESSED
constexpr uint32_t PANEL_BG    = 0x1A1A1A;  // the Column background
constexpr uint32_t LABEL       = 0xFFFFFF;  // Color.White

void fill(SDL_Renderer* r, const SDL_Rect& rc, uint32_t rgb) {
    SDL_SetRenderDrawColor(r, static_cast<Uint8>((rgb >> 16) & 0xFF),
                           static_cast<Uint8>((rgb >> 8) & 0xFF), static_cast<Uint8>(rgb & 0xFF), 255);
    SDL_RenderFillRect(r, &rc);
}

// One button's label as code points — the D-pad gets the arrow glyphs font5x5 already carries (the
// same '↑' '←' '→' '↓' the Kotlin buttons show); the rest are the Kotlin labels ("SEL", "STA", …).
struct Label {
    uint32_t cp[3];
    int      n;
};
Label label_of(Button b) {
    switch (b) {
        case Button::DPAD_UP:    return {{pt::ui::CP_ARROW_UP}, 1};
        case Button::DPAD_DOWN:  return {{pt::ui::CP_ARROW_DOWN}, 1};
        case Button::DPAD_LEFT:  return {{pt::ui::CP_ARROW_LEFT}, 1};
        case Button::DPAD_RIGHT: return {{pt::ui::CP_ARROW_RIGHT}, 1};
        case Button::A:          return {{'A'}, 1};
        case Button::B:          return {{'B'}, 1};
        case Button::L_SHIFT:    return {{'L'}, 1};
        case Button::R_SHIFT:    return {{'R'}, 1};
        case Button::SELECT:     return {{'S', 'E', 'L'}, 3};
        case Button::START:      return {{'S', 'T', 'A'}, 3};
        default:                 return {{'?'}, 1};
    }
}

// Draw a label centred in `rc` at the largest 5×5 scale that leaves a margin. One filled rect per lit
// glyph pixel — a few dozen per label, and only on frames that actually present.
void draw_label(SDL_Renderer* r, Button b, const SDL_Rect& rc) {
    const Label lab = label_of(b);
    // The run is (6n − 1) px wide at scale 1: 5 px per glyph plus a 1 px gap, minus the trailing gap.
    const int denom = 6 * lab.n - 1;
    const int scale = std::max(1, std::min((rc.w * 7 / 10) / denom, (rc.h * 6 / 10) / 5));
    const int runW  = denom * scale;
    const int runH  = 5 * scale;
    int       x     = rc.x + (rc.w - runW) / 2;
    const int y     = rc.y + (rc.h - runH) / 2;

    SDL_SetRenderDrawColor(r, (LABEL >> 16) & 0xFF, (LABEL >> 8) & 0xFF, LABEL & 0xFF, 255);
    for (int k = 0; k < lab.n; ++k) {
        const Glyph& g = glyph_for_codepoint(lab.cp[k]);
        for (int row = 0; row < 5; ++row) {
            for (int col = 0; col < 5; ++col) {
                if (g[row] & (1 << (4 - col))) {
                    SDL_Rect px{x + col * scale, y + row * scale, scale, scale};
                    SDL_RenderFillRect(r, &px);
                }
            }
        }
        x += 6 * scale;
    }
}

// For the trace only.
const char* bname(Button b) {
    switch (b) {
        case Button::DPAD_UP:    return "DPAD_UP";
        case Button::DPAD_DOWN:  return "DPAD_DOWN";
        case Button::DPAD_LEFT:  return "DPAD_LEFT";
        case Button::DPAD_RIGHT: return "DPAD_RIGHT";
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

}  // namespace

void SdlTouch::layout(const SDL_Rect& frame, int outW, int outH) {
    outW_ = outW;
    outH_ = outH;

    // The two letterbox bars either side of the centred frame. In landscape they are what the touch
    // boxes live in; in a layout with no side bars (a frame that fills the width) they are zero-wide
    // and `active_` goes false.
    const int leftW  = frame.x;                 // bar to the LEFT of the frame
    const int rightX = frame.x + frame.w;
    const int rightW = outW - rightX;           // bar to the RIGHT

    active_ = enabled_ && outH > 0 && leftW >= MIN_PANEL_PX && rightW >= MIN_PANEL_PX;
    if (!active_) {
        left_.count  = 0;
        right_.count = 0;
        return;
    }

    leftBox_  = {0, 0, leftW, outH};
    rightBox_ = {rightX, 0, rightW, outH};
    left_     = tl::left_rects(leftW, outH);
    right_    = tl::right_rects(rightW, outH);
}

bool SdlTouch::hit_window(int px, int py, Button& out) const {
    if (px >= leftBox_.x && px < leftBox_.x + leftBox_.w) {
        return tl::hit(left_, px - leftBox_.x, py - leftBox_.y, out);
    }
    if (px >= rightBox_.x && px < rightBox_.x + rightBox_.w) {
        return tl::hit(right_, px - rightBox_.x, py - rightBox_.y, out);
    }
    return false;
}

void SdlTouch::handle_finger(const SDL_Event& e, SdlInput& input, uint64_t now_ms) {
    if (!active_) return;

    // SDL normalises finger coordinates to [0,1] over the window; the renderer output is that same
    // space in pixels on Android (no HiDPI point/pixel split there), which is what the layout uses.
    const SDL_TouchFingerEvent& t  = e.tfinger;
    const int                   px = static_cast<int>(t.x * outW_);
    const int                   py = static_cast<int>(t.y * outH_);

    if (e.type == SDL_FINGERDOWN) {
        Button b{};
        if (hit_window(px, py, b)) {
            input.touch_press(b, now_ms);
            finger_[t.fingerId] = b;
            if (trace_) std::printf("input:   TOUCH DOWN  %4d,%-4d       -> %s\n", px, py, bname(b));
        } else if (trace_) {
            // ⚠️ The load-bearing trace line: a tap that hits nothing says so, so "the buttons don't
            // work" can be told from "the tap landed in a gap" from "no touch event arrived at all".
            std::printf("input:   TOUCH DOWN  %4d,%-4d       -> (no button)\n", px, py);
        }
    } else if (e.type == SDL_FINGERUP) {
        auto it = finger_.find(t.fingerId);
        if (it != finger_.end()) {
            input.touch_release(it->second);
            if (trace_) std::printf("input:   TOUCH UP                    -> %s\n", bname(it->second));
            finger_.erase(it);
        }
    } else if (e.type == SDL_FINGERMOTION) {
        // Sliding off the button a finger went down on RELEASES it, matching Kotlin's per-button
        // pointer scope (tryAwaitRelease ends when the pointer leaves). Sliding back in does not
        // re-press — the gesture is over, as in Compose.
        auto it = finger_.find(t.fingerId);
        if (it != finger_.end()) {
            Button b{};
            const bool still = hit_window(px, py, b) && b == it->second;
            if (!still) {
                input.touch_release(it->second);
                if (trace_)
                    std::printf("input:   TOUCH SLIDE-OFF             -> %s released\n",
                                bname(it->second));
                finger_.erase(it);
            }
        }
    }
}

void SdlTouch::draw(SDL_Renderer* r, const SdlInput& input) const {
    if (!active_) return;

    fill(r, leftBox_, PANEL_BG);
    fill(r, rightBox_, PANEL_BG);

    auto draw_box = [&](const tl::BoxRects& box, int ox, int oy) {
        for (int i = 0; i < box.count; ++i) {
            const tl::ButtonRect& br = box.r[i];
            const SDL_Rect        rc{br.x + ox, br.y + oy, br.w, br.h};
            fill(r, rc, input.is_held(br.button) ? BTN_PRESSED : BTN_NORMAL);
            draw_label(r, br.button, rc);
        }
    };
    draw_box(left_, leftBox_.x, leftBox_.y);
    draw_box(right_, rightBox_.x, rightBox_.y);
}

uint64_t SdlTouch::signature(const SdlInput& input) const {
    if (!active_) return 0;

    uint64_t bits = 0;
    for (int i = 0; i < left_.count; ++i)
        if (input.is_held(left_.r[i].button)) bits |= (1ull << static_cast<int>(left_.r[i].button));
    for (int i = 0; i < right_.count; ++i)
        if (input.is_held(right_.r[i].button)) bits |= (1ull << static_cast<int>(right_.r[i].button));

    // Geometry too, so a rotate/resize that moves the panels forces a repaint even with the same
    // buttons held. The top bit marks "active", so the value is never 0 while panels are on screen.
    return bits | (static_cast<uint64_t>(outW_ & 0xFFFF) << 16) |
           (static_cast<uint64_t>(outH_ & 0xFFFF) << 32) | (1ull << 63);
}
