#pragma once

// ─── .ptg — a groove, as a file ──────────────────────────────────────────────────────────────────
//
// The twin of `ui/scale_io.h` (the reasoning is there): a small human-readable file to save, share
// or drop onto an SD card.
// ⚠️ ALWAYS writes `steps` — under the .ptp's omit-defaults rule a STRAIGHT groove would be `{}`.
// ⚠️ A load never touches the slot's `id`: `GRV` names slots, and an id copied from a file would
// renumber them.

#include <cctype>
#include <string>
#include <vector>

#include "songcore/groove_bank.h"
#include "songcore/model.h"
#include "songcore/project_io.h"     // JsonWriter + JsonLayout, and the pool parser's tolerances
#include "ui/filesystem.h"
#include "vendor/nlohmann/json.hpp"

namespace pt::ui {

/** The extension — the browser's filter, the save path and the seed all read it. */
inline constexpr const char* GROOVE_FILE_EXT = "ptg";

/** A groove → `.ptg` bytes, pretty-printed. Uses `project_io`'s own writer and readers
 *  (`songcore::detail`), as scale_io.h does. */
inline std::string serialize_groove(const songcore::Groove& g) {
    songcore::JsonWriter w{songcore::JsonLayout::Pretty};
    w.begin_object();
    if (!g.name.empty()) w.field_string("name", g.name);
    songcore::detail::emit_int_array(w, "steps", g.steps);
    w.end_object();
    return std::move(w.out);
}

/** `.ptg` bytes → the groove's shape. `out` keeps its `id` and any field the file does not name.
 *  False only when the text is not a JSON object. */
inline bool parse_groove_text(const std::string& text, songcore::Groove& out) {
    const nlohmann::json j = nlohmann::json::parse(text, nullptr, /*allow_exceptions=*/false);
    if (!j.is_object()) return false;

    songcore::Groove g = out;                    // the id, and any field the file is silent about
    g.name  = songcore::detail::get_str(j, "name", g.name);
    g.steps = songcore::detail::parse_int_array(j, "steps", g.steps);

    // The GROOVE screen indexes all sixteen by cursor row; a hand-edited file may hold fewer.
    g.steps.resize(16, -1);
    for (int& v : g.steps)
        if (v < -1 || v > 255) v = -1;           // out of range: the row is absent

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
 * A groove name as a FILENAME: anything outside `[A-Za-z0-9_]` becomes `_` (FAT32-safe; the factory
 * bank's "TRIPLET 16" needs it).
 * ⚠️ No empty fallback here — callers supply it, since `<Grooves>/.ptg` is a dotfile the browser hides.
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
 * Write the compiled-in factory bank to the Grooves folder as editable, shareable files.
 * ⚠️ Only when the folder holds NO `.ptg` at all — a user who deleted some made a decision. Never
 * overwrites. Returns how many were written.
 */
inline int seed_groove_bank(FileSystem& fs) {
    const std::string dir = fs.grooves_directory();

    // ⚠️ Lower-cased first: `FileInfo::extension` is the on-disk case.
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
