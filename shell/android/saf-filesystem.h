#pragma once

// `ui/filesystem.h` on the Storage Access Framework — Android's only way to the user's files, since
// the app asks for no storage permission.
//
// ⚠️ `ui::FileSystem` assumes paths compose (`parent_path` is a string trim, rename builds
// `parent / new_name`); a SAF document URI has no derivable parent or child. So paths stay
// composable and the URI is an implementation detail:
//
//     pt://roots                          the virtual roots directory — one entry per granted tree
//     pt://<root-id>                      the top of a granted tree; its parent is pt://roots
//     pt://<root-id>/Projects/song.ptp    everything below
//
// `<root-id>` is 12 hex characters of SHA-256 over the tree URI (Kotlin side): derived, never stored.
// `roots` is not twelve hex characters, so it cannot collide with an id.
//
// ⚠️ Plain paths arrive too — settings, template and autosave are app-private, read at boot before any
// folder is granted — so every method dispatches: `pt://` to SAF (`pt_path_is_uri`), else to an
// embedded `StdFileSystem`.

#include "ui/filesystem.h"
#include "ui/std_filesystem.h"

#include <string>
#include <unordered_map>
#include <vector>

namespace ptshell {

// The `ui::FileSystem` backed by granted SAF trees, plus an inner `StdFileSystem` for the app-private
// files, so a device that granted nothing still boots with its settings.
class SafFileSystem : public pt::ui::FileSystem {
  public:
    // `private_root` is `context.filesDir`.
    explicit SafFileSystem(std::string private_root);

    // Installs this as `byte_source.h`'s resolver, so file operations below the UI accept `pt://`.
    // Called once, right after construction. All three hooks go together: a render opens its temp
    // through `pt_fopen` and publishes it with `pt_remove` + `pt_rename`.
    // ⚠️ The hooks take no user data, so they reach this through a single-instance pointer; a second
    // instance would steal them, and is logged.
    void install_file_hooks();

    // False is the fresh-install state, not a fault.
    bool has_grant();

    // Printed at boot: 0 and "granted but empty" differ.
    int root_count();

    // The tree the app folders live in, `pt://<root-id>` — or `pt://roots` when nothing is granted,
    // which puts the browser on the row that can fix that.
    // ⚠️ The choice is stored on the Kotlin side (`SafStorage.homeRootId`) so a new grant never moves
    // the app's folders. Public: it is the media root sample paths resolve against, and where the log
    // goes — `config_path()` answers "" with no grant.
    std::string home_root_path();

    // ── The app's directories ───────────────────────────────────────────────────────────────────
    std::string projects_directory() override;
    std::string samples_directory() override;
    std::string renders_directory() override;
    std::string resampled_directory() override;
    std::string instruments_directory() override;
    std::string soundfonts_directory() override;
    std::string themes_directory() override;
    std::string scales_directory() override;
    std::string grooves_directory() override;
    std::string reverbs_directory() override;
    std::string delays_directory() override;

    // ── The app's own files ─────────────────────────────────────────────────────────────────────
    // Three come off the private root. Only `config.json` is in the granted tree: the user edits it,
    // and app-private storage is adb-only.
    std::string template_project_path() override { return priv_.template_project_path(); }
    std::string settings_path() override         { return priv_.settings_path(); }
    std::string autosave_file_path() override    { return priv_.autosave_file_path(); }
    std::string config_path() override;

    // ── Reading ─────────────────────────────────────────────────────────────────────────────────
    bool read_file(const std::string& path, std::string& out) override;
    std::vector<pt::ui::FileInfo> list_files(const std::string& directory) override;

    // ⚠️ `forget`, not `invalidate`: a child deleted from outside the app keeps its URI in `uriCache_`
    // under a name the fresh listing no longer has, resolving to a destroyed document.
    void forget_listing(const std::string& directory) override;

    bool file_exists(const std::string& path) override;
    bool is_directory(const std::string& path) override;
    std::string parent_path(const std::string& path) override;

    // `ADD FOLDER…` in the roots directory: opens the system folder picker and returns at once. The
    // grant lands in the activity; the browser re-lists when the app comes back.
    bool activate(const std::string& path) override;

