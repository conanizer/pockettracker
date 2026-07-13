#pragma once

// ─── FILE BROWSER ────────────────────────────────────────────────────────────────────────────────
//
// The C++ twin of ui/modules/FileBrowserModule.kt. A full-screen (640×480) list that covers the whole
// layout — the top strip and the right bar go away, which is why it is drawn by `TrackerLayout` as a
// special case rather than as a module inside the editor pane.
//
// It is a NAVIGATOR, not an editor: it has no cursor context, no `handle_input`, and it writes nothing
// into the project. What it owns is a listing, a cursor over it, a sort order, a multi-select and a
// file clipboard — and the answer to "which file did the user pick", which the dispatcher then does
// something with depending on WHY the browser was opened (ui/browser_purpose.h).
//
// ── ⚠️ Two modes are NOT ported, because nothing on Android can reach them ────────────────────────
//
// `BrowserMode` has four values in Kotlin — NORMAL, DELETE, RENAME, CREATE — and **nothing anywhere
// assigns RENAME or CREATE**. They are the remains of an in-place text editor that the QWERTY keyboard
// overlay replaced and that was never removed: SELECT+A opens the keyboard with
// `QwertyContext.FILE_RENAME` and SELECT+R with `FOLDER_CREATE` (AppInputDispatcher:2907/2936), and
// the two enum values are dead. Behind them, so is everything that serves them — `renameBuffer` and
// `renameCursor`, two branches of the draw, the D-pad's LEFT/RIGHT arms in the dispatcher, and BOTH
// arms of `getCursorContext` (the NORMAL/DELETE arm returns a `browserLine` context that is only ever
// *read* from the RENAME/CREATE-gated branch, so it is dead by association). `CursorContextFactory
// .browserLine` has no live caller in the app at all.
//
// So this port has NORMAL and DELETE, and the browser has no cursor context. That is not a
// simplification of Kotlin's behaviour — it is all of Kotlin's REACHABLE behaviour, and porting the
// rest would have been porting a second text editor that no button opens.

#include "songcore/model.h"
#include "ui/canvas.h"
#include "ui/filesystem.h"
#include "ui/theme.h"

#include <string>
#include <vector>

namespace pt::ui {

/** 19 file rows + the two top status bars + the bottom bar. */
inline constexpr int BROWSER_VISIBLE_ROWS = 19;

/**
 * Kept out of the theme on purpose, exactly as Kotlin keeps them out of `AppTheme`: these three say
 * what KIND of thing a row is, and that meaning must not change when the user picks a different skin.
 */
inline constexpr Argb COLOR_FOLDER = 0xFF88CCFF;  // light blue
inline constexpr Argb COLOR_VIDEO  = 0xFFFFBB55;  // amber — a container we can show but not load
inline constexpr Argb COLOR_PARENT = 0xFFFFAA88;  // orange — ".."

/**
 * True video containers. Listed so the browser can COLOUR them, not so it can load them: audio is
 * extracted from these through Android's MediaCodec, and there is no host equivalent (the port plan's
 * 2026-07-11 amendment deletes the video→WAV converter from both platforms anyway). They are never in
 * a filter set here, so they only ever appear when the browser is showing everything.
 */
inline const std::vector<std::string>& video_extensions() {
    static const std::vector<std::string> v = {"mp4", "mkv", "webm", "3gp", "mov"};
    return v;
}

/**
 * The sample formats the browser offers for a SAMPLER instrument.
 *
 * ⚠️ **m4a is deliberately absent, where `AudioFormats.SAMPLE_EXTENSIONS` has it.** It is the one
 * compressed format with no native decoder — Android routes it through MediaCodec, and
 * `songcore::is_native_compressed` says so in as many words. Listing it here would offer the user a
 * load that always fails. The day a native AAC decoder lands, this list and that predicate change
 * together.
 */
inline const std::vector<std::string>& sample_extensions() {
    static const std::vector<std::string> v = {"wav", "mp3", "flac", "ogg", "opus"};
    return v;
}

inline const std::vector<std::string>& soundfont_extensions() {
    static const std::vector<std::string> v = {"sf2", "sf3"};
    return v;
}

struct BrowserItem {
    enum class Kind { PARENT, FOLDER, FILE };

    Kind        kind = Kind::FILE;
    std::string path;         // absolute
    std::string displayName;  // "..", "[folder]", or the file's stem
    std::string extension;    // "" for PARENT/FOLDER; case as on disk
    std::string sizeText;     // "12KB" — computed once at build (see FileInfo's note)
    std::string dateText;     // "07-13-26"

