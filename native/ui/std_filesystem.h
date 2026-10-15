#pragma once

// ui/filesystem.h on <filesystem>: the desktop and handheld shells, and every tool.
// ⚠️ The folder names under the root are the same on every platform, so a copied `PocketTracker/`
// folder is found where the app looks.

#include "ui/filesystem.h"

#include <string>
#include <vector>

namespace pt::ui {

class StdFileSystem : public FileSystem {
  public:
    // `root` holds the app folders, created on first use.
    explicit StdFileSystem(std::string root) : StdFileSystem(root, root) {}

    // `private_root` holds settings.json, template.ptp and autosave.ptp. They differ on Android only,
    // where the media tree may be unreadable at boot. config.json stays under `root`: the user edits it.
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
    std::string reverbs_directory() override     { return ensure_dir("Reverbs"); }
    std::string delays_directory() override      { return ensure_dir("Delays"); }

    // ── The app's own files, beside the folders — never inside one the browser lists ─────────────
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

    std::string root_;         // the user's media tree and config.json
    std::string privateRoot_;  // the app's own files
};

// ─── Path helpers ────────────────────────────────────────────────────────────────────────────────
// ⚠️ Not `path::extension()`: here `.bashrc` has the extension "bashrc". The browser filters on it.

std::string path_name(const std::string& path);        // "/a/b/c.wav" → "c.wav"
std::string path_extension(const std::string& path);   // "c.wav" → "wav", "README" → ""
std::string path_stem(const std::string& path);        // "/a/b/c.wav" → "c"

// `[^a-zA-Z0-9_-.]` → '_'. A folder name takes no dot (`allow_dot` false): "a.b" reads as a file.
std::string path_sanitize(const std::string& name, bool allow_dot);

std::string to_lower(std::string s);

// The app's folder when the shell does not say: `$POCKETTRACKER_HOME`, else Documents\PocketTracker
// (Windows, as Explorer resolves it), $HOME/Documents/PocketTracker (macOS),
// $XDG_DATA_HOME or ~/.local/share/PocketTracker (Linux), else ./PocketTracker.
std::string default_app_root();

}  // namespace pt::ui
