#include "sdl-video.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "ui/canvas.h"

// ── The window icon, EMBEDDED ────────────────────────────────────────────────────────────────────
// Windows reads its icon from the .exe's resources (shell/windows/pockettracker.rc) and Android has
// its own launcher icon, so both compile this out; desktop Linux (and PortMaster, a fullscreen no-op)
// is a bare ELF with no asset file, so the bytes ride inside the binary.
#if !defined(_WIN32) && !defined(__ANDROID__)
#include "image.h"
#include "window_icon.h"
#endif

using pt::ui::Canvas;
using pt::ui::DESIGN_H;
using pt::ui::DESIGN_W;

bool SdlVideo::open(const char* title, int windowW, int windowH, bool fullscreen, bool resizable) {
    Uint32 flags = SDL_WINDOW_SHOWN;
    if (fullscreen) flags |= SDL_WINDOW_FULLSCREEN_DESKTOP;

    // ── The WINDOWED host: resizable, opened at a sensible size for THIS display ──────────────────
    //
    // Resizable, so SETTINGS > SCALING has somewhere to show (FIT and INTEGER compute the same rect
    // at exactly the design size). ⚠️ The size is DERIVED from the display — the largest integer
    // multiple that fits — because desktop Linux and PortMaster are the same build: a 640×480 panel
    // gets 1×, a 1280×720 TrimUI 1×, a 1080p desktop 2×, 4K 3×. A hardcoded 2× would overflow every
    // handheld. The 90% margin leaves room for a title bar and taskbar.
    if (resizable) {
        flags |= SDL_WINDOW_RESIZABLE;

        SDL_DisplayMode desktop{};
        if (SDL_GetDesktopDisplayMode(0, &desktop) == 0 && desktop.w > 0 && desktop.h > 0) {
            const int scale = std::max(1, std::min(desktop.w * 9 / 10 / windowW,
                                                   desktop.h * 9 / 10 / windowH));
            // ASCII only: the console's encoding is not ours to choose (a × came back as mojibake).
            std::printf("video:   desktop=%dx%d  opening at %dx (%dx%d), resizable\n", desktop.w,
                        desktop.h, scale, windowW * scale, windowH * scale);
            windowW *= scale;
            windowH *= scale;
        }
    }

    window_ = SDL_CreateWindow(title, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, windowW,
                               windowH, flags);
    if (!window_) {
        std::fprintf(stderr, "SDL_CreateWindow failed: %s\n", SDL_GetError());
        return false;
    }

#if !defined(_WIN32) && !defined(__ANDROID__)
    // ── The window / taskbar icon (desktop Linux) ─────────────────────────────────────────────────
    // SDL_SetWindowIcon COPIES the surface, so nothing here has to outlive this block. image.h decodes
    // to 0xAARRGGBB = SDL_PIXELFORMAT_ARGB8888 on these little-endian targets, so the surface wraps the
    // buffer with no shuffle. A failure keeps SDL's default icon.
    if (ptshell::Image icon = ptshell::decode_png(kWindowIconPng, kWindowIconPngLen); icon.ok()) {
        SDL_Surface* surf = SDL_CreateRGBSurfaceWithFormatFrom(
            icon.pixels.data(), icon.width, icon.height, 32, icon.width * 4, SDL_PIXELFORMAT_ARGB8888);
        if (surf) {
            SDL_SetWindowIcon(window_, surf);
            SDL_FreeSurface(surf);
            // One unconditional line: on a handheld with no console this is the only account of it, and
            // a window icon that failed to set looks exactly like one that was never attempted.
            std::printf("video:   window icon set (%dx%d embedded)\n", icon.width, icon.height);
        } else {
            std::printf("video:   window icon: SDL_CreateRGBSurfaceWithFormatFrom failed: %s\n",
                        SDL_GetError());
        }
    } else {
        std::printf("video:   window icon: embedded PNG did not decode - keeping SDL's default\n");
    }
#endif

    // ⚠️ SDL_RENDERER_ACCELERATED means "REQUIRE accelerated": SDL_CreateRenderer fails outright with
    // no GPU driver (a GE8300 with missing 32-bit GL blobs, a CFW without one), with a useless error.
    // So the flags are tried in order and the first that works wins:
    //   1. accelerated + vsync   — a normal device
    //   2. accelerated           — a GPU whose driver won't vsync
    //   3. anything at all       — the software renderer, which is all a 640×480 blit needs
    struct Attempt {
        Uint32      flags;
        const char* what;
    };
    const Attempt attempts[] = {
        {SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC, "accelerated + vsync"},
        {SDL_RENDERER_ACCELERATED, "accelerated, no vsync"},
        {0, "software"},
    };
    for (const Attempt& a : attempts) {
        renderer_ = SDL_CreateRenderer(window_, -1, a.flags);
        if (renderer_) {
            std::printf("video:   %s\n", a.what);
            break;
        }
    }
    if (!renderer_) {
        std::fprintf(stderr, "SDL_CreateRenderer failed: %s\n", SDL_GetError());
        return false;
    }

    // Whether we actually GOT vsync decides who paces the frame. Ask the renderer rather than assume
    // it, because attempt 1 can succeed on a driver that quietly ignores the flag.
    SDL_RendererInfo info{};
    vsync_ = (SDL_GetRendererInfo(renderer_, &info) == 0) &&
             ((info.flags & SDL_RENDERER_PRESENTVSYNC) != 0);

    if (!create_texture()) return false;
    describe();
    return true;
}

