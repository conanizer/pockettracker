#pragma once

// ─── FILE BROWSER ────────────────────────────────────────────────────────────────────────────────
//
// A full-screen (640×480) list covering the whole layout — the top strip and right bar go away, so
// `TrackerLayout` draws it as a special case rather than inside the editor pane.
//
// A NAVIGATOR, not an editor: no cursor context, no `handle_input`, nothing written to the project.
// It owns a listing, a cursor, a sort order, a multi-select and a file clipboard, and the answer to
// "which file did the user pick" — which the dispatcher acts on according to WHY the browser was
// opened (browser_purpose.h). Rename and new-folder go through the QWERTY keyboard.

#include "common/sample_formats.h"
#include "songcore/model.h"
#include "ui/canvas.h"
#include "ui/filesystem.h"
#include "ui/theme.h"

#include <algorithm>
#include <string>
#include <vector>

namespace pt::ui {

/** 19 file rows + the two top status bars + the bottom bar. */
inline constexpr int BROWSER_VISIBLE_ROWS = 19;

/**
 * Kept out of the theme on purpose: it marks a file the browser can show but not load, and that
 * meaning must not change with the skin. Folders, ".." and ADD FOLDER… draw in `textTitle`.
 */
inline constexpr Argb COLOR_VIDEO  = 0xFFFFBB55;  // amber — a container we can show but not load

/**
 * Containers the browser can COLOUR but NOT load — Matroska/WebM are EBML, which the vendored
 * demuxer cannot read. Never in a filter set, so they appear only when showing everything, tinted.
 */
inline const std::vector<std::string>& video_extensions() {
    static const std::vector<std::string> v = {"mkv", "webm"};
    return v;
}

/** The sample formats the browser offers for a SAMPLER instrument — every one the loader reads. */
inline const std::vector<std::string>& sample_extensions() {
    static const std::vector<std::string> v = [] {
        std::vector<std::string> out;
        for (const SampleFormat& f : SAMPLE_FORMATS) out.emplace_back(f.ext);
        return out;
    }();
    return v;
}

/**
 * `sf3` is `sf2` with Vorbis-compressed samples, offered on the same terms (no size limit; see
 * audio-engine.h). ⚠️ The extension does not decide the decoder: tsf reads a compression flag per
 * `shdr`, so a `.sf2` can carry Vorbis samples and decode correctly.
 */
inline const std::vector<std::string>& soundfont_extensions() {
    static const std::vector<std::string> v = {"sf2", "sf3"};
    return v;
}

/**
 * True when `ext` (lowercase, no dot) is a SoundFont rather than a sample — derived from the list
 * above, so a format added there is handled everywhere.
 */
inline bool is_soundfont_extension(const std::string& ext) {
    const std::vector<std::string>& v = soundfont_extensions();
    return std::find(v.begin(), v.end(), ext) != v.end();
}

struct BrowserItem {
    /** ⚠️ Append, never insert: a member's position is its identity. */
    enum class Kind { PARENT, FOLDER, FILE, ACTION };

    Kind        kind = Kind::FILE;
    std::string path;         // absolute
    std::string displayName;  // "..", "[folder]", or the file's stem
    std::string extension;    // "" for PARENT/FOLDER; case as on disk
    std::string sizeText;     // "12KB" — computed once at build (see FileInfo's note)
    std::string dateText;     // "07-13-26"

    /** The sort keys, carried as data. */
    std::string sortName;      // the FULL name (with extension), lowercased
    int64_t     size         = 0;
    int64_t     lastModified = 0;

    /** A granted tree in Android's roots directory — see `FileInfo::isRoot`. Walk in, nothing else. */
    bool isRoot = false;

    bool is_parent() const { return kind == Kind::PARENT; }

    /**
     * "..", `ADD FOLDER…` and a granted TREE — the rows no file operation may touch.
     * ⚠️ Rename, delete, select, copy and cut all refuse on THIS: one predicate below the sites.
     * ⚠️⚠️ A tree row has EVERYTHING behind it: `delete_path("pt://<id>")` resolves to the granted
     * folder itself. Still a place you walk into, so it is a field rather than another `Kind`.
     */
    bool is_pseudo() const { return kind == Kind::PARENT || kind == Kind::ACTION || isRoot; }
};

/**
 * NORMAL browses; the other three are arm-then-confirm states where A is YES and B is NO.
 * ⚠️ Append, never insert. SET_HOME and FORGET_ROOT confirm because one moves where every app
 * folder is looked for and the other hands back an access permission.
 */
enum class BrowserMode { NORMAL, DELETE, SET_HOME, FORGET_ROOT };

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

    /**
     * The first row a selection may start on: the pinned rows are at the top, so it is the index of
     * the first real entry.
     *
     * ⚠️ Counted, not "1 if there is a `..`": with two pinned kinds a hard-coded 1 lets a
     * selection — what B copies and L+A pastes — start on the second.
     */
    int first_selectable() const {
        int i = 0;
        while (i < static_cast<int>(items.size()) && items[static_cast<size_t>(i)].is_pseudo()) ++i;
        return i;
    }

    /** True when `index` falls inside the live anchor..cursor range. */
    bool is_selected(int index) const;

    /** What SELECT+B deletes: every entry in a live selection, else the one under the cursor. Never a
     *  pinned row or a granted tree. */
    std::vector<const BrowserItem*> delete_targets() const;

    /** "CPY 3 FILES" / "CUT 1 FILE"; empty when the clipboard is. */
    std::string clipboard_info() const;
};

// ─── The listing ─────────────────────────────────────────────────────────────────────────────────

/**
 * Natural order over two already-lowercased `sortName`s: `1, 2, … 9, 10`, not `1, 10, 11, 2`.
 *
 * A digit run on both sides compares as a NUMBER (leading zeros skipped, then longest-run-wins, then
 * digit by digit); anything else compares by byte. The two are consistent because '0'..'9' is one
 * contiguous block, so a non-digit sorts either below every number or above every one of them.
 *
 * ⚠️ TOTAL, deliberately: `std::stable_sort` needs a strict weak ordering or it is UB. Names tying
 * under the numeric rule (`01` vs `1`) fall back to a raw compare.
 */
bool natural_name_less(const std::string& a, const std::string& b);

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
 * ".." stays pinned and folders stay above files: the sort orders each GROUP, it does not merge them.
 *
 * ⚠️ STABLE, a correctness requirement: `build_item_list` already name-sorted each group, so files
 * with the same mtime (a fresh clone, a chop's WAVs) stay in name order under DATE_ASC.
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
