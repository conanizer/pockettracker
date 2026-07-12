// PocketTracker — the SDL shell. Linux-port plan Phase 2.
//
// The first build of PocketTracker with no Kotlin in it at all. It is deliberately the smallest
// thing that can be true: open an audio device, hand songcore a .ptp, and play it. Everything that
// decides how the song SOUNDS — the sequencer, the effect resolution, the voices, the whole DSP
// chain — is the same C++ the APK ships, linked from native/ and reached through exactly one class:
//
//     songcore::SongcoreHost          (native/songcore/host.h)
//
// which is the same class songcore-jni.cpp marshals for Android. That is not a happy accident, it
// is the design, and both ends of it were written down before this file existed: host.h's header
// comment predicted it ("the SDL shell will construct the same class the same way"), and
// tools/ptrender already proved the call sequence end to end —
//
//     push_project(.ptp)  →  load_media(samples + SoundFonts)  →  play
//
// is precisely what ptrender does, with render_song_to_wav() where this has a live audio device.
// So the genuinely new code here is small, and it is all shell: a device, a window, a clock.
//
// ─── WHAT IS NOT HERE, AND IS NOT MISSING ────────────────────────────────────────────────────────
//
// The UI. The ~20 screen modules, the input mapping, the file browser and the 5×5 font are Phase 3
// of the port plan, and pretending otherwise here would be the wrong kind of progress. What this
// draws is a SMOKE-TEST readout — eight track blocks and a playhead strip, no font, no editing —
// and it exists only to make "is it actually playing?" answerable at a glance. It is meant to be
// deleted the day the real UI lands, and nothing else should be built on top of it.
//
//   pockettracker-sdl <project.ptp> [media-base-dir]
//
//     SPACE   play / stop
//     ESC     quit
//
// media-base-dir defaults to the project's own directory, which is what a portable project wants:
// the goldens store their sample paths relative to /testdata ("golden/kick.wav"), while a project
// saved on a device stores absolute paths and resolves the same either way (engine_setup.h).

// <cmath> before <SDL.h> — see the note in sdl-audio-engine.h (M_PI, _USE_MATH_DEFINES, C4005).
#include <cmath>

#define SDL_MAIN_HANDLED
#include <SDL.h>

#include "audio-engine.h"
#include "songcore/host.h"

#include "sdl-audio-engine.h"

#include <cstdio>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>

using namespace songcore;

namespace {

constexpr int WINDOW_W = 640;   // the design resolution the Compose UI draws at, so the future
constexpr int WINDOW_H = 480;   // Phase 3 port lands in a window that is already the right shape

bool read_file(const std::string& path, std::string& out) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    std::ostringstream ss;
    ss << f.rdbuf();
    out = ss.str();
    return true;
}

// The directory a path lives in — the default media base dir, because a project's relative sample
// paths are relative to the project file itself.
std::string dir_of(const std::string& path) {
    const size_t slash = path.find_last_of("/\\");
    return (slash == std::string::npos) ? std::string(".") : path.substr(0, slash);
}

// ─── the smoke-test readout (throwaway — see the header comment) ─────────────────────────────────
void draw(SDL_Renderer* r, AudioEngine& engine, const PlaybackPosition& pos, bool playing) {
    SDL_SetRenderDrawColor(r, 12, 12, 16, 255);
    SDL_RenderClear(r);

    // Eight track blocks, lit by the notes actually sounding right now. This is the live signal —
    // it is the engine's own voice bookkeeping, not the sequencer's — so if these blink in time
    // with what you hear, the whole path from .ptp to speaker is working.
    int active[8] = {0};
    engine.getTrackActiveNotes(active, 8);
    for (int t = 0; t < 8; ++t) {
        const bool lit = active[t] > 0;
        SDL_SetRenderDrawColor(r, lit ? 90 : 30, lit ? 220 : 40, lit ? 140 : 50, 255);
        SDL_Rect block{40 + t * 70, 120, 54, 54};
        SDL_RenderFillRect(r, &block);
    }

    // The phrase step, 0..15 — the fastest-moving playhead, so it is the one worth watching.
    for (int s = 0; s < 16; ++s) {
        const bool here = playing && s == pos.phraseStep;
        SDL_SetRenderDrawColor(r, here ? 235 : 40, here ? 200 : 40, here ? 70 : 48, 255);
        SDL_Rect cell{40 + s * 35, 240, 29, 20};
        SDL_RenderFillRect(r, &cell);
    }

    // Song row as a bar across the bottom — coarse, but it shows the song advancing.
    SDL_SetRenderDrawColor(r, 40, 40, 48, 255);
    SDL_Rect track{40, 320, 560, 10};
    SDL_RenderFillRect(r, &track);
    if (playing) {
        SDL_SetRenderDrawColor(r, 90, 160, 235, 255);
        SDL_Rect head{40 + (pos.songRow * 560) / 256, 316, 6, 18};
        SDL_RenderFillRect(r, &head);
    }

    SDL_RenderPresent(r);
}

}  // namespace