/**
 * Report what the display actually IS, not what was asked for — one line, always on.
 *
 * ⚠️ The shell asks for a 640×480 WINDOW and never for fullscreen; on a panel that is not 640×480
 * that is a guess, and nothing else in the log says what became of it. So: the driver, the panel,
 * what the renderer gave, and the frame rect — a rect smaller than the output IS the letterbox.
 */
void SdlVideo::describe() const {
    int outW = 0, outH = 0;
    SDL_GetRendererOutputSize(renderer_, &outW, &outH);

    SDL_DisplayMode mode{};
    const bool haveMode = SDL_GetCurrentDisplayMode(SDL_GetWindowDisplayIndex(window_), &mode) == 0;

    const SDL_Rect d = dest_rect();
    std::printf("video:   driver=%s  panel=%dx%d@%dHz  output=%dx%d  frame=%dx%d at %d,%d  %s  %s\n",
                SDL_GetCurrentVideoDriver() ? SDL_GetCurrentVideoDriver() : "?",
                haveMode ? mode.w : 0, haveMode ? mode.h : 0, haveMode ? mode.refresh_rate : 0, outW,
                outH, d.w, d.h, d.x, d.y, scaling_ == ScalingMode::FIT ? "FIT" : "INTEGER",
                vsync_ ? "vsync" : "SELF-PACED");

    // The letterbox, named rather than left to be noticed. Bars are correct and expected on a panel
    // that is not a whole multiple of 640x480 — what is NOT expected is the frame overhanging the
    // output, which means the window outgrew the screen and the edges of the UI are simply gone.
    if (d.w > outW || d.h > outH) {
        std::printf("video:   WARNING: the frame is LARGER than the display - edges are cut off\n");
    } else if (d.w < outW || d.h < outH) {
        std::printf("video:   letterbox: %dpx horizontal, %dpx vertical bars\n", (outW - d.w) / 2,
                    (outH - d.h) / 2);
    }
}

bool SdlVideo::create_texture() {
    if (texture_) SDL_DestroyTexture(texture_);

    // ⚠️ SDL2 samples the filter hint when the TEXTURE IS CREATED, not when it is drawn — so this
    // must be set here, and changing the scaling mode later means recreating the texture (set_scaling
    // below). Nearest keeps INTEGER pixel-perfect, which is the entire point of it; linear is what
    // makes FIT's non-integer stretch merely soft instead of visibly uneven.
    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, scaling_ == ScalingMode::FIT ? "1" : "0");

    // ARGB8888 matches Canvas's uint32 pixels bit for bit on a little-endian machine (and every
    // target here is little-endian: x86-64 and aarch64), so the upload is a straight memcpy per row
    // with no swizzle.
    texture_ = SDL_CreateTexture(renderer_, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING,
                                 DESIGN_W, DESIGN_H);
    if (!texture_) {
        std::fprintf(stderr, "SDL_CreateTexture failed: %s\n", SDL_GetError());
        return false;
    }
    return true;
}

