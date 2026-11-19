#include "ui/modules/song_editor.h"

#include "ui/helpers.h"

namespace pt::ui {

using songcore::Track;

namespace {

// Columns of the project name in the title; "SONG: " is extra.
constexpr int TITLE_MAX_CHARS = 20;

// A track's pitch: the marker's slot, a 2-char cell and a gap. The row number takes the same pitch.
constexpr int TRACK_PITCH = 3 * CHAR_W + 5;

// Track `i`'s cell, from the grid's left edge; the row number is at 0.
constexpr int track_x(int i) { return (i + 1) * TRACK_PITCH; }

}  // namespace

void SongEditorModule::draw(Canvas& c, int x, int y, const SongEditorState& s) const {
    const Theme& t = s.theme;

    c.fill_rect(x, y, WIDTH, HEIGHT, t.background);

    // The title carries the transport mode — LIVE changes what START does. Both words are four glyphs.
    c.text(c.cased(s.liveMode ? "LIVE: " : "SONG: ") + Canvas::clip_text(s.project.name, TITLE_MAX_CHARS),
           x + INSET, y + TEXT_PADDING, t.textTitle);

    // `cursorTrack` is 1-based, so it is the header's column index.
    for (int trackId = 0; trackId < 8; ++trackId) {
        c.text(std::to_string(trackId + 1), x + INSET + track_x(trackId), y + HEADER_Y + TEXT_PADDING,
               header_color(s.cursorTrack, trackId + 1, trackId + 1, t));
    }

    for (int rowIndex = 0; rowIndex < VISIBLE_ROWS; ++rowIndex)
        draw_row(c, x, y, rowIndex, s.scrollPosition + rowIndex, s);
}

void SongEditorModule::draw_row(Canvas& c, int x, int y, int row_index, int absolute_row,
                                const SongEditorState& s) const {
    const Theme& t = s.theme;

    const int textY = draw_grid_row(c, x, y, WIDTH, row_index, absolute_row, t);
    const int gridX = x + INSET;

    // Left to right: RowCells joins a selected run across the gutters only in layout order.
    RowCells cells(c, textY, t);

    // Column 0 is a gutter the cursor cannot reach; its number still lights on the cursor row.
    c.text(hex2(absolute_row), gridX, textY,
           row_number_ink(absolute_row, s.cursorRow, /*beats=*/true, t));

    for (int trackId = 0; trackId < 8; ++trackId) {
        const Track& track = s.project.tracks[static_cast<size_t>(trackId)];
        // chainRefs grows on demand; a row past its end is empty.
        const int chainId = (absolute_row < static_cast<int>(track.chainRefs.size()))
                                ? track.chainRefs[static_cast<size_t>(absolute_row)]
                                : -1;

        const bool isCursor   = (absolute_row == s.cursorRow) && (trackId == s.cursorTrack - 1);
        const bool isSelected = s.selectionMode && s.isCellSelected(absolute_row, trackId + 1);

        // A muted track draws dim.
        const Argb value_color = track_audible(s.project, trackId) ? t.textValue : t.textEmpty;

        const int cellX = gridX + track_x(trackId);
        cells.cell(chainId == -1 ? "--" : hex2(chainId), cellX, isCursor, isSelected,
                   /*is_empty=*/chainId == -1, value_color);

        // This track's marker; a stopped track answers −1. ⚠️ One glyph chosen before drawing —
        // `draw_text` does not erase. A LAUNCH draws on the row it will jump to; a STOP where the
        // channel is now.
        const int  markerX     = cellX - CHAR_W;
        const bool playingHere = s.playheads[trackId].songRow == absolute_row;
        const LiveQueue& q     = s.liveQueue[trackId];
        const bool queueLit    = s.liveMode && q.pending() && blink_on(s.blinkPhaseMs, q.immediate);

        if (queueLit && q.stop && playingHere)
            c.text("_", markerX, textY, t.textPlayhead);
        else if (playingHere || (queueLit && !q.stop && q.row == absolute_row))
            draw_playhead(c, markerX, textY, t);
    }
}

CursorContext SongEditorModule::cursor_context(const SongEditorState& s) const {
    if (s.cursorTrack < 1 || s.cursorTrack > 8) return cc::none();

    const Track& track = s.project.tracks[static_cast<size_t>(s.cursorTrack - 1)];
    const int    chainRef = (s.cursorRow < static_cast<int>(track.chainRefs.size()))
                                ? track.chainRefs[static_cast<size_t>(s.cursorRow)]
                                : -1;
    return cc::chain_ref(chainRef, /*can_create=*/true);
}

SongInputResult SongEditorModule::handle_input(songcore::Project& project, int cursor_row,
                                               int cursor_track, const InputAction& action) const {
    SongInputResult r;

    const int trackIndex = cursor_track - 1;
    if (trackIndex < 0 || trackIndex >= static_cast<int>(project.tracks.size())) return r;

    Track& track = project.tracks[static_cast<size_t>(trackIndex)];

    // The list grows only to the row written; the rows before it stay the empties they were.
    const auto grow_to_cursor = [&] {
        while (static_cast<int>(track.chainRefs.size()) <= cursor_row) track.chainRefs.push_back(-1);
    };

    // Past the end reads as empty.
    const auto ref_at_cursor = [&] {
        return cursor_row < static_cast<int>(track.chainRefs.size())
                   ? track.chainRefs[static_cast<size_t>(cursor_row)]
                   : -1;
    };
    const int before = ref_at_cursor();

    switch (action.type) {
        case ActionType::SET_VALUE:
            grow_to_cursor();
            track.chainRefs[static_cast<size_t>(cursor_row)] = action.value;
            r.hasChain        = true;
            r.lastEditedChain = action.value;
            break;

        case ActionType::DELETE:
            // Only touches a row the list has.
            if (cursor_row < static_cast<int>(track.chainRefs.size()))
                track.chainRefs[static_cast<size_t>(cursor_row)] = -1;
            break;


        default:
            break;
    }

    // A press that changes nothing is no edit: counting it is a phantom unsaved-work prompt.
    r.modified = (ref_at_cursor() != before);
    return r;
}

}  // namespace pt::ui
