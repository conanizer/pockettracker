#pragma once

// ─── The file system, as an interface ────────────────────────────────────────────────────────────
//
// An interface because WHERE the app's directories live is per-platform (Android's granted trees,
// a handheld's folder beside the binary or `$XDG_DATA_HOME`). Everything else — list, sort, rename,
// delete, move — is the same everywhere, and `StdFileSystem` implements it in portable C++17.
// `pt-ui` has no POSIX: <filesystem> is the standard library, so the tools link this headless on
// every CI platform.
// Sorting is not here: the browser's `sort_items` keeps ".." pinned and folders first, which a
// filesystem-level sort could not.

#include <cstdint>
#include <string>
#include <vector>

namespace pt::ui {

/**
 * One directory entry. `size` and `lastModified` are read ONCE, when the entry is built — a sort that
 * stat()s inside its comparator costs O(N log N) syscalls on every re-sort.
 */
struct FileInfo {
    std::string path;                 // absolute
    std::string name;                 // file/folder name, with extension
    std::string extension;            // "" for folders; case as it appears on disk
    bool        isDirectory = false;
    int64_t     size         = 0;     // bytes; 0 for folders
    int64_t     lastModified = 0;     // ms since the epoch

    /**
     * ⚠️ An ACTION, not a thing on disk: opening the row DOES something (`FileSystem::activate`), and
     * the browser refuses rename/delete/select/copy/cut on this flag rather than by recognising a path.
     * Only `list_files` produces one (Android's `ADD FOLDER…` in the virtual roots directory);
     * `StdFileSystem` never does.
     */
    bool        isAction = false;

    /**
     * ⚠️ A GRANTED TREE, not a folder inside one — Android's `pt://<root-id>` rows. Walk into it like a
     * directory, but it is a permission, not the app's to rename, move or copy.
     * ⚠️⚠️ `delete_path` on one resolves to the tree's OWN document — SELECT+B + A would remove the user's
     * whole PocketTracker folder. The browser refuses every file operation on this flag
     * (`BrowserItem::is_pseudo`). The only kind of row `FileSystem::set_home_directory` accepts.
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
 * ⚠️ The DECLARATION ORDER is behaviour: both step by index into this enum.
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

/** The label the browser draws. */
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
 * one, so the tests drive the browser against a temp directory.
 */
class FileSystem {
  public:
    virtual ~FileSystem() = default;

    // ── The app's directories (created on first use) ─────────────────────────────────────────────
    virtual std::string projects_directory()   = 0;
    virtual std::string samples_directory()    = 0;
    virtual std::string renders_directory()    = 0;
    virtual std::string resampled_directory()  = 0;
    virtual std::string instruments_directory() = 0;
    virtual std::string soundfonts_directory() = 0;
    virtual std::string themes_directory()     = 0;
    virtual std::string scales_directory()     = 0;
    virtual std::string grooves_directory()   = 0;

    // ── The app's own files ──────────────────────────────────────────────────────────────────────
    //
    // They belong to the APP, are never listed by the browser, and are real paths on every platform.

    /** The song TEMPLATE: what the app boots into. SETTINGS → TEMPLATE writes and deletes it. */
    virtual std::string template_project_path() = 0;

    /**
     * Where `settings.json` lives. ⚠️ Read at BOOT, before the user's storage is known to be reachable,
     * which is why Android answers with an app-private path (see `StdFileSystem`'s two-root
     * constructor).
     */
    virtual std::string settings_path() = 0;

    /** The user's hand-edited `config.json` — default browse folders, read on debug boot. */
    virtual std::string config_path() = 0;

    /**
     * The crash-recovery autosave.
     * ⚠️ Its PRESENCE is the signal: written while there is unsaved work, DELETED on every clean save /
     * load / new / exit — so finding one at launch means an unclean exit (RECOVER WORK?).
     * ⚠️ Somewhere the browser CANNOT see: a `.ptp` in `Projects/` would be offered as a project and
     * deletable under the recovery prompt. It sits in the app root beside `template.ptp` and
     * `settings.json` (or app-private storage on Android).
     */
    virtual std::string autosave_file_path() = 0;

