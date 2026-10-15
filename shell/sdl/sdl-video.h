// The window, the 640×480 streaming texture and the scaler. SDL_Renderer takes a GPU backend when
// there is one and software otherwise; with no shader here, a device with missing GL blobs still runs.

#ifndef POCKETTRACKER_SDL_VIDEO_H
#define POCKETTRACKER_SDL_VIDEO_H

#include <cmath>  // before <SDL.h> — see sdl-audio-engine.h

#include <SDL.h>

#include <cstdint>
#include <functional>
#include <vector>

namespace pt::ui {
class Canvas;
}

// INTEGER scales by a whole number (crisp, the default); FIT by a fraction, filtered (bigger, soft).
// Both keep 4:3 and letterbox the rest. On a whole multiple of the design they differ only in the filter.
enum class ScalingMode { INTEGER, FIT };

class SdlVideo {
public:
    // `resizable`: a windowed host — `SDL_WINDOW_RESIZABLE`, opened at the largest integer scale that
    // fits. ⚠️ Android must pass false: SDL then lets the activity rotate into portrait.
    bool open(const char* title, int windowW, int windowH, bool fullscreen = false,
              bool resizable = false);
    void close();

    // Upload and present, unless identical to the screen; true if it really presented.
    // `letterboxArgb` is a parameter, not a setter, so it can never be stale. `overlay` draws after
    // the frame (the touch panels); `overlaySig` fingerprints it, since a press highlight lies outside
    // the compared canvas.
    bool present(const pt::ui::Canvas& canvas, uint32_t letterboxArgb,
                 const std::function<void(SDL_Renderer*)>& overlay = {}, uint64_t overlaySig = 0,
                 uint32_t modalScrimArgb = 0);

    // Present into `frameDest` with the portrait skin around it: clear to `clearArgb` (the casing),
    // `underlay` (chrome), the frame, `overlay` (buttons). Shares `present`'s upload and pixel gate.
    bool present_skinned(const pt::ui::Canvas& canvas, uint32_t clearArgb, const SDL_Rect& frameDest,
                         const std::function<void(SDL_Renderer*)>& underlay,
                         const std::function<void(SDL_Renderer*)>& overlay, uint64_t overlaySig,
                         uint32_t modalScrimArgb = 0, const SDL_Rect& scrimBounds = {0, 0, 0, 0});

    // Force the next present: ⚠️ the pixel gate assumes the screen still shows the last frame, and
    // Android blanks it on resume (likewise a re-expose). `texture_lost`: the renderer's device was
    // reset (GL context loss), so the texture is recreated too.
    void invalidate_backbuffer(bool texture_lost);

    // Recreates the texture: SDL2 fixes the filter at creation (INTEGER → nearest, FIT → linear).
    void        set_scaling(ScalingMode m);
    ScalingMode scaling() const { return scaling_; }

    // The frame in the top half of the output: a portrait phone with a clip-on pad covering the
    // bottom. Set per frame, so unclipping the pad puts it back.
    void set_top_anchor(bool on) { topAnchor_ = on; }

    SDL_Window* window() const { return window_; }

    // For the shell's skin textures (after `open()`, destroyed before `close()`); pt-ui only draws
    // into the canvas.
    SDL_Renderer* renderer() const { return renderer_; }

    // Where the 640×480 frame lands, in output pixels; the touch panels go in the bars beside it.
    SDL_Rect frame_rect() const { return dest_rect(); }

    // The renderer's output size — the space `frame_rect()` and touch both work in.
    void output_size(int& w, int& h) const { SDL_GetRendererOutputSize(renderer_, &w, &h); }

    // The panel's refresh rate, 0 if unknown. ⚠️ The loop needs the real period: a deadline 0.67 ms
    // short of 60 Hz walks through the vblank and vsync adds every step to the wait. Asked live — a
    // window dragged to another monitor changes it.
    int refresh_hz() const {
        SDL_DisplayMode mode{};
        if (!window_ || SDL_GetCurrentDisplayMode(SDL_GetWindowDisplayIndex(window_), &mode) != 0)
            return 0;
        return mode.refresh_rate;
    }

    // Reported only; the loop's frame deadline paces the draw either way.
    bool vsync() const { return vsync_; }

private:
    bool     create_texture();
    SDL_Rect dest_rect() const;
    int      frame_top(int outH, int h) const;

    // The shared body: pixel gate, upload, clear → underlay → frame → overlay → flip.
    bool present_impl(const pt::ui::Canvas& canvas, uint32_t clearArgb, const SDL_Rect& dest,
                      const std::function<void(SDL_Renderer*)>& underlay,
                      const std::function<void(SDL_Renderer*)>& overlay, uint64_t overlaySig,
                      uint32_t modalScrimArgb, const SDL_Rect& scrimBounds);

    // One line naming the driver, panel, output size and letterbox.
    void describe() const;

    SDL_Window*   window_   = nullptr;
    SDL_Renderer* renderer_ = nullptr;
    SDL_Texture*  texture_  = nullptr;
    ScalingMode   scaling_  = ScalingMode::INTEGER;
    bool          topAnchor_ = false;
    bool          vsync_    = false;

    // The output size last described (0 until the first present, so boot prints once).
    int lastOutW_ = 0;
    int lastOutH_ = 0;

    // ── The idle-redraw net ──────────────────────────────────────────────────────────────────────
    // The last frame put on screen, so an identical one is dropped. ⚠️ The letterbox colour, the
    // rect, the overlay fingerprint and the scrim are compared too — each changes the screen outside
    // the canvas.
    std::vector<uint32_t> lastFrame_;
    uint32_t              lastLetterbox_ = 0;
    SDL_Rect              lastDest_      = {0, 0, 0, 0};
    bool                  haveLast_      = false;

    uint64_t              lastOverlaySig_ = 0;

    uint32_t              lastModalScrim_ = 0;
};

#endif  // POCKETTRACKER_SDL_VIDEO_H
