#include "ui/project_actions.h"

#include <algorithm>
#include <string>
#include <vector>

#include "songcore/project_io.h"   // serialize_project — so a save can go through the FileSystem
#include "songcore/render.h"
#include "ui/lifecycle.h"          // autosave_clear — a save leaves nothing to recover

namespace pt::ui {

namespace {

/**
 * ⚠️ Every file a user would miss — `.ptp`, autosave, template, `.pti` — is written through
 * `FileSystem::write_file` (temp, checked close, rename), so power loss or a pulled SD card mid-save
 * leaves the old file whole. A truncating `ofstream` destroys the old file on open, and its `good()`
 * is read before the flush, so a small payload can vanish while returning true. songcore cannot depend
 * on `ui::FileSystem` (it compiles for the NDK), which is why writes live up here.
 * No check in the tools cuts power mid-write or fills the card — this is the guard.
 */
bool write_project(const songcore::SongcoreHost& host, FileSystem& fs, const std::string& path) {
    return fs.write_file(path, songcore::serialize_project(host.project()));
}

/** `0001`, `0002`, … */
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
    // Ten thousand renders of one song: give up and overwrite the last name.
    return dir + "/" + safeName + "_9999.wav";
}

// ─── SAVE ────────────────────────────────────────────────────────────────────────────────────────

ActionResult save_project(songcore::SongcoreHost& host, FileSystem& fs, AppState& s) {
    // ⚠️ The empty-name fallback matters: an unnamed project would save to "<Projects>/.ptp", a dotfile
    // the browser never lists — SAVED, and invisible for ever. An empty name is reachable (A+B every
    // character on NAME).
    std::string safeName = songcore::safe_project_name(host.project().name);
    if (safeName.empty()) safeName = "UNTITLED";

    const std::string path = fs.projects_directory() + "/" + safeName + ".ptp";

    if (!write_project(host, fs, path)) return ActionResult{false, "SAVE FAILED"};

    // On disk exactly as it stands: no longer dirty, so NEW and EXIT stop asking.
    s.savedProjectVersion = s.projectVersion;
    s.projectPath         = path;

    // …and the crash-recovery autosave goes with it — one fact ("this document is stored"), kept with the
    // version alignment so no caller gets one without the other.
    // ⚠️ The dispatcher's pending 3 s deadline is not cancelled: it re-checks `project_dirty()`, which the
    // line above made false (InputDispatcher::run_due_autosave).
    autosave_clear(fs);

    return ActionResult{true, "SAVED"};
}

// ─── EXPORT → MIX ────────────────────────────────────────────────────────────────────────────────

namespace {

/** The rows a render covers. An unset range means the whole song — derived here once. */
songcore::SongBounds resolve_range(const songcore::Project& project, const RenderRange& range) {
    if (range.startRow < 0) return songcore::find_song_bounds(project);
    songcore::SongBounds b;
    b.startRow = range.startRow;
    b.endRow   = std::max(range.startRow, range.endRow);
    // ⚠️ A range over unwritten rows is EMPTY, not a file of silence that reads as a broken export.
    for (int row = b.startRow; row <= b.endRow; ++row)
        if (songcore::song_row_filled(project, row)) return b;
    return songcore::SongBounds();
}

}  // namespace

ActionResult render_mix(songcore::SongcoreHost& host, FileSystem& fs, AppState& s,
                        const RenderRange& range, const std::function<void(float)>& progress) {
    (void)s;

    const songcore::SongBounds bounds = resolve_range(host.project(), range);
    if (bounds.empty()) return ActionResult{false, "SONG IS EMPTY"};

    const std::string safeName = songcore::safe_project_name(host.project().name);
    const std::string path     = unique_render_path(fs, fs.renders_directory(), safeName);

    // Master bus and all; the file runs past the last row until the tails decay (songcore/render.h).
    songcore::RenderOptions opts;
    opts.stemsMode      = 0;
    opts.applyMasterBus = true;

    const songcore::RenderStats stats =
        host.render_song_range_to_wav(bounds.startRow, bounds.endRow, path, opts, progress,
                                      range.repeat);

    if (!stats.ok || stats.totalFrames <= 0) return ActionResult{false, "EXPORT FAILED"};
    return ActionResult{true, "EXPORTED!"};
}

// ─── EXPORT → STEMS ──────────────────────────────────────────────────────────────────────────────

ActionResult render_stems(songcore::SongcoreHost& host, FileSystem& fs, AppState& s,
                          const RenderRange& range, const std::function<void(float)>& progress) {
    (void)s;

    // ⚠️ Resolved ONCE above the pass loop: every stem must cover the same rows or they will not line up
    // in a DAW.
    const songcore::SongBounds bounds = resolve_range(host.project(), range);
    if (bounds.empty()) return ActionResult{false, "SONG IS EMPTY"};

    const std::vector<songcore::StemPass> passes =
        songcore::stems_plan(host.project(), bounds.startRow, bounds.endRow);
    if (passes.empty()) return ActionResult{false, "NO ACTIVE TRACKS"};

    // Renders/<name>/, one folder per project. Falls back to "project" — an empty folder name would put
    // the stems straight into Renders/.
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
        // ⚠️ Stems BYPASS the master bus by design — re-mixed in a DAW, it would be applied twice.
        opts.applyMasterBus = false;

        const std::string path = stemDir + "/" + safeName + pass.suffix + ".wav";

        // ⚠️ One prepare per PASS (via `render_song_range_to_wav`, the mix's own call): each stem must not
        // begin inside the previous one's reverb tail.
        const int   from = done;
        const auto slice = [&progress, from, total](float p) {
            if (progress) progress((static_cast<float>(from) + p) / static_cast<float>(total));
        };

        const songcore::RenderStats stats =
            host.render_song_range_to_wav(bounds.startRow, bounds.endRow, path, opts,
                                          progress ? slice : std::function<void(float)>(),
                                          range.repeat);

        // ⚠️ Stop at the first failed pass and say how many landed: stems are the largest write the app
        // makes, so a failure is almost always a full card, and later passes would only fill it more.
        if (!stats.ok || stats.totalFrames <= 0)
            return ActionResult{false, done == 0 ? "STEMS FAILED"
                                                 : "STEMS: " + std::to_string(done) + " OF " +
                                                       std::to_string(total)};
        ++done;
    }

