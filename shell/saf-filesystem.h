#pragma once

// ─── FileSystem, on the Storage Access Framework ─────────────────────────────────────────────────
//
// The Android implementation of `ui/filesystem.h`, and the second one to exist — `StdFileSystem` is
// the portable one every other platform uses and continues to use here on every non-SAF launch. The
// seam has been abstract since S6a precisely so that the worst case is a second implementation
// rather than a redesign; this is that second implementation.
//
// ── What a PATH is here, and why it is not a document URI ────────────────────────────────────────
//
// ⚠️ **`ui::FileSystem` assumes paths COMPOSE.** `parent_path` is a string trim, `rename_file` builds
// its target as `parent_path(path) / new_name`, and `project_actions.cpp:70` writes
// `projects_directory() + "/" + name + ".ptp"`. A SAF document URI supports none of that: it is an
// opaque handle with no derivable parent and no derivable child, and `DocumentsContract` has no
// parent-of-this-document primitive at all. Putting document URIs in `FileInfo.path` would leave the
// browser's ".." with nothing to trim and `rename_file` unable to name its own target — **and none of
// that is a compile error.**
//
// So paths stay composable and the document URI becomes an implementation detail:
//
//     pt://roots                          the virtual roots directory — one entry per granted tree
//     pt://<root-id>                      the top of a granted tree; its parent is pt://roots
//     pt://<root-id>/Projects/song.ptp    everything below
//
// `<root-id>` is 12 hex characters of SHA-256 over the persisted tree URI, computed on the Kotlin
// side. It is DERIVED, never stored, so the id→tree mapping is re-derivable from the grant list on
// any boot in any order — there is no table to migrate or to fall out of sync. `getPersistedUriPermissions`
// guarantees no ordering, which is exactly why an index cannot be the id.
//
// ⭐ `pt://roots` cannot collide with a root-id by construction: `roots` is not twelve hex characters.
//
// ── Two kinds of string arrive here, and both must work ──────────────────────────────────────────
//
// ⚠️ `settings.json`, `template.ptp` and `autosave.ptp` are PLAIN app-private paths (P2) and are read
// at boot before any picker has run. They arrive at `read_file`/`write_file` like anything else. So
// every method dispatches on the string — a `pt://` path goes to SAF, anything else is handed to an
// embedded `StdFileSystem`. That dispatch is `pt_path_is_uri`'s job, the same predicate `pt_fopen`
// uses below the engine, so there is one rule about what a URI is and not two.

#include "ui/filesystem.h"
#include "ui/std_filesystem.h"

#include <string>
#include <unordered_map>
#include <vector>

namespace ptshell {

/**
 * The `ui::FileSystem` backed by a granted SAF tree.
 *
 * Constructed only when the shell is asked for it (argv[3] == "saf"); every other launch keeps the
 * `StdFileSystem` it has always had. ⚠️ It OWNS an inner `StdFileSystem` for the app-private files,
 * which is what keeps settings working on a device that has granted nothing at all.
 */
class SafFileSystem : public pt::ui::FileSystem {
  public:
    /**
     * `private_root` is `context.filesDir` — where `settings.json`, `template.ptp` and `autosave.ptp`
     * live since P2, and the reason a fresh install with no grant still boots with its settings.
     */
    explicit SafFileSystem(std::string private_root);

    /**
     * Install this filesystem as `pt_fopen`'s resolver, so the thirteen direct opens below the UI can
     * open a `pt://` string. Called once, by the shell, immediately after construction.
     *
     * ⚠️ **The hook is a plain function pointer with no user-data argument** (`byte_source.h`), so the
     * instance has to be reachable from a free function — hence the single-instance pointer this
     * installs. One `SafFileSystem` per process is the only shape the app has ever needed; a second
     * one would silently steal the hook, so constructing two is logged.
     */
    void install_open_hook();

    /** True when at least one folder has been granted. False is the fresh-install state, not a fault. */
    bool has_grant();

    /** How many folders are granted. Printed at boot, because 0 and "granted but empty" differ. */
    int root_count();

    // ── The app's directories ───────────────────────────────────────────────────────────────────
    std::string projects_directory() override;
    std::string samples_directory() override;
    std::string renders_directory() override;
    std::string resampled_directory() override;
    std::string instruments_directory() override;
    std::string soundfonts_directory() override;
    std::string themes_directory() override;

