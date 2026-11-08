#pragma once

// `.ptr` / `.ptd`: one EFFECTS section's cells as a file to save and share.
// ⚠️ Every cell is written, defaults included: the reader keeps the current value of a missing key.
// Not in the file: INP EQ (a project slot) and the return level (the mixer's). Read tolerantly,
// numbers clamped to the cell.

#include <algorithm>
#include <string>

#include "effects/modules/reverb-presets.h"
#include "songcore/model.h"
#include "songcore/project_io.h"
#include "ui/filesystem.h"
#include "vendor/nlohmann/json.hpp"

namespace pt::ui {

inline constexpr const char* REVERB_FILE_EXT = "ptr";
inline constexpr const char* DELAY_FILE_EXT  = "ptd";

// What fits on the preset row before SAVE, `*` included.
inline constexpr int FX_PRESET_NAME_MAX = 14;

// A preset row's whole state: its name and the send's cells.
struct NamedReverb {
    std::string           name;
    songcore::ReverbCells cells;
};

struct NamedDelay {
    std::string          name;
    songcore::DelayCells cells;
};

inline NamedReverb reverb_cells_of(const songcore::Project& p) { return {p.reverbPreset, p.reverb}; }
inline NamedDelay  delay_cells_of(const songcore::Project& p) { return {p.delayPreset, p.delay}; }

inline void apply_reverb_cells(songcore::Project& p, const NamedReverb& r) {
    p.reverbPreset = r.name;
    p.reverb       = r.cells;
}

inline void apply_delay_cells(songcore::Project& p, const NamedDelay& d) {
    p.delayPreset = d.name;
    p.delay       = d.cells;
}

inline std::string serialize_reverb_cells(const NamedReverb& r) {
    const songcore::ReverbCells& c = r.cells;
    songcore::JsonWriter w{songcore::JsonLayout::Pretty};
    w.begin_object();
    if (!r.name.empty()) w.field_string("name", r.name);
    w.field_int("algo", c.algo);
    w.field_int("size", c.size);
    w.field_int("decay", c.decay);
    w.field_int("damp", c.damp);
    w.field_int("pre", c.pre);
    w.field_int("width", c.width);
    w.field_int("mod", c.mod);
    w.end_object();
    return std::move(w.out);
}

inline std::string serialize_delay_cells(const NamedDelay& d) {
    const songcore::DelayCells& c = d.cells;
    songcore::JsonWriter w{songcore::JsonLayout::Pretty};
    w.begin_object();
    if (!d.name.empty()) w.field_string("name", d.name);
    w.field_bool("pong", c.pong);
    w.field_bool("sync", c.sync);
    w.field_int("time", c.time);
    w.field_int("feedback", c.feedback);
    w.field_int("tone", c.tone);
    w.field_int("wobble", c.wobble);
    w.field_int("reverbSend", c.reverbSend);
    w.end_object();
    return std::move(w.out);
}

namespace detail {

inline int fx_cell(const nlohmann::json& j, const char* key, int current, int max = 255) {
    return std::clamp(songcore::detail::get_int(j, key, current), 0, max);
}

}  // namespace detail

// `out` keeps any cell the file does not name; a missing name falls back to the file's own, never the
// row's. False only when it is not a JSON object.
inline bool parse_reverb_cells(const std::string& text, const std::string& fallback_name,
                               NamedReverb& out) {
    const nlohmann::json j = nlohmann::json::parse(text, nullptr, /*allow_exceptions=*/false);
    if (!j.is_object()) return false;

    songcore::ReverbCells& c = out.cells;
    out.name = songcore::detail::get_str(j, "name", fallback_name);
    // An unknown algorithm keeps the current one; clamping would pick an unrelated reverb.
    const int algo = songcore::detail::get_int(j, "algo", c.algo);
    if (algo >= 0 && algo < kReverbAlgoCount) c.algo = algo;
    c.size  = detail::fx_cell(j, "size", c.size);
    c.decay = detail::fx_cell(j, "decay", c.decay);
    c.damp  = detail::fx_cell(j, "damp", c.damp);
    c.pre   = detail::fx_cell(j, "pre", c.pre);
    c.width = detail::fx_cell(j, "width", c.width);
    c.mod   = detail::fx_cell(j, "mod", c.mod);
    return true;
}

inline bool parse_delay_cells(const std::string& text, const std::string& fallback_name,
                              NamedDelay& out) {
    const nlohmann::json j = nlohmann::json::parse(text, nullptr, /*allow_exceptions=*/false);
    if (!j.is_object()) return false;

    songcore::DelayCells& c = out.cells;
    out.name     = songcore::detail::get_str(j, "name", fallback_name);
    c.pong       = songcore::detail::get_bool(j, "pong", c.pong);
    c.sync       = songcore::detail::get_bool(j, "sync", c.sync);
    c.time       = detail::fx_cell(j, "time", c.time, c.sync ? 11 : 255);
    c.feedback   = detail::fx_cell(j, "feedback", c.feedback);
    c.tone       = detail::fx_cell(j, "tone", c.tone);
    c.wobble     = detail::fx_cell(j, "wobble", c.wobble);
    c.reverbSend = detail::fx_cell(j, "reverbSend", c.reverbSend);
    return true;
}

// `/card/Reverbs/BIG_HALL.ptr` → `BIG_HALL`: the name of a file with no `name` key.
inline std::string fx_preset_file_stem(const std::string& path) {
    const size_t slash = path.find_last_of("/\\");
    std::string  stem  = path.substr(slash == std::string::npos ? 0 : slash + 1);
    const size_t dot   = stem.rfind('.');
    if (dot != std::string::npos) stem.erase(dot);
    return stem.substr(0, static_cast<size_t>(FX_PRESET_NAME_MAX));
}

inline bool save_reverb_file(FileSystem& fs, const std::string& path, const NamedReverb& cells) {
    return fs.write_file(path, serialize_reverb_cells(cells));
}

inline bool load_reverb_file(FileSystem& fs, const std::string& path, NamedReverb& out) {
    std::string text;
    if (!fs.read_file(path, text)) return false;
    return parse_reverb_cells(text, fx_preset_file_stem(path), out);
}

inline bool save_delay_file(FileSystem& fs, const std::string& path, const NamedDelay& cells) {
    return fs.write_file(path, serialize_delay_cells(cells));
}

inline bool load_delay_file(FileSystem& fs, const std::string& path, NamedDelay& out) {
    std::string text;
    if (!fs.read_file(path, text)) return false;
    return parse_delay_cells(text, fx_preset_file_stem(path), out);
}

}  // namespace pt::ui
