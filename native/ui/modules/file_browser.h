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

#include <algorithm>
#include <string>
#include <vector>

namespace pt::ui {

/** 19 file rows + the two top status bars + the bottom bar. */
inline constexpr int BROWSER_VISIBLE_ROWS = 19;

/**
 * Kept out of the theme on purpose: these three say what KIND of thing a row is, and that meaning must
 * not change when the user picks a different skin.
 *
 * ⚠️ A FOLDER is NOT one of them — it draws in `textTitle` and follows the palette. It is the one kind
 * here whose row is not an exception to the list but the ordinary case of it, so a fixed blue simply
 * read as a colour the theme had forgotten.
 */
inline constexpr Argb COLOR_VIDEO  = 0xFFFFBB55;  // amber — a container we can show but not load
inline constexpr Argb COLOR_PARENT = 0xFFFFAA88;  // orange — ".."
inline constexpr Argb COLOR_ACTION = 0xFF88FF88;  // green — a row that DOES something, not one that is

/**
 * Containers the browser can COLOUR but NOT load — audio-in, but no decoder for them. Since the
 * media-unification step the ISO-BMFF containers (mp4/m4a/m4b/mov/3gp) DO load, as in-place samples via
 * minimp4 + FAAD2, so they moved to `sample_extensions()`. What is left is Matroska/WebM, which are
 * EBML, not ISO-BMFF — the vendored decoder cannot demux them (a slim EBML reader is a deferred, lower-
 * priority item; see `linux-port-plan.md` §4.6). They are never in a filter set here, so they only ever
 * appear when the browser is showing everything, tinted so the user sees "shown, but not loadable".
 */
inline const std::vector<std::string>& video_extensions() {
    static const std::vector<std::string> v = {"mkv", "webm"};
    return v;
}

/**
 * The sample formats the browser offers for a SAMPLER instrument.
 *
 * Kept in lockstep with `songcore::is_native_compressed` and `AudioEngine::loadSampleFromCompressed`'s
 * dispatch — the list, the predicate and the decoder change together, as the old comment here promised
 * they would "the day a native AAC decoder lands". That day was the media-unification step: `m4a` and
 * the ISO-BMFF container extensions now load in place via minimp4 + FAAD2, exactly like mp3/ogg. Raw
 * `.aac` (ADTS) stays out — it is a bare stream, not a container the demuxer can open.
 */
inline const std::vector<std::string>& sample_extensions() {
    static const std::vector<std::string> v = {"wav", "mp3", "flac", "ogg", "opus",
                                               "m4a", "mp4", "m4b", "mov", "3gp"};
    return v;
}

/**
 * `sf3` is the same font as `sf2` with its samples Vorbis-compressed, and it is offered on exactly
 * the same terms — no size limit on either. It costs about 3× the load time and HALF the peak memory
 * of the uncompressed twin; the measurements and the reason there is no cap are in `audio-engine.h`.
 *
 * ⚠️ The extension does not decide the decoder. tsf reads a compression flag off each `shdr`, so a
 * file named `.sf2` can carry Vorbis samples and will decode correctly — see `soundfont-voice.cpp`,
 * where the stb_vorbis include that makes that work is guarded by a comment saying why it stays.
 */
inline const std::vector<std::string>& soundfont_extensions() {
    static const std::vector<std::string> v = {"sf2", "sf3"};
    return v;
}

/**
 * True when `ext` — lowercase, no dot — is one the browser treats as a SoundFont rather than a
 * sample. Derived from the list above rather than spelled out again at the sites that ask, so a
 * fourth format added there is handled everywhere at once instead of at whichever call sites
 * someone remembered.
 */
inline bool is_soundfont_extension(const std::string& ext) {
    const std::vector<std::string>& v = soundfont_extensions();
    return std::find(v.begin(), v.end(), ext) != v.end();
}

struct BrowserItem {
    /** ⚠️ ACTION is APPENDED, per the project's rule about enum members: a member's position is its
     *  identity, and inserting one renumbers every value written against the old order. */
    enum class Kind { PARENT, FOLDER, FILE, ACTION };

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

    /** A granted tree in Android's roots directory — see `FileInfo::isRoot`. Walk in, nothing else. */
    bool isRoot = false;

    bool is_parent() const { return kind == Kind::PARENT; }

    /**
     * ".." , `ADD FOLDER…` and a granted TREE — the rows no file operation may touch.
     *
     * ⚠️ **Rename, delete, select, copy and cut all refuse on THIS, not on `is_parent()`.** Those six
     * sites were written when ".." was the only such row; a second one that had to be remembered at
     * each of them would be wrong at whichever was missed, and nothing would say so until a user
     * pressed SELECT+B on it. One predicate, below the sites.
     *
     * ⚠️⚠️ **A tree row is here for a harsher reason than the other two.** The first two have nothing
     * behind them; a tree has EVERYTHING behind it — `delete_path("pt://<id>")` resolves to the granted
     * folder's own document, so SELECT+B and A on that row removed the user's entire PocketTracker
     * directory and said `DELETED`. Unlike them it is still a place you walk into, so the flag is a
     * field rather than another `Kind`.
     */
    bool is_pseudo() const { return kind == Kind::PARENT || kind == Kind::ACTION || isRoot; }
};

/** NORMAL browses; DELETE is the "A=YES B=NO" confirm over the row under the cursor. */
/**
 * NORMAL browses; the other three are arm-then-confirm states where A is YES and B is NO.
 *
 * ⚠️ **SET_HOME and FORGET_ROOT are APPENDED**, per the project's rule about enum members — a member's
 * position is its identity. Both are confirms rather than immediate actions: one changes where every
 * app folder is looked for, the other hands back an access permission, and neither should be reachable
 * by one keypress on a row the user was only trying to open.
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
     * ⚠️ **Counted, not "1 if there is a `..`".** With two pinned kinds a hard-coded 1 lets a
     * selection start ON the second one — and the selection is what B copies and L+A pastes.
     */
    int first_selectable() const {
        int i = 0;
        while (i < static_cast<int>(items.size()) && items[static_cast<size_t>(i)].is_pseudo()) ++i;
        return i;
    }

    /** True when `index` falls inside the live anchor..cursor range. */
    bool is_selected(int index) const;

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
 * ⚠️ **Total, deliberately.** `std::stable_sort` takes a strict weak ordering and a comparator that is
 * not one is undefined behaviour, not merely a wrong order — and the failure is invisible until a
 * different standard library's sort surfaces it. Names that tie under the numeric rule but are not the
 * same string (`01` vs `1`) fall back to a raw compare, so equivalence here means equality.
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