    /** The sort keys, carried as data. See FileInfo — Kotlin re-stats these inside its comparator. */
    std::string sortName;      // the FULL name (with extension), lowercased
    int64_t     size         = 0;
    int64_t     lastModified = 0;

    bool is_parent() const { return kind == Kind::PARENT; }
};

/** NORMAL browses; DELETE is the "A=YES B=NO" confirm over the row under the cursor. */
enum class BrowserMode { NORMAL, DELETE };

struct FileBrowserState {
    std::string              currentDirectory;
    std::vector<BrowserItem> items;
    int                      cursor = 0;
    int                      scroll = 0;
    BrowserMode              mode     = BrowserMode::NORMAL;
    FileSortMode             sortMode = FileSortMode::NAME_ASC;

    std::string statusMessage;
    bool        statusSuccess = true;

    /** Empty = show every file. Otherwise the lowercased extensions that pass the filter. */
    std::vector<std::string> fileExtensions;

    // ── The multi-select and the file clipboard (L+B, B, L+A) ───────────────────────────────────
    bool                     selectionMode   = false;
    int                      selectionAnchor = -1;
    std::vector<std::string> fileClipboard;
    bool                     fileClipboardIsCut = false;

    /** The last L+B tap, for the 500 ms "tap again to select all" window. */
    long long lastSelectTapMs = 0;

    const BrowserItem* item_at(int index) const {
        if (index < 0 || index >= static_cast<int>(items.size())) return nullptr;
        return &items[static_cast<size_t>(index)];
    }
    const BrowserItem* current() const { return item_at(cursor); }

    /** The first row a selection may start on — 1 when a ".." is pinned at the top, else 0. */
    int first_selectable() const {
        return (!items.empty() && items[0].is_parent()) ? 1 : 0;
    }

    /** True when `index` falls inside the live anchor..cursor range. */
    bool is_selected(int index) const;

    /** "CPY 3 FILES" / "CUT 1 FILE"; empty when the clipboard is. */
    std::string clipboard_info() const;
};

// ─── The listing ─────────────────────────────────────────────────────────────────────────────────

/**
 * Build the item list for `directory`: a ".." if it has a parent, then the folders, then the files
 * that pass `extensions` (empty = all). Hidden entries (a leading '.') are dropped.
 *
 * Both groups come out NAME-sorted, and that pre-sort is load-bearing rather than cosmetic — see
 * `sort_items`.
 */
std::vector<BrowserItem> build_item_list(FileSystem& fs, const std::string& directory,
                                         const std::vector<std::string>& extensions);

/**
 * Re-order an existing list by `mode`.
 *
 * ".." stays pinned at the top and folders stay above files, whatever the mode: the sort orders each
 * GROUP, it does not merge them. (A DATE sort that floated a folder into the middle of the files would
 * be a worse browser, not a more consistent one.)
 *
 * ⚠️ **STABLE, and that is a correctness requirement.** Kotlin's `sortedBy` is a stable sort, and
 * `build_item_list` has already ordered each group by name — so two files with the SAME mtime (which
 * is every file a `git clone` just wrote, and every WAV a chop just produced) come out in name order
 * under DATE_ASC. `std::sort` gives no such guarantee and would order them arbitrarily, differently on
 * each toolchain. This is `std::stable_sort`.
 */
void sort_items(std::vector<BrowserItem>& items, FileSortMode mode);

/**
 * Re-read the current directory and sort it. **Rebuild, never re-sort in place** — see the ⚠️ in the
 * body. The cursor is untouched, which is what makes this usable both for a sort change and for a
 * refresh after a rename/delete/paste.
 */
void rebuild_items(FileBrowserState& s, FileSystem& fs);

/** Enter `folder`: re-list it, reset the cursor, and drop any live selection. */
void navigate_to_folder(FileBrowserState& s, FileSystem& fs, const std::string& folder);

/** Up one level. A no-op at a filesystem root, where there is no parent to go to. */
void navigate_to_parent(FileBrowserState& s, FileSystem& fs);

// ─── The screen ──────────────────────────────────────────────────────────────────────────────────

class FileBrowserModule {
  public:
    static constexpr int WIDTH  = 640;
    static constexpr int HEIGHT = 480;   // full screen — it covers the visualizer and the right bar

    void draw(Canvas& c, int x, int y, const FileBrowserState& s, const Theme& t) const;
};

}  // namespace pt::ui
