#include "ui/modules/file_browser.h"

#include "ui/helpers.h"
#include "ui/std_filesystem.h"

#include <algorithm>
#include <cstdio>
#include <ctime>

namespace pt::ui {

namespace {

/** "12B" / "48KB" / "3MB" — integer division, no decimals. */
std::string format_file_size(int64_t bytes) {
    char buf[32];
    if (bytes < 1024) {
        std::snprintf(buf, sizeof(buf), "%lldB", static_cast<long long>(bytes));
    } else if (bytes < 1024 * 1024) {
        std::snprintf(buf, sizeof(buf), "%lldKB", static_cast<long long>(bytes / 1024));
    } else {
        std::snprintf(buf, sizeof(buf), "%lldMB", static_cast<long long>(bytes / (1024 * 1024)));
    }
    return buf;
}

/** "dd-MM-yy" over the file's mtime, in the machine's local zone. */
std::string format_file_date(int64_t millis) {
    const std::time_t secs = static_cast<std::time_t>(millis / 1000);
    std::tm           tm{};
#if defined(_WIN32)
    if (localtime_s(&tm, &secs) != 0) return "--------";
#else
    if (localtime_r(&secs, &tm) == nullptr) return "--------";
#endif
    // `% 100` quiets -Wformat-truncation; a valid `tm` is unchanged.
    const int day   = tm.tm_mday % 100;
    const int month = (tm.tm_mon + 1) % 100;
    const int year  = (tm.tm_year + 1900) % 100;

    char buf[16];
    std::snprintf(buf, sizeof(buf), "%02d-%02d-%02d", day, month, year);
    return buf;
}

bool ext_matches(const std::string& ext, const std::vector<std::string>& allowed) {
    if (allowed.empty()) return true;   // no filter = every file
    const std::string lower = to_lower(ext);
    return std::find(allowed.begin(), allowed.end(), lower) != allowed.end();
}

// Column budgets for the clipped strings on this screen.
constexpr int NAME_COLS   = 20;
constexpr int ROOT_COLS   = 32;
constexpr int PATH_COLS   = 36;
constexpr int STATUS_COLS = 31;

bool is_digit(char c) { return c >= '0' && c <= '9'; }

}  // namespace

// ─── The name order ──────────────────────────────────────────────────────────────────────────────

bool natural_name_less(const std::string& a, const std::string& b) {
    size_t i = 0, j = 0;
    while (i < a.size() && j < b.size()) {
        if (is_digit(a[i]) && is_digit(b[j])) {
            // Skip leading zeros: then the longer run is the larger number.
            size_t si = i, sj = j;
            while (si < a.size() && a[si] == '0') ++si;
            while (sj < b.size() && b[sj] == '0') ++sj;
            size_t ei = si, ej = sj;
            while (ei < a.size() && is_digit(a[ei])) ++ei;
            while (ej < b.size() && is_digit(b[ej])) ++ej;

            if (ei - si != ej - sj) return (ei - si) < (ej - sj);
            for (size_t k = 0; k < ei - si; ++k) {
                if (a[si + k] != b[sj + k]) return a[si + k] < b[sj + k];
            }
            i = ei;   // the same number — step over both
            j = ej;
            continue;
        }
        // Unsigned, so non-ASCII (UTF-8) bytes sort above ASCII.
        const unsigned char ca = static_cast<unsigned char>(a[i]);
        const unsigned char cb = static_cast<unsigned char>(b[j]);
        if (ca != cb) return ca < cb;
        ++i;
        ++j;
    }

    // The shorter name is smaller; if every token matched, the raw compare keeps the order total.
    if (i < a.size() || j < b.size()) return i >= a.size();
    return a < b;
}

// ─── State ───────────────────────────────────────────────────────────────────────────────────────

bool FileBrowserState::is_selected(int index) const {
    if (!selectionMode || selectionAnchor < 0) return false;
    const int lo = std::max(std::min(selectionAnchor, cursor), first_selectable());
    const int hi = std::max(selectionAnchor, cursor);
    return index >= lo && index <= hi;
}

std::vector<const BrowserItem*> FileBrowserState::delete_targets() const {
    std::vector<const BrowserItem*> out;
    if (selectionMode) {
        for (int i = 0; i < static_cast<int>(items.size()); ++i)
            if (is_selected(i) && !items[static_cast<size_t>(i)].is_pseudo())
                out.push_back(&items[static_cast<size_t>(i)]);
    } else if (const BrowserItem* item = current(); item && !item->is_pseudo()) {
        out.push_back(item);
    }
    return out;
}

std::string FileBrowserState::clipboard_info() const {
    if (fileClipboard.empty()) return "";
    const size_t n = fileClipboard.size();
    return std::string(fileClipboardIsCut ? "CUT " : "CPY ") + std::to_string(n) +
           (n == 1 ? " FILE" : " FILES");
}

// ─── The listing ─────────────────────────────────────────────────────────────────────────────────

std::vector<BrowserItem> build_item_list(FileSystem& fs, const std::string& directory,
                                         const std::vector<std::string>& extensions) {
    std::vector<BrowserItem> items;

    // ".." only when there is somewhere to go.
    const std::string parent = fs.parent_path(directory);
    if (!parent.empty()) {
        BrowserItem up;
        up.kind        = BrowserItem::Kind::PARENT;
        up.path        = parent;
        up.displayName = "..";
        items.push_back(std::move(up));
    }

    std::vector<FileInfo> entries = fs.list_files(directory);

    std::vector<BrowserItem> actions;
    std::vector<BrowserItem> folders;
    std::vector<BrowserItem> files;
    for (const FileInfo& e : entries) {
        // An ACTION row is neither hidden nor filtered, and is drawn verbatim: its name is a sentence.
        if (e.isAction) {
            BrowserItem act;
            act.kind        = BrowserItem::Kind::ACTION;
            act.path        = e.path;
            act.displayName = e.name;
            act.sortName    = to_lower(e.name);
            actions.push_back(std::move(act));
            continue;
        }

        if (!e.name.empty() && e.name[0] == '.') continue;   // hidden

        BrowserItem it;
        it.path         = e.path;
        it.sortName     = to_lower(e.name);
        it.size         = e.size;
        it.lastModified = e.lastModified;
        it.isRoot       = e.isRoot;

        if (e.isDirectory) {
            it.kind        = BrowserItem::Kind::FOLDER;
            it.displayName = "[" + e.name + "]";
            folders.push_back(std::move(it));
        } else {
            if (!ext_matches(e.extension, extensions)) continue;
            it.kind        = BrowserItem::Kind::FILE;
            it.extension   = e.extension;
            it.displayName = e.name_without_extension();
            it.sizeText    = format_file_size(e.size);
            it.dateText    = format_file_date(e.lastModified);
            files.push_back(std::move(it));
        }
    }

    // By name: the base every other sort is stable against. ⚠️ `natural_name_less`, or DATE and SIZE
    // would break ties unlike NAME.
    auto by_name = [](const BrowserItem& a, const BrowserItem& b) {
        return natural_name_less(a.sortName, b.sortName);
    };
    std::stable_sort(folders.begin(), folders.end(), by_name);
    std::stable_sort(files.begin(), files.end(), by_name);

    // The actions sit under "..", unsorted: they are ways out, not things in here.
    items.insert(items.end(), actions.begin(), actions.end());
    items.insert(items.end(), folders.begin(), folders.end());
    items.insert(items.end(), files.begin(), files.end());
    return items;
}

void sort_items(std::vector<BrowserItem>& items, FileSortMode mode) {
    // ".." and the actions stay pinned; folders and files are sorted as two groups.
    std::vector<BrowserItem> pinned, folders, files;
    for (BrowserItem& it : items) {
        switch (it.kind) {
            case BrowserItem::Kind::PARENT:
            case BrowserItem::Kind::ACTION: pinned.push_back(std::move(it)); break;
            case BrowserItem::Kind::FOLDER: folders.push_back(std::move(it)); break;
            case BrowserItem::Kind::FILE:   files.push_back(std::move(it)); break;
        }
    }

    // ⚠️ stable_sort: same-second files keep the name order.
    auto apply = [mode](std::vector<BrowserItem>& v) {
        switch (mode) {
            case FileSortMode::NAME_ASC:
                std::stable_sort(v.begin(), v.end(), [](const BrowserItem& a, const BrowserItem& b) {
                    return natural_name_less(a.sortName, b.sortName);
                });
                break;
            case FileSortMode::NAME_DESC:
                std::stable_sort(v.begin(), v.end(), [](const BrowserItem& a, const BrowserItem& b) {
                    return natural_name_less(b.sortName, a.sortName);
                });
                break;
            case FileSortMode::DATE_ASC:
                std::stable_sort(v.begin(), v.end(), [](const BrowserItem& a, const BrowserItem& b) {
                    return a.lastModified < b.lastModified;
                });
                break;
            case FileSortMode::DATE_DESC:
                std::stable_sort(v.begin(), v.end(), [](const BrowserItem& a, const BrowserItem& b) {
                    return b.lastModified < a.lastModified;
                });
                break;
            case FileSortMode::SIZE_ASC:
                std::stable_sort(v.begin(), v.end(),
                                 [](const BrowserItem& a, const BrowserItem& b) { return a.size < b.size; });
                break;
            case FileSortMode::SIZE_DESC:
                std::stable_sort(v.begin(), v.end(),
                                 [](const BrowserItem& a, const BrowserItem& b) { return b.size < a.size; });
                break;
        }
    };
    apply(folders);
    apply(files);

    items.clear();
    items.insert(items.end(), std::make_move_iterator(pinned.begin()), std::make_move_iterator(pinned.end()));
    items.insert(items.end(), std::make_move_iterator(folders.begin()), std::make_move_iterator(folders.end()));
    items.insert(items.end(), std::make_move_iterator(files.begin()), std::make_move_iterator(files.end()));
}

void rebuild_items(FileBrowserState& s, FileSystem& fs) {
    // ⚠️ Rebuild, then sort — never re-sort what is on screen, or ties would follow the previous mode.
    s.items = build_item_list(fs, s.currentDirectory, s.fileExtensions);
    sort_items(s.items, s.sortMode);
}

void navigate_to_folder(FileBrowserState& s, FileSystem& fs, const std::string& folder) {
    s.currentDirectory = folder;
    rebuild_items(s, fs);
    s.cursor           = 0;
    s.scroll           = 0;
    s.statusMessage.clear();
    s.statusSuccess   = true;

    // ⚠️ The selection's anchor indexes the directory left behind; B would copy unpicked files.
    s.selectionMode   = false;
    s.selectionAnchor = -1;

    // A directory we cannot read lists empty.
}

void navigate_to_parent(FileBrowserState& s, FileSystem& fs) {
    const std::string parent = fs.parent_path(s.currentDirectory);
    if (parent.empty()) return;   // already at a root
    navigate_to_folder(s, fs, parent);
}

// ─── Drawing ─────────────────────────────────────────────────────────────────────────────────────

void FileBrowserModule::draw(Canvas& c, int x, int y, const FileBrowserState& s,
                             const Theme& t) const {
    c.fill_rect(x, y, WIDTH, HEIGHT, t.background);

    // ── The two top bars: the hint line, then the path ──────────────────────────────────────────
    const int barY1 = y + TEXT_PADDING;
    const int barY2 = barY1 + ROW_HEIGHT;
    c.fill_rect(x, barY1, WIDTH, ROW_HEIGHT * 2, t.meterBackground);

    std::string hint;
    Argb        hintColor;
    if (s.mode == BrowserMode::DELETE) {
        const std::vector<const BrowserItem*> doomed = s.delete_targets();
        const std::string what = doomed.size() == 1 ? Canvas::clip_text(doomed[0]->displayName, 16)
                                                    : std::to_string(doomed.size()) + " FILES";
        hint = "DELETE " + what + "? A=YES B=NO";
        hintColor = 0xFFFF0000;
    } else if (s.mode == BrowserMode::SET_HOME) {
        const BrowserItem* item = s.current();
        // Not red: nothing is destroyed. Still a confirm — it moves where every app folder is.
        hint = "HOME FOLDER? " + Canvas::clip_text(item ? item->displayName : "", 14) + " A=YES B=NO";
        hintColor = t.textTitle;
    } else if (s.mode == BrowserMode::FORGET_ROOT) {
        const BrowserItem* item = s.current();
        // ⚠️ FORGET, never DELETE: it hands back a permission; every file stays.
        hint = "FORGET " + Canvas::clip_text(item ? item->displayName : "", 15) + "? A=YES B=NO";
        hintColor = t.textTitle;
    } else if (s.selectionMode) {
        hint      = "B=COPY L+A=CUT L+B=ALL L+R=CANCEL";
        hintColor = t.rowSelection;
    } else if (!s.fileClipboard.empty()) {
        hint      = "L+A=PASTE  " + s.clipboard_info();
        hintColor = t.textTitle;
    } else if (const BrowserItem* item = s.current(); item && item->isRoot) {
        // On a granted tree the file chords are refused, so the bar names what works there.
        hint      = "SEL+A=SET HOME  SEL+B=FORGET";
        hintColor = t.textParam;
    } else {
        hint      = "SEL+A=RENAME SEL+B=DEL SEL+R=NEW";
        hintColor = t.textParam;
    }
    c.draw_text(hint, x + 10, barY1 + TEXT_PADDING, hintColor, CHAR_SPACING, FONT_SCALE);

    // Head-clipped: the end of the path says where you are.
    const std::string path = Canvas::clip_text_head(s.currentDirectory, PATH_COLS);
    c.draw_text(path, x + 10, barY2 + TEXT_PADDING, t.textEmpty, CHAR_SPACING, FONT_SCALE);

    // ── The list ────────────────────────────────────────────────────────────────────────────────
    int rowY = barY2 + ROW_HEIGHT + 5;

    const int total = static_cast<int>(s.items.size());
    for (int i = 0; i < BROWSER_VISIBLE_ROWS; ++i) {
        const int index = s.scroll + i;
        if (index >= total) break;

        const BrowserItem& item     = s.items[static_cast<size_t>(index)];
        const bool         isCursor = (index == s.cursor);
        const bool         isSel    = s.is_selected(index);

        // ⚠️ The stripe follows the item index, or it re-phases on scroll. The cursor is the whole row
        // here: name, size and date are read together.
        Argb bg;
        if (isCursor)            bg = t.rowCursor;
        else if (isSel)          bg = t.rowSelection;
        else if (index % 2 == 0) bg = t.background;
        else                     bg = t.rowEvery4th;
        c.fill_rect(x, rowY, WIDTH, ROW_HEIGHT, bg);

        // Initialized for -Wmaybe-uninitialized. A selected row inverts, so its kind stops colouring it.
        Argb textColor = t.textValue;
        if (isCursor) {
            textColor = cursor_cell_ink(t);
        } else if (isSel) {
            textColor = selection_cell_ink(t);
        } else {
            switch (item.kind) {
                case BrowserItem::Kind::PARENT:
                case BrowserItem::Kind::ACTION:
                case BrowserItem::Kind::FOLDER: textColor = t.textTitle; break;
                case BrowserItem::Kind::FILE:
                    textColor = ext_matches(item.extension, video_extensions()) ? COLOR_VIDEO
                                                                                : t.textValue;
                    break;
            }
        }

        if (isCursor) c.draw_text(">", x + 10, rowY + TEXT_PADDING, cursor_cell_ink(t), CHAR_SPACING, FONT_SCALE);

        // 20 fits before the size column; a granted tree has none, so its marker gets more room.
        c.draw_text(Canvas::clip_text(item.displayName, item.isRoot ? ROOT_COLS : NAME_COLS),
                    x + 30, rowY + TEXT_PADDING, textColor, CHAR_SPACING, FONT_SCALE);

        if (item.kind == BrowserItem::Kind::FILE) {
            c.draw_text(item.sizeText, x + 370, rowY + TEXT_PADDING, t.textEmpty, CHAR_SPACING, FONT_SCALE);
            c.draw_text(item.dateText, x + 480, rowY + TEXT_PADDING, t.textEmpty, CHAR_SPACING, FONT_SCALE);
        }

        rowY += ROW_HEIGHT;
    }

    // ── The bottom bar ──────────────────────────────────────────────────────────────────────────
    const int bottomY = y + HEIGHT - ROW_HEIGHT;
    c.fill_rect(x, bottomY, WIDTH, ROW_HEIGHT, t.meterBackground);

    // A status message takes the left half when there is one.
    if (!s.statusMessage.empty()) {
        c.draw_text(Canvas::clip_text(s.statusMessage, STATUS_COLS), x + 10, bottomY + TEXT_PADDING,
                    s.statusSuccess ? t.textTitle : 0xFFFF4444, CHAR_SPACING, FONT_SCALE);
    } else {
        // Real arrows (the font has them; '^' draws blank). 28 code points leave the counter room.
        c.draw_text("A=OPN B=BCK R+\xE2\x86\x90=UP R+\xE2\x86\x91\xE2\x86\x93=SRT", x + 10,
                    bottomY + TEXT_PADDING, t.textParam, CHAR_SPACING, FONT_SCALE);
    }

    if (total > 0) {
        // Right-aligned, so the count grows leftward; `text_width` because the trailing gap is not ink.
        const std::string count  = std::to_string(s.cursor + 1) + "/" + std::to_string(total);
        const int         countW = Canvas::text_width(count, CHAR_SPACING, FONT_SCALE);
        c.draw_text(count, x + WIDTH - 10 - countW, bottomY + TEXT_PADDING, t.textParam,
                    CHAR_SPACING, FONT_SCALE);
    }
}

}  // namespace pt::ui