    if (progress) progress(1.0f);
    return ActionResult{true, "STEMS EXPORTED!"};
}

// ─── SONG selection → RESAMPLE ─────────────────────────────────────────────────────────────────

std::string resample_base_name(FileSystem& fs) {
    const std::string dir = fs.resampled_directory();
    for (int index = 1; index < 10000; ++index) {
        const std::string base = "Resample_" + pad4(index);
        if (!fs.file_exists(dir + "/" + base + ".wav")) return base;
    }
    // Ten thousand resamples with none freed: give up, return the last name.
    return "Resample_9999";
}

ActionResult render_resample(songcore::SongcoreHost& host, FileSystem& fs,
                             int startRow, int endRow, const std::set<int>& trackFilter,
                             const std::string& customBaseName, std::string& outPath,
                             const std::function<void(float)>& progress) {
    const std::string dir = fs.resampled_directory();

    // A typed name is used verbatim (and OVERWRITES); an empty one auto-names and de-duplicates. The
    // keyboard opens pre-filled with resample_base_name(), already the first free slot.
    const std::string path = customBaseName.empty()
                                 ? unique_render_path(fs, dir, "Resample")
                                 : dir + "/" + songcore::safe_project_name(customBaseName) + ".wav";

    // prepare → schedule(range, filter) → render → finish. finish_render MUST run even when nothing was
    // scheduled: prepare_render silenced the live stream and reset the chains.
    host.prepare_render(startRow, endRow);
    const int64_t songFrames = host.schedule_song_range(startRow, endRow, &trackFilter);
    if (songFrames <= 0) {
        host.finish_render();
        return ActionResult{false, "RESAMPLE FAILED"};
    }

    // stemsMode 0 + master bus ON — a resample is a MIX of the selected tracks.
    const songcore::RenderStats stats =
        host.render_to_wav(path, songFrames, /*stemsMode=*/0, /*applyMasterBus=*/true, progress);
    host.finish_render();

    if (!stats.ok || stats.totalFrames <= 0) return ActionResult{false, "RESAMPLE FAILED"};

    outPath = path;
    return ActionResult{true, ""};   // the caller reports "RESAMPLED -> INST xx" once the slot lands
}

int create_resampled_instrument(songcore::SongcoreHost& host, const std::string& wavPath) {
    songcore::Project& p = host.edit_project();

    int slot = -1;
    for (int i = 0; i < static_cast<int>(p.instruments.size()); ++i) {
        if (songcore::instrument_is_free(p.instruments[static_cast<size_t>(i)])) { slot = i; break; }
    }
    if (slot < 0) return -1;

    // Decode into the slot, learn its rate ratio, push its playback params. On failure the slot stays
    // untouched and free.
    if (!host.load_sample(slot, wavPath)) return -1;

    // The slot was free (clean SAMPLER at defaults), but type/SF/root/vol/pan are set anyway so it can
    // never be a hybrid — then re-pushed.
    songcore::Instrument& ins = p.instruments[static_cast<size_t>(slot)];
    ins.instrumentType = songcore::InstrumentType::SAMPLER;
    ins.soundfontPath.reset();
    ins.sampleFilePath = wavPath;
    ins.sampleId       = slot;
    ins.root           = songcore::Note::C4();
    ins.volume         = 0xFF;
    ins.pan            = 0x80;
    host.push_instrument(slot);

    return slot;
}

// ─── The song TEMPLATE ───────────────────────────────────────────────────────────────────────────

bool save_instrument_preset(const songcore::SongcoreHost& host, FileSystem& fs, int id,
                            const std::string& path) {
    return fs.write_file(path, songcore::serialize_instrument_preset(
                                   songcore::make_instrument_preset(host.project(), id)));
}

ActionResult save_template(songcore::SongcoreHost& host, FileSystem& fs) {
    if (!write_project(host, fs, fs.template_project_path()))
        return ActionResult{false, "SAVE FAILED"};
    return ActionResult{true, "TEMPLATE SAVED"};
}

ActionResult clear_template(FileSystem& fs) {
    const std::string path = fs.template_project_path();
    // Clearing an absent template is not a failure.
    if (!fs.file_exists(path)) return ActionResult{true, "TEMPLATE CLEARED"};
    if (!fs.delete_path(path)) return ActionResult{false, "CLEAR FAILED"};
    return ActionResult{true, "TEMPLATE CLEARED"};
}

}  // namespace pt::ui
