#pragma once

// ─── FileSystem, on <filesystem> ─────────────────────────────────────────────────────────────────
//
// The portable implementation of ui/filesystem.h — the one the SDL shell and every host tool use. It
// is the counterpart of `platform/android/AndroidFileSystem.kt`, and the only thing it does not
// inherit from it is *where the root is*: Android hard-codes `Documents/PocketTracker` (it must — that
// is the one place scoped storage lets it write and the user browse), while here the root is handed in
// by the shell, because a handheld has no Documents directory and a test wants a temp one.
//
// Everything under the root has the SAME seven names as Android, and that is not cosmetic: a user who
// copies their `PocketTracker/` folder off a phone and onto an SD card must find their projects where
// the app looks for them.

#include "ui/filesystem.h"

#include <string>
#include <vector>

namespace pt::ui {

class StdFileSystem : public FileSystem {
  public:
    /**
     * `root` is the directory the seven app folders live under — `<root>/Projects`, `<root>/Samples`
     * and so on. It is created on first use, as Android's are.
     *
     * The shell picks it (see `default_app_root()` below); a tool points it at a temp directory. That
     * is the whole of the platform-specific part of files, and it is one string.
     */
    explicit StdFileSystem(std::string root) : root_(std::move(root)) {}

    const std::string& root() const { return root_; }

    // ── The app's directories ───────────────────────────────────────────────────────────────────
    std::string projects_directory() override    { return ensure_dir("Projects"); }
    std::string samples_directory() override     { return ensure_dir("Samples"); }
    std::string renders_directory() override     { return ensure_dir("Renders"); }
    std::string resampled_directory() override   { return ensure_dir("Samples/Resampled"); }
    std::string instruments_directory() override { return ensure_dir("Instruments"); }
    std::string soundfonts_directory() override  { return ensure_dir("Soundfonts"); }
    std::string themes_directory() override      { return ensure_dir("Themes"); }

    // ── The app's own files ─────────────────────────────────────────────────────────────────────
    //
    // Both sit in the ROOT, not in a sub-directory: they are the app's, not the user's, and the six
    // folders above are what a user sees when the SD card goes into a card reader. (A PortMaster
    // launch script points the root at CONFDIR on the card, so both survive an app update.)
    std::string template_project_path() override { return root_ + "/template.ptp"; }
    std::string settings_path() override         { return root_ + "/settings.json"; }

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

    std::string root_;
};

// ─── Path helpers — Kotlin's java.io.File accessors, exactly ─────────────────────────────────────
//
// ⚠️ NOT `std::filesystem::path::extension()`, and the difference is not theoretical. Kotlin's
// `File.extension` is `name.substringAfterLast('.', "")`, so `.bashrc` has the extension "bashrc";
// <filesystem> treats a leading dot as a stem and answers "". The browser FILTERS on this string, so
// the two must agree — and the code that agrees with Kotlin is the code that reads like Kotlin.

/** The last path segment: "/a/b/c.wav" → "c.wav". */
std::string path_name(const std::string& path);

/** `File.extension` — "c.wav" → "wav", "README" → "". Case as it appears on disk. */
std::string path_extension(const std::string& path);

/** `File.nameWithoutExtension` — "/a/b/c.wav" → "c". */
std::string path_stem(const std::string& path);

/** Lowercased, for the case-insensitive comparisons (the extension filter, the NAME sort). */
std::string to_lower(std::string s);

/**
 * Where the app's folder goes when the shell does not say.
 *
 * `$POCKETTRACKER_HOME` wins if it is set — which is what a PortMaster launch script uses to point the
 * app at the SD card's ports directory. Otherwise `$XDG_DATA_HOME/PocketTracker`, then
 * `$HOME/.local/share/PocketTracker`, and finally `./PocketTracker` for a box with neither (a Windows
 * dev machine, a stripped-down CFW).
 */
std::string default_app_root();

}  // namespace pt::ui
