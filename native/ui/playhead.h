#pragma once

// One track's playhead, refilled every frame. ⚠️ −1 is not row 0: it draws nothing. A marker is drawn
// where the screen's id matches the track's, never on a row number alone.

namespace pt::ui {

struct TrackPlayhead {
    int songRow  = -1;   // the row of this track's own song column
    int chainId  = -1;   // the chain that `chainRow` is a row OF
    int chainRow = -1;
    int phraseId = -1;   // the phrase that `step` is a step OF
    int step     = -1;
};

// LIVE mode's queue for one track, mirrored for drawing. ⚠️ `row < 0` without `stop` is an empty slot.
struct LiveQueue {
    int  row       = -1;      // the song row queued to launch on this channel
    bool stop      = false;   // …or this channel is queued to fall silent
    bool immediate = false;   // the next phrase boundary, not the next chain end — a FAST blink
    // Not yet committed to a frame. A scheduled-but-unheard launch still blinks (`pending()`), but a
    // second press treats it as committed.
    bool armed     = false;

    bool pending() const { return row >= 0 || stop; }
};

}  // namespace pt::ui
