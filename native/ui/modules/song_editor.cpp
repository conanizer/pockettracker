#include "ui/modules/song_editor.h"

#include "ui/helpers.h"

namespace pt::ui {

using songcore::Track;

namespace {

// Columns of the project name in the title; "SONG: " is extra.
constexpr int TITLE_MAX_CHARS = 20;

}  // namespace

void SongEditorModule::draw(Canvas& c, int x, int y, const SongEditorState& s) const {
    const Theme& t = s.theme;

    c.fill_rect(x, y, WIDTH, HEIGHT, t.background);

    // 56 px per track: a marker column ahead of each cell. The step gutter takes the same pitch.
    constexpr int TRACK_PITCH = 30 + 26;
    int       colX  = x + 10;
    const int stepX = colX; colX += TRACK_PITCH;
    int       trackColumns[8];
    for (int i = 0; i < 8; ++i) { trackColumns[i] = colX; colX += TRACK_PITCH; }

    int rowY = y + TEXT_PADDING;
    // The title carries the transport mode — LIVE changes what START does. Both words are four glyphs.
    c.draw_text(std::string(s.liveMode ? "LIVE: " : "SONG: ") +
                    Canvas::clip_text(s.project.name, TITLE_MAX_CHARS),
                x + 10, rowY, t.textTitle, CHAR_SPACING, FONT_SCALE);

    // `cursorTrack` is 1-based, so it is the header's column index.
    rowY = y + ROW_HEIGHT + 14 + TEXT_PADDING;
    for (int trackId = 0; trackId < 8; ++trackId) {
        c.draw_text(std::to_string(trackId + 1), trackColumns[trackId], rowY,
                    header_color(s.cursorTrack, trackId + 1, trackId + 1, t), CHAR_SPACING,
                    FONT_SCALE);
    }

    for (int rowIndex = 0; rowIndex < VISIBLE_ROWS; ++rowIndex) {
        draw_row(c, x, y, rowIndex, s.scrollPosition + rowIndex, s, stepX, trackColumns);
    }
}

void SongEditorModule::draw_row(Canvas& c, int x, int y, int row_index, int absolute_row,
                                const SongEditorState& s, int stepX,
                                const int* trackColumns) const {
    const Theme& t = s.theme;

    const int dataRowY = y + ROW_HEIGHT + 14 + ROW_HEIGHT + (row_index * ROW_HEIGHT);

    c.fill_rect(x, dataRowY, WIDTH, ROW_HEIGHT, row_bg_color(absolute_row, t));

    const int textY = dataRowY + TEXT_PADDING;

    // Left to right: RowCells joins a selected run across the gutters only in layout order.
    RowCells cells(c, textY, t);

    // Column 0 is a gutter the cursor cannot reach; its number still lights on the cursor row.
    c.draw_text(hex2(absolute_row), stepX, textY,
                (absolute_row == s.cursorRow) ? cursor_mark_ink(t)
                : (absolute_row % 4 == 0)     ? t.textParam
                                              : t.textEmpty,
                CHAR_SPACING, FONT_SCALE);

    for (int trackId = 0; trackId < 8; ++trackId) {
        const Track& track = s.project.tracks[static_cast<size_t>(trackId)];
        // chainRefs grows on demand; a row past its end is empty.
        const int chainId = (absolute_row < static_cast<int>(track.chainRefs.size()))
                                ? track.chainRefs[static_cast<size_t>(absolute_row)]
                                : -1;

        const bool isCursor   = (absolute_row == s.cursorRow) && (trackId == s.cursorTrack - 1);
        const bool isSelected = s.selectionMode && s.isCellSelected(absolute_row, trackId + 1);

        // A track making no sound draws dim, so SOLO dims the other seven for free.
        const Argb value_color = track_audible(s.project, trackId) ? t.textValue : t.textEmpty;

        cells.cell(chainId == -1 ? "--" : hex2(chainId), trackColumns[trackId], isCursor, isSelected,
                   /*is_empty=*/chainId == -1, value_color);

        // This track's marker; a stopped track answers −1. ⚠️ One glyph chosen before drawing —
        // `draw_text` does not erase. A LAUNCH draws on the row it will jump to; a STOP where the
        // channel is now.
        const int  markerX     = trackColumns[trackId] - CHAR_W;
        const bool playingHere = s.playheads[trackId].songRow == absolute_row;
        const LiveQueue& q     = s.liveQueue[trackId];
        const bool queueLit    = s.liveMode && q.pending() && blink_on(s.blinkPhaseMs, q.immediate);

        if (queueLit && q.stop && playingHere)
            c.draw_text("_", markerX, textY, t.textPlayhead, CHAR_SPACING, FONT_SCALE);
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

        case ActionType::INSERT_DEFAULT:
            grow_to_cursor();
            track.chainRefs[static_cast<size_t>(cursor_row)] = 0;
            r.hasChain        = true;
            r.lastEditedChain = 0;
            break;

        default:
            break;
    }

    // ⚠️ `modified` compares before and after: A+B on an empty ref writes −1 over −1, and counting
    // that is a phantom unsaved-work prompt.
    r.modified = (ref_at_cursor() != before);
    return r;
}

}  // namespace pt::ui
