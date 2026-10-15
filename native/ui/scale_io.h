#pragma once

// `.pts`: one scale slot as a file to save and share. `enabled` is always written (a Chromatic scale
// would otherwise be `{}`). Read tolerantly; a load never touches the slot's `id`.

#include <string>
#include <vector>

#include "songcore/model.h"
#include "songcore/project_io.h"
#include "songcore/scale_bank.h"
#include "ui/filesystem.h"
#include "vendor/nlohmann/json.hpp"

namespace pt::ui {

inline constexpr const char* SCALE_FILE_EXT = "pts";

// Through `project_io`'s own writer, so every file spells an int array alike.
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

// `out` keeps its `id` and whatever the file does not name. False only when it is not a JSON object.
inline bool parse_scale_text(const std::string& text, songcore::Scale& out) {
    const nlohmann::json j = nlohmann::json::parse(text, nullptr, /*allow_exceptions=*/false);
    if (!j.is_object()) return false;

    songcore::Scale s = out;
    s.name    = songcore::detail::get_str(j, "name", s.name);
    s.enabled = songcore::detail::parse_int_array(j, "enabled", s.enabled);
    s.offset  = songcore::detail::parse_int_array(j, "offset",  s.offset);

    // A short hand-edited array must not leave a 12-degree reader running off its end.
    s.enabled.resize(12, 1);
    s.offset.resize(12, 0);

    out = s;
    return true;
}

inline bool save_scale_file(FileSystem& fs, const std::string& path, const songcore::Scale& scale) {
    return fs.write_file(path, serialize_scale(scale));
}

inline bool load_scale_file(FileSystem& fs, const std::string& path, songcore::Scale& out) {
    std::string text;
    if (!fs.read_file(path, text)) return false;
    return parse_scale_text(text, out);
}

// Write the factory bank to the Scales folder — only when it holds no `.pts` at all (a user who deleted
// them meant it). Never overwrites. Returns how many were written.
inline int seed_scale_bank(FileSystem& fs) {
    const std::string dir = fs.scales_directory();
    if (folder_has_extension(fs, dir, SCALE_FILE_EXT)) return 0;

    const std::vector<songcore::ScaleBankEntry>& bank = songcore::scale_bank();
    int written = 0;
    for (int i = 0; i < static_cast<int>(bank.size()); ++i) {
        const std::string safe = safe_file_name(bank[static_cast<size_t>(i)].name);
        const std::string path = dir + "/" + (safe.empty() ? std::string("SCALE") : safe) + ".pts";
        if (fs.file_exists(path)) continue;

        songcore::Scale s;
        songcore::scale_apply_bank(s, i);
        if (save_scale_file(fs, path, s)) ++written;
    }
    return written;
}

}  // namespace pt::ui
