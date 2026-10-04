#pragma once

// ─── WHERE ONE TRACK IS ──────────────────────────────────────────────────────────────────────────
//
// The UI's copy of one track's playhead, refilled from songcore every frame — eight of them; there is
// no single song position.
// ⚠️ −1 is a real answer and NOT row 0: a phrase played alone is in no chain or song, and a track
// whose column ran out has stopped. Both draw NOTHING.
// ⚠️ The ids are load-bearing: a marker is drawn where the screen's id matches the track's, never on a
// row number alone (two tracks can be in one chain at different rows).

namespace pt::ui {

struct TrackPlayhead {
    int songRow  = -1;   // the row of this track's own song column
    int chainId  = -1;   // the chain that `chainRow` is a row OF
    int chainRow = -1;
    int phraseId = -1;   // the phrase that `step` is a step OF
    int step     = -1;
};

// ─── …AND WHAT ONE TRACK IS WAITING TO DO ────────────────────────────────────────────────────────
//
// LIVE mode's queue, mirrored for drawing — pt-ui never includes the sequencer.
// ⚠️ `row < 0` without `stop` is an EMPTY slot, not row 0 (a stop queue carries no row).
struct LiveQueue {
    int  row       = -1;      // the song row queued to launch on this channel
    bool stop      = false;   // …or this channel is queued to fall silent
    bool immediate = false;   // the next phrase boundary, not the next chain end — a FAST blink
    /**
     * The sequencer has not committed this to a frame yet. ⚠️ The marker and a second press ask
     * different questions: a scheduled-but-unheard launch still BLINKS (`pending()`), but to a press it
     * is committed — pulling it earlier would cut a chain the player still hears (`armed` promotes).
     */
    bool armed     = false;

    bool pending() const { return row >= 0 || stop; }
};

}  // namespace pt::ui
