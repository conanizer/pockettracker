#include "sdl-video.h"

#include <algorithm>
#include <cstdio>

#include "ui/canvas.h"

using pt::ui::Canvas;
using pt::ui::DESIGN_H;
using pt::ui::DESIGN_W;

bool SdlVideo::open(const char* title, int windowW, int windowH, bool fullscreen) {
    Uint32 flags = SDL_WINDOW_SHOWN;
    if (fullscreen) flags |= SDL_WINDOW_FULLSCREEN_DESKTOP;

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

    return create_texture();
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
    scaling_ = m;
    create_texture();
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
        return SDL_Rect{0, 0, outW, outH};
    }

    // INTEGER: the largest whole multiple that fits, centred. Clamped to 1 so a window smaller than
    // the design (possible on a desktop, where the user can drag it to anything) still shows
    // something rather than collapsing to nothing.
    const int scale = std::max(1, std::min(outW / DESIGN_W, outH / DESIGN_H));
    const int w     = DESIGN_W * scale;
    const int h     = DESIGN_H * scale;
    return SDL_Rect{(outW - w) / 2, (outH - h) / 2, w, h};
}

void SdlVideo::present(const Canvas& canvas) {
    // The WHOLE frame is uploaded every time, with no dirty-rect tracking. That is a deliberate
    // difference from the Android renderer, which goes to some trouble to avoid repainting when
    // nothing has changed — but Compose is avoiding a full RECOMPOSITION there (walking the tree,
    // allocating, and risking the GC pauses that crashed RenderThread on Snapdragon drivers), not a
    // memcpy. Here the frame is already drawn into RAM; handing 1.2 MB to the GPU costs microseconds.
    //
    // What still matters on a handheld is not repainting when nothing MOVED — that is a battery
    // question, and it is answered one level up, in the app loop, by not calling this at all on an
    // idle frame. The port plan calls that "dirty-redraw + audio-active refresh"; it lands with the
    // oscilloscope, which is the thing that decides when the screen is animating.
    void* dst   = nullptr;
    int   pitch = 0;
    if (SDL_LockTexture(texture_, nullptr, &dst, &pitch) != 0) return;

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

    SDL_SetRenderDrawColor(renderer_, 0, 0, 0, 255);
    SDL_RenderClear(renderer_);  // paints the letterbox bars
    const SDL_Rect dst_rect = dest_rect();
    SDL_RenderCopy(renderer_, texture_, nullptr, &dst_rect);
    SDL_RenderPresent(renderer_);

    // ── Pacing ───────────────────────────────────────────────────────────────────────────────────
    // With vsync, SDL_RenderPresent blocks until the display is ready and the whole app loop rides
    // the refresh — nothing to do. WITHOUT it (the software renderer, or a driver that ignored the
    // flag) nothing blocks at all, and the loop would spin as fast as the CPU allows: a pegged core,
    // a hot device and a flat battery, on hardware chosen for none of those. So the pacing has to
    // live wherever the vsync decision does, which is here.
    if (!vsync_) {
        const Uint64 now     = SDL_GetTicks64();
        const Uint64 elapsed = now - lastPresentMs_;
        if (elapsed < FRAME_MS) SDL_Delay(static_cast<Uint32>(FRAME_MS - elapsed));
        lastPresentMs_ = SDL_GetTicks64();
    }
}