void SdlVideo::set_scaling(ScalingMode m) {
    if (m == scaling_) return;
    const ScalingMode was = scaling_;
    scaling_              = m;
    create_texture();

    // Once per CHANGE (the early return makes it cheap from the frame loop). ⚠️ The RECT is printed
    // beside the mode: "BILINEAR" says what was asked for, `frame=1280x960` what the user sees — on a
    // handheld the log is how to tell "the setting does nothing" from "the window is wrong".
    const SDL_Rect d = dest_rect();
    std::printf("video:   scaling %s -> %s   frame=%dx%d at %d,%d\n",
                was == ScalingMode::FIT ? "FIT" : "INTEGER",
                scaling_ == ScalingMode::FIT ? "FIT" : "INTEGER", d.w, d.h, d.x, d.y);
    std::fflush(stdout);
}

void SdlVideo::close() {
    if (texture_) SDL_DestroyTexture(texture_);
    if (renderer_) SDL_DestroyRenderer(renderer_);
    if (window_) SDL_DestroyWindow(window_);
    texture_  = nullptr;
    renderer_ = nullptr;
    window_   = nullptr;
}

SDL_Rect SdlVideo::dest_rect() const {
    int outW = 0, outH = 0;
    SDL_GetRendererOutputSize(renderer_, &outW, &outH);

    if (scaling_ == ScalingMode::FIT) {
        // ⚠️⚠️ FIT PRESERVES THE ASPECT RATIO; IT IS NOT A STRETCH TO THE WINDOW EDGES. The row is
        // "BILINEAR" — a filtering choice: FIT differs from INTEGER only in allowing a fractional
        // scale and filtering it. One factor, the `min` of the two axis ratios, applied to both.
        // ⚠️ INTEGER arithmetic, not `min(outW/640.0f, outH/480.0f)`: a float scale can land
        // 480 × 2.0f at 959.99997 and leave a 1px bar on an exact multiple. Cross-multiplying picks
        // the binding axis exactly, and that axis keeps the window's own size.
        int w, h;
        if (outW * DESIGN_H >= outH * DESIGN_W) {  // window wider than 4:3 → height binds
            h = outH;
            w = outH * DESIGN_W / DESIGN_H;
        } else {                                   // taller than 4:3 → width binds
            w = outW;
            h = outW * DESIGN_H / DESIGN_W;
        }
        return SDL_Rect{(outW - w) / 2, frame_top(outH, h), w, h};
    }

    // INTEGER: the largest whole multiple that fits, centred. Clamped to 1 so a window smaller than
    // the design (possible on a desktop, where the user can drag it to anything) still shows
    // something rather than collapsing to nothing.
    const int scale = std::max(1, std::min(outW / DESIGN_W, outH / DESIGN_H));
    const int w     = DESIGN_W * scale;
    const int h     = DESIGN_H * scale;
    return SDL_Rect{(outW - w) / 2, frame_top(outH, h), w, h};
}

// Where the frame's top edge goes - the ONE place the vertical placement is decided, so the two
// scaling modes cannot drift apart.
//
// The anchored answer is "centred in the TOP HALF", not "flush with the top": it is the same
// arithmetic one output shorter, it keeps the frame clear of the status bar, and it lands close to
// where the PORTRAIT2 skin puts its bezel - which is what the request asked it to look like.
//
// Clamped at 0: a frame taller than half the output has no top half to sit in, and a negative y
// would push its first rows off the screen.
int SdlVideo::frame_top(int outH, int h) const {
    if (!topAnchor_) return (outH - h) / 2;
    return std::max(0, (outH / 2 - h) / 2);
}

void SdlVideo::invalidate_backbuffer(bool texture_lost) {
    // GL context loss (an Android DEVICE reset) takes the streaming texture with it — recreate it, or
    // the forced present below uploads into a dead handle. A plain re-expose keeps the texture.
    if (texture_lost) create_texture();

    // The one line that matters: the next present cannot take its "identical to what's on screen" skip,
    // because what is on screen is no longer what we last drew. See the header for the full mechanism.
    haveLast_ = false;
}

