// sdl-video.{h,cpp} — the window, the 640×480 streaming texture, and the scaler.
//
// `pt-ui` draws the whole app into a 640×480 ARGB framebuffer and knows nothing about a display;
// this file uploads it to one streaming texture and blits it, at an integer or a fractional (FIT)
// scale — never stretched out of aspect (`ScalingMode`).
//
// GPU-optional on purpose. SDL_Renderer will take an accelerated backend when the device has one and
// fall back to software when it does not — and a single 640×480 blit is trivial either way. That
// sidesteps the classic PortMaster failure (a device whose GL blobs are missing or 32-bit-only, e.g.
// TrimUI's GE8300) with no code: there is no shader here to fail.

#ifndef POCKETTRACKER_SDL_VIDEO_H
#define POCKETTRACKER_SDL_VIDEO_H

#include <cmath>  // before <SDL.h> — see sdl-audio-engine.h (M_PI / C4005)

#include <SDL.h>

#include <cstdint>
#include <functional>
#include <vector>

namespace pt::ui {
class Canvas;
}

/**
 * INTEGER scales by a whole number — every design pixel becomes an N×N block. FIT scales by a
 * FRACTIONAL factor and filters the result. **Both preserve the 4:3 aspect ratio and letterbox the
 * remainder**; neither ever stretches the design out of shape.
 *
 * The device zoo makes it a real choice: 640×480 is 1× on the RG35xx class, but on a 1280×720 or
 * 720×720 screen INTEGER means 1× with thick borders (crisp, small) and FIT 1.5× (bigger, soft).
 * INTEGER is the default — a pixel-art tracker should not go blurry on a 720p handheld.
 *
 * ⚠️ On an output that IS a whole multiple of the design the two compute the same rect and differ
 * only in the sampling filter.
 */
enum class ScalingMode { INTEGER, FIT };

class SdlVideo {
public:
    /**
     * @param resizable  A WINDOWED host: the window gets `SDL_WINDOW_RESIZABLE` and opens at the
     *                   largest integer multiple of the design that fits the desktop, instead of at
     *                   `windowW × windowH` exactly.
     *
     * ⚠️⚠️ ANDROID MUST PASS FALSE, AND IT IS NOT ABOUT RESIZING: SDL passes this flag to
     * `Android_JNI_SetOrientation`, and with no `SDL_HINT_ORIENTATIONS` a non-resizable 640×480
     * window gets SENSOR_LANDSCAPE while a RESIZABLE one gets FULL_USER and may rotate into portrait.
     */
    bool open(const char* title, int windowW, int windowH, bool fullscreen = false,
              bool resizable = false);
    void close();

    /**
     * Upload the canvas and present it — unless the result would be identical to what is already on
     * screen, in which case this does nothing at all. Returns true if it really presented.
     *
     * @param letterboxArgb  What the bars around the frame are painted (a `pt::ui::Argb`,
     *                       0xAARRGGBB). A PARAMETER rather than a setter on purpose: it is a pure
     *                       function of the live theme, so passing it makes a stale value
     *                       unrepresentable, where a `set_letterbox_colour` would be one more thing
     *                       every future present site has to remember to keep current.
     * @param overlay        Drawn AFTER the frame and BEFORE the flip — the touch panels in the
     *                       letterbox bars. Empty on layouts with no on-screen controls.
     * @param overlaySig     A cheap fingerprint of what `overlay` draws (geometry + held buttons).
     *                       It joins the pixel gate: a press highlight is a change OUTSIDE the
     *                       compared canvas and would otherwise be skipped.
     */
    bool present(const pt::ui::Canvas& canvas, uint32_t letterboxArgb,
                 const std::function<void(SDL_Renderer*)>& overlay = {}, uint64_t overlaySig = 0,
                 uint32_t modalScrimArgb = 0);

    /**
     * Present the frame into an EXPLICIT rect, with a skin composited around it — the PORTRAIT2
     * device skin. The frame lands where the caller says (band 2's inner bezel), with chrome behind
     * it and buttons in front.
     *
     * @param clearArgb  What the whole output is cleared to first — the device CASING colour. Part
     *                   of the pixel gate, as `letterboxArgb` is for `present`.
     * @param frameDest  Where the 640×480 texture blits. NOT `dest_rect()` — see `PortraitSkin`.
     * @param underlay   Drawn after the clear and BEFORE the frame: the chrome bands, inner bezel.
     * @param overlay    Drawn AFTER the frame: the button cluster; highlights ride `overlaySig`.
     *
     * Shares `present`'s upload, pixel gate and pacing (one private `present_impl`).
     */
    bool present_skinned(const pt::ui::Canvas& canvas, uint32_t clearArgb, const SDL_Rect& frameDest,
                         const std::function<void(SDL_Renderer*)>& underlay,
                         const std::function<void(SDL_Renderer*)>& overlay, uint64_t overlaySig,
                         uint32_t modalScrimArgb = 0, const SDL_Rect& scrimBounds = {0, 0, 0, 0});

    /**
     * Force the NEXT present, whatever the pixels say — the frame on screen is no longer the one we
     * last put there.
     *
     * ⚠️⚠️ THE PIXEL GATE IS BLIND TO A SURFACE THE PLATFORM CLEARED BEHIND ITS BACK. `present` skips
     * a frame identical to `lastFrame_`, assuming the display still SHOWS it. Android blanks the
     * window on sleep→resume, so the redrawn identical frame would be skipped and the screen stay
     * black until a keypress. A re-expose and a renderer reset are the same shape.
     *
     * @param texture_lost  The renderer's DEVICE was reset — GL context loss on an Android resume — so
     *                      the streaming texture is gone with it and is recreated here, or the forced
     *                      present would upload into a dead texture. A plain re-expose does NOT lose it.
     */
    void invalidate_backbuffer(bool texture_lost);