    // Stores `pt://<root-id>` as home, on the Kotlin side (one home, not two). Refuses anything not a
    // granted tree, and a tree whose folder is gone.
    bool set_home_directory(const std::string& path) override;

    // Releases the permission on `pt://<root-id>`. ⚠️ Deletes nothing. The only way to drop a
    // `(MISSING)` row: Android otherwise keeps a grant forever.
    bool revoke_access(const std::string& path) override;

    // ── Writing ─────────────────────────────────────────────────────────────────────────────────
    // A plain path still goes to the inner `StdFileSystem`.
    bool write_file(const std::string& path, const std::string& content) override;
    bool write_bytes(const std::string& path, const void* data, size_t len) override;
    bool delete_path(const std::string& path) override;
    bool rename_file(const std::string& path, const std::string& new_base_name) override;
    std::string create_folder(const std::string& parent, const std::string& folder_name) override;
    bool move_file(const std::string& from, const std::string& to) override;
    bool copy_file(const std::string& from, const std::string& to) override;

    // `pt://…` → an owned OS descriptor, or -1. A write mode creates the document (its parent and
    // name derive from the path). Public for the hook trampoline.
    int open_fd(const std::string& path, const char* mode);

    // `std::remove` / `std::rename` for `pt://` paths — 0 on success, like libc. Public for the hooks.
    // ⚠️ Not `delete_path` / `rename_file`: the interface's rename takes a BASE name and re-attaches
    // the extension, so publishing `song.wav.tmp` as `song.wav` would end back at `.tmp`.
    int hook_remove(const std::string& path);
    int hook_rename(const std::string& from, const std::string& to);

  private:
    struct Root {
        std::string id;
        std::string name;
        std::string docUri;
        bool        live = true;   // ⚠️ a grant outlives its folder; false = the document is gone
    };

    // The granted trees from Kotlin, re-read rather than cached across a launch.
    const std::vector<Root>& roots(bool refresh = false);

    // Is `path` a granted tree itself (`pt://<id>`)? ⚠️ Mutations refuse it: it resolves to the tree's
    // own root document, so a delete would take the user's whole folder.
    bool is_granted_tree(const std::string& path);

    // `pt://<id>/a/b` → the document URI, or "" if a segment is missing. Caches what it learns.
    std::string resolve(const std::string& path);

    // A `pt://` directory's listing, cached.
    const std::vector<pt::ui::FileInfo>* listing(const std::string& dirPath);

    // `<home>/<sub>` as a `pt://` path, created if missing.
    std::string ensure_dir(const char* sub);

    // `pt://<id>/Projects/song.ptp` → `song.ptp`; "" for a bare tree or the roots.
    std::string leaf_name(const std::string& path);

    // The document URI for a file `path`, created under its existing parent. "" on failure.
    std::string ensure_file(const std::string& path);

    // An owned descriptor for a document URI, or -1.
    int open_uri_fd(const std::string& uri, const char* mode);

    // Truncates a document and writes `len` bytes.
    bool write_uri(const std::string& uri, const void* data, size_t len);

    // Deletes by URI (the writers hold a URI, not a path).
    bool delete_uri(const std::string& uri);

    // Drops `path` and everything beneath it from both caches. ⚠️ Every mutation calls it: a renamed
    // or deleted folder leaves its children's URIs cached, and a stale one fails as "file is corrupt".
    void forget(const std::string& path);

    // Recursive copy for the move/copy fallbacks; `to` must not exist.
    bool copy_tree(const std::string& from, const std::string& to);

    void invalidate(const std::string& dirPath);

    pt::ui::StdFileSystem priv_;   // the app-private files, and every plain path that reaches here

    std::vector<Root> roots_;
    std::string       homeId_;        // filled with roots_, from Java; "" = nothing granted
    bool              rootsLoaded_ = false;

    std::unordered_map<std::string, std::string>                 uriCache_;   // pt:// path → doc URI
    std::unordered_map<std::string, std::vector<pt::ui::FileInfo>> listCache_; // pt:// dir  → children
};

}  // namespace ptshell
