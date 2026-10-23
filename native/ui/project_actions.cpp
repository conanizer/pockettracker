#include "ui/project_actions.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "common/byte_source.h"   // pt_fopen / pt_remove / pt_rename
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
    const std::string path =
        fs.projects_directory() + "/" + songcore::project_file_name(host.project().name) + ".ptp";

    if (!write_project(host, fs, path)) return ActionResult{false, "SAVE FAILED"};

    // Clean, and the autosave goes with it (its pending deadline re-checks the dirty flag).
    s.savedProjectVersion = s.projectVersion;
    s.projectPath         = path;
    autosave_clear(fs);

    return ActionResult{true, "SAVED"};
}

// ─── COLLECT ─────────────────────────────────────────────────────────────────────────────────────

std::string collect_folder_name(const songcore::SongcoreHost& host) {
    return songcore::project_file_name(host.project().name);
}

bool collect_folder_exists(FileSystem& fs, const std::string& folder) {
    return fs.file_exists(fs.songs_directory() + "/" + folder);
}

std::string collect_free_folder_name(FileSystem& fs, const std::string& folder) {
    for (int n = 2; n < 1000; ++n) {
        const std::string name = folder + "_" + std::to_string(n);
        if (!collect_folder_exists(fs, name)) return name;
    }
    return folder + "_999";
}

namespace {

// For comparing two spellings of one path: one kind of slash, one case.
std::string comparable_path(std::string p) {
    for (char& c : p)
        c = (c == '\\') ? '/' : static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return p;
}

// Bytes in the file, or 0 when it cannot be measured (it then adds nothing to the bar).
int64_t media_file_bytes(const std::string& path) {
    std::FILE* f = pt_fopen(path.c_str(), "rb");
    if (!f) return 0;
    const bool    seeked = std::fseek(f, 0, SEEK_END) == 0;
    const int64_t bytes  = seeked ? static_cast<int64_t>(std::ftell(f)) : 0;
    std::fclose(f);
    return bytes > 0 ? bytes : 0;
}

// In pieces, so `onBytes` can move the bar inside a big file, and through "<dest>.tmp" + rename, so a
// failed copy leaves no partial file. ⚠️ Through pt_fopen, not `FileSystem::copy_file`: on Android
// that reads the whole file into memory first.
bool copy_in_pieces(const std::string& from, const std::string& to,
                    const std::function<void(int64_t)>& onBytes) {
    std::FILE* in = pt_fopen(from.c_str(), "rb");
    if (!in) return false;
    const std::string tmp = to + ".tmp";
    std::FILE* out = pt_fopen(tmp.c_str(), "wb");
    if (!out) {
        std::fclose(in);
        return false;
    }

    std::vector<char> buf(256 * 1024);
    bool ok = true;
    for (;;) {
        const size_t n = std::fread(buf.data(), 1, buf.size(), in);
        if (n == 0) break;
        if (std::fwrite(buf.data(), 1, n, out) != n) {
            ok = false;
            break;
        }
        onBytes(static_cast<int64_t>(n));
    }
    if (std::ferror(in)) ok = false;
    std::fclose(in);
    // ⚠️ `fclose` checked, and on its own line: a full card surfaces in this flush or nowhere.
    const bool flushed = std::fclose(out) == 0;
    ok = ok && flushed;

    if (ok) {
        pt_remove(to.c_str());   // a rename onto a taken name fails (or, on Android, de-duplicates)
        ok = pt_rename(tmp.c_str(), to.c_str()) == 0;
    }
    if (!ok) pt_remove(tmp.c_str());
    return ok;
}

}  // namespace

