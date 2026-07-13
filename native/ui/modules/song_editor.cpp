#include "ui/modules/song_editor.h"

#include "ui/helpers.h"

namespace pt::ui {

using songcore::Track;

void SongEditorModule::draw(Canvas& c, int x, int y, const SongEditorState& s) const {
    const Theme& t = s.theme;

    c.fill_rect(x, y, WIDTH, HEIGHT, t.background);

    int       colX  = x + 10;
    const int stepX = colX; colX += 30 + 20;
    int       trackColumns[8];
    for (int i = 0; i < 8; ++i) { trackColumns[i] = colX; colX += 30 + 20; }

    int rowY = y + TEXT_PADDING;
    // The status overlay (SAVED / LOADED / …) is drawn by the layout on the visualizer header, not
    // here — the title row stays put.
    c.draw_text("SONG: " + s.project.name.substr(0, 20), x + 10, rowY, t.textTitle, CHAR_SPACING,
                FONT_SCALE);

    rowY = y + ROW_HEIGHT + 14 + TEXT_PADDING;
    for (int trackId = 0; trackId < 8; ++trackId) {
        c.draw_text(std::to_string(trackId + 1), trackColumns[trackId], rowY, t.textParam,
                    CHAR_SPACING, FONT_SCALE);
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

    bool isRowSelected = false;
    if (s.selectionMode) {
        for (int col = 1; col <= 8 && !isRowSelected; ++col)
            isRowSelected = s.isCellSelected(absolute_row, col);
    }

    c.fill_rect(x, dataRowY, WIDTH, ROW_HEIGHT,
                row_bg_color(absolute_row, s.cursorRow, s.playbackRow, s.isPlaying, isRowSelected, t));

    const int textY = dataRowY + TEXT_PADDING;

    // No cursor colour: column 0 is a gutter the cursor cannot reach (cursorTrack starts at 1).
    c.draw_text(hex2(absolute_row), stepX, textY,
                (absolute_row % 4 == 0) ? t.textParam : t.textEmpty, CHAR_SPACING, FONT_SCALE);

    for (int trackId = 0; trackId < 8; ++trackId) {
        const Track& track = s.project.tracks[static_cast<size_t>(trackId)];
        // chainRefs is a GROWING list (`mutableListOf()`), not a fixed 256 — an untouched track is
        // empty, and a row past its end is empty rather than out of bounds.
        const int chainId = (absolute_row < static_cast<int>(track.chainRefs.size()))
                                ? track.chainRefs[static_cast<size_t>(absolute_row)]
                                : -1;

        const bool isCursor   = (absolute_row == s.cursorRow) && (trackId == s.cursorTrack - 1);
        const bool isSelected = s.selectionMode && s.isCellSelected(absolute_row, trackId + 1);

        draw_cell(c, chainId == -1 ? "--" : hex2(chainId), trackColumns[trackId], textY, isCursor,
                  isSelected, /*is_empty=*/chainId == -1, t.textValue, t);
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

    // The list only grows to reach the row being written — an edit at row 200 does not materialise
    // rows 0..199 as data, it materialises them as the empties they already were.
    const auto grow_to_cursor = [&] {
        while (static_cast<int>(track.chainRefs.size()) <= cursor_row) track.chainRefs.push_back(-1);
    };

    switch (action.type) {
        case ActionType::SET_VALUE:
            grow_to_cursor();
            track.chainRefs[static_cast<size_t>(cursor_row)] = action.value;
            r.hasChain        = true;
            r.lastEditedChain = action.value;
            break;

        case ActionType::DELETE:
            // clearSongChainRef(): only touches a row the list actually has.
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

    r.modified = (action.type != ActionType::NONE);
    return r;
}

}  // namespace pt::ui
