// tools/blocktest — DEBUG diagnostic (untracked-intent, like tools/pttrace).
//
// Renders a .ptp through the REAL LIVE audio path — play_song() + poll() + processLiveBlock() — at a
// chosen audio-callback block size, and writes the result to a WAV. Comparing two runs at different
// block sizes isolates block-size-dependent processing inside processAudioBlock, with the live
// scheduler and engine otherwise identical.
//
// Why this exists: the device's ALSA callback is 940 frames (the Flip rounds 512 up); renderOffline
// chunks at 256. ptrender's "live == render" check drives BOTH sides through renderOffline (256), so
// the 940 path has never been exercised against 256. See docs/internal/port-parity-audit.md finding 3.
//
//   blocktest <ptp> <baseDir> <blockSize> <out.wav> [seconds]

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#include "audio-engine.h"
#include "songcore/host.h"
#include "songcore/render.h"
#include "songcore/wav_writer.h"

int main(int argc, char** argv) {
    if (argc < 5) {
        std::fprintf(stderr, "usage: %s <ptp> <baseDir> <blockSize> <out.wav> [seconds]\n", argv[0]);
        return 2;
    }
    const std::string ptp     = argv[1];
    const std::string baseDir = argv[2];
    const int         block   = std::atoi(argv[3]);
    const std::string outWav  = argv[4];
    const double      seconds = (argc > 5) ? std::atof(argv[5]) : 20.0;
    const int         subblk  = (argc > 6) ? std::atoi(argv[6]) : 0;  // >0: process each callback in subblk-sized pieces (simulates the fix)
    const int         pollSub = (argc > 7) ? std::atoi(argv[7]) : 0;  // 1: poll() before EVERY sub-chunk (isolates poll cadence)
    if (block <= 0) { std::fprintf(stderr, "bad block size\n"); return 2; }

    const int SR = 44100;
    auto engine = std::make_unique<AudioEngine>();
    engine->setDeviceSampleRate(SR);
    songcore::SongcoreHost host(engine.get(), SR);

    std::ifstream f(ptp, std::ios::binary);
    if (!f) { std::fprintf(stderr, "cannot open %s\n", ptp.c_str()); return 1; }
    std::string blob((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    if (!host.push_project(blob)) { std::fprintf(stderr, "push_project failed\n"); return 1; }
    host.load_media(baseDir);
    host.push_params();

    const songcore::SongBounds b = songcore::find_song_bounds(host.project());
    if (b.empty()) { std::fprintf(stderr, "empty song\n"); return 1; }

    // The live path, exactly as the SDL shell drives it: transport on, then poll()+processLiveBlock().
    engine->setOfflineRendering(false);
    host.play_song(b.startRow);

    songcore::WavStreamWriter writer(outWav, SR, 2);
    if (!writer.is_open()) { std::fprintf(stderr, "cannot open %s for write\n", outWav.c_str()); return 1; }

    const int64_t totalFrames = static_cast<int64_t>(seconds * SR);
    std::vector<float> buf(static_cast<size_t>(block) * 2);
    int64_t rendered = 0;
    while (rendered < totalFrames) {
        host.poll();   // main-thread lookahead scheduler (2 phrases ahead — cadence-immune)
        const int n = static_cast<int>(std::min<int64_t>(block, totalFrames - rendered));
        std::memset(buf.data(), 0, sizeof(float) * static_cast<size_t>(n) * 2);
        if (subblk <= 0) {
            engine->processLiveBlock(buf.data(), n, 2, static_cast<float>(SR));
        } else {
            // One callback of n frames, processed in subblk-sized pieces — what the fix makes
            // processLiveBlock do internally. poll() still runs once per CALLBACK, as on device.
            int off = 0;
            while (off < n) {
                const int sc = std::min(subblk, n - off);
                if (pollSub && off > 0) host.poll();   // poll per sub-chunk: isolates scheduler cadence
                engine->processLiveBlock(buf.data() + static_cast<size_t>(off) * 2, sc, 2, static_cast<float>(SR));
                off += sc;
            }
        }
        writer.append_interleaved(buf.data(), n);
        rendered += n;
    }
    writer.finish();
    std::fprintf(stderr, "OK %s  frames=%lld  block=%d  stillPlaying=%d\n",
                 outWav.c_str(), static_cast<long long>(rendered), block, static_cast<int>(host.is_playing()));
    return 0;
}
