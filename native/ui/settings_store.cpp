#include "ui/settings_store.h"

#include <string>

#include "vendor/nlohmann/json.hpp"

namespace pt::ui {

namespace {

using nlohmann::json;

/** A key that is absent, null, or of the wrong type leaves the default alone. */
bool get_bool(const json& j, const char* key, bool fallback) {
    const auto it = j.find(key);
    if (it == j.end() || !it->is_boolean()) return fallback;
    return it->get<bool>();
}

int get_int(const json& j, const char* key, int fallback) {
    const auto it = j.find(key);
    if (it == j.end() || !it->is_number_integer()) return fallback;
    return it->get<int>();
}

std::string get_string(const json& j, const char* key, const std::string& fallback) {
    const auto it = j.find(key);
    if (it == j.end() || !it->is_string()) return fallback;
    return it->get<std::string>();
}

int clamp(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

constexpr int VISUALIZER_COUNT = 6;   // VisualizerType — SCOPE … SPECTRUM_PEAKS

}  // namespace

bool load_settings(FileSystem& fs, SettingsValues& values, Theme& theme) {
    std::string blob;
    if (!fs.read_file(fs.settings_path(), blob)) return false;   // first launch

    // Tolerant by design: a settings file the user has hand-edited into nonsense costs them their
    // settings, not their session.
    const json j = json::parse(blob, nullptr, /*allow_exceptions=*/false);
    if (!j.is_object()) return false;

    values.scalingBilinear    = get_bool(j, "scalingBilinear",    values.scalingBilinear);
    values.insertBefore       = get_bool(j, "insertBefore",       values.insertBefore);
    values.cursorRemember     = get_bool(j, "cursorRemember",     values.cursorRemember);
    values.notePreviewEnabled = get_bool(j, "notePreview",        values.notePreviewEnabled);
    values.traceEnabled       = get_bool(j, "trace",              values.traceEnabled);

    // The visualizer is the theme's field but the USER's choice, so it survives the theme swap below
    // — which is why it is read first and handed in rather than left to be overwritten.
    const int viz = clamp(get_int(j, "visualizer", static_cast<int>(theme.visualizerType)),
                          0, VISUALIZER_COUNT - 1);

    theme = theme_by_name(get_string(j, "theme", theme.name), static_cast<VisualizerType>(viz));
    return true;
}

bool save_settings(FileSystem& fs, const SettingsValues& values, const Theme& theme) {
    json j;
    j["scalingBilinear"] = values.scalingBilinear;
    j["insertBefore"]    = values.insertBefore;
    j["cursorRemember"]  = values.cursorRemember;
    j["notePreview"]     = values.notePreviewEnabled;
    j["trace"]           = values.traceEnabled;
    j["visualizer"]      = static_cast<int>(theme.visualizerType);
    j["theme"]           = theme.name;

    return fs.write_file(fs.settings_path(), j.dump(2) + "\n");
}

}  // namespace pt::ui
