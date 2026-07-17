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
    bool open(const char* title, int windowW, int windowH, bool fullscreen = false);
    void close();

    /** Upload the canvas and present it. The whole frame, every time — see the note in the .cpp. */
    void present(const pt::ui::Canvas& canvas);

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
};

#endif  // POCKETTRACKER_SDL_VIDEO_H