    /** Recreates the texture: the sampling filter is fixed at creation time in SDL2, and the two
     *  modes want different ones (INTEGER → nearest, FIT → linear). */
    void        set_scaling(ScalingMode m);
    ScalingMode scaling() const { return scaling_; }

    /**
     * Put the frame in the TOP HALF of the output instead of the middle of it.
     *
     * ⚠️ For a phone held in PORTRAIT with a clip-on gamepad - a GameSir Pocket Taco, an
     * 8BitDo FlipPad - whose grips cover the bottom of the screen. A physical pad turns the
     * on-screen buttons off, so nothing else moves the frame out from behind it. Set per frame
     * from the same gate that decides the touch layout, so unclipping the pad or turning the
     * phone puts the frame back without a restart.
     */
    void set_top_anchor(bool on) { topAnchor_ = on; }

    SDL_Window* window() const { return window_; }

    /**
     * The renderer, for the shell's skin/texture layer to create its textures from (after `open()`,
     * destroyed before `close()`). Not for the frame path: pt-ui draws into the canvas only.
     */
    SDL_Renderer* renderer() const { return renderer_; }

    /**
     * The rect the 640×480 frame lands in, in renderer-output pixels. In landscape the centred frame
     * leaves a bar on each side, and those bars are where the touch panels go.
     */
    SDL_Rect frame_rect() const { return dest_rect(); }

    /** The renderer's output size in pixels — the coordinate space `frame_rect()` and the touch layer
     *  both work in (SDL finger events are normalised to it). */
    void output_size(int& w, int& h) const { SDL_GetRendererOutputSize(renderer_, &w, &h); }

    /**
     * The panel's refresh rate in Hz, or 0 when the platform will not say.
     *
     * ⚠️ The app loop needs the REAL period, not 16 ms: a deadline 0.67 ms short of a 60 Hz refresh
     * walks forward through the vblank a frame at a time, and with vsync every step of that walk is
     * added to the wait inside `SDL_RenderPresent`. Asked live, because a window dragged to a second
     * monitor lands on a different panel.
     */
    int refresh_hz() const {
        SDL_DisplayMode mode{};
        if (!window_ || SDL_GetCurrentDisplayMode(SDL_GetWindowDisplayIndex(window_), &mode) != 0)
            return 0;
        return mode.refresh_rate;
    }

    /** False when the renderer gave us no vsync. The app loop's frame deadline holds the draw to
     *  60 Hz either way — see THE TWO RATES in app.cpp — so this reports, it does not decide. */
    bool vsync() const { return vsync_; }

private:
    bool     create_texture();
    SDL_Rect dest_rect() const;
    int      frame_top(int outH, int h) const;

    /** The shared body of `present` and `present_skinned`: the pixel gate, the streaming-texture
     *  upload, and clear → underlay → frame → overlay → flip. The two differ only in where the frame
     *  lands (`dest`) and whether anything draws behind it (`underlay`). */
    bool present_impl(const pt::ui::Canvas& canvas, uint32_t clearArgb, const SDL_Rect& dest,
                      const std::function<void(SDL_Renderer*)>& underlay,
                      const std::function<void(SDL_Renderer*)>& overlay, uint64_t overlaySig,
                      uint32_t modalScrimArgb, const SDL_Rect& scrimBounds);

    /** One line naming the driver, the panel, the output size and the letterbox. See the .cpp. */
    void describe() const;

    SDL_Window*   window_   = nullptr;
    SDL_Renderer* renderer_ = nullptr;
    SDL_Texture*  texture_  = nullptr;
    ScalingMode   scaling_  = ScalingMode::INTEGER;
    bool          topAnchor_ = false;
    bool          vsync_    = false;

    /** The last renderer output size `present` saw, so a change can re-`describe()` itself. Zero
     *  until the first present, which is what suppresses a duplicate line at boot. See the .cpp. */
    int lastOutW_ = 0;
    int lastOutH_ = 0;

    // ── The idle-redraw net ──────────────────────────────────────────────────────────────────────
    // The last frame actually PUT ON SCREEN, so an identical one can be dropped — "did anything
    // change?" asked of the pixels themselves, which cannot miss a field.
    // ⚠️ The letterbox colour and the destination rect are part of the comparison: a theme change
    // repaints the BARS without moving a canvas pixel, and a resize moves the frame unchanged.
    std::vector<uint32_t> lastFrame_;
    uint32_t              lastLetterbox_ = 0;
    SDL_Rect              lastDest_      = {0, 0, 0, 0};
    bool                  haveLast_      = false;

    // The overlay fingerprint last presented — part of the gate above, so a touch highlight that
    // changes nothing in the canvas still forces the frame through. Zero when there is no overlay,
    // which is what makes the whole net a no-op on the platforms without one.
    uint64_t              lastOverlaySig_ = 0;

    // The modal-scrim colour last presented — part of the same gate: the scrim is drawn in the BARS,
    // outside the compared canvas.
    uint32_t              lastModalScrim_ = 0;
};

#endif  // POCKETTRACKER_SDL_VIDEO_H
