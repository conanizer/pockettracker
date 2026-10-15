#include "ui/project_actions.h"

#include <algorithm>
#include <string>
#include <vector>

#include "songcore/project_io.h"
#include "songcore/render.h"
#include "ui/lifecycle.h"

namespace pt::ui {

namespace {

// ⚠️ Every file a user would miss goes through `FileSystem::write_file` (temp, rename), so a pulled
// card mid-save leaves the old file whole — never a truncating `ofstream`.
bool write_project(const songcore::SongcoreHost& host, FileSystem& fs, const std::string& path) {
    return fs.write_file(path, songcore::serialize_project(host.project()));
}

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
    return dir + "/" + safeName + "_9999.wav";
}

// ─── SAVE ────────────────────────────────────────────────────────────────────────────────────────

ActionResult save_project(songcore::SongcoreHost& host, FileSystem& fs, AppState& s) {
    // ⚠️ An unnamed project would save as ".ptp", a dotfile the browser never lists.
    std::string safeName = songcore::safe_project_name(host.project().name);
    if (safeName.empty()) safeName = "UNTITLED";

    const std::string path = fs.projects_directory() + "/" + safeName + ".ptp";

    if (!write_project(host, fs, path)) return ActionResult{false, "SAVE FAILED"};

    // Clean, and the autosave goes with it (its pending deadline re-checks the dirty flag).
    s.savedProjectVersion = s.projectVersion;
    s.projectPath         = path;
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

    // With the master bus; the file runs on until the tails decay.
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

    // Once, so every stem covers the same rows.
    const songcore::SongBounds bounds = resolve_range(host.project(), range);
    if (bounds.empty()) return ActionResult{false, "SONG IS EMPTY"};

    const std::vector<songcore::StemPass> passes =
        songcore::stems_plan(host.project(), bounds.startRow, bounds.endRow);
    if (passes.empty()) return ActionResult{false, "NO ACTIVE TRACKS"};

    // Renders/<name>/; an empty name would put them straight into Renders/.
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
        opts.applyMasterBus = false;   // re-mixed in a DAW, it would be applied twice

        const std::string path = stemDir + "/" + safeName + pass.suffix + ".wav";

        // One prepare per pass, so no stem begins inside the last one's reverb tail.
        const int   from = done;
        const auto slice = [&progress, from, total](float p) {
            if (progress) progress((static_cast<float>(from) + p) / static_cast<float>(total));
        };

        const songcore::RenderStats stats =
            host.render_song_range_to_wav(bounds.startRow, bounds.endRow, path, opts,
                                          progress ? slice : std::function<void(float)>(),
                                          range.repeat);

        // Stop at the first failure (most likely a full card) and say how many landed.
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
    return "Resample_9999";
}

ActionResult render_resample(songcore::SongcoreHost& host, FileSystem& fs,
                             int startRow, int endRow, const std::set<int>& trackFilter,
                             const std::string& customBaseName, std::string& outPath,
                             const std::function<void(float)>& progress) {
    const std::string dir = fs.resampled_directory();

    // A typed name is used as is and overwrites; an empty one is numbered.
    const std::string path = customBaseName.empty()
                                 ? unique_render_path(fs, dir, "Resample")
                                 : dir + "/" + songcore::safe_project_name(customBaseName) + ".wav";

    // ⚠️ finish_render runs even when nothing was scheduled: prepare silenced the live stream.
    host.prepare_render(startRow, endRow);
    const int64_t songFrames = host.schedule_song_range(startRow, endRow, &trackFilter);
    if (songFrames <= 0) {
        host.finish_render();
        return ActionResult{false, "RESAMPLE FAILED"};
    }

    // A mix of the selected tracks, with the master bus.
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

    if (!host.load_sample(slot, wavPath)) return -1;   // the slot stays free on failure

    // Set anyway, so it can never be a hybrid.
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

// ─── The instrument preset and the song template ─────────────────────────────────────────────────

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
    if (!fs.file_exists(path)) return ActionResult{true, "TEMPLATE CLEARED"};
    if (!fs.delete_path(path)) return ActionResult{false, "CLEAR FAILED"};
    return ActionResult{true, "TEMPLATE CLEARED"};
}

}  // namespace pt::ui
