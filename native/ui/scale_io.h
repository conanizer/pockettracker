#pragma once

// ─── .pts — a scale, as a file ───────────────────────────────────────────────────────────────────
//
// One of the project's sixteen scale slots as a small human-readable file to save, share or drop onto
// an SD card (what `ui/theme_io.h` is to a palette).
// ⚠️ ALWAYS writes `enabled`, unlike every other writer here (which omit defaults): a Chromatic scale
// would otherwise be `{}`. The deferred microtuning `offset` is written only once moved, as in a .ptp.
// ⚠️ The reader is TOLERANT like `parse_theme` (unknown keys ignored; missing or wrong-typed keep the
// current value) and fails only when the text is not a JSON object — reported as LOAD FAILED.
// ⚠️ A load never touches the slot's `id`: the file is a shape, and a copied id would renumber slots.

#include <cctype>
#include <string>
#include <vector>

#include "songcore/model.h"
#include "songcore/project_io.h"     // JsonWriter + JsonLayout, and the pool parser's tolerances
#include "songcore/scale_bank.h"
#include "ui/filesystem.h"
#include "vendor/nlohmann/json.hpp"

namespace pt::ui {

/** The extension — the browser's filter, the save path and the seed all read it. */
inline constexpr const char* SCALE_FILE_EXT = "pts";

/** A scale → `.pts` bytes, pretty-printed. ⚠️ Uses `project_io`'s own writer and readers
 *  (`songcore::detail`) — one spelling of an int array and a tolerant key read across all files. */
inline std::string serialize_scale(const songcore::Scale& s) {
    songcore::JsonWriter w{songcore::JsonLayout::Pretty};
    w.begin_object();
    if (!s.name.empty()) w.field_string("name", s.name);
    songcore::detail::emit_int_array(w, "enabled", s.enabled);
    if (s.offset != std::vector<int>(12, 0))
        songcore::detail::emit_int_array(w, "offset", s.offset);
    w.end_object();
    return std::move(w.out);
}

/** `.pts` bytes → the scale's shape. `out` keeps its `id` and any field the file does not name.
 *  False only when the text is not a JSON object. */
inline bool parse_scale_text(const std::string& text, songcore::Scale& out) {
    const nlohmann::json j = nlohmann::json::parse(text, nullptr, /*allow_exceptions=*/false);
    if (!j.is_object()) return false;

    songcore::Scale s = out;                     // the id, and any field the file is silent about
    s.name    = songcore::detail::get_str(j, "name", s.name);
    s.enabled = songcore::detail::parse_int_array(j, "enabled", s.enabled);
    s.offset  = songcore::detail::parse_int_array(j, "offset",  s.offset);

    // A hand-edited or truncated array must not leave a 12-degree consumer reading past its end.
    s.enabled.resize(12, 1);
    s.offset.resize(12, 0);

    out = s;
    return true;
}

/** Write `scale` to `path`. */
inline bool save_scale_file(FileSystem& fs, const std::string& path, const songcore::Scale& scale) {
    return fs.write_file(path, serialize_scale(scale));
}

/** Read a scale from `path` into `out`, keeping `out.id`. */
inline bool load_scale_file(FileSystem& fs, const std::string& path, songcore::Scale& out) {
    std::string text;
    if (!fs.read_file(path, text)) return false;
    return parse_scale_text(text, out);
}

/**
 * A scale name as a FILENAME: anything outside `[A-Za-z0-9_]` becomes `_` (FAT32-safe; "Phrygian
 * Dominant" needs it).
 * ⚠️ No empty fallback here — callers supply it, since `<Scales>/.pts` is a dotfile the browser hides.
 */
inline std::string sanitize_scale_filename(const std::string& name) {
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
 * Write the compiled-in factory bank to the Scales folder as editable, shareable files.
 * ⚠️ Only when the folder holds NO `.pts` at all — a user who deleted thirty made a decision. Never
 * overwrites. The SCALE screen's cycle reads the compiled-in list, never these files. Returns how
 * many were written.
 */
inline int seed_scale_bank(FileSystem& fs) {
    const std::string dir = fs.scales_directory();

    // ⚠️ Lower-cased first: `FileInfo::extension` is the on-disk case (`MAJOR.PTS`).
    for (const FileInfo& f : fs.list_files(dir)) {
        if (f.isDirectory) continue;
        std::string ext = f.extension;
        for (char& c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (ext == SCALE_FILE_EXT) return 0;
    }

    const std::vector<songcore::ScaleBankEntry>& bank = songcore::scale_bank();
    int written = 0;
    for (int i = 0; i < static_cast<int>(bank.size()); ++i) {
        const std::string safe = sanitize_scale_filename(bank[static_cast<size_t>(i)].name);
        const std::string path = dir + "/" + (safe.empty() ? std::string("SCALE") : safe) + ".pts";
        if (fs.file_exists(path)) continue;

        songcore::Scale s;
        songcore::scale_apply_bank(s, i);
        if (save_scale_file(fs, path, s)) ++written;
    }
    return written;
}

}  // namespace pt::ui
