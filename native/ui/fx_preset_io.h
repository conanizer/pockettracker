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

struct ReverbCells {
    std::string name;
    int algo = 0, size = 0x60, decay = 0x60, damp = 0x80, pre = 0x00, width = 0x80, mod = 0x10;
};

// `time` is a note division (0..B) when `sync`, else 00..FF.
struct DelayCells {
    std::string name;
    bool pong = false, sync = false;
    int  time = 0x40, feedback = 0x60, tone = 0xFF, wobble = 0x00, reverbSend = 0x00;
};

inline ReverbCells reverb_cells_of(const songcore::Project& p) {
    return {p.reverbPreset, p.reverbAlgo, p.reverbSize, p.reverbFeedback, p.reverbDamp,
            p.reverbPreDelay, p.reverbWidth, p.reverbMod};
}

inline void apply_reverb_cells(songcore::Project& p, const ReverbCells& c) {
    p.reverbPreset   = c.name;
    p.reverbAlgo     = c.algo;
    p.reverbSize     = c.size;
    p.reverbFeedback = c.decay;
    p.reverbDamp     = c.damp;
    p.reverbPreDelay = c.pre;
    p.reverbWidth    = c.width;
    p.reverbMod      = c.mod;
}

inline DelayCells delay_cells_of(const songcore::Project& p) {
    return {p.delayPreset, p.delayPong, p.delaySync, p.delayTime, p.delayFeedback, p.delayTone,
            p.delayWobble, p.delayReverbSend};
}

inline void apply_delay_cells(songcore::Project& p, const DelayCells& c) {
    p.delayPreset     = c.name;
    p.delayPong       = c.pong;
    p.delaySync       = c.sync;
    p.delayTime       = c.time;
    p.delayFeedback   = c.feedback;
    p.delayTone       = c.tone;
    p.delayWobble     = c.wobble;
    p.delayReverbSend = c.reverbSend;
}

inline std::string serialize_reverb_cells(const ReverbCells& c) {
    songcore::JsonWriter w{songcore::JsonLayout::Pretty};
    w.begin_object();
    if (!c.name.empty()) w.field_string("name", c.name);
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

inline std::string serialize_delay_cells(const DelayCells& c) {
    songcore::JsonWriter w{songcore::JsonLayout::Pretty};
    w.begin_object();
    if (!c.name.empty()) w.field_string("name", c.name);
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
                               ReverbCells& out) {
    const nlohmann::json j = nlohmann::json::parse(text, nullptr, /*allow_exceptions=*/false);
    if (!j.is_object()) return false;

    out.name = songcore::detail::get_str(j, "name", fallback_name);
    // An unknown algorithm keeps the current one; clamping would pick an unrelated reverb.
    const int algo = songcore::detail::get_int(j, "algo", out.algo);
    if (algo >= 0 && algo < kReverbAlgoCount) out.algo = algo;
    out.size  = detail::fx_cell(j, "size", out.size);
    out.decay = detail::fx_cell(j, "decay", out.decay);
    out.damp  = detail::fx_cell(j, "damp", out.damp);
    out.pre   = detail::fx_cell(j, "pre", out.pre);
    out.width = detail::fx_cell(j, "width", out.width);
    out.mod   = detail::fx_cell(j, "mod", out.mod);
    return true;
}

inline bool parse_delay_cells(const std::string& text, const std::string& fallback_name,
                              DelayCells& out) {
    const nlohmann::json j = nlohmann::json::parse(text, nullptr, /*allow_exceptions=*/false);
    if (!j.is_object()) return false;

    out.name = songcore::detail::get_str(j, "name", fallback_name);
    out.pong = songcore::detail::get_bool(j, "pong", out.pong);
    out.sync = songcore::detail::get_bool(j, "sync", out.sync);
    out.time       = detail::fx_cell(j, "time", out.time, out.sync ? 11 : 255);
    out.feedback   = detail::fx_cell(j, "feedback", out.feedback);
    out.tone       = detail::fx_cell(j, "tone", out.tone);
    out.wobble     = detail::fx_cell(j, "wobble", out.wobble);
    out.reverbSend = detail::fx_cell(j, "reverbSend", out.reverbSend);
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

inline bool save_reverb_file(FileSystem& fs, const std::string& path, const ReverbCells& cells) {
    return fs.write_file(path, serialize_reverb_cells(cells));
}

inline bool load_reverb_file(FileSystem& fs, const std::string& path, ReverbCells& out) {
    std::string text;
    if (!fs.read_file(path, text)) return false;
    return parse_reverb_cells(text, fx_preset_file_stem(path), out);
}

inline bool save_delay_file(FileSystem& fs, const std::string& path, const DelayCells& cells) {
    return fs.write_file(path, serialize_delay_cells(cells));
}

inline bool load_delay_file(FileSystem& fs, const std::string& path, DelayCells& out) {
    std::string text;
    if (!fs.read_file(path, text)) return false;
    return parse_delay_cells(text, fx_preset_file_stem(path), out);
}

}  // namespace pt::ui
