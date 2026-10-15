#pragma once

// Copy / cut / paste / delete over a selection on PHRASE, CHAIN, SONG and TABLE. A flat list of items,
// each one cell (row, column, one field); paste re-anchors by column offset and skips absent fields.
// Cut is copy then `delete_*`, where each field's empty value lives.

#include <optional>
#include <string>
#include <vector>

#include "songcore/model.h"
#include "ui/screen.h"

namespace pt::ui {

enum class ClipboardType { PHRASE_STEPS, CHAIN_ROWS, SONG_CELLS, TABLE_ROWS };

// Columns: 1 note, 2 velocity, 3 instrument, 4/6/8 FX type, 5/7/9 FX value.
struct PhraseStepClipItem {
    int row    = 0;  // within the selection
    int column = 0;  // the source column
    std::optional<songcore::Note> note;
    std::optional<int> volume;
    std::optional<int> instrument;
    std::optional<int> fxType;
    std::optional<int> fxValue;
};

// Columns: 1 phrase, 2 transpose.
struct ChainRowClipItem {
    int row    = 0;
    int column = 0;
    std::optional<int> phraseRef;
    std::optional<int> transpose;
};

// The column is the track, 1..8.
struct SongCellClipItem {
    int row      = 0;
    int column   = 0;
    int chainRef = -1;
};

// Columns: 1 transpose, 2 volume, 3/5/7 FX type, 4/6/8 FX value.
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
        NO_CLIPBOARD,
        SUCCESS,
        WRONG_SCREEN   // PHRASE data onto CHAIN, say
    };
    Kind kind        = Kind::NO_CLIPBOARD;
    int  itemsPasted = 0;
};

// Only the vector named by `type_` is ever non-empty.
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

    // Paste at the cursor into item `targetId` (ignored on SONG). The type must match the screen.
    PasteResult paste(songcore::Project& p, ScreenType target, int targetId, int cursorRow,
                      int cursorColumn);

    // ── Cut: copy + delete; returns the cells cleared ────────────────────────────────────────────
    int cut_phrase_steps(songcore::Project& p, int phraseId, int startRow, int startColumn,
                         int endRow, int endColumn);
    int cut_chain_rows(songcore::Project& p, int chainId, int startRow, int startColumn, int endRow,
                       int endColumn);
    int cut_song_cells(songcore::Project& p, int startRow, int startColumn, int endRow,
                       int endColumn);
    int cut_table_rows(songcore::Project& p, int tableId, int startRow, int startColumn, int endRow,
                       int endColumn);

    // ── Delete: A+B over a selection; the clipboard untouched ────────────────────────────────────
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

    // "PHR:2x3", or "" when empty.
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

    // Empty every list and take on `type` and its size; the copy then fills its own list.
    void begin_copy(ClipboardType type, int width, int height);

    PasteResult paste_phrase_steps(songcore::Project& p, int phraseId, int cursorRow,
                                   int cursorColumn);
    PasteResult paste_chain_rows(songcore::Project& p, int chainId, int cursorRow,
                                 int cursorColumn);
    PasteResult paste_song_cells(songcore::Project& p, int cursorRow, int cursorColumn);
    PasteResult paste_table_rows(songcore::Project& p, int tableId, int cursorRow,
                                 int cursorColumn);
};

}  // namespace pt::ui
