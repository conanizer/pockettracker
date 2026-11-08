#include "ui/clipboard.h"

#include <algorithm>

#include "ui/fx_columns.h"
#include "ui/grid_cursor.h"

namespace pt::ui {

using songcore::Note;
using songcore::Project;

namespace {

int clamp_table_id(const Project& p, int tableId) {
    const int last = static_cast<int>(p.tables.size()) - 1;
    return std::max(0, std::min(tableId, last));
}

// The selection rectangle with its corners sorted.
struct Bounds {
    int minRow, maxRow, minCol, maxCol;
};

Bounds normalise(int startRow, int startColumn, int endRow, int endColumn) {
    return {std::min(startRow, endRow), std::max(startRow, endRow),
            std::min(startColumn, endColumn), std::max(startColumn, endColumn)};
}

// A command column's half of an item: the TYPE column carries the type and its switch, the VALUE
// column the value. `first` is the screen's FX1 TYPE column; false when `col` is no command column.
template <typename Item, typename Row> bool copy_fx_cell(Item& item, const Row& row, int col, int first) {
    const int slot = fx_slot_of_column(col, first);
    if (slot == 0) return false;
    const songcore::FxSlot& f = songcore::fx_slot(row, slot);
    if (is_fx_type_column(col, first)) { item.fxType = f.type; item.fxOff = f.off; }
    else                               item.fxValue = f.value;
    return true;
}
// An item without the column's half writes nothing: false.
template <typename Item, typename Row> bool paste_fx_cell(Row& row, const Item& item, int col, int first) {
    const int slot = fx_slot_of_column(col, first);
    if (slot == 0) return false;
    songcore::FxSlot& f = songcore::fx_slot_ref(row, slot);
    if (is_fx_type_column(col, first)) {
        if (!item.fxType) return false;
        f.type = *item.fxType;
        f.off  = item.fxOff;
    } else {
        if (!item.fxValue) return false;
        f.value = *item.fxValue;
    }
    return true;
}

// The leftmost copied column — what a paste re-offsets from.
template <typename Item>
int min_source_column(const std::vector<Item>& items) {
    int m = items.front().column;
    for (const Item& it : items) m = std::min(m, it.column);
    return m;
}

}  // namespace

// ─── Copy ────────────────────────────────────────────────────────────────────────────────────────

void Clipboard::copy_phrase_steps(const Project& p, int phraseId, int startRow, int startColumn,
                                  int endRow, int endColumn) {
    const songcore::Phrase& phrase = p.phrases[static_cast<size_t>(phraseId)];

    const auto [minRow, maxRow, minCol, maxCol] =
        normalise(startRow, startColumn, endRow, endColumn);
    begin_copy(ClipboardType::PHRASE_STEPS, maxCol - minCol + 1, maxRow - minRow + 1);
    for (int row = minRow; row <= maxRow; ++row) {
        const songcore::PhraseStep& step = phrase.steps[static_cast<size_t>(row)];
        for (int col = minCol; col <= maxCol; ++col) {
            PhraseStepClipItem item;
            item.row    = row - minRow;
            item.column = col;
            switch (col) {
                case 1: item.note       = step.note;       break;
                case 2: item.volume     = step.volume;     break;
                case 3: item.instrument = step.instrument; break;
                default:   // outside the editable columns — no item at all
                    if (!copy_fx_cell(item, step, col, PHRASE_FX_COLUMN)) continue;
                    break;
            }
            phraseItems_.push_back(item);
        }
    }

}

void Clipboard::copy_chain_rows(const Project& p, int chainId, int startRow, int startColumn,
                                int endRow, int endColumn) {
    const songcore::Chain& chain = p.chains[static_cast<size_t>(chainId)];

    const auto [minRow, maxRow, minCol, maxCol] =
        normalise(startRow, startColumn, endRow, endColumn);
    begin_copy(ClipboardType::CHAIN_ROWS, maxCol - minCol + 1, maxRow - minRow + 1);
    for (int row = minRow; row <= maxRow; ++row) {
        for (int col = minCol; col <= maxCol; ++col) {
            ChainRowClipItem item;
            item.row    = row - minRow;
            item.column = col;
            switch (col) {
                case 1: item.phraseRef = chain.phraseRefs[static_cast<size_t>(row)];      break;
                case 2: item.transpose = chain.transposeValues[static_cast<size_t>(row)]; break;
                default: continue;
            }
            chainItems_.push_back(item);
        }
    }

}

void Clipboard::copy_song_cells(const Project& p, int startRow, int startColumn, int endRow,
                                int endColumn) {
    const auto [minRow, maxRow, minCol, maxCol] =
        normalise(startRow, startColumn, endRow, endColumn);
    begin_copy(ClipboardType::SONG_CELLS, maxCol - minCol + 1, maxRow - minRow + 1);
    for (int row = minRow; row <= maxRow; ++row) {
        for (int col = minCol; col <= maxCol; ++col) {
            const int trackIndex = col - 1;  // the column IS the track number, 1-based
            if (trackIndex < 0 || trackIndex >= 8) continue;

            const songcore::Track& track = p.tracks[static_cast<size_t>(trackIndex)];
            // A track's chainRefs is variable-length; past its end the cell is empty.
            const int chainRef = (row < static_cast<int>(track.chainRefs.size()))
                                     ? track.chainRefs[static_cast<size_t>(row)]
                                     : -1;

            SongCellClipItem item;
            item.row      = row - minRow;
            item.column   = col;
            item.chainRef = chainRef;
            songItems_.push_back(item);
        }
    }

}

void Clipboard::copy_table_rows(const Project& p, int tableId, int startRow, int startColumn,
                                int endRow, int endColumn) {
    const songcore::Table& table = p.tables[static_cast<size_t>(clamp_table_id(p, tableId))];

    const auto [minRow, maxRow, minCol, maxCol] =
        normalise(startRow, startColumn, endRow, endColumn);
    begin_copy(ClipboardType::TABLE_ROWS, maxCol - minCol + 1, maxRow - minRow + 1);
    for (int row = minRow; row <= maxRow; ++row) {
        const songcore::TableRow& tr = table.rows[static_cast<size_t>(row)];
        for (int col = minCol; col <= maxCol; ++col) {
            TableRowClipItem item;
            item.row    = row - minRow;
            item.column = col;
            switch (col) {
                case 1: item.transpose = tr.transpose; break;
                case 2: item.volume    = tr.volume;    break;
                default:
                    if (!copy_fx_cell(item, tr, col, TABLE_FX_COLUMN)) continue;
                    break;
            }
            tableItems_.push_back(item);
        }
    }

}

// ─── Paste ───────────────────────────────────────────────────────────────────────────────────────

PasteResult Clipboard::paste(Project& p, ScreenType target, int targetId, int cursorRow,
                             int cursorColumn) {
    if (!hasData_) return PasteResult{PasteResult::Kind::NO_CLIPBOARD, 0};

    switch (type_) {
        case ClipboardType::PHRASE_STEPS:
            if (target != ScreenType::PHRASE)
                return PasteResult{PasteResult::Kind::WRONG_SCREEN, 0};
            return paste_phrase_steps(p, targetId, cursorRow, cursorColumn);

        case ClipboardType::CHAIN_ROWS:
            if (target != ScreenType::CHAIN)
                return PasteResult{PasteResult::Kind::WRONG_SCREEN, 0};
            return paste_chain_rows(p, targetId, cursorRow, cursorColumn);

        case ClipboardType::SONG_CELLS:
            if (target != ScreenType::SONG)
                return PasteResult{PasteResult::Kind::WRONG_SCREEN, 0};
            return paste_song_cells(p, cursorRow, cursorColumn);

        case ClipboardType::TABLE_ROWS:
            if (target != ScreenType::TABLE)
                return PasteResult{PasteResult::Kind::WRONG_SCREEN, 0};
            return paste_table_rows(p, targetId, cursorRow, cursorColumn);
    }
    return PasteResult{PasteResult::Kind::NO_CLIPBOARD, 0};
}

PasteResult Clipboard::paste_phrase_steps(Project& p, int phraseId, int cursorRow,
                                          int cursorColumn) {
    if (phraseItems_.empty()) return PasteResult{PasteResult::Kind::SUCCESS, 0};

    songcore::Phrase& phrase = p.phrases[static_cast<size_t>(phraseId)];
    const int minSourceCol   = min_source_column(phraseItems_);
    int       itemsPasted    = 0;

    for (const PhraseStepClipItem& item : phraseItems_) {
        const int targetRow = cursorRow + item.row;
        const int targetCol = cursorColumn + (item.column - minSourceCol);

        if (!in_grid(PHRASE_GRID, targetRow, targetCol)) continue;

        songcore::PhraseStep& step = phrase.steps[static_cast<size_t>(targetRow)];

        // Each arm asks for the target column's field; an item without it writes nothing.
        switch (targetCol) {
            case 1: if (item.note)       { step.note       = *item.note;       ++itemsPasted; } break;
            case 2: if (item.volume)     { step.volume     = *item.volume;     ++itemsPasted; } break;
            case 3: if (item.instrument) { step.instrument = *item.instrument; ++itemsPasted; } break;
            default: if (paste_fx_cell(step, item, targetCol, PHRASE_FX_COLUMN)) ++itemsPasted; break;
        }
    }
    return PasteResult{PasteResult::Kind::SUCCESS, itemsPasted};
}

PasteResult Clipboard::paste_chain_rows(Project& p, int chainId, int cursorRow, int cursorColumn) {
    if (chainItems_.empty()) return PasteResult{PasteResult::Kind::SUCCESS, 0};

    songcore::Chain& chain = p.chains[static_cast<size_t>(chainId)];
    const int minSourceCol = min_source_column(chainItems_);
    int       itemsPasted  = 0;

    for (const ChainRowClipItem& item : chainItems_) {
        const int targetRow = cursorRow + item.row;
        const int targetCol = cursorColumn + (item.column - minSourceCol);

        if (!in_grid(CHAIN_GRID, targetRow, targetCol)) continue;

        switch (targetCol) {
            case 1:
                if (item.phraseRef) {
                    chain.phraseRefs[static_cast<size_t>(targetRow)] = *item.phraseRef;
                    ++itemsPasted;
                }
                break;
            case 2:
                if (item.transpose) {
                    chain.transposeValues[static_cast<size_t>(targetRow)] = *item.transpose;
                    ++itemsPasted;
                }
                break;
            default: break;
        }
    }
    return PasteResult{PasteResult::Kind::SUCCESS, itemsPasted};
}

PasteResult Clipboard::paste_song_cells(Project& p, int cursorRow, int cursorColumn) {
    if (songItems_.empty()) return PasteResult{PasteResult::Kind::SUCCESS, 0};

    const int minSourceCol = min_source_column(songItems_);
    int       itemsPasted  = 0;

    for (const SongCellClipItem& item : songItems_) {
        const int targetRow = cursorRow + item.row;
        const int targetCol = cursorColumn + (item.column - minSourceCol);

        if (!in_grid(SONG_GRID, targetRow, targetCol)) continue;

        songcore::Track& track = p.tracks[static_cast<size_t>(targetCol - 1)];

        // Grow the track to reach the row.
        while (static_cast<int>(track.chainRefs.size()) <= targetRow) track.chainRefs.push_back(-1);

        track.chainRefs[static_cast<size_t>(targetRow)] = item.chainRef;
        ++itemsPasted;
    }
    return PasteResult{PasteResult::Kind::SUCCESS, itemsPasted};
}

PasteResult Clipboard::paste_table_rows(Project& p, int tableId, int cursorRow, int cursorColumn) {
    if (tableItems_.empty()) return PasteResult{PasteResult::Kind::SUCCESS, 0};

    songcore::Table& table = p.tables[static_cast<size_t>(clamp_table_id(p, tableId))];
    const int minSourceCol = min_source_column(tableItems_);
    int       itemsPasted  = 0;

    for (const TableRowClipItem& item : tableItems_) {
        const int targetRow = cursorRow + item.row;
        const int targetCol = cursorColumn + (item.column - minSourceCol);

        if (!in_grid(TABLE_GRID, targetRow, targetCol)) continue;

        songcore::TableRow& tr = table.rows[static_cast<size_t>(targetRow)];

        switch (targetCol) {
            case 1: if (item.transpose) { tr.transpose = *item.transpose; ++itemsPasted; } break;
            case 2: if (item.volume)    { tr.volume    = *item.volume;    ++itemsPasted; } break;
            default: if (paste_fx_cell(tr, item, targetCol, TABLE_FX_COLUMN)) ++itemsPasted; break;
        }
    }
    return PasteResult{PasteResult::Kind::SUCCESS, itemsPasted};
}

// ─── Cut = copy + delete ─────────────────────────────────────────────────────────────────────────

int Clipboard::cut_phrase_steps(Project& p, int phraseId, int startRow, int startColumn, int endRow,
                                int endColumn) {
    copy_phrase_steps(p, phraseId, startRow, startColumn, endRow, endColumn);
    return delete_phrase_steps(p, phraseId, startRow, startColumn, endRow, endColumn);
}

int Clipboard::cut_chain_rows(Project& p, int chainId, int startRow, int startColumn, int endRow,
                              int endColumn) {
    copy_chain_rows(p, chainId, startRow, startColumn, endRow, endColumn);
    return delete_chain_rows(p, chainId, startRow, startColumn, endRow, endColumn);
}

int Clipboard::cut_song_cells(Project& p, int startRow, int startColumn, int endRow,
                              int endColumn) {
    copy_song_cells(p, startRow, startColumn, endRow, endColumn);
    return delete_song_cells(p, startRow, startColumn, endRow, endColumn);
}

int Clipboard::cut_table_rows(Project& p, int tableId, int startRow, int startColumn, int endRow,
                              int endColumn) {
    copy_table_rows(p, tableId, startRow, startColumn, endRow, endColumn);
    return delete_table_rows(p, tableId, startRow, startColumn, endRow, endColumn);
}

// ─── Delete: a cleared velocity is 0x7F, a table volume −1, a chain ref −1, a transpose 0x00 ─────
//
// Each returns the cells that CHANGED, so a delete over cells already empty is no edit. A command's
// type column clears its value with it, as A+B on the one cell does.

namespace {

template <class T> void clear_to(T& field, const T& empty, int& changed) {
    if (field == empty) return;
    field = empty;
    ++changed;
}

template <typename Row> void delete_fx_cell(Row& row, int col, int first, int& changed) {
    const int slot = fx_slot_of_column(col, first);
    if (slot == 0) return;
    songcore::FxSlot& f = songcore::fx_slot_ref(row, slot);
    if (is_fx_type_column(col, first)) clear_to(f.type, 0, changed);
    clear_to(f.value, 0, changed);
}

}  // namespace

int Clipboard::delete_phrase_steps(Project& p, int phraseId, int startRow, int startColumn,
                                   int endRow, int endColumn) {
    songcore::Phrase& phrase = p.phrases[static_cast<size_t>(phraseId)];

    const auto [minRow, maxRow, minCol, maxCol] =
        normalise(startRow, startColumn, endRow, endColumn);
    int changed = 0;

    for (int row = minRow; row <= maxRow; ++row) {
        songcore::PhraseStep& step = phrase.steps[static_cast<size_t>(row)];
        for (int col = minCol; col <= maxCol; ++col) {
            switch (col) {
                case 1: clear_to(step.note, Note::EMPTY(), changed); break;
                case 2: clear_to(step.volume, 0x7F, changed); break;  // max velocity
                case 3: clear_to(step.instrument, 0, changed); break;
                default: delete_fx_cell(step, col, PHRASE_FX_COLUMN, changed); break;
            }
        }
    }
    return changed;
}

int Clipboard::delete_chain_rows(Project& p, int chainId, int startRow, int startColumn, int endRow,
                                 int endColumn) {
    songcore::Chain& chain = p.chains[static_cast<size_t>(chainId)];

    const auto [minRow, maxRow, minCol, maxCol] =
        normalise(startRow, startColumn, endRow, endColumn);
    int changed = 0;

    for (int row = minRow; row <= maxRow; ++row) {
        for (int col = minCol; col <= maxCol; ++col) {
            if (col == 1) clear_to(chain.phraseRefs[static_cast<size_t>(row)], -1, changed);
            if (col == 2) clear_to(chain.transposeValues[static_cast<size_t>(row)], 0x00, changed);
        }
    }
    return changed;
}

int Clipboard::delete_song_cells(Project& p, int startRow, int startColumn, int endRow,
                                 int endColumn) {
    const auto [minRow, maxRow, minCol, maxCol] =
        normalise(startRow, startColumn, endRow, endColumn);
    int changed = 0;

    for (int row = minRow; row <= maxRow; ++row) {
        for (int col = minCol; col <= maxCol; ++col) {
            const int trackIndex = col - 1;
            if (trackIndex < 0 || trackIndex >= 8) continue;

            songcore::Track& track = p.tracks[static_cast<size_t>(trackIndex)];
            // Only rows the track has: delete does not grow it.
            if (row < static_cast<int>(track.chainRefs.size()))
                clear_to(track.chainRefs[static_cast<size_t>(row)], -1, changed);
        }
    }
    return changed;
}

int Clipboard::delete_table_rows(Project& p, int tableId, int startRow, int startColumn, int endRow,
                                 int endColumn) {
    songcore::Table& table = p.tables[static_cast<size_t>(clamp_table_id(p, tableId))];

    const auto [minRow, maxRow, minCol, maxCol] =
        normalise(startRow, startColumn, endRow, endColumn);
    int changed = 0;

    for (int row = minRow; row <= maxRow; ++row) {
        songcore::TableRow& tr = table.rows[static_cast<size_t>(row)];
        for (int col = minCol; col <= maxCol; ++col) {
            switch (col) {
                case 1: clear_to(tr.transpose, 0x00, changed); break;
                case 2: clear_to(tr.volume, -1, changed); break;  // a table volume CAN be empty
                default: delete_fx_cell(tr, col, TABLE_FX_COLUMN, changed); break;
            }
        }
    }
    return changed;
}

// ─── Utility ─────────────────────────────────────────────────────────────────────────────────────

void Clipboard::clear() {
    hasData_ = false;
    width_   = 0;
    height_  = 0;
    phraseItems_.clear();
    chainItems_.clear();
    songItems_.clear();
    tableItems_.clear();
}

void Clipboard::begin_copy(ClipboardType type, int width, int height) {
    clear();
    type_    = type;
    width_   = width;
    height_  = height;
    hasData_ = true;
}

std::string Clipboard::info() const {
    if (!hasData_) return "";
    const char* t = "";
    switch (type_) {
        case ClipboardType::PHRASE_STEPS: t = "PHR"; break;
        case ClipboardType::CHAIN_ROWS:   t = "CHN"; break;
        case ClipboardType::SONG_CELLS:   t = "SNG"; break;
        case ClipboardType::TABLE_ROWS:   t = "TBL"; break;
    }
    return std::string(t) + ":" + std::to_string(width_) + "x" + std::to_string(height_);
}

}  // namespace pt::ui
