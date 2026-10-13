#pragma once

// ─── .ptr and .ptd — a reverb or a delay preset, as a file ───────────────────────────────────────
//
// The cells of one EFFECTS section as a small human-readable file to save and share (what
// `ui/scale_io.h` is to a scale).
// ⚠️ ALWAYS writes every cell, defaults included: the reader keeps the current value of a key the file
// does not name, so an omitted default would load as whatever the project happened to hold.
// ⚠️ INP EQ is not in the file — it names one of the PROJECT's EQ slots — and neither is the return
// level, which is the MIXER's.
// ⚠️ The reader is TOLERANT like `parse_scale_text` (unknown keys ignored; missing or wrong-typed keep
// the current value; numbers clamped to the cell) and fails only when the text is not a JSON object.

#include <algorithm>
#include <string>

#include "effects/modules/reverb-presets.h"   // kReverbAlgoCount
#include "songcore/model.h"
#include "songcore/project_io.h"               // JsonWriter, and the pool parser's tolerances
#include "ui/filesystem.h"
#include "vendor/nlohmann/json.hpp"

namespace pt::ui {

/** The extensions — the browser's filter and the save path both read them. */
inline constexpr const char* REVERB_FILE_EXT = "ptr";
inline constexpr const char* DELAY_FILE_EXT  = "ptd";

/** The longest name SAVE takes: what fits on the preset row before the SAVE cell, `*` included. */
inline constexpr int FX_PRESET_NAME_MAX = 14;

/** What a `.ptr` carries: the reverb section's cells, and the name its row shows. */
struct ReverbCells {
    std::string name;
    int algo = 0, size = 0x60, decay = 0x60, damp = 0x80, pre = 0x00, width = 0x80, mod = 0x10;
};

/** What a `.ptd` carries. `time` is a note division (0..B) when `sync`, else a free 00..FF. */
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

/**
 * `.ptr` text → the cells. `out` keeps any cell the file does not name; the name falls back to
 * `fallback_name` (the file's own), never to the name already on the row — that one described other
 * cells. False only when the text is not a JSON object.
 */
inline bool parse_reverb_cells(const std::string& text, const std::string& fallback_name,
                               ReverbCells& out) {
    const nlohmann::json j = nlohmann::json::parse(text, nullptr, /*allow_exceptions=*/false);
    if (!j.is_object()) return false;

    out.name = songcore::detail::get_str(j, "name", fallback_name);
    // ⚠️ An algorithm this build does not have keeps the current one: a number is an identity, and
    // clamping it would pick an unrelated reverb.
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

/** `.ptd` text → the cells, on the same terms. */
inline bool parse_delay_cells(const std::string& text, const std::string& fallback_name,
                              DelayCells& out) {
    const nlohmann::json j = nlohmann::json::parse(text, nullptr, /*allow_exceptions=*/false);
    if (!j.is_object()) return false;

    out.name = songcore::detail::get_str(j, "name", fallback_name);
    out.pong = songcore::detail::get_bool(j, "pong", out.pong);
    out.sync = songcore::detail::get_bool(j, "sync", out.sync);
    // ⚠️ Clamped into whichever vocabulary `sync` leaves live: a synced TIME indexes twelve names.
    out.time       = detail::fx_cell(j, "time", out.time, out.sync ? 11 : 255);
    out.feedback   = detail::fx_cell(j, "feedback", out.feedback);
    out.tone       = detail::fx_cell(j, "tone", out.tone);
    out.wobble     = detail::fx_cell(j, "wobble", out.wobble);
    out.reverbSend = detail::fx_cell(j, "reverbSend", out.reverbSend);
    return true;
}

/**
 * A preset name as a FILENAME: anything outside `[A-Za-z0-9_]` becomes `_` (FAT32-safe).
 * ⚠️ No empty fallback here — callers supply it, since `<dir>/.ptr` is a dotfile the browser hides.
 */
inline std::string sanitize_fx_preset_filename(const std::string& name) {
    std::string out;
    out.reserve(name.size());
    for (const char c : name) {
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                        (c >= '0' && c <= '9') || c == '_';
        out += ok ? c : '_';
    }
    return out;
}

/** `/card/Reverbs/BIG_HALL.ptr` → `BIG_HALL`: the name a file with no `name` key loads under. */
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
