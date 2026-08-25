#pragma once

// ─── WHERE ONE TRACK IS ──────────────────────────────────────────────────────────────────────────
//
// The UI's copy of one track's playhead, refilled from songcore every frame. Eight of them: with
// independent song cursors "the song is on row 5" is not a fact anybody can state, so there is no
// single playhead field anywhere above this line.
//
// ⚠️ **−1 IS A REAL ANSWER AND IT IS NOT ROW 0.** A phrase played on its own is in no chain and in
// no song; a track whose song column has run out has stopped. Both cases must draw NOTHING, and a
// zero would draw a marker on the first row of a screen that is not playing at all — which is
// exactly the bug the row highlight had, because a highlight always lands on some row.
//
// ⚠️ **THE IDS ARE LOAD-BEARING, because a row number is not a place.** The CHAIN screen shows ONE
// chain and two tracks can be inside it at two different rows, while a third is inside a chain the
// screen is not showing. The marker is drawn where the id the screen is looking at matches the id
// the track is in — never on a row number alone.

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
// LIVE mode's queue, mirrored for the drawing layer exactly as `TrackPlayhead` mirrors a playback
// position — pt-ui reads back what the sequencer decided and never includes it.
//
// ⚠️ **`row < 0` WITHOUT `stop` IS AN EMPTY SLOT, AND IT IS NOT ROW 0** — the same rule as above, for
// the same reason: a stop queue carries no row at all, so "nothing queued" cannot be said by the row
// on its own, and a marker drawn on a zero would blink on the first row of a channel with nothing
// waiting.
struct LiveQueue {
    int  row       = -1;      // the song row queued to launch on this channel
    bool stop      = false;   // …or this channel is queued to fall silent
    bool immediate = false;   // at the next phrase boundary, not the next chain end — a FAST blink
    /**
     * The sequencer has not committed this to a frame yet.
     *
     * ⚠️ **THE MARKER AND THE SECOND PRESS ASK DIFFERENT QUESTIONS OF THE SAME SLOT.** A launch the
     * walk has scheduled but the transport has not reached is still WAITING to the eye — the marker
     * must keep blinking — while to a press it is already committed, and pulling it earlier would cut
     * short a chain the player is still hearing. `pending()` draws; `armed` promotes.
     */
    bool armed     = false;

    bool pending() const { return row >= 0 || stop; }
};

}  // namespace pt::ui