    // ── Reading ─────────────────────────────────────────────────────────────────────────────────
    /** Whole file → string. False (and `out` untouched) if it cannot be read. */
    virtual bool read_file(const std::string& path, std::string& out) = 0;

    /** Entries in `directory`, unsorted and unfiltered. Empty if it does not exist or cannot be read. */
    virtual std::vector<FileInfo> list_files(const std::string& directory) = 0;

    /**
     * Discard anything remembered about `directory`, so the next `list_files` asks the source again.
     * The default is a no-op: `StdFileSystem` walks the directory every time. Android's
     * `SafFileSystem` caches (a listing is a content-provider query per entry), and only this can tell
     * it a file arrived from elsewhere. ⚠️ The browser's REFRESH calls it, not navigation.
     */
    virtual void forget_listing(const std::string& directory) { (void)directory; }

    virtual bool file_exists(const std::string& path) = 0;
    virtual bool is_directory(const std::string& path) = 0;

    /** The parent of `path`, or "" when it has none (i.e. `path` is a filesystem root). */
    virtual std::string parent_path(const std::string& path) = 0;

    /**
     * Run the ACTION entry at `path` (`FileInfo::isAction`). False = nothing started.
     * ⚠️ NOT necessarily finished on return: Android's folder picker is a separate activity, and the
     * grant lands seconds later or never. Waiting would block `SDL_APP_WILLENTERBACKGROUND`, delivered on
     * this thread — no autosave while the user picks. The browser's foreground refresh catches up.
     */
    virtual bool activate(const std::string& path) { (void)path; return false; }

    /**
     * Make `path` — an `isRoot` row — the app's home directory, PERSISTENTLY. False = refused, or no
     * such choice on this platform.
     * ⚠️ The directory accessors answer differently once this returns true: re-ask any derived path.
     * Files stay where they are; only where the app LOOKS moves.
     * Without it, on Android the first granted folder stays the home for ever. The home is never
     * derived from the grant set (`SafStorage.homeRootId` says why).
     */
    virtual bool set_home_directory(const std::string& path) { (void)path; return false; }

    /**
     * Give up the access an `isRoot` row was granted with. False = refused, or nothing to give up.
     * ⚠️ DELETES NOTHING — it drops a permission; the folder can be granted again. Without it a grant
     * (persisted even after its folder is deleted) could never leave the browser.
     * ⚠️ Giving up the HOME must not leave a stored home naming a vanished grant.
     */
    virtual bool revoke_access(const std::string& path) { (void)path; return false; }

    // ── Writing ─────────────────────────────────────────────────────────────────────────────────
    /** Write to a temp file, then rename into place: power loss or a pulled SD card mid-save must not
     *  leave a half-written project where the whole one was. */
    virtual bool write_file(const std::string& path, const std::string& content) = 0;
    virtual bool write_bytes(const std::string& path, const void* data, size_t len) = 0;

    /** Recursive for a folder. */
    virtual bool delete_path(const std::string& path) = 0;

    /** Rename in place, keeping the extension. `new_base_name` is sanitised to `[A-Za-z0-9_-.]`; FAILS
     *  rather than clobbering an existing target. */
    virtual bool rename_file(const std::string& path, const std::string& new_base_name) = 0;

    /** Create `folder_name` under `parent`. Returns its path, or "" if it exists or cannot be made. */
    virtual std::string create_folder(const std::string& parent, const std::string& folder_name) = 0;

    /** Move (cut/paste); copy + delete across filesystems. */
    virtual bool move_file(const std::string& from, const std::string& to) = 0;

    /** Copy (copy/paste). Fails if `to` exists — the caller de-duplicates the name first. */
    virtual bool copy_file(const std::string& from, const std::string& to) = 0;
};

}  // namespace pt::ui
