#pragma once

// `.ptg`: one groove slot as a file, the twin of ui/scale_io.h. `steps` is always written (a straight
// groove would otherwise be `{}`), with its ruler. A load never touches the slot's `id` (`GRV` names slots).

#include <string>
#include <vector>

#include "songcore/groove_bank.h"
#include "songcore/model.h"
#include "songcore/project_io.h"
#include "ui/filesystem.h"
#include "vendor/nlohmann/json.hpp"

namespace pt::ui {

inline constexpr const char* GROOVE_FILE_EXT = "ptg";

inline std::string serialize_groove(const songcore::Groove& g) {
    songcore::JsonWriter w{songcore::JsonLayout::Pretty};
    w.begin_object();
    if (!g.name.empty()) w.field_string("name", g.name);
    songcore::detail::emit_int_array(w, "steps", g.steps);
    if (g.ticksPerStep != songcore::GROOVE_DEFAULT_RULER) w.field_int("ticksPerStep", g.ticksPerStep);
    w.end_object();
    return std::move(w.out);
}

// `out` keeps its `id` and whatever the file does not name. False only when it is not a JSON object.
inline bool parse_groove_text(const std::string& text, songcore::Groove& out) {
    const nlohmann::json j = nlohmann::json::parse(text, nullptr, /*allow_exceptions=*/false);
    if (!j.is_object()) return false;

    songcore::Groove g = out;
    g.name  = songcore::detail::get_str(j, "name", g.name);
    g.steps = songcore::detail::parse_int_array(j, "steps", g.steps);
    // ⚠️ Not the slot's: a file naming no ruler was written in the default one.
    g.ticksPerStep = songcore::detail::get_int(j, "ticksPerStep", songcore::GROOVE_DEFAULT_RULER);

    // The screen indexes all sixteen; a hand-edited file may hold fewer.
    g.steps.resize(16, -1);
    for (int& v : g.steps)
        if (v < -1 || v > 255) v = -1;           // out of range: the row is absent

    out = g;
    return true;
}

inline bool save_groove_file(FileSystem& fs, const std::string& path, const songcore::Groove& groove) {
    return fs.write_file(path, serialize_groove(groove));
}

inline bool load_groove_file(FileSystem& fs, const std::string& path, songcore::Groove& out) {
    std::string text;
    if (!fs.read_file(path, text)) return false;
    return parse_groove_text(text, out);
}

// As `seed_scale_bank`.
inline int seed_groove_bank(FileSystem& fs) {
    const std::string dir = fs.grooves_directory();
    if (folder_has_extension(fs, dir, GROOVE_FILE_EXT)) return 0;

    const std::vector<songcore::GrooveBankEntry>& bank = songcore::groove_bank();
    int written = 0;
    for (int i = 0; i < static_cast<int>(bank.size()); ++i) {
        const std::string safe = safe_file_name(bank[static_cast<size_t>(i)].name);
        const std::string path = dir + "/" + (safe.empty() ? std::string("GROOVE") : safe) + ".ptg";
        if (fs.file_exists(path)) continue;

        songcore::Groove g;
        songcore::groove_apply_bank(g, i);
        if (save_groove_file(fs, path, g)) ++written;
    }
    return written;
}

}  // namespace pt::ui