ActionResult collect_project(const songcore::SongcoreHost& host, FileSystem& fs,
                             const std::string& folder, const std::function<void(float)>& progress) {
    const std::string songSub = std::string(songcore::SONGS_FOLDER) + "/" + folder;
    const std::string songDir = fs.make_app_folder(songSub);
    if (songDir.empty()) return ActionResult{false, "COLLECT FAILED"};
    const std::string songDirKey = comparable_path(songDir + "/");

    songcore::Project copy = host.project();

    // The one path per instrument the loader opens (engine_setup.h `load_project_media`).
    std::vector<std::string*> paths;
    for (songcore::Instrument& ins : copy.instruments) {
        if (ins.instrumentType == songcore::InstrumentType::SOUNDFONT && ins.soundfontPath)
            paths.push_back(&*ins.soundfontPath);
        else if (ins.sampleFilePath)
            paths.push_back(&*ins.sampleFilePath);
    }

    // First what to copy, each file once however many instruments play it — so the bar knows the
    // total before the first byte moves.
    struct Copy {
        std::string source;
        std::string rel;
        int64_t     bytes = 0;
        bool        ok    = false;
        std::vector<std::string*> users;   // the paths that name it once it is copied
    };
    std::vector<Copy> copies;
    int64_t totalBytes = 0;
    int     missing    = 0;
    for (std::string* stored : paths) {
        const std::string source = songcore::resolve_media_path(*stored, host.media_roots());
        if (!songcore::path_exists(source)) {
            ++missing;   // keeps its old path: the file may turn up where it was
            continue;
        }

        // ⚠️ Already in this folder (the song was opened from here): it IS the copy. Replacing it
        // would delete the source before reading it.
        const std::string sourceKey = comparable_path(source);
        if (sourceKey.rfind(songDirKey, 0) == 0) {
            *stored = source.substr(songDirKey.size());
            continue;
        }

        const std::string rel = songcore::collected_media_path(*stored);
        auto it = std::find_if(copies.begin(), copies.end(),
                               [&rel](const Copy& c) { return c.rel == rel; });
        if (it == copies.end()) {
            Copy c;
            c.source = source;
            c.rel    = rel;
            c.bytes  = media_file_bytes(source);
            totalBytes += c.bytes;
            copies.push_back(std::move(c));
            it = copies.end() - 1;
        }
        it->users.push_back(stored);
    }

    // Reported only when the whole percent changes: each report is a repaint.
    int64_t doneBytes   = 0;
    int     lastPercent = -1;
    const auto onBytes = [&](int64_t n) {
        doneBytes += n;
        if (!progress || totalBytes <= 0) return;
        const int percent = static_cast<int>(std::min<int64_t>(100, doneBytes * 100 / totalBytes));
        if (percent == lastPercent) return;
        lastPercent = percent;
        progress(static_cast<float>(percent) / 100.0f);
    };
    onBytes(0);

    int failed = 0;
    for (Copy& c : copies) {
        const int64_t      before = doneBytes;
        const std::string  dest   = songDir + "/" + c.rel;
        const size_t       slash  = c.rel.find_last_of('/');
        c.ok = slash == std::string::npos ||
               !fs.make_app_folder(songSub + "/" + c.rel.substr(0, slash)).empty();
        if (c.ok && fs.file_exists(dest)) c.ok = fs.delete_path(dest);
        c.ok = c.ok && copy_in_pieces(c.source, dest, onBytes);
        // A file that failed, or measured wrong, still moves the bar by what it was counted as.
        onBytes(before + c.bytes - doneBytes);
        if (!c.ok) {
            ++failed;
            continue;
        }
        for (std::string* stored : c.users) *stored = c.rel;
    }

    if (!fs.write_file(songDir + "/" + folder + ".ptp", songcore::serialize_project(copy)))
        return ActionResult{false, "COLLECT FAILED"};
    fs.forget_listing(fs.songs_directory());
    fs.forget_listing(songDir);

    if (failed > 0)  return ActionResult{false, "COLLECTED: " + std::to_string(failed) + " NOT COPIED"};
    if (missing > 0) return ActionResult{false, "COLLECTED: " + std::to_string(missing) + " MISSING"};
    return ActionResult{true, "COLLECTED: " + songSub};
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
