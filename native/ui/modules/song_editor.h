#pragma once

// ─── SONG EDITOR ─────────────────────────────────────────────────────────────────────────────────
//
// Chains across 8 tracks, 256 rows deep, 16 visible. Unlike the other grid editors:
//
//   • The cursor column is a TRACK, 1-based (`cursorTrack` 1..8 → `tracks[cursorTrack - 1]`). Column 0
//     is the row-number gutter and is not reachable.
//   • It SCROLLS, so a row has an `absoluteRow` (data, cursor, playhead) and a `rowIndex` (pixels).
//     Mixing them up is this module's one likely bug.

#include "songcore/model.h"
#include "ui/canvas.h"
#include "ui/cursor.h"
#include "ui/playhead.h"
#include "ui/theme.h"

#include <functional>

namespace pt::ui {

struct SongEditorState {
    const songcore::Project& project;
    int  cursorRow      = 0;  // absolute (0..255), not the on-screen index
    int  cursorTrack    = 1;  // 1..8 — NOT 0..7
    int  scrollPosition = 0;
    // Eight markers, one per track (ui/playhead.h) — a track with no position draws none.
    TrackPlayhead playheads[8] = {};
    // LIVE mode's launcher: what each channel is waiting to do, and the blink phase — handed in rather
    // than read from a clock, so a tool can draw it deterministically.
    bool      liveMode     = false;
    LiveQueue liveQueue[8] = {};
    int       blinkPhaseMs = 0;
    bool selectionMode  = false;
    std::function<bool(int, int)> isCellSelected = [](int, int) { return false; };
    Theme theme = theme_classic();
};

struct SongInputResult {
    bool modified        = false;
    bool hasChain        = false;
    int  lastEditedChain = 0;
};

class SongEditorModule {
public:
    static constexpr int WIDTH        = 510;
    static constexpr int HEIGHT       = 392;
    static constexpr int VISIBLE_ROWS = 16;

    void draw(Canvas& c, int x, int y, const SongEditorState& s) const;

    CursorContext cursor_context(const SongEditorState& s) const;

    SongInputResult handle_input(songcore::Project& project, int cursor_row, int cursor_track,
                                 const InputAction& action) const;

private:
    void draw_row(Canvas& c, int x, int y, int row_index, int absolute_row,
                  const SongEditorState& s, int stepX, const int* trackColumns) const;
};

}  // namespace pt::ui