bool SdlVideo::present(const Canvas& canvas, uint32_t letterboxArgb,
                       const std::function<void(SDL_Renderer*)>& overlay, uint64_t overlaySig,
                       uint32_t modalScrimArgb) {
    // ⚠️ RE-DESCRIBE WHEN THE OUTPUT CHANGES: on Android the surface is still 1280x904 when the window
    // is created and becomes 1280x960 a few frames later (the system bars hide asynchronously), so a
    // boot-only line would describe the wrong session.
    // ⚠️ ABOVE the pixel-gate skip: INTEGER scaling can absorb a small output change without moving
    // the rect, and the skip would hide exactly that resize.
    // ⚠️ The landscape/centred path only: the PORTRAIT2 skin reports its own geometry from app.cpp,
    // since `describe()` reports the centred `dest_rect()`.
    int outW = 0, outH = 0;
    SDL_GetRendererOutputSize(renderer_, &outW, &outH);
    if (outW != lastOutW_ || outH != lastOutH_) {
        if (lastOutW_ != 0) describe();   // not at boot; open() has just printed the same thing
        lastOutW_ = outW;
        lastOutH_ = outH;
    }

    // The centred frame, no underlay. The modal scrim (if any) dims the whole output around it.
    return present_impl(canvas, letterboxArgb, dest_rect(), {}, overlay, overlaySig, modalScrimArgb,
                        SDL_Rect{0, 0, outW, outH});
}

bool SdlVideo::present_skinned(const Canvas& canvas, uint32_t clearArgb, const SDL_Rect& frameDest,
                               const std::function<void(SDL_Renderer*)>& underlay,
                               const std::function<void(SDL_Renderer*)>& overlay, uint64_t overlaySig,
                               uint32_t modalScrimArgb, const SDL_Rect& scrimBounds) {
    // No re-describe: the PORTRAIT2 geometry is logged by app.cpp. The modal scrim (if any) is
    // bounded to `scrimBounds` — the bezel's inner glass — so it never dims the casing or the cluster.
    return present_impl(canvas, clearArgb, frameDest, underlay, overlay, overlaySig, modalScrimArgb,
                        scrimBounds);
}