    // ── The app's own files ─────────────────────────────────────────────────────────────────────
    //
    // Three come off the private root, byte-for-byte as `StdFileSystem` answers them (P2). Only
    // `config.json` is in the granted tree, because it is the one file the USER hand-edits and
    // app-private storage is reachable over adb alone.
    std::string template_project_path() override { return priv_.template_project_path(); }
    std::string settings_path() override         { return priv_.settings_path(); }
    std::string autosave_file_path() override    { return priv_.autosave_file_path(); }
    std::string config_path() override;

    // ── Reading ─────────────────────────────────────────────────────────────────────────────────
    bool read_file(const std::string& path, std::string& out) override;
    std::vector<pt::ui::FileInfo> list_files(const std::string& directory) override;
    bool file_exists(const std::string& path) override;
    bool is_directory(const std::string& path) override;
    std::string parent_path(const std::string& path) override;

    // ── Writing ─────────────────────────────────────────────────────────────────────────────────
    //
    // ⚠️ A PLAIN path still goes to the inner `StdFileSystem`, which is what keeps `settings.json`,
    // `autosave.ptp` and `template.ptp` saving on a device that has granted nothing (P2).
    bool write_file(const std::string& path, const std::string& content) override;
    bool write_bytes(const std::string& path, const void* data, size_t len) override;
    bool delete_path(const std::string& path) override;
    bool rename_file(const std::string& path, const std::string& new_base_name) override;
    std::string create_folder(const std::string& parent, const std::string& folder_name) override;
    bool move_file(const std::string& from, const std::string& to) override;
    bool copy_file(const std::string& from, const std::string& to) override;

    /**
     * `pt://…` → an owned OS descriptor, or -1. Public because the hook trampoline calls it.
     *
     * ⭐ **A write mode CREATES the document, and `byte_source.h`'s note that a hook cannot is about
     * the URI it assumed, not about this.** The restriction was that a document must exist before it
     * can be opened for write and an opaque `content://` handle has no nameable child — but a `pt://`
     * path's parent is a string trim and its name is the tail, so the create is derivable here. That
     * is the second thing §5a's composable paths bought, after the browser's "..".
     */
    int open_fd(const std::string& path, const char* mode);

  private:
    struct Root {
        std::string id;
        std::string name;
        std::string docUri;
    };

    /** The granted trees, refreshed from Java. Cheap and re-read rather than cached across a launch. */
    const std::vector<Root>& roots(bool refresh = false);

    /** `pt://<id>/a/b` → the document URI, or "" if any segment is missing. Caches what it learns. */
    std::string resolve(const std::string& path);

    /** The listing of a `pt://` directory, cached. */
    const std::vector<pt::ui::FileInfo>* listing(const std::string& dirPath);

    /** `<home>/<sub>` as a `pt://` path, creating the document if it is not there yet. */
    std::string ensure_dir(const char* sub);

    /** The last segment: `pt://<id>/Projects/song.ptp` → `song.ptp`. "" for a bare tree or the roots. */
    std::string leaf_name(const std::string& path);

    /** The document URI for a FILE `path`, creating it under its (existing) parent. "" on failure. */
    std::string ensure_file(const std::string& path);

    /** An owned descriptor for a document URI, or -1. `open_fd` and the writers share it. */
    int open_uri_fd(const std::string& uri, const char* mode);

    /** Truncate a document and write `len` bytes over it. */
    bool write_uri(const std::string& uri, const void* data, size_t len);

    /** Delete a document by URI. Separate from `delete_path` because the writers hold a URI, not a path. */
    bool delete_uri(const std::string& uri);

    /**
     * Drop `path` AND everything beneath it from both caches.
     *
     * ⚠️ Every mutation must call this, and a prefix sweep rather than one erase: deleting or renaming
     * a FOLDER leaves its children's URIs cached under paths that no longer exist, and a stale entry
     * resolves to a document the provider has already destroyed — which fails as "the file is corrupt"
     * rather than as anything about a cache.
     */
    void forget(const std::string& path);

    /** Recursive copy, for the move/copy fallbacks. `to` must not exist; folders copy as folders. */
    bool copy_tree(const std::string& from, const std::string& to);

    /** The tree the seven app folders live in: the lowest root-id. "" when nothing is granted. */
    std::string home_root_path();

    void invalidate(const std::string& dirPath);

    pt::ui::StdFileSystem priv_;   // the app-private files, and every plain path that reaches here

    std::vector<Root> roots_;
    bool              rootsLoaded_ = false;

    std::unordered_map<std::string, std::string>                 uriCache_;   // pt:// path → doc URI
    std::unordered_map<std::string, std::vector<pt::ui::FileInfo>> listCache_; // pt:// dir  → children
};

}  // namespace ptshell
