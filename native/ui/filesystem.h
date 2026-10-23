#pragma once

// The file system as an interface: where the app's directories live is per platform. `StdFileSystem`
// is the portable implementation; Android's goes through granted trees.

#include <cctype>
#include <cstdint>
#include <string>
#include <vector>

namespace pt::ui {

// One directory entry; `size` and `lastModified` are read once, so a sort does no syscalls.
struct FileInfo {
    std::string path;                 // absolute
    std::string name;                 // file/folder name, with extension
    std::string extension;            // "" for folders; case as it appears on disk
    bool        isDirectory = false;
    int64_t     size         = 0;     // bytes; 0 for folders
    int64_t     lastModified = 0;     // ms since the epoch

    // An action, not a file (Android's ADD FOLDER…): opening it calls `activate`, and the browser
    // refuses every file operation on it.
    bool        isAction = false;

    // A granted tree (Android's `pt://<root-id>` rows): walk into it, but it is a permission.
    // ⚠️ `delete_path` on one deletes the user's whole folder; the browser refuses every file operation.
    bool        isRoot = false;

    // "mysong.ptp" → "mysong".
    std::string name_without_extension() const {
        if (extension.empty() || name.size() <= extension.size() + 1) return name;
        return name.substr(0, name.size() - extension.size() - 1);
    }
};

// What R+UP/DOWN cycle through, in this order.
enum class FileSortMode {
    DATE_DESC,  // newest first
    DATE_ASC,   // oldest first
    NAME_ASC,   // A-Z
    NAME_DESC,  // Z-A
    SIZE_ASC,   // smallest first
    SIZE_DESC   // largest first
};

inline constexpr int FILE_SORT_MODE_COUNT = 6;

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

// Everything the app does to a disk.
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
    virtual std::string reverbs_directory()    = 0;
    virtual std::string delays_directory()     = 0;
    virtual std::string songs_directory()      = 0;   // COLLECT's song folders

    // ── The app's own files, never listed by the browser ─────────────────────────────────────────

    // What the app boots into; SETTINGS → TEMPLATE writes and deletes it.
    virtual std::string template_project_path() = 0;

    // Read at boot, before the user's storage is known to be reachable — app-private on Android.
    virtual std::string settings_path() = 0;

    // The hand-edited `config.json`, read on debug boot.
    virtual std::string config_path() = 0;

    // The crash autosave. Its presence is the signal: deleted on every clean save, load, new and exit.
    // ⚠️ Kept where the browser cannot see it, or it would be offered as a project.
    virtual std::string autosave_file_path() = 0;

    // ── Reading ─────────────────────────────────────────────────────────────────────────────────
    // False, `out` untouched, if it cannot be read.
    virtual bool read_file(const std::string& path, std::string& out) = 0;

    // Unsorted and unfiltered; empty if unreadable.
    virtual std::vector<FileInfo> list_files(const std::string& directory) = 0;

    // Drop a cached listing (Android caches; the default walks every time). The browser's refresh calls it.
    virtual void forget_listing(const std::string& directory) { (void)directory; }

    virtual bool file_exists(const std::string& path) = 0;
    virtual bool is_directory(const std::string& path) = 0;

    // "" for a filesystem root.
    virtual std::string parent_path(const std::string& path) = 0;

    // Run an `isAction` entry; false = nothing started.
    // ⚠️ Not finished on return: Android's folder picker is another activity, and blocking here would
    // block the background event (the autosave). The foreground refresh catches up.
    virtual bool activate(const std::string& path) { (void)path; return false; }

    // Make an `isRoot` row the app's home, persistently; false = refused or not offered here.
    // ⚠️ The directory accessors answer differently afterwards: re-ask any derived path.
    virtual bool set_home_directory(const std::string& path) { (void)path; return false; }

    // Give up an `isRoot` row's grant. Deletes nothing.
    // ⚠️ Giving up the home must not leave a stored home naming a vanished grant.
    virtual bool revoke_access(const std::string& path) { (void)path; return false; }

    // ── Writing ─────────────────────────────────────────────────────────────────────────────────
    // Through a temp file and a rename, so a pulled card cannot leave a half-written project.
    virtual bool write_file(const std::string& path, const std::string& content) = 0;
    virtual bool write_bytes(const std::string& path, const void* data, size_t len) = 0;

    // Recursive for a folder.
    virtual bool delete_path(const std::string& path) = 0;

    // Rename in place, keeping the extension; the name is sanitised. Fails rather than overwrite.
    virtual bool rename_file(const std::string& path, const std::string& new_base_name) = 0;

    // Returns the new path, or "" if it exists or cannot be made.
    virtual std::string create_folder(const std::string& parent, const std::string& folder_name) = 0;

    // `sub` ("Songs/X/Samples/My Kit") under the app folder, parents included and names kept as
    // given; its path, existing or new. ⚠️ Unlike `create_folder`, nothing is sanitised.
    virtual std::string make_app_folder(const std::string& sub) = 0;

    // Copy + delete across filesystems.
    virtual bool move_file(const std::string& from, const std::string& to) = 0;

    // Fails if `to` exists; the caller de-duplicates the name first.
    virtual bool copy_file(const std::string& from, const std::string& to) = 0;
};

// A name as a FAT32-safe filename: anything outside `[A-Za-z0-9_]` → `_`. The caller supplies a
// fallback for an empty one (`.pts` alone is a hidden dotfile).
inline std::string safe_file_name(const std::string& name) {
    std::string out;
    out.reserve(name.size());
    for (const char c : name) {
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                        (c >= '0' && c <= '9') || c == '_';
        out += ok ? c : '_';
    }
    return out;
}

// Does `dir` hold a file with this extension, in any case?
inline bool folder_has_extension(FileSystem& fs, const std::string& dir, const std::string& ext) {
    for (const FileInfo& f : fs.list_files(dir)) {
        if (f.isDirectory) continue;
        std::string e = f.extension;
        for (char& c : e) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (e == ext) return true;
    }
    return false;
}

}  // namespace pt::ui