bool SdlVideo::present_impl(const Canvas& canvas, uint32_t clearArgb, const SDL_Rect& dest,
                            const std::function<void(SDL_Renderer*)>& underlay,
                            const std::function<void(SDL_Renderer*)>& overlay, uint64_t overlaySig,
                            uint32_t modalScrimArgb, const SDL_Rect& scrimBounds) {
    // ── DON'T PRESENT A FRAME THAT IS ALREADY ON SCREEN ──────────────────────────────────────────
    //
    // The pixel-level half of the idle-redraw discipline: `app.cpp` decides when not to DRAW, this
    // when a drawn frame is not worth sending — a net no forgotten state field can fool.
    // ⚠️ The pixels, the letterbox colour, the destination rect AND `overlaySig`: a theme change
    // repaints the bars, a resize moves the frame, and a virtual-button press repaints only the panel
    // — each invisible to a canvas compare. `overlaySig` is zero where there are no on-screen controls.
    const size_t n = static_cast<size_t>(DESIGN_W) * DESIGN_H;
    if (haveLast_ && clearArgb == lastLetterbox_ && overlaySig == lastOverlaySig_ &&
        modalScrimArgb == lastModalScrim_ &&
        dest.x == lastDest_.x && dest.y == lastDest_.y && dest.w == lastDest_.w &&
        dest.h == lastDest_.h &&
        std::memcmp(lastFrame_.data(), canvas.pixels(), n * sizeof(uint32_t)) == 0) {
        return false;
    }

    // The WHOLE frame is uploaded every time, with no dirty-rect tracking. Handing 1.2 MB to the GPU
    // costs microseconds, and a partial upload would need dirty rectangles that `pt::ui::Canvas` does
    // not track — the all-or-nothing skip above is where the saving actually is.
    void* dst   = nullptr;
    int   pitch = 0;
    if (SDL_LockTexture(texture_, nullptr, &dst, &pitch) != 0) return false;

    const auto* src = reinterpret_cast<const uint8_t*>(canvas.pixels());
    const int   row = canvas.pitch_bytes();
    if (pitch == row) {
        SDL_memcpy(dst, src, static_cast<size_t>(row) * DESIGN_H);
    } else {
        // A driver may hand back a padded pitch; copy row by row when it does.
        auto* out = reinterpret_cast<uint8_t*>(dst);
        for (int y = 0; y < DESIGN_H; ++y) {
            SDL_memcpy(out + static_cast<size_t>(y) * pitch, src + static_cast<size_t>(y) * row,
                       static_cast<size_t>(row));
        }
    }
    SDL_UnlockTexture(texture_);

    // ── The letterbox bars, in the LIVE theme's background colour rather than black ───────────────
    // Black bars draw a hard edge and make the app a small picture on a black wall; the UI's own
    // background makes the window one surface. On a phone the touch skin is drawn over this clear.
    SDL_SetRenderDrawColor(renderer_, static_cast<Uint8>((clearArgb >> 16) & 0xFF),
                           static_cast<Uint8>((clearArgb >> 8) & 0xFF),
                           static_cast<Uint8>(clearArgb & 0xFF), 255);
    SDL_RenderClear(renderer_);  // paints the letterbox bars (landscape) or the casing (PORTRAIT2)

    // ⚠️ The UNDERLAY — after the clear, BEFORE the frame: PORTRAIT2's chrome bands and inner bezel.
    // A no-op on the centred path.
    if (underlay) underlay(renderer_);

    // ── The modal scrim, so the dim reaches AROUND the frame ──────────────────────────────────────
    // A full-canvas modal dims the tracker inside the frame; the shell fills the four regions AROUND
    // `dest`, bounded to `scrimBounds`, with the same colour — never the frame itself. Landscape
    // passes the whole output (the letterbox bars); PORTRAIT2 passes the bezel's inner glass, so the
    // gap around an integer-scaled frame dims while the casing and button cluster stay bright.
    // (Filling the whole output and re-copying the frame over it left the tracker black.)
    if (modalScrimArgb != 0) {
        SDL_SetRenderDrawBlendMode(renderer_, SDL_BLENDMODE_BLEND);
        SDL_SetRenderDrawColor(renderer_, static_cast<Uint8>((modalScrimArgb >> 16) & 0xFF),
                               static_cast<Uint8>((modalScrimArgb >> 8) & 0xFF),
                               static_cast<Uint8>(modalScrimArgb & 0xFF),
                               static_cast<Uint8>((modalScrimArgb >> 24) & 0xFF));
        // The frame edges, clamped INTO the bounds, so a frame poking outside can never make a bar with a
        // negative dimension (and a frame that fills the bounds leaves four zero-size, no-op bars).
        const int bx = scrimBounds.x, by = scrimBounds.y;
        const int br = scrimBounds.x + scrimBounds.w, bb = scrimBounds.y + scrimBounds.h;
        const int fx = std::max(bx, std::min(br, dest.x));
        const int fy = std::max(by, std::min(bb, dest.y));
        const int fr = std::max(bx, std::min(br, dest.x + dest.w));
        const int fb = std::max(by, std::min(bb, dest.y + dest.h));
        const SDL_Rect bars[4] = {
            {bx, by, br - bx, fy - by},   // above the frame
            {bx, fb, br - bx, bb - fb},   // below the frame
            {bx, fy, fx - bx, fb - fy},   // left of the frame
            {fr, fy, br - fr, fb - fy},   // right of the frame
        };
        SDL_RenderFillRects(renderer_, bars, 4);
    }

    SDL_RenderCopy(renderer_, texture_, nullptr, &dest);

    // ⚠️ The OVERLAY — after the frame, before the flip: the landscape touch panels or PORTRAIT2's
    // button cluster. Never onto the tracker itself.
    if (overlay) overlay(renderer_);

    SDL_RenderPresent(renderer_);

    // What is now on screen, so the next frame can tell whether it would change anything. Kept AFTER
    // the present rather than before it: this array's meaning is "the displayed image", and a copy
    // taken on a frame that then failed to present would make the next comparison lie.
    if (lastFrame_.size() != n) lastFrame_.resize(n);
    std::memcpy(lastFrame_.data(), canvas.pixels(), n * sizeof(uint32_t));
    lastLetterbox_  = clearArgb;
    lastDest_       = dest;
    lastOverlaySig_ = overlaySig;
    lastModalScrim_ = modalScrimArgb;
    haveLast_       = true;

    // ⚠️ NOTHING HERE PACES ANYTHING: the app loop owns both rates (THE TWO RATES in app.cpp). With
    // vsync SDL_RenderPresent still blocks, but the frame deadline is what holds the draw to 60 Hz.
    return true;
}
