#pragma once

// ─── The clipboard ───────────────────────────────────────────────────────────────────────────────
//
// Copy / cut / paste / delete over a rectangular selection on PHRASE, CHAIN, SONG and TABLE.
//
// It holds a FLAT LIST of items, not a rectangle: each carries its (row, column) and exactly ONE field.
// Paste re-anchors by column OFFSET from the leftmost copied, so an FX type+value pair pasted on
// column 6 lands type on 6 and value on 7. An absent field is SKIPPED on paste, not written as zero —
// pasting note-column cells leaves velocity, instrument and FX untouched.
// ⚠️ Cut is copy-then-delete, delegating to `delete_*`, because the "empty" values differ per field
// and must live in one place.

#include <optional>
#include <string>
#include <vector>

#include "songcore/model.h"
#include "ui/screen.h"

namespace pt::ui {

enum class ClipboardType { PHRASE_STEPS, CHAIN_ROWS, SONG_CELLS, TABLE_ROWS };

/** One copied phrase cell. Columns: 1=note 2=velocity 3=instrument 4/6/8=FX type 5/7/9=FX value. */
struct PhraseStepClipItem {
    int row    = 0;  // row WITHIN the selection (0-based), not within the phrase
    int column = 0;  // the source column, kept so paste can re-anchor by offset
    std::optional<songcore::Note> note;
    std::optional<int> volume;
    std::optional<int> instrument;
    std::optional<int> fxType;
    std::optional<int> fxValue;
};

/** One copied chain cell. Columns: 1=phraseRef 2=transpose. */
struct ChainRowClipItem {
    int row    = 0;
    int column = 0;
    std::optional<int> phraseRef;
    std::optional<int> transpose;
};

/** One copied song cell. Column IS the track number, 1-based (1..8). */
struct SongCellClipItem {
    int row      = 0;
    int column   = 0;
    int chainRef = -1;  // not optional: a song cell is only ever a chain ref, −1 for empty
};

/** One copied table cell. Columns: 1=transpose 2=volume 3/5/7=FX type 4/6/8=FX value. */
struct TableRowClipItem {
    int row    = 0;
    int column = 0;
    std::optional<int> transpose;
    std::optional<int> volume;
    std::optional<int> fxType;
    std::optional<int> fxValue;
};

struct PasteResult {
    enum class Kind {
        NO_CLIPBOARD,  // nothing has been copied yet
        SUCCESS,
        WRONG_SCREEN   // e.g. PHRASE data onto CHAIN
    };
    Kind kind        = Kind::NO_CLIPBOARD;
    int  itemsPasted = 0;
};

/** One instance lives in the dispatcher. Only the vector named by `type_` is ever non-empty. */
class Clipboard {
  public:
    // ── Copy ─────────────────────────────────────────────────────────────────────────────────────
    void copy_phrase_steps(const songcore::Project& p, int phraseId, int startRow, int startColumn,
                           int endRow, int endColumn);
    void copy_chain_rows(const songcore::Project& p, int chainId, int startRow, int startColumn,
                         int endRow, int endColumn);
    void copy_song_cells(const songcore::Project& p, int startRow, int startColumn, int endRow,
                         int endColumn);
    void copy_table_rows(const songcore::Project& p, int tableId, int startRow, int startColumn,
                         int endRow, int endColumn);

    /**
     * Paste at the cursor. `target_id` is the phrase/chain/table being edited (ignored for SONG).
     * The type must match the screen — there is no meaningful conversion, and inventing one would
     * destroy data (WRONG_SCREEN).
     */
    PasteResult paste(songcore::Project& p, ScreenType target, int targetId, int cursorRow,
                      int cursorColumn);

    // ── Cut (copy + delete) — returns the number of cells cleared ─────────────────────────────────
    int cut_phrase_steps(songcore::Project& p, int phraseId, int startRow, int startColumn,
                         int endRow, int endColumn);
    int cut_chain_rows(songcore::Project& p, int chainId, int startRow, int startColumn, int endRow,
                       int endColumn);
    int cut_song_cells(songcore::Project& p, int startRow, int startColumn, int endRow,
                       int endColumn);
    int cut_table_rows(songcore::Project& p, int tableId, int startRow, int startColumn, int endRow,
                       int endColumn);

    // ── Delete (clear, without touching the clipboard) — A+B over a selection ─────────────────────
    int delete_phrase_steps(songcore::Project& p, int phraseId, int startRow, int startColumn,
                            int endRow, int endColumn);
    int delete_chain_rows(songcore::Project& p, int chainId, int startRow, int startColumn,
                          int endRow, int endColumn);
    int delete_song_cells(songcore::Project& p, int startRow, int startColumn, int endRow,
                          int endColumn);
    int delete_table_rows(songcore::Project& p, int tableId, int startRow, int startColumn,
                          int endRow, int endColumn);

    // ── Utility ──────────────────────────────────────────────────────────────────────────────────
    void clear();
    bool has_data() const { return hasData_; }

    /** The top-strip readout: "PHR:2x3". "" when empty. */
    std::string info() const;

    ClipboardType type() const { return type_; }
    int width() const { return width_; }
    int height() const { return height_; }

  private:
    bool          hasData_ = false;
    ClipboardType type_    = ClipboardType::PHRASE_STEPS;
    int           width_   = 0;
    int           height_  = 0;

    std::vector<PhraseStepClipItem> phraseItems_;
    std::vector<ChainRowClipItem>   chainItems_;
    std::vector<SongCellClipItem>   songItems_;
    std::vector<TableRowClipItem>   tableItems_;

    PasteResult paste_phrase_steps(songcore::Project& p, int phraseId, int cursorRow,
                                   int cursorColumn);
    PasteResult paste_chain_rows(songcore::Project& p, int chainId, int cursorRow,
                                 int cursorColumn);
    PasteResult paste_song_cells(songcore::Project& p, int cursorRow, int cursorColumn);
    PasteResult paste_table_rows(songcore::Project& p, int tableId, int cursorRow,
                                 int cursorColumn);
};

}  // namespace pt::ui
