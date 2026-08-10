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
    explicit StdFileSystem(std::string root) : StdFileSystem(root, root) {}

    /**
     * The two-root form: `root` is the user's media tree, `private_root` is where the app's own
     * `settings.json`, `template.ptp` and `autosave.ptp` live.
     *
     * ⚠️ **They differ on Android alone, and only because the media tree there is not guaranteed to be
     * readable at boot.** Those three files are read before the UI exists — before any folder has been
     * granted, and on a fresh install there is no granted folder at all. A settings file that cannot be
     * read at boot is one that gets overwritten with the factory defaults at quit, which is precisely
     * the failure `load_settings`'s two-meaning `false` was built to distinguish. So Android hands in
     * `context.filesDir`: app-private, needs no permission, and survives the media tree being absent,
     * unreadable or revoked.
     *
     * ⚠️ **`config.json` stays under `root`, not here**, and that is a decision rather than an
     * oversight: it is the one app file the *user* hand-edits, and app-private storage is reachable
     * only over adb. `load_folder_config` already treats "no file" as its common case, so a media tree
     * that is not there yet reads exactly like a config that was never written.
     *
     * Every non-Android caller passes the same string twice — the one-argument constructor above — and
     * behaves identically to a build that had never heard of a private root.
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

    // ── The app's own files ─────────────────────────────────────────────────────────────────────
    //
    // None of the four sits in a sub-directory: they are the app's, not the user's, and the six folders
    // above are what a user sees when the SD card goes into a card reader. (A PortMaster launch script
    // points both roots at CONFDIR on the card, so all four survive an app update.)
    //
    // ⚠️ The autosave especially: a `.ptp` inside `Projects/` would be listed by the browser, offered
    // as a project to LOAD, and deletable with SELECT+B — and its whole meaning is "nobody put this
    // here on purpose". See FileSystem::autosave_file_path.
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
    std::string privateRoot_;  // the app's own three files; equal to root_ off Android
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

/**
 * `[^a-zA-Z0-9_-.]` → '_', which is what `FileSystem::rename_file` documents. `allow_dot` is
 * `create_folder`'s rule, which does NOT permit one (a folder named "a.b" would read as a file).
 *
 * ⚠️ Declared here rather than kept private to `StdFileSystem` because the rule belongs to the
 * INTERFACE, not to one implementation: `SafFileSystem` sanitises the same names for the same reason,
 * and two copies of a character class are two things that can drift apart while both look right.
 */
std::string path_sanitize(const std::string& name, bool allow_dot);

/** Lowercased, for the case-insensitive comparisons (the extension filter, the NAME sort). */
std::string to_lower(std::string s);

/**
 * Where the app's folder goes when the shell does not say.
 *
 * `$POCKETTRACKER_HOME` wins on every platform if it is set — which is what a PortMaster launch script
 * uses to point the app at the SD card's ports directory. Otherwise, per platform:
 *
 *   Windows   `Documents\PocketTracker`, Documents as Explorer resolves it (so: the OneDrive one, if
 *             that is what the machine has), falling back to `%USERPROFILE%\Documents`
 *   macOS     `$HOME/Documents/PocketTracker`
 *   Linux     `$XDG_DATA_HOME/PocketTracker`, then `$HOME/.local/share/PocketTracker`
 *
 * and finally the relative `./PocketTracker` for a box that answered none of the above.
 *
 * ⚠️ Windows and macOS choose Documents where Linux chooses a data directory, and that is deliberate,
 * not an inconsistency: a handheld has no Documents folder and its user reaches files over the SD card,
 * while a desktop user reaches them through a file manager. It is the same reasoning that put Android's
 * root under `/Documents/PocketTracker`, and it keeps the seven folder names identical on all four.
 */
std::string default_app_root();

}  // namespace pt::ui
