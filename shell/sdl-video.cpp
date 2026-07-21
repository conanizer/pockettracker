#include "sdl-video.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "ui/canvas.h"

using pt::ui::Canvas;
using pt::ui::DESIGN_H;
using pt::ui::DESIGN_W;

bool SdlVideo::open(const char* title, int windowW, int windowH, bool fullscreen, bool resizable) {
    Uint32 flags = SDL_WINDOW_SHOWN;
    if (fullscreen) flags |= SDL_WINDOW_FULLSCREEN_DESKTOP;

    // ── The WINDOWED host: resizable, and opened at a sensible size for THIS display ──────────────
    //
    // Until now every platform opened at exactly 640×480 and could not be resized at all, which had
    // two consequences that only ever showed up away from a handheld. On a desktop the tracker was a
    // postage stamp on a 1440p monitor with no way to grow it — and, less obviously, it is why
    // SETTINGS > SCALING was invisible there even after C4 finally wired it up: FIT and INTEGER
    // compute the SAME rect when the output is exactly the design size, so the setting genuinely had
    // nothing to do. Making the window resizable is what gives that setting somewhere to show.
    //
    // ⚠️ The size is DERIVED from the display rather than hardcoded to 2×, and that is what makes one
    // code path correct on three very different targets with no build-time discriminator to test —
    // and there is none: desktop Linux and PortMaster are the same Linux build of the same main.cpp.
    // A 640×480 handheld panel computes 1× and is therefore byte-for-byte what shipped; a 1280×720
    // TrimUI computes 1× (2× does not fit); a 1920×1080 desktop computes 2×; 4K computes 3×.
    // Hardcoding 2× would have overflowed every handheld panel in the zoo, which is exactly the
    // "frame is LARGER than the display" case describe() warns about.
    //
    // The 90% margin is for the things a desktop puts around a window and a handheld does not: a
    // title bar, a taskbar, a dock. Without it a 1280×960 desktop would compute a 2× window whose
    // chrome does not fit on the screen it has to sit on.
    if (resizable) {
        flags |= SDL_WINDOW_RESIZABLE;

        SDL_DisplayMode desktop{};
        if (SDL_GetDesktopDisplayMode(0, &desktop) == 0 && desktop.w > 0 && desktop.h > 0) {
            const int scale = std::max(1, std::min(desktop.w * 9 / 10 / windowW,
                                                   desktop.h * 9 / 10 / windowH));
            // ASCII, and this line got it WRONG first time round: a U+00D7 multiplication sign came
            // back on a 1251 console as `1Г—`. This file's own describe() comment and app.cpp's
            // banner both state the rule — the console's encoding is not ours to choose — and the
            // mojibake was in the very first run's output.
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

    // ⚠️ SDL_RENDERER_ACCELERATED does NOT mean "prefer accelerated" — it means "REQUIRE accelerated",
    // and SDL_CreateRenderer FAILS outright if no driver offers it. That is the opposite of what a
    // handheld port wants, and it fails on exactly the device the port plan names: TrimUI's GE8300,
    // whose 32-bit GL blobs are missing, and any CFW booted without a GPU driver. The app would not
    // start, and the error ("Couldn't find matching render driver") names nothing useful.
    //
    // So the flags are tried in descending order of what we'd LIKE, and the first that works wins:
    //   1. accelerated + vsync   — a normal device
    //   2. accelerated           — a GPU whose driver won't vsync
    //   3. anything at all       — SDL picks the software renderer, which is all a 640×480 blit needs
    //
    // The software fallback is not a degraded mode here. Blitting one 640×480 texture is trivial on a
    // CPU; that is *why* this design draws into a framebuffer instead of using shaders.
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
 * ⚠️ This is the §10 bring-up row ("KMSDRM/rotation: no letterbox surprise") made answerable, and it
 * exists because the shell asks for a 640x480 WINDOW and never asks for fullscreen. On the Miyoo Flip
 * that is invisible: the panel IS 640x480, and KMSDRM has no window manager to put it in a box. On any
 * device whose panel is not 640x480 the request is a guess, and nothing in the log would say what
 * became of it.
 *
 * So: print the driver, the panel, what the renderer really gave us, and the rect the frame lands in.
 * A dest rect smaller than the output IS the letterbox, in numbers, before anyone squints at a photo.
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
        std::printf("video:   ⚠️ the frame is LARGER than the display — edges are cut off\n");
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

    // Once per CHANGE, never per frame — the early return above is what makes this cheap enough to
    // call from the frame loop, which is where SETTINGS > SCALING is polled from (app.cpp).
    //
    // ⚠️ The RECT is printed beside the mode, and it is the whole point of the line. "SCALING =
    // BILINEAR" only says what was asked for; `frame=1280x960` versus `frame=640x480` says what the
    // user will actually see — and this setting's entire history is of being asked for and not
    // applied. On a handheld with no console you read this back out of the log; on a phone it is the
    // difference between "the setting does nothing" and "the setting works, the WINDOW is wrong",
    // which are the two bugs C4 had to tell apart.
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
        // ⚠️⚠️ **FIT PRESERVES THE ASPECT RATIO. IT IS NOT A STRETCH TO THE WINDOW EDGES**, and it
        // used to be — `return {0, 0, outW, outH}`, which squashes the 4:3 design into whatever shape
        // the window happens to be. Reported by the user against the resizable desktop window, which
        // is what made the setting visible on a non-4:3 output for the first time.
        //
        // ⭐ **The Kotlin this is a port OF has never stretched.** All three BILINEAR paths in
        // `ScreenLayouts.kt` (FullScreen :189, Portrait :228, the bezel layout :472) compute ONE
        // factor as a `minOf` of the two axis ratios and apply it to BOTH — `scaleX = scaleY =
        // fillFactor` — and the bezel path even computes explicit `transX`/`transY` centring offsets,
        // i.e. it expects a letterbox gap. So square pixels are not what INTEGER buys over FIT; the
        // difference is only that FIT allows a FRACTIONAL scale and filters the result.
        //
        // ⭐ The row is called "BILINEAR", which says the same thing: it names a FILTERING choice, not
        // a geometry one. The geometry divergence arrived with the wording "fit-stretch" in the port
        // plan's §4.5 and was never checked against the app being converged onto.
        // ⚠️ INTEGER arithmetic, not `float scale = min(outW/640.0f, outH/480.0f)`. A float scale
        // truncates: at 1280×960 it computes 960/480 = 2.0f, and `480 * 2.0f` can land at 959.99997,
        // giving a 1px bar on an output that is an EXACT multiple of the design and should have none.
        // Cross-multiplying answers "which axis binds?" exactly, and the bound axis then keeps the
        // window's own size rather than a value round-tripped through a ratio.
        int w, h;
        if (outW * DESIGN_H >= outH * DESIGN_W) {  // window wider than 4:3 → height binds
            h = outH;
            w = outH * DESIGN_W / DESIGN_H;
        } else {                                   // taller than 4:3 → width binds
            w = outW;
            h = outW * DESIGN_H / DESIGN_W;
        }
        return SDL_Rect{(outW - w) / 2, (outH - h) / 2, w, h};
    }

    // INTEGER: the largest whole multiple that fits, centred. Clamped to 1 so a window smaller than
    // the design (possible on a desktop, where the user can drag it to anything) still shows
    // something rather than collapsing to nothing.
    const int scale = std::max(1, std::min(outW / DESIGN_W, outH / DESIGN_H));
    const int w     = DESIGN_W * scale;
    const int h     = DESIGN_H * scale;
    return SDL_Rect{(outW - w) / 2, (outH - h) / 2, w, h};
}

/**
 * Pace a frame that presented nothing.
 *
 * ⚠️⚠️ **THIS IS THE TRAP IN C7 AND IT INVERTS THE FEATURE IF MISSED.** With vsync it is
 * `SDL_RenderPresent` that BLOCKS until the display is ready — that call is what paces the entire app
 * loop. Skip it to save power and nothing blocks at all: the loop spins as fast as the CPU allows,
 * burning a whole core to avoid a blit. The idle path would then cost MORE than the busy one, which
 * is the exact opposite of what C7 exists for. So every path that does not present must pace here.
 */
void SdlVideo::pace() {
    const Uint64 now     = SDL_GetTicks64();
    const Uint64 elapsed = now - lastPresentMs_;
    if (elapsed < FRAME_MS) SDL_Delay(static_cast<Uint32>(FRAME_MS - elapsed));
    lastPresentMs_ = SDL_GetTicks64();
}

void SdlVideo::idle_frame() { pace(); }

void SdlVideo::invalidate_backbuffer(bool texture_lost) {
    // GL context loss (an Android DEVICE reset) takes the streaming texture with it — recreate it, or
    // the forced present below uploads into a dead handle. A plain re-expose keeps the texture.
    if (texture_lost) create_texture();

    // The one line that matters: the next present cannot take its "identical to what's on screen" skip,
    // because what is on screen is no longer what we last drew. See the header for the full mechanism.
    haveLast_ = false;
}

bool SdlVideo::present(const Canvas& canvas, uint32_t letterboxArgb,
                       const std::function<void(SDL_Renderer*)>& overlay, uint64_t overlaySig) {
    // ⚠️ **RE-DESCRIBE WHEN THE OUTPUT CHANGES, AND ANDROID IS WHY (C4).** `describe()` used to run
    // exactly once, at `open()` — which on this platform is the one moment it is guaranteed to be
    // WRONG. `SdlActivity.hideSystemBars()` has to POST itself (API 30+ wants an attached DecorView),
    // so the surface is still 1280x904 when the window is created and becomes 1280x960 a few frames
    // later. The boot line therefore reported a 1x letterboxed window for a session that was actually
    // running 2x full-screen, and on a handheld with no console that line is the ONLY account of what
    // the user is looking at. An instrument aimed at the wrong instant is worse than none.
    //
    // ⚠️ ABOVE the C7 skip (in present_impl), deliberately. INTEGER scaling can absorb a small output
    // change without moving the dest rect at all (1284→1285 wide still lands 1280 at x=2), so a check
    // placed after the skip would miss exactly the resize this instrument exists to report.
    //
    // ⚠️ This is the LANDSCAPE/centred path's describe. The PORTRAIT2 skin (present_skinned) reports its
    // own geometry from app.cpp, because `describe()` reports the CENTRED `dest_rect()` — which is not
    // where a skinned frame lands, so calling it there would be a lying instrument (the very failure
    // this comment block exists to prevent, one mode over).
    //
    // Fires on change only: a window resize, a rotation, or the system bars coming and going.
    int outW = 0, outH = 0;
    SDL_GetRendererOutputSize(renderer_, &outW, &outH);
    if (outW != lastOutW_ || outH != lastOutH_) {
        if (lastOutW_ != 0) describe();   // not at boot; open() has just printed the same thing
        lastOutW_ = outW;
        lastOutH_ = outH;
    }

    // The centred frame, no underlay — the pre-D present, now expressed through the shared body.
    return present_impl(canvas, letterboxArgb, dest_rect(), {}, overlay, overlaySig);
}

bool SdlVideo::present_skinned(const Canvas& canvas, uint32_t clearArgb, const SDL_Rect& frameDest,
                               const std::function<void(SDL_Renderer*)>& underlay,
                               const std::function<void(SDL_Renderer*)>& overlay, uint64_t overlaySig) {
    // No re-describe here: the PORTRAIT2 mode's geometry is logged by app.cpp (see the note in
    // `present` above). Otherwise identical to the centred path — same upload, same gate, same pacing.
    return present_impl(canvas, clearArgb, frameDest, underlay, overlay, overlaySig);
}

bool SdlVideo::present_impl(const Canvas& canvas, uint32_t clearArgb, const SDL_Rect& dest,
                            const std::function<void(SDL_Renderer*)>& underlay,
                            const std::function<void(SDL_Renderer*)>& overlay, uint64_t overlaySig) {
    // ── C7: DON'T PRESENT A FRAME THAT IS ALREADY ON SCREEN ──────────────────────────────────────
    //
    // The pixel-level half of the idle-redraw discipline. `app.cpp` decides when not to DRAW; this
    // decides when a drawn frame is not worth sending — and it is the safety net under that decision,
    // because it cannot be fooled by a state field somebody forgot to include in a predicate. If the
    // pixels, the letterbox colour and the destination rect are all identical, the display is already
    // showing this exact image and uploading it again changes nothing a user could see.
    //
    // ⚠️ ALL THREE, not just the canvas: a theme change repaints the BARS without moving one canvas
    // pixel, and a resize moves the frame without changing it. Comparing the canvas alone would leave
    // both stale on screen — the C4 SCALING bug's shape (a value read but never applied), one layer
    // over.
    // ⚠️ `overlaySig` is part of this compare, and the touch skin is why (Phase D). A virtual-button
    // press repaints only the PANEL — a highlight OUTSIDE the 640×480 canvas — so the canvas memcmp
    // alone would find the frame identical and skip it, leaving the button looking un-pressed. That is
    // the C7 blind-channel shape exactly (a change the comparison cannot see), one panel over from the
    // sleep-resume case; folding the overlay's fingerprint into the gate closes it. Zero when there is
    // no overlay, so this is a no-op wherever there are no on-screen controls.
    const size_t n = static_cast<size_t>(DESIGN_W) * DESIGN_H;
    if (haveLast_ && clearArgb == lastLetterbox_ && overlaySig == lastOverlaySig_ &&
        dest.x == lastDest_.x && dest.y == lastDest_.y && dest.w == lastDest_.w &&
        dest.h == lastDest_.h &&
        std::memcmp(lastFrame_.data(), canvas.pixels(), n * sizeof(uint32_t)) == 0) {
        pace();          // ⚠️ never skip this — see pace() for why it inverts the feature
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

    // ── The letterbox bars, in the THEME's background colour rather than black ────────────────────
    //
    // The design is 4:3 and almost nothing else is, so on a 16:9 desktop window or a phone the frame
    // sits in a box with bars either side. Painting those black drew a hard edge around the tracker
    // and made the app look like a small picture on a large black wall; painting them the same colour
    // the UI already fills its own background with makes the whole window read as one surface, which
    // is the "fullscreen experience" this is for.
    //
    // ⚠️ It is the LIVE theme, so it tracks the theme editor and any loaded .ptt with no wiring of its
    // own — the colour arrives as an argument on every present (see the header for why not a setter).
    //
    // ⚠️ Phase D composites the touch skin into exactly this space on a phone. That is not a conflict:
    // the skin's textures are drawn OVER a cleared background, so this stays the correct thing
    // underneath them, and remains the whole answer on every layout that has no virtual buttons.
    SDL_SetRenderDrawColor(renderer_, static_cast<Uint8>((clearArgb >> 16) & 0xFF),
                           static_cast<Uint8>((clearArgb >> 8) & 0xFF),
                           static_cast<Uint8>(clearArgb & 0xFF), 255);
    SDL_RenderClear(renderer_);  // paints the letterbox bars (landscape) or the casing (PORTRAIT2)

    // ⚠️ The UNDERLAY goes here — after the clear, BEFORE the frame — for the one thing drawn BEHIND the
    // tracker: PORTRAIT2's chrome bands and the black inner bezel the frame sits on. Empty (and so a
    // no-op) on the centred landscape/desktop path, where nothing is behind the frame but the clear.
    if (underlay) underlay(renderer_);

    SDL_RenderCopy(renderer_, texture_, nullptr, &dest);

    // ⚠️ The overlay goes HERE — after the frame, before the flip — drawn OVER it: the landscape touch
    // panels in the bars beside the frame, or PORTRAIT2's button cluster on its backing. Never onto the
    // 640×480 tracker itself. Empty on any layout with no on-screen controls, and then this does nothing.
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
    haveLast_       = true;

    // ── Pacing ───────────────────────────────────────────────────────────────────────────────────
    // With vsync, SDL_RenderPresent blocks until the display is ready and the whole app loop rides
    // the refresh — nothing to do. WITHOUT it (the software renderer, or a driver that ignored the
    // flag) nothing blocks at all, and the loop would spin as fast as the CPU allows: a pegged core,
    // a hot device and a flat battery, on hardware chosen for none of those. So the pacing has to
    // live wherever the vsync decision does, which is here.
    //
    // ⚠️ The timestamp is stamped EITHER WAY, and that is C7's doing: with vsync the present itself
    // paced us and there is nothing to wait for, but `lastPresentMs_` is what the SKIP path measures
    // against — leave it stale through a run of vsync'd frames and the first skipped frame computes a
    // huge elapsed, delays nothing, and spins one frame hot before self-correcting.
    if (!vsync_) {
        pace();
    } else {
        lastPresentMs_ = SDL_GetTicks64();
    }
    return true;
}
