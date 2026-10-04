#pragma once

// ─── FileSystem, on <filesystem> ─────────────────────────────────────────────────────────────────
//
// The portable implementation of ui/filesystem.h, used by the desktop/handheld shells and every host
// tool. The root is handed in (a handheld has no Documents; a test wants a temp directory).
// ⚠️ The seven folder names under it are the same on every platform, so a `PocketTracker/` folder
// copied off a phone onto an SD card is found where the app looks.

#include "ui/filesystem.h"

#include <string>
#include <vector>

namespace pt::ui {

class StdFileSystem : public FileSystem {
  public:
    /** `root` holds the seven app folders (`<root>/Projects`, `<root>/Samples`, …), created on first use.
     *  The shell picks it (`default_app_root()`); a tool uses a temp directory. */
    explicit StdFileSystem(std::string root) : StdFileSystem(root, root) {}

    /**
     * The two-root form: `root` is the user's media tree, `private_root` where `settings.json`,
     * `template.ptp` and `autosave.ptp` live.
     * ⚠️ They differ only on Android, where the media tree may be unreadable at boot (nothing granted
     * yet): a settings file unreadable at boot is overwritten with defaults at quit. Android passes
     * `context.filesDir` — app-private, no permission needed.
     * ⚠️ `config.json` stays under `root`: the user hand-edits it, and app-private storage is reachable
     * only over adb.
     */
    StdFileSystem(std::string root, std::string private_root)
        : root_(std::move(root)), privateRoot_(std::move(private_root)) {}

    const std::string& root() const { return root_; }
    const std::string& private_root() const { return privateRoot_; }

    // ── The app's directories ───────────────────────────────────────────────────────────────────
    std::string projects_directory() override    { return ensure_dir("Projects"); }
    std::string samples_directory() override     { return ensure_dir("Samples"); }
    std::string renders_directory() override     { return ensure_dir("Renders"); }
    std::string resampled_directory() override   { return ensure_dir("Samples/Resampled"); }
    std::string instruments_directory() override { return ensure_dir("Instruments"); }
    std::string soundfonts_directory() override  { return ensure_dir("Soundfonts"); }
    std::string themes_directory() override      { return ensure_dir("Themes"); }
    std::string scales_directory() override      { return ensure_dir("Scales"); }
    std::string grooves_directory() override    { return ensure_dir("Grooves"); }

    // ── The app's own files ─────────────────────────────────────────────────────────────────────
    //
    // Not in a sub-directory: they are the app's, and the six folders above are what a user sees in a
    // card reader. (A PortMaster launch script points both roots at CONFDIR, so they survive updates.)
    // ⚠️ Especially the autosave: inside `Projects/` it would be listed, loadable and deletable
    // (FileSystem::autosave_file_path).
    std::string template_project_path() override { return privateRoot_ + "/template.ptp"; }
    std::string settings_path() override         { return privateRoot_ + "/settings.json"; }
    std::string config_path() override           { return root_ + "/config.json"; }
    std::string autosave_file_path() override    { return privateRoot_ + "/autosave.ptp"; }

    // ── Reading ─────────────────────────────────────────────────────────────────────────────────
    bool read_file(const std::string& path, std::string& out) override;
    std::vector<FileInfo> list_files(const std::string& directory) override;
    bool file_exists(const std::string& path) override;
    bool is_directory(const std::string& path) override;
    std::string parent_path(const std::string& path) override;

    // ── Writing ─────────────────────────────────────────────────────────────────────────────────
    bool write_file(const std::string& path, const std::string& content) override;
    bool write_bytes(const std::string& path, const void* data, size_t len) override;
    bool delete_path(const std::string& path) override;
    bool rename_file(const std::string& path, const std::string& new_base_name) override;
    std::string create_folder(const std::string& parent, const std::string& folder_name) override;
    bool move_file(const std::string& from, const std::string& to) override;
    bool copy_file(const std::string& from, const std::string& to) override;

  private:
    std::string ensure_dir(const char* sub);

    std::string root_;         // the user's media tree: the seven folders, and config.json
    std::string privateRoot_;  // the app's own three files; equal to root_ except on Android
};

// ─── Path helpers ────────────────────────────────────────────────────────────────────────────────
//
// ⚠️ NOT `std::filesystem::path::extension()`: here `.bashrc` has the extension "bashrc" (everything
// after the last dot), where <filesystem> answers "". The browser FILTERS on this string.

/** The last path segment: "/a/b/c.wav" → "c.wav". */
std::string path_name(const std::string& path);

/** "c.wav" → "wav", "README" → "". Case as it appears on disk. */
std::string path_extension(const std::string& path);

/** "/a/b/c.wav" → "c". */
std::string path_stem(const std::string& path);

/**
 * `[^a-zA-Z0-9_-.]` → '_' (`FileSystem::rename_file`'s rule). `allow_dot` false is `create_folder`'s rule
 * — a folder named "a.b" would read as a file. Public because `SafFileSystem` sanitises the same names.
 */
std::string path_sanitize(const std::string& name, bool allow_dot);

/** Lowercased, for the case-insensitive comparisons (the extension filter, the NAME sort). */
std::string to_lower(std::string s);

/**
 * Where the app's folder goes when the shell does not say. `$POCKETTRACKER_HOME` wins everywhere (a
 * PortMaster launch script points it at the SD card). Otherwise:
 *
 *   Windows   `Documents\PocketTracker`, Documents as Explorer resolves it (OneDrive's if that is what
 *             the machine uses), falling back to `%USERPROFILE%\Documents`
 *   macOS     `$HOME/Documents/PocketTracker`
 *   Linux     `$XDG_DATA_HOME/PocketTracker`, then `$HOME/.local/share/PocketTracker`
 *
 * and finally `./PocketTracker`. Desktops use Documents (a file manager reaches it); a handheld has none
 * and is reached over the SD card.
 */
std::string default_app_root();

}  // namespace pt::ui