int main(int argc, char** argv) {
    SDL_SetMainReady();

    if (argc < 2) {
        std::fprintf(stderr, "usage: pockettracker-sdl <project.ptp> [media-base-dir]\n");
        return 2;
    }
    const std::string projectPath = argv[1];
    const std::string baseDir     = (argc > 2) ? argv[2] : dir_of(projectPath);

    std::string blob;
    if (!read_file(projectPath, blob)) {
        std::fprintf(stderr, "cannot read %s\n", projectPath.c_str());
        return 1;
    }

    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        std::fprintf(stderr, "SDL_Init(VIDEO) failed: %s\n", SDL_GetError());
        return 1;
    }

    // ⚠️ HEAP, not a local. AudioEngine's per-block DSP scratch, spectrum rings and 256-slot table
    // pool are members, and they blow a 1 MB stack instantly (0xC00000FD). ptrender hit this and so
    // will every future host; make_unique is the whole fix.
    auto engine = std::make_unique<AudioEngine>();

    SdlAudioEngine audio(engine.get());
    if (!audio.openStream()) {
        SDL_Quit();
        return 1;
    }

    // The engine asks for its stream back without knowing what a stream is (S5's
    // AudioEngine::onResumeRequested — the consumer calls it before scheduling a note). On Android
    // the shell wires this to Oboe's resumeStream; here, to the SDL device.
    engine->onResumeRequested = [&audio] { audio.resumeStream(); };

    SongcoreHost host(engine.get(), audio.sampleRate());

    if (!host.push_project(blob)) {
        std::fprintf(stderr, "%s did not parse as a .ptp\n", projectPath.c_str());
        SDL_Quit();
        return 1;
    }

    const MediaLoadResult media = host.load_media(baseDir);
    std::printf("project: %s\nmedia:   %d loaded, %d failed (base dir: %s)\n", projectPath.c_str(),
                media.loaded, media.failed, baseDir.c_str());
    if (media.failed > 0) {
        // Not fatal — the song still plays, just missing voices. Loud, because a project whose media
        // silently failed to open renders near-silence, and "no sound" would otherwise look like a
        // bug in the shell rather than a missing file.
        std::fprintf(stderr,
                     "warning: %d sample(s)/SoundFont(s) failed to load — those instruments will be "
                     "silent\n",
                     media.failed);
    }

    SDL_Window* window =
        SDL_CreateWindow("PocketTracker", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, WINDOW_W,
                         WINDOW_H, SDL_WINDOW_SHOWN);
    if (!window) {
        std::fprintf(stderr, "SDL_CreateWindow failed: %s\n", SDL_GetError());
        SDL_Quit();
        return 1;
    }
    // PRESENTVSYNC is what paces the loop — see the poll() call below, which wants ~60 Hz.
    SDL_Renderer* renderer =
        SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!renderer) {
        std::fprintf(stderr, "SDL_CreateRenderer failed: %s\n", SDL_GetError());
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }

    std::printf("\nSPACE play/stop   ESC quit\n\n");
    host.play_song(0);

    bool    running   = true;
    Uint64 lastStatus = 0;

    while (running) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_QUIT) {
                running = false;
            } else if (e.type == SDL_KEYDOWN && e.key.repeat == 0) {
                switch (e.key.keysym.sym) {
                    case SDLK_ESCAPE:
                        running = false;
                        break;
                    case SDLK_SPACE:
                        if (host.is_playing()) host.stop();
                        else host.play_song(0);
                        break;
                    default:
                        break;
                }
            }
        }

        // The lookahead pump. On Android, PixelPerfectRenderer's 60 Hz loop drives exactly this call
        // (and drove PlaybackController.updatePlaybackBuffer() before songcore existed). So the
        // transport here is not merely *like* the app's — it is the same code on the same cadence,
        // reading the same clock: the engine's frame counter, advanced by the audio callback.
        host.poll();

        const PlaybackPosition pos = host.playheads();
        draw(renderer, *engine, pos, host.is_playing());

        // A status line once a second. This is not decoration: on a headless box, over ssh, or
        // during a handheld bring-up where you cannot yet see or hear anything, these three numbers
        // are what tell you the chain is alive — the FRAME COUNTER means the audio device is calling
        // back, the PLAYHEAD means the sequencer is advancing, and VOICES means events are reaching
        // the engine and turning into sound. Any one of them stuck at zero names the broken link.
        const Uint64 now = SDL_GetTicks64();
        if (now - lastStatus >= 1000) {
            lastStatus = now;
            int active[8] = {0};
            engine->getTrackActiveNotes(active, 8);
            char tracks[9];
            for (int t = 0; t < 8; ++t) tracks[t] = active[t] > 0 ? '#' : '.';
            tracks[8] = '\0';
            std::printf("%s  frame %-10lld  song %3d  chain %2d  step %2d   voices %2d   [%s]\n",
                        host.is_playing() ? "play" : "stop",
                        static_cast<long long>(engine->getCurrentFrame()), pos.songRow, pos.chainRow,
                        pos.phraseStep, engine->getActiveVoiceCount(), tracks);
            std::fflush(stdout);   // block-buffered to a pipe otherwise, and then it says nothing
        }
    }

    // Stop before the device closes: stop() restores the master EQ and clears the engine's queues,
    // and it wants an engine that is still being pumped.
    host.stop();
    // Drop the resume hook before `audio` goes out of scope — it captures a reference to it, and the
    // engine outlives it (declared first, destroyed last).
    engine->onResumeRequested = nullptr;
    audio.closeStream();
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}
