// sdl-video.{h,cpp} — the window, the 640×480 streaming texture, and the scaler.
//
// The other half of the shell seam. `pt-ui` draws the whole app into a 640×480 ARGB framebuffer and
// knows nothing about a display; this file is the only code that does. It uploads that framebuffer to
// one streaming texture and blits it to the screen, which is the port plan's §4.5 video plan
// unchanged: "render the 640×480 design into a streaming texture; present with integer scale
// (default) or fit-stretch (setting)".
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
#include <vector>

namespace pt::ui {
class Canvas;
}

/**
 * INTEGER keeps pixels square — every design pixel becomes an N×N block, and the remainder is
 * letterboxed. FIT fills the screen and accepts non-square pixels.
 *
 * This is a real choice rather than a preference, and the reason is the device zoo: 640×480 is 1×
 * exactly on the RG35xx class, but a TrimUI Smart Pro is 1280×720 and an RGB30 is 720×720. On those,
 * INTEGER means 1× with thick borders (crisp, small) while FIT means a stretched, filtered image
 * (full-screen, soft). Neither is right for everyone, so it becomes a setting — with INTEGER as the
 * default, because a pixel-art tracker that has been pixel-perfect since it was born should not go
 * blurry the first time it meets a 720p handheld.
 */
enum class ScalingMode { INTEGER, FIT };

class SdlVideo {
public:
    /**
     * @param resizable  A WINDOWED host: the window gets `SDL_WINDOW_RESIZABLE` and opens at the
     *                   largest integer multiple of the design that fits the desktop, instead of at
     *                   `windowW × windowH` exactly.
     *
     * ⚠️⚠️ **ANDROID MUST PASS FALSE, AND IT IS NOT ABOUT RESIZING.** SDL feeds this flag straight
     * to `Android_JNI_SetOrientation` (SDL_androidwindow.c:52), and `SDLActivity.setOrientationBis`
     * branches on it: with no `SDL_HINT_ORIENTATIONS` set — and this shell sets none — a NON-resizable
     * window picks its orientation from `w > h`, giving `SCREEN_ORIENTATION_SENSOR_LANDSCAPE` for the
     * 640×480 design, while a RESIZABLE one becomes `SCREEN_ORIENTATION_FULL_USER` and the activity
     * is free to rotate into PORTRAIT. That would undo C4's pixel-exact 2× landscape window on a
     * phone, arriving through a flag whose name says nothing about orientation. Read out of the
     * vendored source, not remembered.
     */
    bool open(const char* title, int windowW, int windowH, bool fullscreen = false,
              bool resizable = false);
    void close();

    /**
     * Upload the canvas and present it — unless the result would be identical to what is already on
     * screen, in which case this does nothing but pace the frame. Returns true if it really presented.
     *
     * @param letterboxArgb  What the bars around the frame are painted (a `pt::ui::Argb`,
     *                       0xAARRGGBB). A PARAMETER rather than a setter on purpose: it is a pure
     *                       function of the live theme, so passing it makes a stale value
     *                       unrepresentable, where a `set_letterbox_colour` would be one more thing
     *                       every future present site has to remember to keep current.
     */
    bool present(const pt::ui::Canvas& canvas, uint32_t letterboxArgb);

    /** Pace a frame that drew nothing — see the .cpp. The app loop calls this when it skips the
     *  draw entirely, so a still screen costs the same wall-clock as a moving one. */
    void idle_frame();

    /** Recreates the texture: the sampling filter is fixed at creation time in SDL2, and the two
     *  modes want different ones (INTEGER → nearest, FIT → linear). */
    void        set_scaling(ScalingMode m);
    ScalingMode scaling() const { return scaling_; }

    SDL_Window* window() const { return window_; }

    /** False when the renderer gave us no vsync — `present` then paces the frame itself. */
    bool vsync() const { return vsync_; }

private:
    bool     create_texture();
    SDL_Rect dest_rect() const;

    /** One line naming the driver, the panel, the output size and the letterbox. See the .cpp. */
    void describe() const;

    static constexpr Uint64 FRAME_MS = 16;  // ~60 Hz, when we have to pace it ourselves

    SDL_Window*   window_   = nullptr;
    SDL_Renderer* renderer_ = nullptr;
    SDL_Texture*  texture_  = nullptr;
    ScalingMode   scaling_  = ScalingMode::INTEGER;
    bool          vsync_    = false;
    Uint64        lastPresentMs_ = 0;

    /** The last renderer output size `present` saw, so a change can re-`describe()` itself. Zero
     *  until the first present, which is what suppresses a duplicate line at boot. See the .cpp. */
    int lastOutW_ = 0;
    int lastOutH_ = 0;

    /** Pace a frame without presenting one. Shared by `present`'s skip path and `idle_frame`. */
    void pace();

    // ── The idle-redraw net (C7) ─────────────────────────────────────────────────────────────────
    // The last frame actually PUT ON SCREEN, so an identical one can be dropped. This is the C++
    // stand-in for Compose recomposition: Kotlin asks "did any observed state change?", and this asks
    // the same question one layer later, of the pixels themselves — which cannot miss a field.
    // ⚠️ The letterbox colour and the destination rect are part of the comparison, NOT just the
    // canvas: a theme change repaints the BARS without moving a single canvas pixel, and a window
    // resize moves the frame without changing it. Comparing the canvas alone would leave both stale.
    std::vector<uint32_t> lastFrame_;
    uint32_t              lastLetterbox_ = 0;
    SDL_Rect              lastDest_      = {0, 0, 0, 0};
    bool                  haveLast_      = false;
};

#endif  // POCKETTRACKER_SDL_VIDEO_H
