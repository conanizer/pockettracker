#include "ui/modules/song_editor.h"

#include "ui/helpers.h"

namespace pt::ui {

using songcore::Track;

namespace {

/** How much of the project name the title row shows, in columns; the "SONG: " prefix is extra. */
constexpr int TITLE_MAX_CHARS = 20;

}  // namespace

void SongEditorModule::draw(Canvas& c, int x, int y, const SongEditorState& s) const {
    const Theme& t = s.theme;

    c.fill_rect(x, y, WIDTH, HEIGHT, t.background);

    // TRACK_PITCH is 56, not the 50 it was, and the six pixels bought one marker column per track:
    // eight of them have to fit ahead of eight cells on a 510px module. Track 7 now ends at 490 and
    // the editor clip is 499, so the widening spends the free space on the right and no more.
    // The step gutter takes the same pitch so track 0's marker clears the two-digit row number.
    constexpr int TRACK_PITCH = 30 + 26;
    int       colX  = x + 10;
    const int stepX = colX; colX += TRACK_PITCH;
    int       trackColumns[8];
    for (int i = 0; i < 8; ++i) { trackColumns[i] = colX; colX += TRACK_PITCH; }

    int rowY = y + TEXT_PADDING;
    // The status overlay (SAVED / LOADED / …) is drawn by the layout on the visualizer header, not
    // here — the title row stays put.
    // The title carries the transport mode, because nothing else can: LIVE changes what START means
    // on this screen, and a mode you have to press a button to discover is a mode that surprises you
    // mid-performance. Both words are four glyphs, so the project name neither shifts nor re-clips.
    c.draw_text(std::string(s.liveMode ? "LIVE: " : "SONG: ") +
                    Canvas::clip_text(s.project.name, TITLE_MAX_CHARS),
                x + 10, rowY, t.textTitle, CHAR_SPACING, FONT_SCALE);

    // The track number lights for the track the cursor is in — it is half of what the row highlight
    // used to say, the row number being the other half. `cursorTrack` is already 1-based, so it IS
    // the column index the header stands over.
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

    // The eight track cells, left to right — RowCells joins a selected run into one block across
    // the gutters, and it can only do that if the cells arrive in the order they are laid out.
    RowCells cells(c, textY, t);

    // No cell background: column 0 is a gutter the cursor cannot reach (cursorTrack starts at 1).
    // It still lights on the cursor row, though — with the row no longer painted, the row number is
    // the only thing saying which of 256 rows is being edited.
    c.draw_text(hex2(absolute_row), stepX, textY,
                (absolute_row == s.cursorRow) ? t.textCursor
                : (absolute_row % 4 == 0)     ? t.textParam
                                              : t.textEmpty,
                CHAR_SPACING, FONT_SCALE);

    for (int trackId = 0; trackId < 8; ++trackId) {
        const Track& track = s.project.tracks[static_cast<size_t>(trackId)];
        // chainRefs is a GROWING list (`mutableListOf()`), not a fixed 256 — an untouched track is
        // empty, and a row past its end is empty rather than out of bounds.
        const int chainId = (absolute_row < static_cast<int>(track.chainRefs.size()))
                                ? track.chainRefs[static_cast<size_t>(absolute_row)]
                                : -1;

        const bool isCursor   = (absolute_row == s.cursorRow) && (trackId == s.cursorTrack - 1);
        const bool isSelected = s.selectionMode && s.isCellSelected(absolute_row, trackId + 1);

        // A track making no sound draws its chain numbers in the EMPTY colour, so the arrangement
        // reads as what you are hearing rather than as what is written down. The predicate is the
        // audible one, not `mute`, which is what gives SOLO a display for free: solo one channel and
        // the other seven dim, because they are the ones that stopped. Cursor and selection colours
        // still win inside the painter, so the cursor is never lost on a muted channel.
        const Argb value_color = track_audible(s.project, trackId) ? t.textValue : t.textEmpty;

        cells.cell(chainId == -1 ? "--" : hex2(chainId), trackColumns[trackId], isCursor, isSelected,
                   /*is_empty=*/chainId == -1, value_color);

        // This track and no other. Eight cursors means eight answers, and a track that has stopped
        // (or is only lending its number to a PHRASE being auditioned) answers −1 and gets nothing
        // drawn — which is the whole reason the row highlight had to go.
        // ⚠️ ONE GLYPH IN ONE COLUMN, decided before anything is drawn. The marker column is a single
        // character wide, and `draw_text` paints without erasing — so a stop queue drawn "over" the
        // playhead it replaces superimposes `_` on `>` and reads as neither.
        //
        // ⚠️ THE TWO QUEUE MARKERS SIT IN DIFFERENT PLACES, because they answer different questions.
        // A LAUNCH is drawn on the row it will jump to, so the blinking marker walks ahead of the
        // playhead to the cell you aimed at. A STOP has no target row at all — it can only be drawn
        // where the channel is now, in place of the `>` it is about to end.
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

    // The list only grows to reach the row being written — an edit at row 200 does not materialise
    // rows 0..199 as data, it materialises them as the empties they already were.
    const auto grow_to_cursor = [&] {
        while (static_cast<int>(track.chainRefs.size()) <= cursor_row) track.chainRefs.push_back(-1);
    };

    // A row past the end of the list reads as empty, which is what it is.
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

    // ⚠️ **`modified` IS A BEFORE/AFTER ANSWER, NOT "AN ACTION WAS DISPATCHED"**, and the difference
    // is not cosmetic. A chain-ref cell reports `canDelete` even when it is EMPTY: `cc::chain_ref`
    // hands `hex_byte` a 0 in place of the -1 sentinel, so `is_empty` is computed as `0 == -1`. That
    // is Kotlin's behaviour and the golden pins it in 90 cases, so it is the cell that stays and this
    // that changes. A+B on an empty cell therefore dispatches DELETE and writes -1 over -1 — and
    // derived from the action's TYPE that read as an edit, bumping the dirty counter: EXIT then asks
    // about unsaved work nobody did, and three seconds later the autosave lands, so the next launch
    // offers RECOVER WORK? for a project the user only looked at.
    r.modified = (ref_at_cursor() != before);
    return r;
}

}  // namespace pt::ui
