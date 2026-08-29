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
 * The column budget of each clipped string on this screen. The two name limits are explained where
 * the row is drawn; the path bar and the status line each own their row outright. The three SEL
 * prompts keep their budget inline, beside the wording they have to share 640px with.
 */
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
            // Leading zeros carry no value, so skip them before measuring: after that the LONGER run
            // is the larger number, and only equal-length runs need a digit-by-digit compare.
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
            i = ei;   // the two runs are the same number — step over both and keep walking
            j = ej;
            continue;
        }
        // Unsigned, so a byte above 0x7F (UTF-8 continuation bytes, and every non-ASCII name is made
        // of them) sorts above ASCII rather than below it.
        const unsigned char ca = static_cast<unsigned char>(a[i]);
        const unsigned char cb = static_cast<unsigned char>(b[j]);
        if (ca != cb) return ca < cb;
        ++i;
        ++j;
    }

    // One ran out first: the shorter name is the smaller one. Both ran out and every token matched,
    // so any difference left is leading zeros — the raw compare below is what keeps the order TOTAL.
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

    std::vector<BrowserItem> actions;
    std::vector<BrowserItem> folders;
    std::vector<BrowserItem> files;
    for (const FileInfo& e : entries) {
        // An ACTION row before the hidden-file test and before the extension filter, because it is
        // neither: it has no extension to pass a filter and no file to be hidden. It is drawn
        // verbatim — no brackets, no stem — because its name is a sentence the user reads.
        if (e.isAction) {
            BrowserItem act;
            act.kind        = BrowserItem::Kind::ACTION;
            act.path        = e.path;
            act.displayName = e.name;
            act.sortName    = to_lower(e.name);
            actions.push_back(std::move(act));
            continue;
        }

        if (!e.name.empty() && e.name[0] == '.') continue;   // hidden — `showHidden` is never true

        BrowserItem it;
        it.path         = e.path;
        it.sortName     = to_lower(e.name);
        it.size         = e.size;
        it.lastModified = e.lastModified;
        it.isRoot       = e.isRoot;   // a granted tree: walk in, but no file operation may touch it

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
    // ⚠️ Which is why it is `natural_name_less` and not a plain compare: leave this one lexicographic
    // and DATE and SIZE would resolve their (very common) ties in a different order from NAME.
    auto by_name = [](const BrowserItem& a, const BrowserItem& b) {
        return natural_name_less(a.sortName, b.sortName);
    };
    std::stable_sort(folders.begin(), folders.end(), by_name);
    std::stable_sort(files.begin(), files.end(), by_name);

    // The actions sit directly under "..", above every real entry, and are NOT sorted with them: they
    // are how you leave this directory, not things in it.
    items.insert(items.end(), actions.begin(), actions.end());
    items.insert(items.end(), folders.begin(), folders.end());
    items.insert(items.end(), files.begin(), files.end());
    return items;
}

