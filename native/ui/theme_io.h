#pragma once

// `.ptt`, a theme as a file. Parsed tolerantly; written byte-exact (a golden pins it):
//   * a field equal to `Theme{}`'s (CLASSIC) is omitted, so an unmodified CLASSIC is `{}`;
//   * 4-space indent, keys in THEME_COLORS order, no trailing newline;
//   * a colour is a plain decimal (0xFF0A0A0A is `4278518794`);
//   * `visualizerType` is its serial name — not the settings label, which would read back as SCOPE.

#include <cstdint>
#include <string>

#include "songcore/project_io.h"   // JsonWriter + JsonLayout
#include "ui/filesystem.h"
#include "ui/theme.h"
#include "vendor/nlohmann/json.hpp"

namespace pt::ui {

// ⚠️ The words in settings.json, not the names on screen (FLAT is EMPTY, GUIDE is HINTS): renaming
// one here resets every saved choice.
inline const char* visualizer_serial_name(VisualizerType v) {
    switch (v) {
        case VisualizerType::SCOPE:          return "SCOPE";
        case VisualizerType::FLAT:           return "FLAT";
        case VisualizerType::OCTA:           return "OCTA";
        case VisualizerType::OCTA_FULL:      return "OCTA_FULL";
        case VisualizerType::SPECTRUM:       return "SPECTRUM";
        case VisualizerType::SPECTRUM_PEAKS: return "SPECTRUM_PEAKS";
        case VisualizerType::GUIDE:          return "GUIDE";
    }
    return "SCOPE";
}

// An unknown name reads as the default.
inline VisualizerType visualizer_from_serial_name(const std::string& s) {
    if (s == "FLAT")           return VisualizerType::FLAT;
    if (s == "OCTA")           return VisualizerType::OCTA;
    if (s == "OCTA_FULL")      return VisualizerType::OCTA_FULL;
    if (s == "SPECTRUM")       return VisualizerType::SPECTRUM;
    if (s == "SPECTRUM_PEAKS") return VisualizerType::SPECTRUM_PEAKS;
    if (s == "GUIDE")          return VisualizerType::GUIDE;
    return VisualizerType::SCOPE;   // incl. "SCOPE", and any value another build might write
}

inline std::string serialize_theme(const Theme& t) {
    const Theme d{};   // the field defaults — the omission yardstick
    songcore::JsonWriter w{songcore::JsonLayout::Pretty};
    w.begin_object();

    if (t.name != d.name) w.field_string("name", t.name);

    // A borrowed colour is omitted when it equals what `derive_borrowed_colors` gives this theme.
    Theme e = t;
    derive_borrowed_colors(e);
    for (const ThemeColor& c : THEME_COLORS) {
        const Argb v = t.*c.field;
        if (v != (c.borrowed ? e : d).*c.field) w.field_int(c.key, static_cast<long long>(v));
    }

    if (t.visualizerType != d.visualizerType)
        w.field_string("visualizerType", visualizer_serial_name(t.visualizerType));

    w.end_object();
    return std::move(w.out);
}

// Tolerant: unknown keys are ignored, a bad key keeps the default. False only when it is not a JSON object.
inline bool parse_theme(const std::string& text, Theme& out) {
    const nlohmann::json j = nlohmann::json::parse(text, nullptr, /*allow_exceptions=*/false);
    if (!j.is_object()) return false;

    Theme t{};   // the field defaults fill every key the file lacks

    if (const auto it = j.find("name"); it != j.end() && it->is_string())
        t.name = it->get<std::string>();

    auto read = [&](const ThemeColor& c) {
        const auto it = j.find(c.key);
        if (it == j.end() || !it->is_number_integer()) return;
        t.*c.field = static_cast<Argb>(static_cast<uint64_t>(it->get<int64_t>()) & 0xFFFFFFFFu);
    };

    // The borrowed ones are derived from what was read, then overwritten by whatever the file names.
    for (const ThemeColor& c : THEME_COLORS) if (!c.borrowed) read(c);
    derive_borrowed_colors(t);
    for (const ThemeColor& c : THEME_COLORS) if (c.borrowed) read(c);

    if (const auto it = j.find("visualizerType"); it != j.end() && it->is_string())
        t.visualizerType = visualizer_from_serial_name(it->get<std::string>());

    out = t;
    return true;
}

inline bool save_theme_file(FileSystem& fs, const std::string& path, const Theme& theme) {
    return fs.write_file(path, serialize_theme(theme));
}

// The visualizer is not taken from the file: it is the user's, not the theme's.
inline bool load_theme_file(FileSystem& fs, const std::string& path, Theme& out) {
    std::string text;
    if (!fs.read_file(path, text)) return false;

    Theme loaded{};
    if (!parse_theme(text, loaded)) return false;

    loaded.visualizerType = out.visualizerType;   // the user's, kept across the load
    out = loaded;
    return true;
}

}  // namespace pt::ui
