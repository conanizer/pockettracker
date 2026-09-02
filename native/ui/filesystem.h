#pragma once

// ─── The file system, as an interface ────────────────────────────────────────────────────────────
//
// The C++ twin of core/storage/IFileSystem.kt, and the reason it is an INTERFACE rather than a set of
// free functions is the same reason Kotlin's is: *where the app's directories live* is the one thing
// about files that is genuinely per-platform. Android resolves them under
// `Documents/PocketTracker/…` through `Environment` + scoped storage; a handheld running PortMaster
// has neither, and puts them beside the binary or under `$XDG_DATA_HOME`. Everything ELSE about files
// — list, sort, rename, delete, move — is identical everywhere, and `StdFileSystem` (std_filesystem.h)
// implements the lot in portable C++17.
//
// ⚠️ `pt-ui` still has no POSIX in it. <filesystem> is the C++ standard library, not a platform API:
// it compiles on MSVC, on gcc/libstdc++ and on clang/libc++ from the same source, which is exactly the
// property that lets `tools/ptshot` and `tools/ptinput` link this and run headless on all three CI
// runners. Reaching for <dirent.h> and stat(2) would have been the platform dependency the port plan
// forbids — and would not even have built on the Windows box the port is written on.
//
// ── The interface is SMALLER than Kotlin's, deliberately ─────────────────────────────────────────
//
// `IFileSystem` has three members with no live caller, and they are not ported:
//   • `sortFiles(files, mode)` — the browser never calls it; it sorts BrowserItems through its own
//     `sort_items` (which keeps ".." pinned and folders above files, neither of which this could do).
//     Two sorts with different rules, one of them dead, is how they drift.
//   • `hasStoragePermission()` — an Android runtime-permission question. A handheld has no such thing;
//     a directory it cannot read reports itself when the listing comes back empty.
//   • `getTemplateProjectPath()` / `getAutosaveFilePath()` — these land with the PROJECT screen, which
//     is what has the NEW / autosave-recovery flows behind it.

#include <cstdint>
#include <string>
#include <vector>

namespace pt::ui {

/**
 * One directory entry. The C++ twin of `core/storage/FileInfo`, plus two fields Kotlin does not have:
 * `size` and `lastModified` are carried as DATA here, not re-read from the disk on demand.
 *
 * ⚠️ That is not a gratuitous difference — it is the fix for a real cost. Kotlin's `sortItems` sorts by
 * `it.file.lastModified()` and `it.file.length()`, which are `stat(2)` calls made *inside the
 * comparator*: an N-entry directory therefore costs O(N log N) syscalls to sort, and the browser
 * re-sorts on every R+UP. (The Kotlin module already learned half this lesson — `FileItem` caches
 * `sizeText`/`dateText` at build time because formatting them in the 60 fps draw pass "meant ~2.3k
 * syscalls/s" — but the sort kept its own.) The keys are read ONCE, when the entry is built, and the
 * ORDER is identical; only the syscall count differs.
 */
struct FileInfo {
    std::string path;                 // absolute
    std::string name;                 // file/folder name, with extension
    std::string extension;            // "" for folders; case as it appears on disk
    bool        isDirectory = false;
    int64_t     size         = 0;     // bytes; 0 for folders
    int64_t     lastModified = 0;     // ms since the epoch, as java.io.File.lastModified() reports it

    /**
     * ⚠️ **An ACTION, not a thing on disk.** Opening the row DOES something (`FileSystem::activate`)
     * and there is nothing behind it to rename, delete, select, copy or cut — the browser refuses all
     * of those on the strength of this flag rather than by recognising a path, so a second action can
     * never be added and forgotten at one of those six sites.
     *
     * `list_files` is the only thing that can produce one. The one that exists is Android's
     * `ADD FOLDER…` in the virtual roots directory, which fires the system folder picker;
     * `StdFileSystem` never sets it, so every other platform simply has no such row and needs no
     * `#ifdef` to say so.
     */
    bool        isAction = false;

    /**
     * ⚠️ **A GRANTED TREE, not a folder inside one** — Android's `pt://<root-id>` rows, the children of
     * the virtual roots directory. It is a directory and you walk into it exactly like one, but it is
     * not the app's to rename, delete, move or copy: what is behind it is a *permission*, revoked in
     * the system's own settings and nowhere else.
     *
     * ⚠️⚠️ **`delete_path` on one resolves to the tree's OWN document**, so SELECT+B + A on such a row
     * would remove the user's whole PocketTracker folder. The browser refuses every file operation on
     * the strength of this flag (`BrowserItem::is_pseudo`), not by recognising a path — the same rule,
     * and the same reason, as `isAction` above. `StdFileSystem` never sets it.
     *
     * It is also what `FileSystem::set_home_directory` accepts, and the only kind of row that offers it.
     */
    bool        isRoot = false;

