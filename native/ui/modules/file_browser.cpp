#include "ui/modules/file_browser.h"

#include "ui/helpers.h"
#include "ui/std_filesystem.h"

#include <algorithm>
#include <cstdio>
#include <ctime>

namespace pt::ui {

namespace {

/** "12B" / "48KB" / "3MB" — `formatFileSize`, verbatim (integer division, no decimals). */
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

/** `SimpleDateFormat("dd-MM-yy", Locale.US)` over the file's mtime, in the machine's local zone. */
std::string format_file_date(int64_t millis) {
    const std::time_t secs = static_cast<std::time_t>(millis / 1000);
    std::tm           tm{};
#if defined(_WIN32)
    if (localtime_s(&tm, &secs) != 0) return "--------";
#else
    if (localtime_r(&secs, &tm) == nullptr) return "--------";
#endif
    // The `% 100` on all three is what tells the compiler these are two-digit numbers. Without it gcc
    // has to assume `tm_mday` and `tm_mon` could be any int (they are plain `int` fields) and warns
    // that the output might not fit — -Wformat-truncation. Behaviour-free: a valid `tm` has mday in
    // 1..31 and mon in 0..11, so the modulo cannot change what is printed.
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

/**
 * Clip to `max_chars`, marking the truncation with a trailing ".." over the last two.
 *
 * Two callers with two limits, and both are geometry: 20 for a list row (past that the name would run
 * under the size column at x+370), and 16 for the DELETE prompt (`DELETE <name>? A=YES B=NO` has to fit
 * inside 640px).
 */
std::string clip_name(const std::string& name, size_t max_chars) {
    if (name.size() <= max_chars) return name;
    return name.substr(0, max_chars - 2) + "..";
}

}  // namespace

// ─── State ───────────────────────────────────────────────────────────────────────────────────────

bool FileBrowserState::is_selected(int index) const {
    if (!selectionMode || selectionAnchor < 0) return false;
    const int lo = std::max(std::min(selectionAnchor, cursor), first_selectable());
    const int hi = std::max(selectionAnchor, cursor);
    return index >= lo && index <= hi;
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

    // ".." first, and only when there is somewhere to go. At a filesystem root there is not.
    const std::string parent = fs.parent_path(directory);
    if (!parent.empty()) {
        BrowserItem up;
        up.kind        = BrowserItem::Kind::PARENT;
        up.path        = parent;
        up.displayName = "..";
        items.push_back(std::move(up));
    }

    std::vector<FileInfo> entries = fs.list_files(directory);

    std::vector<BrowserItem> folders;
    std::vector<BrowserItem> files;
    for (const FileInfo& e : entries) {
        if (!e.name.empty() && e.name[0] == '.') continue;   // hidden — `showHidden` is never true

        BrowserItem it;
        it.path         = e.path;
        it.sortName     = to_lower(e.name);
        it.size         = e.size;
        it.lastModified = e.lastModified;

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

    // Both groups by name. This is the base order every other sort mode is STABLE against — see
    // sort_items — so it is not merely the default view, it is the tiebreak for all five others.
    auto by_name = [](const BrowserItem& a, const BrowserItem& b) { return a.sortName < b.sortName; };
    std::stable_sort(folders.begin(), folders.end(), by_name);
    std::stable_sort(files.begin(), files.end(), by_name);

    items.insert(items.end(), folders.begin(), folders.end());
    items.insert(items.end(), files.begin(), files.end());
    return items;
}

void sort_items(std::vector<BrowserItem>& items, FileSortMode mode) {
    // ".." is pinned; folders and files are sorted as two separate groups and re-concatenated.
    std::vector<BrowserItem> parent, folders, files;
    for (BrowserItem& it : items) {
        switch (it.kind) {
            case BrowserItem::Kind::PARENT: parent.push_back(std::move(it)); break;
            case BrowserItem::Kind::FOLDER: folders.push_back(std::move(it)); break;
            case BrowserItem::Kind::FILE:   files.push_back(std::move(it)); break;
        }
    }

    // ⚠️ stable_sort, not sort. Kotlin's `sortedBy` is stable, and build_item_list left each group in
    // name order — so equal keys (two files written in the same second, which is most of them) keep
    // that name order. std::sort would scramble them, differently per toolchain.
    auto apply = [mode](std::vector<BrowserItem>& v) {
        switch (mode) {
            case FileSortMode::NAME_ASC:
                std::stable_sort(v.begin(), v.end(),
                                 [](const BrowserItem& a, const BrowserItem& b) { return a.sortName < b.sortName; });
                break;
            case FileSortMode::NAME_DESC:
                std::stable_sort(v.begin(), v.end(),
                                 [](const BrowserItem& a, const BrowserItem& b) { return b.sortName < a.sortName; });
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
    items.insert(items.end(), std::make_move_iterator(parent.begin()), std::make_move_iterator(parent.end()));
    items.insert(items.end(), std::make_move_iterator(folders.begin()), std::make_move_iterator(folders.end()));
    items.insert(items.end(), std::make_move_iterator(files.begin()), std::make_move_iterator(files.end()));
}

void rebuild_items(FileBrowserState& s, FileSystem& fs) {
    // ⚠️ **REBUILD, then sort — never re-sort the list already on screen**, and the difference is
    // visible the moment two files share an mtime (which is every file a `git clone` wrote, and every
    // WAV a chop just produced).
    //
    // `sort_items` is a STABLE sort, so ties keep the order they came in with. Rebuilding means they
    // come in NAME-ordered every time (build_item_list guarantees it), and the tie-break is therefore
    // the same no matter which sort mode you arrived from. Re-sorting the existing list instead would
    // make it depend on the PREVIOUS mode — NAME_ASC → SIZE_DESC → DATE_DESC would tie-break
    // differently from NAME_ASC → DATE_DESC, for no reason a user could ever discover.
    //
    // Android gets this for free, and by construction rather than by care: its listing is a
    // `LaunchedEffect(currentDirectory, sortMode, listRefreshTick)` whose whole body is
    // `sortItems(buildItemList(dir, ext, exts), sort)` — a rebuild on every sort change, because a
    // Compose effect keyed on the sort mode has nothing to re-sort in place. There is no LaunchedEffect
    // here, so the invariant has to be stated, and this function is where it lives.
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

    // ⚠️ **A selection does not survive a directory change**, and on Android it DOES — which is a bug.
    // `navigateToFolder` copies the state without clearing `selectionMode` / `selectionAnchor`, so
    // entering a folder with a selection live leaves the anchor pointing at an index in the directory
    // you just LEFT. The rows between it and the (now reset) cursor render highlighted in the NEW
    // listing, and B there copies files the user never picked — which an L+A paste then duplicates.
    // Fixed on Android too (zone B, per order-of-work §4).
    s.selectionMode   = false;
    s.selectionAnchor = -1;

    // Kotlin's `permissionError` (an Android runtime-permission state) has no counterpart here. A
    // directory we cannot read simply lists empty, which is the same thing the user sees and one fewer
    // state to keep true. Its four-line "grant All Files Access" overlay goes with it.
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
        const BrowserItem* item = s.current();
        hint = "DELETE " + clip_name(item ? item->displayName : "", 16) + "? A=YES B=NO";
        hintColor = 0xFFFF0000;
    } else if (s.selectionMode) {
        hint      = "B=COPY L+A=CUT L+B=ALL L+R=CANCEL";
        hintColor = t.rowSelection;
    } else if (!s.fileClipboard.empty()) {
        hint      = "L+A=PASTE  " + s.clipboard_info();
        hintColor = t.textTitle;
    } else {
        hint      = "SEL+A=RENAME SEL+B=DEL SEL+R=NEW";
        hintColor = t.textParam;
    }
    c.draw_text(hint, x + 10, barY1 + TEXT_PADDING, hintColor, CHAR_SPACING, FONT_SCALE);

    // The path, tail-clipped: it is the one string on screen that can be arbitrarily long, and the
    // END of it is the part that says where you are.
    std::string path = s.currentDirectory;
    if (path.size() > 36) path = ".." + path.substr(path.size() - 34);
    c.draw_text(path, x + 10, barY2 + TEXT_PADDING, t.textEmpty, CHAR_SPACING, FONT_SCALE);

    // ── The list ────────────────────────────────────────────────────────────────────────────────
    int rowY = barY2 + ROW_HEIGHT + 5;   // the 5px spacer where the header used to be

    const int total = static_cast<int>(s.items.size());
    for (int i = 0; i < BROWSER_VISIBLE_ROWS; ++i) {
        const int index = s.scroll + i;
        if (index >= total) break;

        const BrowserItem& item     = s.items[static_cast<size_t>(index)];
        const bool         isCursor = (index == s.cursor);
        const bool         isSel    = s.is_selected(index);

        Argb bg;
        if (isCursor)        bg = t.rowCursor;
        else if (isSel)      bg = t.rowSelection;
        else if (i % 2 == 0) bg = t.background;
        else                 bg = 0xFF111111;
        c.fill_rect(x, rowY, WIDTH, ROW_HEIGHT, bg);

        // Initialized, not merely assigned in every arm below: the `switch` covers all three
        // enumerators, but a scoped enum can legally hold a value outside them, so gcc is right that
        // this could be read uninitialized (-Wmaybe-uninitialized). `textValue` is the same colour the
        // FILE arm falls back to, so no reachable case changes.
        Argb textColor = t.textValue;
        if (isCursor) {
            textColor = t.textCursor;
        } else {
            switch (item.kind) {
                case BrowserItem::Kind::PARENT: textColor = COLOR_PARENT; break;
                case BrowserItem::Kind::FOLDER: textColor = COLOR_FOLDER; break;
                case BrowserItem::Kind::FILE:
                    textColor = ext_matches(item.extension, video_extensions()) ? COLOR_VIDEO
                                                                                : t.textValue;
                    break;
            }
        }

        if (isCursor) c.draw_text(">", x + 10, rowY + TEXT_PADDING, t.textCursor, CHAR_SPACING, FONT_SCALE);

        c.draw_text(clip_name(item.displayName, 20), x + 30, rowY + TEXT_PADDING, textColor,
                    CHAR_SPACING, FONT_SCALE);

        if (item.kind == BrowserItem::Kind::FILE) {
            c.draw_text(item.sizeText, x + 370, rowY + TEXT_PADDING, t.textEmpty, CHAR_SPACING, FONT_SCALE);
            c.draw_text(item.dateText, x + 480, rowY + TEXT_PADDING, t.textEmpty, CHAR_SPACING, FONT_SCALE);
        }

        rowY += ROW_HEIGHT;
    }

    // ── The bottom bar ──────────────────────────────────────────────────────────────────────────
    const int bottomY = y + HEIGHT - ROW_HEIGHT;
    c.fill_rect(x, bottomY, WIDTH, ROW_HEIGHT, t.meterBackground);

    // The status message wins the left half when there is one — a failed rename or a finished paste
    // has something to say, and the control hints are the same every frame.
    if (!s.statusMessage.empty()) {
        c.draw_text(clip_name(s.statusMessage, 31), x + 10, bottomY + TEXT_PADDING,
                    s.statusSuccess ? t.textTitle : 0xFFFF4444, CHAR_SPACING, FONT_SCALE);
    } else {
        // ⚠️ REAL ARROWS (U+2190/2191/2193), as Kotlin draws them — not "<" and "^v". The 5×5 font has
        // had the four arrow glyphs since S1 and `draw_text` has advanced per CODE POINT since S4, so
        // they render; a '^' does NOT (there is no caret in the font, and it comes out as a BLANK,
        // which is how the first version of this line shipped a bottom bar reading "R+ V=SORT").
        c.draw_text("A=OPEN B=BACK R+\xE2\x86\x90=UP R+\xE2\x86\x91\xE2\x86\x93=SORT", x + 10,
                    bottomY + TEXT_PADDING, t.textParam, CHAR_SPACING, FONT_SCALE);
    }

    if (total > 0) {
        const std::string count = std::to_string(s.cursor + 1) + "/" + std::to_string(total);
        c.draw_text(count, x + 550, bottomY + TEXT_PADDING, t.textParam, CHAR_SPACING, FONT_SCALE);
    }
}

}  // namespace pt::ui
