#pragma once

// ─── SONG EDITOR ─────────────────────────────────────────────────────────────────────────────────
//
// The C++ twin of ui/modules/SongEditorModule.kt. The top-level view: chains arranged across 8 tracks,
// 256 rows deep, 16 visible at a time.
//
// Two things about it are unlike every other grid editor, and both are inherited deliberately:
//
//   • THE CURSOR COLUMN IS A TRACK, AND IT IS 1-BASED. `cursorTrack` runs 1..8 and indexes
//     `project.tracks[cursorTrack - 1]`. It is the SAME `AppState::cursorColumn` that PHRASE and CHAIN
//     use — the song screen simply reads it as a track number. Column 0 is the row-number gutter and
//     is not reachable, which is why the step cell below has no cursor colour at all.
//   • IT SCROLLS. 256 rows through a 16-row window, so a row has an `absoluteRow` (the data, the
//     cursor, the playhead) and a `rowIndex` (the pixels). Mixing them up is the one bug this module
//     is prone to; they are named apart everywhere below.

#include "songcore/model.h"
#include "ui/canvas.h"
#include "ui/cursor.h"
#include "ui/theme.h"

#include <functional>

namespace pt::ui {

struct SongEditorState {
    const songcore::Project& project;
    int  cursorRow      = 0;  // absolute (0..255), not the on-screen index
    int  cursorTrack    = 1;  // 1..8 — NOT 0..7
    int  scrollPosition = 0;
    bool isPlaying      = false;
    int  playbackRow    = 0;
    bool selectionMode  = false;
    std::function<bool(int, int)> isCellSelected = [](int, int) { return false; };
    Theme theme = theme_classic();
};

struct SongInputResult {
    bool modified        = false;
    bool hasChain        = false;  // Kotlin's `lastEditedChain: Int?`, minus the optional
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