    /** "mysong.ptp" → "mysong". Folders and extension-less files return the name unchanged. */
    std::string name_without_extension() const {
        if (extension.empty() || name.size() <= extension.size() + 1) return name;
        return name.substr(0, name.size() - extension.size() - 1);
    }
};

/**
 * How the browser's R+UP / R+DOWN cycles the listing.
 *
 * ⚠️ **The DECLARATION ORDER is behaviour, not documentation.** R+UP steps to the next mode and R+DOWN
 * to the previous, both by index into this enum (`FileSortMode.values()` on Android — see
 * AppInputDispatcher.handleRUp). Reordering these six silently changes what the sort button does.
 */
enum class FileSortMode {
    DATE_DESC,  // newest first
    DATE_ASC,   // oldest first
    NAME_ASC,   // A-Z
    NAME_DESC,  // Z-A
    SIZE_ASC,   // smallest first
    SIZE_DESC   // largest first
};

inline constexpr int FILE_SORT_MODE_COUNT = 6;

/** The label the browser draws. Kotlin carries these on the enum itself (`FileSortMode(val label)`). */
inline const char* file_sort_label(FileSortMode m) {
    switch (m) {
        case FileSortMode::DATE_DESC: return "DATE v";
        case FileSortMode::DATE_ASC:  return "DATE ^";
        case FileSortMode::NAME_ASC:  return "NAME ^";
        case FileSortMode::NAME_DESC: return "NAME v";
        case FileSortMode::SIZE_ASC:  return "SIZE ^";
        case FileSortMode::SIZE_DESC: return "SIZE v";
    }
    return "";
}

/**
 * Everything the app does to a disk. One implementation per platform; the UI never names a concrete
 * one, which is what lets `tools/ptinput` drive the browser against a temp directory and the SDL shell
 * drive it against the user's.
 */
class FileSystem {
  public:
    virtual ~FileSystem() = default;

    // ── The app's directories (created on first use, as Android's are) ───────────────────────────
    virtual std::string projects_directory()   = 0;
    virtual std::string samples_directory()    = 0;
    virtual std::string renders_directory()    = 0;
    virtual std::string resampled_directory()  = 0;
    virtual std::string instruments_directory() = 0;
    virtual std::string soundfonts_directory() = 0;
    virtual std::string themes_directory()     = 0;
    virtual std::string scales_directory()     = 0;

    // ── The app's own files ──────────────────────────────────────────────────────────────────────
    //
    // Files that belong to the APP rather than to the user, and none of them is ever listed by the
    // browser. Every one is a real path on every platform — there is no SharedPreferences path and no
    // key-value store behind any of them.

    /** The song TEMPLATE: what the app boots into. SETTINGS → TEMPLATE writes and deletes it. */
    virtual std::string template_project_path() = 0;

    /**
     * Where the shell keeps `settings.json` — a real file on every platform. There is no key-value
     * store behind it anywhere; `MainActivity.importLegacySettings` writes this very file, having
     * migrated the values out of SharedPreferences once.
     *
     * ⚠️ It is read at BOOT, before any of the user's storage is known to be reachable, which is why
     * Android answers with an app-private path rather than one inside the media tree. See
     * `StdFileSystem`'s two-root constructor for the whole of that argument.
     */
    virtual std::string settings_path() = 0;

    /** The user's hand-edited `config.json` — default browse folders, read on debug boot. */
    virtual std::string config_path() = 0;

    /**
     * The CRASH-RECOVERY autosave (S10).
     *
     * ⚠️ **Its PRESENCE is the signal, and that is the whole design.** The file is written while there
     * is unsaved work and DELETED on every clean save / load / new / exit — so finding one at launch
     * means the last session did not end cleanly (a launcher's kill, a flat battery, a crash), and
     * that is exactly what the RECOVER WORK? prompt keys on. Kotlin says the same in one line:
     * "its presence at next launch signals an unclean exit" (AutosaveManager).
     *
     * ⚠️ It must therefore be somewhere the browser CANNOT see — a `.ptp` in `Projects/` would be
     * offered as a normal project to load, and deleting it from under the recovery prompt would be a
     * button press away. Android gets this for free from app-private storage; elsewhere it sits in the
     * root beside `template.ptp` and `settings.json`, which is the same argument those two already
     * made: the root is the app's, and the six sub-directories are the user's.
     */
    virtual std::string autosave_file_path() = 0;

    // ── Reading ─────────────────────────────────────────────────────────────────────────────────
    /** Whole file → string. False (and `out` untouched) if it cannot be read. */
    virtual bool read_file(const std::string& path, std::string& out) = 0;

    /** Entries in `directory`, unsorted and unfiltered. Empty if it does not exist or cannot be read. */
    virtual std::vector<FileInfo> list_files(const std::string& directory) = 0;

