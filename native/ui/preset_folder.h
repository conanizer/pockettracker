#pragma once

// The preset rows that step a list: SCALE, GROOVE, the reverb, the delay and THEME. A row's list is the
// factory presets, which live in the app and are never written out, then the user's files in its folder;
// the values on screen sit at one of them, or at none — and then a step would lose them.

#include <algorithm>
#include <cstddef>
#include <string>
#include <vector>

#include "effects/modules/delay-presets.h"
#include "effects/modules/reverb-presets.h"
#include "songcore/groove_bank.h"
#include "songcore/scale_bank.h"
#include "songcore/scales.h"
#include "ui/filesystem.h"
#include "ui/fx_preset_io.h"
#include "ui/preset_row.h"
#include "ui/groove_io.h"
#include "ui/scale_io.h"
#include "ui/theme_io.h"

namespace pt::ui {

// One entry of a row's list; `value.name` is the name the row shows (the file's own, else its stem).
// A factory entry's `path` is `factory:<name>`, never a file.
template <typename T>
struct FolderPreset {
    std::string path;
    T           value;
};

inline constexpr const char* FACTORY_PRESET_PREFIX = "factory:";

inline bool is_factory_preset(const std::string& path) { return path.rfind(FACTORY_PRESET_PREFIX, 0) == 0; }

// A row's list as last read; `read` false means "read it on the next use".
template <typename T>
struct PresetFolderCache {
    std::vector<FolderPreset<T>> list;
    bool                         read = false;
};

// Every `.ext` file in `dir` that parses, by file name. `parse(text, stem, out)` fills a default `T`.
// ⚠️ A row finds where it stands by name, so a second file with a name already listed is left out —
// stepping onto it would read back as the first, and the row could never step past.
template <typename T, typename Parse>
std::vector<FolderPreset<T>> read_preset_folder(FileSystem& fs, const std::string& dir, const char* ext,
                                                Parse parse) {
    std::vector<FileInfo> files;
    for (const FileInfo& f : fs.list_files(dir))
        if (!f.isDirectory && !f.isAction && to_lower_ascii(f.extension) == ext) files.push_back(f);
    std::sort(files.begin(), files.end(),
              [](const FileInfo& a, const FileInfo& b) {
                  const std::string la = to_lower_ascii(a.name), lb = to_lower_ascii(b.name);
                  return la != lb ? la < lb : a.name < b.name;
              });

    std::vector<FolderPreset<T>> out;
    for (const FileInfo& f : files) {
        std::string text;
        if (!fs.read_file(f.path, text)) continue;
        const std::string stem = path_stem(f.path);
        T v{};
        if (!parse(text, stem, v)) continue;
        if (v.name.empty()) v.name = stem;
        const bool listed = std::any_of(out.begin(), out.end(),
                                        [&v](const FolderPreset<T>& e) { return e.value.name == v.name; });
        if (!listed) out.push_back({f.path, std::move(v)});
    }
    return out;
}

// The factory presets in table order, then the files. A file holding a factory preset's values under its
// name (any case) is left out: it is a copy an earlier version wrote. An edited one stays, as the user's.
template <typename T, typename Same>
std::vector<FolderPreset<T>> with_factory(std::vector<T> factory, std::vector<FolderPreset<T>> files,
                                          Same same) {
    std::vector<FolderPreset<T>> out;
    for (T& v : factory) out.push_back({FACTORY_PRESET_PREFIX + v.name, std::move(v)});
    const size_t factoryCount = out.size();
    for (FolderPreset<T>& f : files) {
        const std::string name = to_lower_ascii(f.value.name);
        const bool copy = std::any_of(out.begin(), out.begin() + static_cast<std::ptrdiff_t>(factoryCount),
                                      [&](const FolderPreset<T>& e) {
                                          return to_lower_ascii(e.value.name) == name && same(e.value, f.value);
                                      });
        if (!copy) out.push_back(std::move(f));
    }
    return out;
}

// The entry named `name` holding `now`, else the first named `name`, else the first holding `now`, else −1.
// ⚠️ A factory preset and an edited file can share a name: found by name alone, the row standing on the
// file would read back as the factory one and never step past it.
template <typename T, typename Same>
int preset_position(const std::vector<FolderPreset<T>>& list, const std::string& name, const T& now,
                    Same same) {
    if (!name.empty()) {
        for (size_t i = 0; i < list.size(); ++i)
            if (list[i].value.name == name && same(list[i].value, now)) return static_cast<int>(i);
        for (size_t i = 0; i < list.size(); ++i)
            if (list[i].value.name == name) return static_cast<int>(i);
    }
    for (size_t i = 0; i < list.size(); ++i)
        if (same(list[i].value, now)) return static_cast<int>(i);
    return -1;
}

// `blank` is what A+B returns to; null for a row with none.
template <typename T, typename Same>
PresetRowView preset_row_view(const std::vector<FolderPreset<T>>& list, const std::string& name,
                              const T& now, Same same, const T* blank) {
    PresetRowView v;
    v.index  = preset_position(list, name, now, same);
    v.count  = static_cast<int>(list.size());
    v.marked = v.index >= 0 && !name.empty() && list[static_cast<size_t>(v.index)].value.name == name &&
               !same(list[static_cast<size_t>(v.index)].value, now);
    if (blank) v.defaultIndex = std::max(0, preset_position(list, std::string(), *blank, same));
    return v;
}

// Would a step lose the values? Not when a file holds them.
template <typename T, typename Same>
bool preset_values_kept(const std::vector<FolderPreset<T>>& list, const std::string& name, const T& now,
                        Same same) {
    const int at = preset_position(list, name, now, same);
    return at >= 0 && same(list[static_cast<size_t>(at)].value, now);
}

// ── What "the same" means per row: what the file sets, never the name ───────────────────────────

// The degrees only.
inline bool same_scale(const songcore::Scale& a, const songcore::Scale& b) {
    return songcore::scale_mask(a) == songcore::scale_mask(b);
}

// The same feel in any ruler.
inline bool same_groove(const songcore::Groove& a, const songcore::Groove& b) {
    const std::vector<int> sa = songcore::groove_active_steps(a);
    const std::vector<int> sb = songcore::groove_active_steps(b);
    if (sa.size() != sb.size()) return false;
    const int ra = songcore::groove_ticks_per_step(a);
    const int rb = songcore::groove_ticks_per_step(b);
    for (size_t i = 0; i < sa.size(); ++i)
        if (sa[i] * rb != sb[i] * ra) return false;
    return true;
}

inline bool same_reverb(const NamedReverb& a, const NamedReverb& b) { return a.cells == b.cells; }
inline bool same_delay(const NamedDelay& a, const NamedDelay& b) { return a.cells == b.cells; }

// The colours; the visualizer is the user's, not the theme's.
inline bool same_theme(Theme a, Theme b) {
    a.name = b.name = std::string();
    a.visualizerType = b.visualizerType = VisualizerType{};
    return serialize_theme(a) == serialize_theme(b);
}

// ── The lists ────────────────────────────────────────────────────────────────────────────────────

inline NamedReverb reverb_preset_cells(const ReverbPreset& r) {
    return {r.name, r.cells};
}

inline NamedDelay delay_preset_cells(const DelayPreset& d) {
    return {d.name, d.cells};
}

inline std::vector<FolderPreset<songcore::Scale>> scale_preset_list(FileSystem& fs) {
    std::vector<songcore::Scale> factory(songcore::scale_bank().size());
    for (size_t i = 0; i < factory.size(); ++i) songcore::scale_apply_bank(factory[i], static_cast<int>(i));
    return with_factory(std::move(factory),
                        read_preset_folder<songcore::Scale>(
                            fs, fs.scales_directory(), SCALE_FILE_EXT,
                            [](const std::string& text, const std::string&, songcore::Scale& out) {
                                return parse_scale_text(text, out);
                            }),
                        same_scale);
}

inline std::vector<FolderPreset<songcore::Groove>> groove_preset_list(FileSystem& fs) {
    std::vector<songcore::Groove> factory(songcore::groove_bank().size());
    for (size_t i = 0; i < factory.size(); ++i) songcore::groove_apply_bank(factory[i], static_cast<int>(i));
    return with_factory(std::move(factory),
                        read_preset_folder<songcore::Groove>(
                            fs, fs.grooves_directory(), GROOVE_FILE_EXT,
                            [](const std::string& text, const std::string&, songcore::Groove& out) {
                                return parse_groove_text(text, out);
                            }),
                        same_groove);
}

inline std::vector<FolderPreset<NamedReverb>> reverb_preset_list(FileSystem& fs) {
    std::vector<NamedReverb> factory;
    for (const ReverbPreset& r : kReverbPresets) factory.push_back(reverb_preset_cells(r));
    return with_factory(std::move(factory),
                        read_preset_folder<NamedReverb>(fs, fs.reverbs_directory(), REVERB_FILE_EXT,
                                                        parse_reverb_cells),
                        same_reverb);
}

inline std::vector<FolderPreset<NamedDelay>> delay_preset_list(FileSystem& fs) {
    std::vector<NamedDelay> factory;
    for (const DelayPreset& d : kDelayPresets) factory.push_back(delay_preset_cells(d));
    return with_factory(std::move(factory),
                        read_preset_folder<NamedDelay>(fs, fs.delays_directory(), DELAY_FILE_EXT,
                                                       parse_delay_cells),
                        same_delay);
}

inline std::vector<FolderPreset<Theme>> theme_preset_list(FileSystem& fs) {
    return with_factory(theme_builtins(),
                        read_preset_folder<Theme>(fs, fs.themes_directory(), "ptt",
                                                  [](const std::string& text, const std::string&, Theme& out) {
                                                      return parse_theme(text, out);
                                                  }),
                        same_theme);
}

}  // namespace pt::ui
