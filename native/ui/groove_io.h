#pragma once

// ─── .ptg — a groove, as a file ──────────────────────────────────────────────────────────────────
//
// `ui/scale_io.h` for one of the project's grooves, and deliberately the same file in every respect
// it can be: a small human-readable document a user can save, rename, hand to someone else, or drop
// onto an SD card. Where the two differ it is noted below; where they do not, the reasoning is over
// there and is not repeated.
//
// ⚠️ IT ALWAYS WRITES `steps`, where a `.ptp` writes a field only when it differs from the default.
// A `.ptg` has exactly one subject, and a STRAIGHT groove written under the project's rule would be
// an empty object — a file that says nothing about the only thing it is for.
//
// ⚠️ A LOAD NEVER TOUCHES THE SLOT'S `id`. The id is *which of the grooves this is*, and `GRV` in a
// phrase names it; copying an id out of a file would let a groove saved from slot 3 renumber slot 9
// on the way in, and silently move every phrase that pointed at either.

#include <cctype>
#include <string>
#include <vector>

#include "songcore/groove_bank.h"
#include "songcore/model.h"
#include "songcore/project_io.h"     // JsonWriter + JsonLayout, and the pool parser's tolerances
#include "ui/filesystem.h"
#include "vendor/nlohmann/json.hpp"

namespace pt::ui {

/** The extension, in one place — the browser's filter, the save path and the seed all read it. */
inline constexpr const char* GROOVE_FILE_EXT = "ptg";

/**
 * A groove → `.ptg` bytes. Pretty-printed: this is a file a person may open.
 *
 * ⚠️ The array writer and the readers below are `project_io`'s own (`songcore::detail`), reached
 * into deliberately rather than reimplemented — the same bargain `scale_io.h` states at length.
 */
inline std::string serialize_groove(const songcore::Groove& g) {
    songcore::JsonWriter w{songcore::JsonLayout::Pretty};
    w.begin_object();
    if (!g.name.empty()) w.field_string("name", g.name);
    songcore::detail::emit_int_array(w, "steps", g.steps);
    w.end_object();
    return std::move(w.out);
}

/**
 * `.ptg` bytes → the shape half of a groove. `out` keeps its `id`; everything else the file names is
 * replaced, and everything it does not name keeps what `out` already held.
 *
 * Returns false only when the text is not a JSON object.
 */
inline bool parse_groove_text(const std::string& text, songcore::Groove& out) {
    const nlohmann::json j = nlohmann::json::parse(text, nullptr, /*allow_exceptions=*/false);
    if (!j.is_object()) return false;

    songcore::Groove g = out;                    // the id, and any field the file is silent about
    g.name  = songcore::detail::get_str(j, "name", g.name);
    g.steps = songcore::detail::parse_int_array(j, "steps", g.steps);

    // The GROOVE screen indexes all sixteen directly with a cursor row, and a hand-edited or
    // truncated file could hand it fewer — the repair `parse_groove` makes inside a project.
    g.steps.resize(16, -1);
    for (int& v : g.steps)
        if (v < -1 || v > 255) v = -1;           // outside the legal range, the row is simply absent

    out = g;
    return true;
}

/** Write `groove` to `path`. */
inline bool save_groove_file(FileSystem& fs, const std::string& path, const songcore::Groove& groove) {
    return fs.write_file(path, serialize_groove(groove));
}

/** Read a groove from `path` into `out`, keeping `out.id`. */
inline bool load_groove_file(FileSystem& fs, const std::string& path, songcore::Groove& out) {
    std::string text;
    if (!fs.read_file(path, text)) return false;
    return parse_groove_text(text, out);
}

/**
 * A groove name, as a FILENAME: anything outside `[A-Za-z0-9_]` becomes `_`, so a name survives a
 * FAT32 card. The SPACE in "TRIPLET 16" is what makes it matter for the factory bank, which is the
 * one caller that hits it every time.
 *
 * ⚠️ It does not supply the empty fallback — the callers do, because `<Grooves>/.ptg` is a dotfile
 * the browser does not list, and a save that produces an invisible file reports success by silence.
 */
inline std::string sanitize_groove_filename(const std::string& name) {
    std::string out;
    out.reserve(name.size());
    for (const char c : name) {
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                        (c >= '0' && c <= '9') || c == '_';
        out += ok ? c : '_';
    }
    return out;
}

/**
 * Write the compiled-in factory bank to the Grooves folder, so the shapes exist as files that can be
 * edited, renamed and shared. `seed_scale_bank`'s twin, on the same two terms:
 *
 * ⚠️ THE TEST IS "DOES THIS FOLDER HOLD ANY `.ptg` AT ALL", not "is each file missing" — a user who
 * deletes the ones they never use has made a decision, and a per-file seed would undo it on every
 * launch. ⚠️ AND IT NEVER OVERWRITES, so an edited file of the same name is safe.
 *
 * Returns how many files it wrote — 0 when the folder already had grooves in it.
 */
inline int seed_groove_bank(FileSystem& fs) {
    const std::string dir = fs.grooves_directory();

    // ⚠️ Lower-cased before it is compared: `FileInfo::extension` is the case that is ON DISK.
    for (const FileInfo& f : fs.list_files(dir)) {
        if (f.isDirectory) continue;
        std::string ext = f.extension;
        for (char& c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (ext == GROOVE_FILE_EXT) return 0;
    }

    const std::vector<songcore::GrooveBankEntry>& bank = songcore::groove_bank();
    int written = 0;
    for (int i = 0; i < static_cast<int>(bank.size()); ++i) {
        const std::string safe = sanitize_groove_filename(bank[static_cast<size_t>(i)].name);
        const std::string path = dir + "/" + (safe.empty() ? std::string("GROOVE") : safe) + ".ptg";
        if (fs.file_exists(path)) continue;

        songcore::Groove g;
        songcore::groove_apply_bank(g, i);
        if (save_groove_file(fs, path, g)) ++written;
    }
    return written;
}

}  // namespace pt::ui