    /**
     * Discard whatever is remembered about `directory`, so the next `list_files` asks the source of
     * truth again. Cheap enough to call unconditionally; a directory that was never listed is fine.
     *
     * ⚠️ **The default is a no-op because most implementations remember nothing.** `StdFileSystem`
     * walks the directory on every call and is therefore always current. Android's `SafFileSystem`
     * caches, because a listing there is a content-provider query per entry — and a cache dropped
     * only by this app's own writes cannot see a file that arrived from a file manager, a download
     * or a PC. Those exist, so a listing needs a way to be declared stale by something other than a
     * write.
     *
     * ⚠️ It is the BROWSER'S REFRESH that calls this, not navigation. Walking into a folder is
     * allowed to be cheap; a refresh is the one gesture that means *the world may have moved
     * underneath me*.
     */
    virtual void forget_listing(const std::string& directory) { (void)directory; }

    virtual bool file_exists(const std::string& path) = 0;
    virtual bool is_directory(const std::string& path) = 0;

    /** The parent of `path`, or "" when it has none (i.e. `path` is a filesystem root). */
    virtual std::string parent_path(const std::string& path) = 0;

    /**
     * Run the ACTION entry at `path` (see `FileInfo::isAction`). False = nothing was started.
     *
     * ⚠️ **It has NOT necessarily finished when this returns.** The one implementation fires Android's
     * folder picker, which is a separate activity: this returns as soon as the intent is away, and the
     * grant lands seconds later, minutes later, or never. Waiting here would be worse than useless —
     * `SDL_APP_WILLENTERBACKGROUND` is delivered on the thread that calls this, inside its own
     * `SDL_PollEvent`, so a thread parked in a picker is a session whose autosave never runs for as
     * long as the user is picking. The listing catches up through the browser's foreground refresh.
     *
     * Not pure: an implementation that produces no action entries can never be asked to run one.
     */
    virtual bool activate(const std::string& path) { (void)path; return false; }

    /**
     * Make `path` — a row carrying `FileInfo::isRoot` — the directory the app's own folders live under
     * from now on, PERSISTENTLY. False = refused, or this platform has no such choice to make.
     *
     * ⚠️ **The seven accessors answer differently the moment this returns true**, so a caller holding a
     * derived path (a media base dir, an app root, a cached browse directory) is holding a stale one and
     * must re-ask. Everything already on disk stays exactly where it is: this moves where the app
     * LOOKS, never any files.
     *
     * ⚠️ It exists because on Android the choice is otherwise **unrecoverable**: the first folder a user
     * grants becomes the home and stays it, and a user who granted the wrong one had no way back except
     * clearing the app's data. Nothing derives the home from the grant set, and nothing may — see
     * `SafStorage.homeRootId` for why a computed answer moves a user's songs the day they grant a second
     * folder.
     *
     * Not pure: a platform whose root is a fixed location has nothing to set, and says so by refusing.
     */
    virtual bool set_home_directory(const std::string& path) { (void)path; return false; }

    /**
     * Give up the access `path` — an `isRoot` row — was granted with. False = refused, or this platform
     * grants nothing to give up.
     *
     * ⚠️ **This DELETES NOTHING.** It drops a permission; every file stays where it is, and the user can
     * grant the same folder again. It is the counterpart to `activate`'s ADD FOLDER…, and without it a
     * grant is permanent: Android keeps a persisted permission after the folder behind it is deleted, so
     * a row that opens on nothing would otherwise sit in the browser for the life of the install with no
     * gesture able to remove it.
     *
     * ⚠️ Giving up the HOME leaves the app to pick another; the implementation must not leave a stored
     * home naming a grant that no longer exists.
     */
    virtual bool revoke_access(const std::string& path) { (void)path; return false; }

    // ── Writing ─────────────────────────────────────────────────────────────────────────────────
    /**
     * Write, then rename into place. The temp-file dance is Android's (`AndroidFileSystem.writeFile`)
     * and it is worth keeping on a handheld for the reason it exists: a device that loses power — or a
     * user who pulls the SD card — mid-save must not be left with a half-written project where the
     * whole one used to be.
     */
    virtual bool write_file(const std::string& path, const std::string& content) = 0;
    virtual bool write_bytes(const std::string& path, const void* data, size_t len) = 0;

    /** Recursive for a folder, as `deleteFileOrFolder` is. */
    virtual bool delete_path(const std::string& path) = 0;

    /**
     * Rename in place, keeping a file's extension. `new_base_name` is sanitised to
     * `[A-Za-z0-9_-.]` (Android does the same) and the call FAILS rather than clobbering an existing
     * target.
     */
    virtual bool rename_file(const std::string& path, const std::string& new_base_name) = 0;

    /** Create `folder_name` under `parent`. Returns its path, or "" if it exists or cannot be made. */
    virtual std::string create_folder(const std::string& parent, const std::string& folder_name) = 0;

    /** Move (cut/paste). Falls back to copy+delete across filesystems, as Android's does. */
    virtual bool move_file(const std::string& from, const std::string& to) = 0;

    /** Copy (copy/paste). Fails if `to` exists — the caller de-duplicates the name first. */
    virtual bool copy_file(const std::string& from, const std::string& to) = 0;
};

}  // namespace pt::ui
