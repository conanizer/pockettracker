#pragma once

// ─── The clipboard ───────────────────────────────────────────────────────────────────────────────
//
// A 1:1 port of core/logic/ClipboardManager.kt. Copy / cut / paste / delete over a rectangular
// selection, on each of the four screens that have one (PHRASE, CHAIN, SONG, TABLE).
//
// The design is Kotlin's and it is worth spelling out, because it is not the obvious one: the
// clipboard does NOT hold a rectangle of cells. It holds a FLAT LIST of items, each carrying its own
// (row, column) and exactly ONE populated field. A copy of PHRASE columns 4..5 therefore produces
// items that remember they were an FX *type* and an FX *value* — and a paste re-anchors them by
// column OFFSET from the leftmost column copied, so pasting that pair onto column 6 lands the type on
// 6 and the value on 7, not two types side by side. A rectangle of raw ints could not do that.
//
// The consequence, and it is the reason `std::optional` appears at all: an item whose field is absent
// is SKIPPED on paste rather than written as zero (`item.note?.let { … }` in Kotlin). Pasting a
// note-column selection over a phrase leaves every velocity, instrument and FX byte in it untouched.
//
// ⚠️ Cut is copy-then-delete, and delegates to `delete_*` for the clearing rather than clearing
// inline. That is deliberate in the Kotlin ("Delete owns the clearing logic so cut and delete can
// never diverge") and the empty-value conventions are exactly what would diverge: clearing a phrase
// velocity means 0x7F, clearing a table volume means −1, clearing a chain ref means −1, and clearing
// a transpose means 0x00. Four different "empty"s, one place that knows them.

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
        WRONG_SCREEN   // PHRASE data cannot be pasted onto CHAIN — the only error Kotlin can return
    };
    Kind kind        = Kind::NO_CLIPBOARD;
    int  itemsPasted = 0;
};

/**
 * `ClipboardManager`. One instance lives in the dispatcher, exactly as one lives in MainActivity.
 *
 * Kotlin holds `data: Any` and casts on the way out; the four typed vectors here say the same thing
 * without the cast, and only the one named by `type_` is ever non-empty.
 */
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
     *
     * The type must match the screen — phrase data onto the CHAIN screen is WRONG_SCREEN, not a
     * best-effort conversion. There is no meaningful mapping between a phrase step and a chain row,
     * and inventing one would silently destroy data.
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
