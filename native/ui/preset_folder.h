#pragma once

// The preset rows that step through a folder: SCALE, GROOVE, the reverb, the delay and THEME. A row's
// list is its folder's files by file name, factory ones among
// them; the values on screen sit at one of them, or at none — and then a step would lose them.

#include <algorithm>
#include <string>
#include <vector>

#include "effects/modules/delay-presets.h"
#include "effects/modules/reverb-presets.h"
#include "songcore/groove_bank.h"
#include "songcore/scales.h"
#include "ui/filesystem.h"
#include "ui/fx_preset_io.h"
#include "ui/preset_row.h"
#include "ui/groove_io.h"
#include "ui/scale_io.h"
#include "ui/theme_io.h"

namespace pt::ui {

// One file of a row's folder; `value.name` is the name the row shows (the file's own, else its stem).
template <typename T>
struct FolderPreset {
    std::string path;
    T           value;
};

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

// The entry named `name`, else the first whose values equal `now`, else −1.
template <typename T, typename Same>
int preset_position(const std::vector<FolderPreset<T>>& list, const std::string& name, const T& now,
                    Same same) {
    if (!name.empty())
        for (size_t i = 0; i < list.size(); ++i)
            if (list[i].value.name == name) return static_cast<int>(i);
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

inline std::vector<FolderPreset<songcore::Scale>> scale_preset_list(FileSystem& fs) {
    return read_preset_folder<songcore::Scale>(
        fs, fs.scales_directory(), SCALE_FILE_EXT,
        [](const std::string& text, const std::string&, songcore::Scale& out) {
            return parse_scale_text(text, out);
        });
}

inline std::vector<FolderPreset<songcore::Groove>> groove_preset_list(FileSystem& fs) {
    return read_preset_folder<songcore::Groove>(
        fs, fs.grooves_directory(), GROOVE_FILE_EXT,
        [](const std::string& text, const std::string&, songcore::Groove& out) {
            return parse_groove_text(text, out);
        });
}

inline std::vector<FolderPreset<NamedReverb>> reverb_preset_list(FileSystem& fs) {
    return read_preset_folder<NamedReverb>(fs, fs.reverbs_directory(), REVERB_FILE_EXT, parse_reverb_cells);
}

inline std::vector<FolderPreset<NamedDelay>> delay_preset_list(FileSystem& fs) {
    return read_preset_folder<NamedDelay>(fs, fs.delays_directory(), DELAY_FILE_EXT, parse_delay_cells);
}

inline std::vector<FolderPreset<Theme>> theme_preset_list(FileSystem& fs) {
    return read_preset_folder<Theme>(fs, fs.themes_directory(), "ptt",
                                     [](const std::string& text, const std::string&, Theme& out) {
                                         return parse_theme(text, out);
                                     });
}

// ── The factory reverbs, delays and themes as files ──────────────────────────────────────────────

inline NamedReverb reverb_preset_cells(const ReverbPreset& r) {
    return {r.name, r.cells};
}

inline NamedDelay delay_preset_cells(const DelayPreset& d) {
    return {d.name, d.cells};
}

// Written ONCE per install, then never again: these folders may already hold the user's own files,
// so "only into an empty folder" would leave the factory ones out. ⚠️ The note that it was done sits
// beside settings.json, never in a folder the browser lists.
// Does the folder already hold this preset, by file or by name, in any case? The user's copy wins.
template <typename T>
bool folder_has_preset(const std::vector<FolderPreset<T>>& list, const std::string& name) {
    const std::string want = to_lower_ascii(name), file = to_lower_ascii(safe_name(name));
    return std::any_of(list.begin(), list.end(), [&](const FolderPreset<T>& e) {
        return to_lower_ascii(e.value.name) == want || to_lower_ascii(path_stem(e.path)) == file;
    });
}

template <typename Write>
int seed_factory_once(FileSystem& fs, const std::string& dir, const char* what, Write write) {
    const std::string settings = fs.settings_path();
    const size_t      slash    = settings.find_last_of("/\\");
    if (slash == std::string::npos || dir.empty()) return 0;
    const std::string note = settings.substr(0, slash) + "/seeded-" + what;
    if (fs.file_exists(note)) return 0;

    const int written = write(dir);
    fs.write_file(note, "1");
    return written;
}

// Each factory preset not in `dir` yet, by file or by name; how many were written.
inline int write_reverb_bank(FileSystem& fs, const std::string& dir) {
    if (dir.empty()) return 0;
    const auto have    = reverb_preset_list(fs);
    int        written = 0;
    for (const ReverbPreset& r : kReverbPresets) {
        if (folder_has_preset(have, r.name)) continue;
        const std::string path = dir + "/" + safe_name(r.name) + "." + REVERB_FILE_EXT;
        if (fs.file_exists(path)) continue;
        if (save_reverb_file(fs, path, reverb_preset_cells(r))) ++written;
    }
    return written;
}

inline int write_delay_bank(FileSystem& fs, const std::string& dir) {
    if (dir.empty()) return 0;
    const auto have    = delay_preset_list(fs);
    int        written = 0;
    for (const DelayPreset& d : kDelayPresets) {
        if (folder_has_preset(have, d.name)) continue;
        const std::string path = dir + "/" + safe_name(d.name) + "." + DELAY_FILE_EXT;
        if (fs.file_exists(path)) continue;
        if (save_delay_file(fs, path, delay_preset_cells(d))) ++written;
    }
    return written;
}

inline int write_theme_bank(FileSystem& fs, const std::string& dir) {
    if (dir.empty()) return 0;
    const auto have    = theme_preset_list(fs);
    int        written = 0;
    for (const Theme& t : theme_builtins()) {
        if (folder_has_preset(have, t.name)) continue;
        const std::string path = dir + "/" + safe_name(t.name) + ".ptt";
        if (fs.file_exists(path)) continue;
        if (save_theme_file(fs, path, t)) ++written;
    }
    return written;
}

inline int seed_reverb_bank(FileSystem& fs) {
    return seed_factory_once(fs, fs.reverbs_directory(), "reverbs",
                             [&fs](const std::string& dir) { return write_reverb_bank(fs, dir); });
}
inline int seed_delay_bank(FileSystem& fs) {
    return seed_factory_once(fs, fs.delays_directory(), "delays",
                             [&fs](const std::string& dir) { return write_delay_bank(fs, dir); });
}
inline int seed_theme_bank(FileSystem& fs) {
    return seed_factory_once(fs, fs.themes_directory(), "themes",
                             [&fs](const std::string& dir) { return write_theme_bank(fs, dir); });
}

}  // namespace pt::ui
