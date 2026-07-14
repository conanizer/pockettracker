#include "ui/project_actions.h"

#include <string>
#include <vector>

#include "songcore/project_io.h"   // serialize_project — so a save can go through the FileSystem
#include "songcore/render.h"
#include "ui/lifecycle.h"          // autosave_clear — a save leaves nothing to recover (S10)

namespace pt::ui {

namespace {

/**
 * ⚠️ **A `.ptp` IS WRITTEN THROUGH THE FileSystem, NOT THROUGH `SongcoreHost::save_project_file`** —
 * and S10 is the session that noticed the two had drifted.
 *
 * `FileSystem::write_file` writes `<path>.tmp` and renames it over the target. Its own doc comment
 * says why, and it is not hypothetical on the hardware this port is aimed at: *"a device that loses
 * power — or a user who pulls the SD card — mid-save must not be left with a half-written project
 * where the whole one used to be."* Android has done exactly that since the beginning
 * (`AndroidFileSystem.writeFile`), and Kotlin's `FileController.saveProject` goes through it.
 *
 * `SongcoreHost::save_project_file` is a plain `ofstream` opened with `trunc`, and it is right for
 * songcore to have one: songcore must keep compiling for the NDK, where *where files live* is scoped
 * storage and Kotlin's problem, so it cannot depend on `ui::FileSystem`. But **pt-ui can**, and since
 * S7 it had been calling the truncating writer anyway — so the port promised atomicity in the
 * interface, inherited it from Android in the implementation, and then quietly opted out of it in the
 * only two places a user's song is ever written.
 *
 * The failure needs no imagination and no tool could have seen it: every check in the ladder asserts
 * the file LANDS and PARSES, which it does. None of them cuts the power halfway through.
 */
bool write_project(const songcore::SongcoreHost& host, FileSystem& fs, const std::string& path) {
    return fs.write_file(path, songcore::serialize_project(host.project()));
}

/** `0001`, `0002`, … — Kotlin's `index.toString().padStart(4, '0')`. */
std::string pad4(int v) {
    std::string s = std::to_string(v);
    while (s.size() < 4) s.insert(s.begin(), '0');
    return s;
}

}  // namespace

std::string unique_render_path(FileSystem& fs, const std::string& dir, const std::string& safeName) {
    for (int index = 1; index < 10000; ++index) {
        const std::string path = dir + "/" + safeName + "_" + pad4(index) + ".wav";
        if (!fs.file_exists(path)) return path;
    }
    // Ten thousand renders of one song. Kotlin gives up the same way — it stops at 10000 and returns
    // the last name it built, overwriting it.
    return dir + "/" + safeName + "_9999.wav";
}

// ─── SAVE ────────────────────────────────────────────────────────────────────────────────────────

ActionResult save_project(songcore::SongcoreHost& host, FileSystem& fs, AppState& s) {
    // ⚠️ The empty-name fallback is load-bearing, not cosmetic. Without it an unnamed project saves to
    // "<Projects>/.ptp" — a DOTFILE — and the browser skips dotfiles (`build_item_list`). The save
    // succeeds, the status line says SAVED, and the file is invisible to the app forever. An empty name
    // is reachable: A+B every character on the NAME row, or apply an empty keyboard field. Android had
    // the same hole and is fixed with it (FileController.saveProject).
    std::string safeName = songcore::safe_project_name(host.project().name);
    if (safeName.empty()) safeName = "UNTITLED";

    const std::string path = fs.projects_directory() + "/" + safeName + ".ptp";

    if (!write_project(host, fs, path)) return ActionResult{false, "SAVE FAILED"};

    // The document is now on disk exactly as it stands, so it is no longer dirty. This is what makes
    // NEW and EXIT stop asking (TrackerController: `savedProjectVersion = projectVersion`).
    s.savedProjectVersion = s.projectVersion;
    s.projectPath         = path;

    // …and the crash-recovery autosave goes with it: the work is now safely in a real file the user
    // named, so there is nothing left to recover (TrackerController.saveProject, same two lines). It
    // lives HERE, beside the version alignment, rather than in the dispatcher's SAVE arm — the two are
    // one fact ("this document is now stored"), and a caller that gets one without the other leaves a
    // recovery prompt hanging over a project that is safely saved.
    //
    // ⚠️ The dispatcher's pending 3 s deadline is NOT cancelled from here and does not need to be: it
    // re-checks `project_dirty()` when it fires, and the line above has just made that false. That
    // re-check is the ONLY thing standing between a save-inside-the-debounce-window and the autosave
    // being written straight back out — see InputDispatcher::run_due_autosave.
    autosave_clear(fs);

    return ActionResult{true, "SAVED"};
}

// ─── EXPORT → MIX ────────────────────────────────────────────────────────────────────────────────

ActionResult render_mix(songcore::SongcoreHost& host, FileSystem& fs, AppState& s,
                        const std::function<void(float)>& progress) {
    (void)s;

    const songcore::SongBounds bounds = songcore::find_song_bounds(host.project());
    if (bounds.empty()) return ActionResult{false, "SONG IS EMPTY"};

    const std::string safeName = songcore::safe_project_name(host.project().name);
    const std::string path     = unique_render_path(fs, fs.renders_directory(), safeName);

    // The whole song, master bus and all — and the file is LONGER than the song, because the render
    // runs on past the last step until the reverb tail, the delay repeats and the note releases have
    // decayed (songcore/render.h, S6b).
    songcore::RenderOptions opts;
    opts.stemsMode      = 0;
    opts.applyMasterBus = true;

    const songcore::RenderStats stats =
        host.render_song_range_to_wav(bounds.startRow, bounds.endRow, path, opts, progress);

    if (!stats.ok || stats.totalFrames <= 0) return ActionResult{false, "EXPORT FAILED"};
    return ActionResult{true, "EXPORTED!"};
}

// ─── EXPORT → STEMS ──────────────────────────────────────────────────────────────────────────────

ActionResult render_stems(songcore::SongcoreHost& host, FileSystem& fs, AppState& s,
                          const std::function<void(float)>& progress) {
    (void)s;

    const songcore::SongBounds bounds = songcore::find_song_bounds(host.project());
    if (bounds.empty()) return ActionResult{false, "SONG IS EMPTY"};

    const std::vector<songcore::StemPass> passes = songcore::stems_plan(host.project());
    if (passes.empty()) return ActionResult{false, "NO ACTIVE TRACKS"};

    // Renders/<name>/ — one folder per project, so a stems set does not scatter across the renders
    // directory. `ifEmpty { "project" }` is the stems path's own fallback, and unlike the mix path it
    // HAS one, because an empty folder name would put the stems straight into Renders/.
    std::string safeName = songcore::safe_project_name(host.project().name);
    if (safeName.empty()) safeName = "project";

    const std::string rendersDir = fs.renders_directory();
    const std::string stemDir    = rendersDir + "/" + safeName;
    if (!fs.file_exists(stemDir)) {
        const std::string created = fs.create_folder(rendersDir, safeName);
        if (created.empty()) return ActionResult{false, "STEMS FAILED"};
    }

    const int total = static_cast<int>(passes.size());
    int       done  = 0;

    for (const songcore::StemPass& pass : passes) {
        songcore::RenderOptions opts;
        opts.stemsMode = pass.stemsMode;
        // ⚠️ Stems BYPASS the master bus (OTT / DUST / master EQ) by design — you are meant to be able
        // to re-mix them in a DAW without the bus baked in twice.
        opts.applyMasterBus = false;

        const std::string path = stemDir + "/" + safeName + pass.suffix + ".wav";

        // ⚠️ ONE prepare per PASS, not one for the set. prepare_render is what wipes the effect
        // chains, and without it each stem would begin inside the PREVIOUS stem's reverb tail.
        // `render_song_range_to_wav` does prepare → schedule → render → finish, so that comes free —
        // and it is the same call the mix makes, which is what keeps the two paths from drifting.
        const int   from = done;
        const auto slice = [&progress, from, total](float p) {
            if (progress) progress((static_cast<float>(from) + p) / static_cast<float>(total));
        };

        host.render_song_range_to_wav(bounds.startRow, bounds.endRow, path, opts,
                                      progress ? slice : std::function<void(float)>());
        ++done;
    }

    if (progress) progress(1.0f);
    return ActionResult{true, "STEMS EXPORTED!"};
}

// ─── The song TEMPLATE ───────────────────────────────────────────────────────────────────────────

ActionResult save_template(songcore::SongcoreHost& host, FileSystem& fs) {
    if (!write_project(host, fs, fs.template_project_path()))
        return ActionResult{false, "SAVE FAILED"};
    return ActionResult{true, "TEMPLATE SAVED"};
}

ActionResult clear_template(FileSystem& fs) {
    const std::string path = fs.template_project_path();
    // Kotlin returns TRUE when there was nothing to delete: clearing an absent template is not a
    // failure, it is a no-op that leaves you exactly where you asked to be.
    if (!fs.file_exists(path)) return ActionResult{true, "TEMPLATE CLEARED"};
    if (!fs.delete_path(path)) return ActionResult{false, "CLEAR FAILED"};
    return ActionResult{true, "TEMPLATE CLEARED"};
}

}  // namespace pt::ui
