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

    // ── Reading ─────────────────────────────────────────────────────────────────────────────────
    /** Whole file → string. False (and `out` untouched) if it cannot be read. */
    virtual bool read_file(const std::string& path, std::string& out) = 0;

    /** Entries in `directory`, unsorted and unfiltered. Empty if it does not exist or cannot be read. */
    virtual std::vector<FileInfo> list_files(const std::string& directory) = 0;

    virtual bool file_exists(const std::string& path) = 0;
    virtual bool is_directory(const std::string& path) = 0;

    /** The parent of `path`, or "" when it has none (i.e. `path` is a filesystem root). */
    virtual std::string parent_path(const std::string& path) = 0;

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
