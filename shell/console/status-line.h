#ifndef POCKETTRACKER_SHELL_STATUS_LINE_H
#define POCKETTRACKER_SHELL_STATUS_LINE_H

// The console's once-a-second line: frame counter (the device calls back), playhead (the sequencer
// advances), voices (notes reach the engine) — whichever is stuck names the broken link on a box with
// no screen. `cpu`/`max` are the last second's mean and worst block; `late` counts every overrun
// block since launch, each one an audible dropout.

#include <cstdio>

#include "audio-engine.h"
#include "songcore/host.h"
#include "ui/app_state.h"
#include "ui/layout.h"   // screen_label

namespace ptshell {

// The frame loop's own counts: a working idle skip and a broken one look the same on screen.
struct LoopCounts {
    long long drawn = 0, presented = 0, skipped = 0, polls = 0;
};

inline void print_status_line(songcore::SongcoreHost& host, AudioEngine& engine, const pt::ui::AppState& state,
                              const LoopCounts& n) {
    const pt::ui::TrackPlayhead&   t0 = state.playheads[0];
    const AudioEngine::BlockTiming bt = engine.getBlockTiming();
    std::printf(
        "%s  frame %-10lld  song %3d  chain %2d  step %2d   voices %2d   %-10s cursor %X,%d"
        "   drew %lld skip %lld same %lld poll %lld   cpu %d.%d%% max %d.%d%% late %lld\n",
        host.is_playing() ? "play" : "stop",
        static_cast<long long>(engine.getCurrentFrame()), t0.songRow, t0.chainRow,
        t0.step, engine.getActiveVoiceCount(), pt::ui::screen_label(state.currentScreen),
        state.cursorRow, state.cursorColumn, n.presented, n.skipped, n.drawn - n.presented, n.polls,
        bt.meanLoad / 10, bt.meanLoad % 10, bt.worstLoad / 10, bt.worstLoad % 10,
        static_cast<long long>(bt.lateBlocks));
    std::fflush(stdout);  // block-buffered to a pipe otherwise
}

}  // namespace ptshell

#endif  // POCKETTRACKER_SHELL_STATUS_LINE_H
