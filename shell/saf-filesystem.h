#pragma once

// ─── FileSystem, on the Storage Access Framework ─────────────────────────────────────────────────
//
// The Android implementation of `ui/filesystem.h`, and the second one to exist — `StdFileSystem` is
// the portable one every other platform uses. The seam has been abstract since S6a precisely so that
// the worst case is a second implementation rather than a redesign; this is that second
// implementation, and on Android it is the only one: the app declares no storage permission, so
// `/storage/emulated/0` is unreadable to the process and a granted tree is the only way to a user's
// files.
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
 * ⚠️ It OWNS an inner `StdFileSystem` for the app-private files, which is what keeps settings,
 * template and autosave working on a device that has granted nothing at all.
 */
class SafFileSystem : public pt::ui::FileSystem {
  public:
    /**
     * `private_root` is `context.filesDir` — where `settings.json`, `template.ptp` and `autosave.ptp`
     * live since P2, and the reason a fresh install with no grant still boots with its settings.
     */
    explicit SafFileSystem(std::string private_root);

    /**
     * Install this filesystem as `byte_source.h`'s resolver, so the direct file operations below the
     * UI can act on a `pt://` string. Called once, by the shell, immediately after construction.
     *
     * All three hooks go in together, which is `byte_source.h`'s rule and not a convenience: a
     * render opens its temp through `pt_fopen` and then publishes it with `pt_remove`+`pt_rename`,
     * so a host that installed only the opener would write every byte correctly and produce no file.
     *
     * ⚠️ **The hooks are plain function pointers with no user-data argument** (`byte_source.h`), so
     * the instance has to be reachable from free functions — hence the single-instance pointer this
     * installs. One `SafFileSystem` per process is the only shape the app has ever needed; a second
     * one would silently steal the hooks, so constructing two is logged.
     */
    void install_file_hooks();

    /** True when at least one folder has been granted. False is the fresh-install state, not a fault. */
    bool has_grant();

    /** How many folders are granted. Printed at boot, because 0 and "granted but empty" differ. */
    int root_count();

    /**
     * The tree the seven app folders live in — `pt://<root-id>`, or `pt://roots` when nothing is
     * granted (which is what puts the browser on the roots directory, i.e. on the one row that can fix
     * that).
     *
     * ⚠️ **The choice is Java's and it is PERSISTED there** (`SafStorage.homeRootId`), because it must
     * not move when the grant SET changes: the id is derived from the tree URI, so granting a second
     * folder whose id happens to sort lower would otherwise relocate every app folder the user has.
     * Read once per roots load into `homeId_`, and falls back to the lowest id if Java answers "" —
     * which is the answer that says "nothing granted", so the fallback is a control, not a policy.
     *
     * ⚠️ Public because it is THE ANDROID MEDIA ROOT and two things above this class need it: what a
     * project's relative sample paths resolve against (`AppConfig::mediaBaseDir`), and where the log
     * file goes. Neither can be derived from `config_path()`, which answers "" with no grant.
     */
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

    /**
     * The one implementation that has something to forget — see `ui::FileSystem::forget_listing`.
     *
     * ⚠️ It is `forget`, not `invalidate`: a directory that may have changed from OUTSIDE this app
     * may have had a child DELETED, and that child's URI stays in `uriCache_` under a name the fresh
     * listing no longer mentions. A surviving child heals itself (the re-list overwrites its URI); a
     * departed one does not, and resolves to a document the provider has already destroyed.
     */
    void forget_listing(const std::string& directory) override;

    bool file_exists(const std::string& path) override;
    bool is_directory(const std::string& path) override;
    std::string parent_path(const std::string& path) override;

    /**
     * The one ACTION entry: `ADD FOLDER…` in the roots directory, which fires
     * `ACTION_OPEN_DOCUMENT_TREE`.
     *
     * ⚠️ **Returns as soon as the intent is away.** The grant arrives in the activity's
     * `onActivityResult`, which cannot run until this process's SDL thread has let go — see
     * `ui::FileSystem::activate` for why waiting here would cost the session its autosave. The new
     * root shows up because the roots listing is never cached and the browser re-lists on foreground.
     */
    bool activate(const std::string& path) override;

    /**
     * `ui::FileSystem::set_home_directory` — persist `pt://<root-id>` as the tree the seven app folders
     * live under, and make the accessors answer from it immediately.
     *
     * ⚠️ **The choice is Java's to STORE** (`SafStorage.setHomeRoot`, the same pref `homeRootId` reads),
     * so there is one home and not two opinions about it. Refuses anything that is not a granted tree,
     * and refuses a tree whose folder is gone — storing that would put the app straight into the state
     * the liveness test exists to leave.
     */
    bool set_home_directory(const std::string& path) override;

    /**
     * `ui::FileSystem::revoke_access` — release the persisted permission on `pt://<root-id>`.
     *
     * ⚠️ **Deletes nothing.** It is the only way to remove a granted folder from the roots listing, and
     * the only way to clear one whose directory no longer exists — Android keeps the grant forever
     * otherwise, so a `(MISSING)` row would be permanent.
     */
    bool revoke_access(const std::string& path) override;

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

    /**
     * `std::remove` and `std::rename` for a `pt://` path — 0 on success, the libc convention, because
     * the call sites are the ones that used to call libc. Public because the hook trampolines do.
     *
     * ⚠️ **These are NOT `delete_path` and `rename_file`.** Those two implement the *interface*, whose
     * rename takes a BASE name, sanitises it and re-attaches the source's extension — the browser's
     * contract. A temp file being published is not a user typing a name: publishing `song.wav.tmp` as
     * `song.wav` through that contract re-attaches `.tmp`, arrives back at the name it started from,
     * and is then refused as a clobber. Same provider call underneath, different contract above.
     */
    int hook_remove(const std::string& path);
    int hook_rename(const std::string& from, const std::string& to);

  private:
    struct Root {
        std::string id;
        std::string name;
        std::string docUri;
        /** ⚠️ A GRANT OUTLIVES ITS FOLDER — see `SafStorage.Root.live`. False = the document is gone. */
        bool        live = true;
    };

    /** The granted trees, refreshed from Java. Cheap and re-read rather than cached across a launch. */
    const std::vector<Root>& roots(bool refresh = false);

    /**
     * Is `path` a granted tree itself (`pt://<id>`), rather than something inside one?
     *
     * ⚠️ The mutating methods refuse on this: `resolve` turns such a path into the tree's OWN root
     * document, so a delete or a move at that level takes the user's whole folder with it.
     */
    bool is_granted_tree(const std::string& path);

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

    void invalidate(const std::string& dirPath);

    pt::ui::StdFileSystem priv_;   // the app-private files, and every plain path that reaches here

    std::vector<Root> roots_;
    std::string       homeId_;        // filled with roots_, from Java; "" = nothing granted
    bool              rootsLoaded_ = false;

    std::unordered_map<std::string, std::string>                 uriCache_;   // pt:// path → doc URI
    std::unordered_map<std::string, std::vector<pt::ui::FileInfo>> listCache_; // pt:// dir  → children
};

}  // namespace ptshell
