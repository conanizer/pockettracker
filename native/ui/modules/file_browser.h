#pragma once

// The file browser: a full-screen list (no top strip or right bar). A navigator, not an editor — it
// owns a listing, cursor, sort, multi-select and file clipboard; the dispatcher acts on the pick
// according to why it was opened (browser_purpose.h).

#include "common/sample_formats.h"
#include "songcore/model.h"
#include "ui/canvas.h"
#include "ui/filesystem.h"
#include "ui/theme.h"

#include <algorithm>
#include <string>
#include <vector>

namespace pt::ui {

inline constexpr int BROWSER_VISIBLE_ROWS = 19;

// Outside the theme on purpose: "can show, cannot load" must not change with the skin.
inline constexpr Argb COLOR_VIDEO  = 0xFFFFBB55;  // amber — a container we can show but not load

// Containers the browser can colour but not load (EBML, which the demuxer cannot read). Shown only
// when showing everything.
inline const std::vector<std::string>& video_extensions() {
    static const std::vector<std::string> v = {"mkv", "webm"};
    return v;
}

// Every sample format the loader reads.
inline const std::vector<std::string>& sample_extensions() {
    static const std::vector<std::string> v = [] {
        std::vector<std::string> out;
        for (const SampleFormat& f : SAMPLE_FORMATS) out.emplace_back(f.ext);
        return out;
    }();
    return v;
}

// ⚠️ The extension does not pick the decoder: tsf reads a compression flag per sample, so a `.sf2`
// can carry Vorbis.
inline const std::vector<std::string>& soundfont_extensions() {
    static const std::vector<std::string> v = {"sf2", "sf3"};
    return v;
}

// `ext` lowercase, no dot.
inline bool is_soundfont_extension(const std::string& ext) {
    const std::vector<std::string>& v = soundfont_extensions();
    return std::find(v.begin(), v.end(), ext) != v.end();
}

struct BrowserItem {
    // ⚠️ Append, never insert.
    enum class Kind { PARENT, FOLDER, FILE, ACTION };

    Kind        kind = Kind::FILE;
    std::string path;         // absolute
    std::string displayName;  // "..", "[folder]", or the file's stem
    std::string extension;    // "" for PARENT/FOLDER; case as on disk
    std::string sizeText;     // "12KB", computed once at build
    std::string dateText;     // "07-13-26"

    std::string sortName;      // the FULL name (with extension), lowercased
    int64_t     size         = 0;
    int64_t     lastModified = 0;

    // A granted tree in Android's roots directory: walk in, nothing else.
    bool isRoot = false;

    bool is_parent() const { return kind == Kind::PARENT; }

    // "..", `ADD FOLDER…` and a granted tree: no file operation may touch these.
    // ⚠️ A tree row has everything behind it — `delete_path("pt://<id>")` is the granted folder.
    bool is_pseudo() const { return kind == Kind::PARENT || kind == Kind::ACTION || isRoot; }
};

// NORMAL browses; the others are arm-then-confirm (A = YES, B = NO). ⚠️ Append, never insert.
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

    // Empty = every file; else the lowercased extensions that pass.
    std::vector<std::string> fileExtensions;

    // ── The multi-select and the file clipboard (L+B, B, L+A) ───────────────────────────────────
    bool                     selectionMode   = false;
    int                      selectionAnchor = -1;
    std::vector<std::string> fileClipboard;
    bool                     fileClipboardIsCut = false;

    // The last L+B tap, for "tap again to select all".
    long long lastSelectTapMs = 0;

    const BrowserItem* item_at(int index) const {
        if (index < 0 || index >= static_cast<int>(items.size())) return nullptr;
        return &items[static_cast<size_t>(index)];
    }
    const BrowserItem* current() const { return item_at(cursor); }

    // The first row a selection may start on: the first after the pinned rows. ⚠️ Counted, not
    // "1 if there is a `..`" — there are two pinned kinds.
    int first_selectable() const {
        int i = 0;
        while (i < static_cast<int>(items.size()) && items[static_cast<size_t>(i)].is_pseudo()) ++i;
        return i;
    }

    bool is_selected(int index) const;

    // A live selection, else the entry under the cursor. Never a pinned row or a granted tree.
    std::vector<const BrowserItem*> delete_targets() const;

    // "CPY 3 FILES" / "CUT 1 FILE"; empty when the clipboard is.
    std::string clipboard_info() const;
};

// ─── The listing ─────────────────────────────────────────────────────────────────────────────────

// Natural order over lowercased names: `1, 2, … 10`, not `1, 10, 2`. ⚠️ Total on purpose
// (`stable_sort` needs a strict weak order): `01` vs `1` falls back to a raw compare.
bool natural_name_less(const std::string& a, const std::string& b);

// ".." if there is a parent, then folders, then the files `extensions` passes (empty = all), each
// name-sorted. Hidden entries dropped.
std::vector<BrowserItem> build_item_list(FileSystem& fs, const std::string& directory,
                                         const std::vector<std::string>& extensions);

// ".." stays pinned and folders above files. ⚠️ Stable on purpose: files with the same mtime stay
// in the name order `build_item_list` gave them.
void sort_items(std::vector<BrowserItem>& items, FileSortMode mode);

// Re-read and sort (rebuild, never re-sort in place). The cursor is untouched.
void rebuild_items(FileBrowserState& s, FileSystem& fs);

// Re-list, reset the cursor, drop any selection.
void navigate_to_folder(FileBrowserState& s, FileSystem& fs, const std::string& folder);

// A no-op at a filesystem root.
void navigate_to_parent(FileBrowserState& s, FileSystem& fs);

// ─── The screen ──────────────────────────────────────────────────────────────────────────────────

class FileBrowserModule {
  public:
    static constexpr int WIDTH  = 640;
    static constexpr int HEIGHT = 480;

    void draw(Canvas& c, int x, int y, const FileBrowserState& s, const Theme& t) const;
};

}  // namespace pt::ui