void sort_items(std::vector<BrowserItem>& items, FileSortMode mode) {
    // ".." and the actions are pinned, in the order build_item_list produced them; folders and files
    // are sorted as two separate groups and re-concatenated.
    std::vector<BrowserItem> pinned, folders, files;
    for (BrowserItem& it : items) {
        switch (it.kind) {
            case BrowserItem::Kind::PARENT:
            case BrowserItem::Kind::ACTION: pinned.push_back(std::move(it)); break;
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
        hint = "DELETE " + Canvas::clip_text(item ? item->displayName : "", 16) + "? A=YES B=NO";
        hintColor = 0xFFFF0000;
    } else if (s.mode == BrowserMode::SET_HOME) {
        const BrowserItem* item = s.current();
        // Not red: it destroys nothing and is undone by choosing another tree. It IS a confirm, though
        // — it moves where every one of the app's folders is looked for.
        hint = "HOME FOLDER? " + Canvas::clip_text(item ? item->displayName : "", 14) + " A=YES B=NO";
        hintColor = t.textTitle;
    } else if (s.mode == BrowserMode::FORGET_ROOT) {
        const BrowserItem* item = s.current();
        // ⚠️ **The word is FORGET and it must never read as DELETE** — this hands back a permission and
        // removes a row; every file in the folder stays. Not red for exactly that reason: the red bar
        // in this app means "something is about to be destroyed".
        hint = "FORGET " + Canvas::clip_text(item ? item->displayName : "", 15) + "? A=YES B=NO";
        hintColor = t.textTitle;
    } else if (s.selectionMode) {
        hint      = "B=COPY L+A=CUT L+B=ALL L+R=CANCEL";
        hintColor = t.rowSelection;
    } else if (!s.fileClipboard.empty()) {
        hint      = "L+A=PASTE  " + s.clipboard_info();
        hintColor = t.textTitle;
    } else if (const BrowserItem* item = s.current(); item && item->isRoot) {
        // ⭐ **On a granted tree all three of those chords are refused**, so advertising them there
        // would name three things that do nothing and hide the one thing that works. The row's own
        // kind decides what the bar says, which is also how a user discovers the gesture at all —
        // there is no settings row for it, and no other screen mentions a home folder.
        hint      = "SEL+A=SET HOME  SEL+B=FORGET";
        hintColor = t.textParam;
    } else {
        hint      = "SEL+A=RENAME SEL+B=DEL SEL+R=NEW";
        hintColor = t.textParam;
    }
    c.draw_text(hint, x + 10, barY1 + TEXT_PADDING, hintColor, CHAR_SPACING, FONT_SCALE);

    // The path, HEAD-clipped: it is the one string on screen that can be arbitrarily long, and the
    // END of it is the part that says where you are.
    const std::string path = Canvas::clip_text_head(s.currentDirectory, PATH_COLS);
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

        // ⚠️ The stripe alternates on the ITEM index, not on the on-screen row. Keyed on `i` it
        // re-phases whenever the list scrolls by an odd number of rows, so a row changes colour
        // without its content changing.
        // ⚠️ THE CURSOR IS A WHOLE ROW HERE, and it is the one screen in the app where it still is.
        // A row carries three columns the cursor cannot land on — the name, the size and the date —
        // and they are read TOGETHER: highlighting the name alone leaves the size and date of the
        // file you are on looking like every other row's. The row IS the unit of this screen.
        Argb bg;
        if (isCursor)            bg = t.rowCursor;
        else if (isSel)          bg = t.rowSelection;
        else if (index % 2 == 0) bg = t.background;
        else                     bg = t.rowEvery4th;
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
                case BrowserItem::Kind::ACTION: textColor = COLOR_ACTION; break;
                case BrowserItem::Kind::FOLDER: textColor = t.textTitle; break;
                case BrowserItem::Kind::FILE:
                    textColor = ext_matches(item.extension, video_extensions()) ? COLOR_VIDEO
                                                                                : t.textValue;
                    break;
            }
        }

        if (isCursor) c.draw_text(">", x + 10, rowY + TEXT_PADDING, t.textCursor, CHAR_SPACING, FONT_SCALE);

        // ⚠️ **20 is the FILE limit and it exists because of the size column at x+370** — a granted
        // tree has no size and no date, so nothing is under it to run into, and clipping one at 20 cut
        // off the `(HOME)` / `(MISSING)` marker that is the whole reason those rows say anything.
        // Derived from the row's own kind rather than from a second constant nobody would keep in step.
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

    // The status message wins the left half when there is one — a failed rename or a finished paste
    // has something to say, and the control hints are the same every frame.
    if (!s.statusMessage.empty()) {
        c.draw_text(Canvas::clip_text(s.statusMessage, STATUS_COLS), x + 10, bottomY + TEXT_PADDING,
                    s.statusSuccess ? t.textTitle : 0xFFFF4444, CHAR_SPACING, FONT_SCALE);
    } else {
        // ⚠️ REAL ARROWS (U+2190/2191/2193), as Kotlin draws them — not "<" and "^v". The 5×5 font has
        // had the four arrow glyphs since S1 and `draw_text` has advanced per CODE POINT since S4, so
        // they render; a '^' does NOT (there is no caret in the font, and it comes out as a BLANK,
        // which is how the first version of this line shipped a bottom bar reading "R+ V=SORT").
        // ⚠️ Do not "shorten" this line by touching the arrows — the verbs are the slack. 28 code
        // points from x+10 ends at x+484, which is what leaves the counter below room to grow.
        c.draw_text("A=OPN B=BCK R+\xE2\x86\x90=UP R+\xE2\x86\x91\xE2\x86\x93=SRT", x + 10,
                    bottomY + TEXT_PADDING, t.textParam, CHAR_SPACING, FONT_SCALE);
    }

    if (total > 0) {
        // RIGHT-ALIGNED against the same 10px spacer the hint line and the path use on the left, so
        // the bar is symmetric and the count grows leftward from a fixed edge however long it gets. A
        // fixed left anchor put "100/256" 29px past the panel (7 chars × 17 = 119 from x+550, and
        // WIDTH is 640).
        //
        // `text_width` and not `size() * CHAR_W`: the trailing gap after the last glyph is not ink, and
        // counting it would leave the digits 2px shy of the mirror. It may reach 8 characters —
        // `999/9999`, 134px — before it touches the hint line, which ends at x+484.
        const std::string count  = std::to_string(s.cursor + 1) + "/" + std::to_string(total);
        const int         countW = Canvas::text_width(count, CHAR_SPACING, FONT_SCALE);
        c.draw_text(count, x + WIDTH - 10 - countW, bottomY + TEXT_PADDING, t.textParam,
                    CHAR_SPACING, FONT_SCALE);
    }
}

}  // namespace pt::ui
