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

}  // namespace pt::ui
