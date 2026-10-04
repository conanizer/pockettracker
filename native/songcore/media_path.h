#ifndef POCKETTRACKER_SONGCORE_MEDIA_PATH_H
#define POCKETTRACKER_SONGCORE_MEDIA_PATH_H

// ─── Making a stored path mean something on THIS device ──────────────────────────────────────────
//
// A path in a project or config.json may be RELATIVE to the project, ABSOLUTE under another
// install's app root, or differ in CASE from a case-sensitive disk. All three are handled here.
// ⚠️ `resolve_media_path` (samples, SF2s) and `pt::ui::resolve_browse_dir` (config.json folders) must
// both re-root through `app_root_relative_tail`, or a project's samples end up in a folder the
// browser will not open.

#include <filesystem>   // resolve_case_insensitive only
#include <cstdio>
#include <string>
#include <vector>

#include "../byte_source.h"  // pt_fopen

namespace songcore {

// A cheap "is this file here?" — an open probe, not <filesystem>. Used only to decide whether an
// absolute path needs relocating.
// ⚠️ Must go through pt_fopen: a wrong "no" here re-roots a correct path, and the instrument loads
// empty with no error anywhere.
inline bool path_exists(const std::string& path) {
    if (path.empty()) return false;
    FILE* f = pt_fopen(path.c_str(), "rb");
    if (!f) return false;
    std::fclose(f);
    return true;
}

// The app-root-relative tail ("Samples/Pads/kick.wav") of an absolute path authored under ANOTHER
// install, or empty when it lies under no recognisable app sub-tree.
// A `pt://<root-id>/…` URI is deliberately included: re-granting the folder changes the id, and
// such paths re-root like any other. Only ever run on a path that has already failed to open.
// ⚠️ Anchor 2 lists the MEDIA sub-trees only, so a browse folder under a foreign root not named
// PocketTracker falls back to its category default rather than a wrong guess.
inline std::string app_root_relative_tail(const std::string& path) {
    // 1) Everything after the LAST "/PocketTracker/" (the phone's root). rfind, so a user sub-folder
    //    with that name loses to the real root above it.
    static const std::string kPtAnchor = "/PocketTracker/";
    const size_t pt = path.rfind(kPtAnchor);
    if (pt != std::string::npos) return path.substr(pt + kPtAnchor.size());

    // 2) A root not named PocketTracker: anchor on the media sub-tree, keeping it in the tail.
    static const std::string kSubtrees[] = { "/Samples/", "/Soundfonts/", "/Renders/" };
    size_t best = std::string::npos;
    for (const std::string& sub : kSubtrees) {
        const size_t at = path.rfind(sub);
        if (at != std::string::npos && (best == std::string::npos || at > best)) best = at;
    }
    if (best == std::string::npos) return "";
    return path.substr(best + 1);   // drop the leading '/', keep "Samples/…"
}

inline std::string to_lower_ascii(std::string s) {
    for (char& c : s) if (c >= 'A' && c <= 'Z') c = static_cast<char>(c + 32);
    return s;
}

// Resolve a path whose stored CASE may not match the disk (Android storage is case-insensitive; a
// Linux SD card is not). Walks from the longest existing prefix, taking a case-insensitive match
// where the exact child is missing. Returns the ORIGINAL path when no chain exists, so the failure
// names what the project asked for. Returns at once when the exact path exists, so only a real miss
// ever lists a directory.
// A URI is returned untouched: case drift is a disk property, and a provider resolves its own names.
inline std::string resolve_case_insensitive(const std::string& path) {
    if (path.empty() || pt_path_is_uri(path.c_str())) return path;
    namespace fs = std::filesystem;
    std::error_code ec;
    if (fs::exists(fs::path(path), ec)) return path;   // exact hit — the common case

    // Split by hand rather than via fs::path: device roots like "//mnt/SDCARD/…" start with "//",
    // which fs::path treats in an implementation-defined way.
    const bool absolute = path[0] == '/' || path[0] == '\\';
    std::vector<std::string> parts;
    std::string cur;
    for (const char c : path) {
        if (c == '/' || c == '\\') { if (!cur.empty()) { parts.push_back(cur); cur.clear(); } }
        else                       { cur.push_back(c); }
    }
    if (!cur.empty()) parts.push_back(cur);

    fs::path have = absolute ? fs::path("/") : fs::path(".");
    size_t start = 0;
    if (!absolute && !parts.empty() && parts[0].size() == 2 && parts[0][1] == ':') {
        have = fs::path(parts[0] + "/");   // a Windows drive ("C:") anchors the walk
        start = 1;
    }
    if (!fs::exists(have, ec)) return path;   // nothing to anchor the walk on

    for (size_t i = start; i < parts.size(); ++i) {
        const fs::path exact = have / parts[i];
        if (fs::exists(exact, ec)) { have = exact; continue; }
        bool matched = false;
        if (fs::is_directory(have, ec)) {
            const std::string target = to_lower_ascii(parts[i]);
            for (const auto& entry : fs::directory_iterator(have, ec)) {
                if (to_lower_ascii(entry.path().filename().string()) == target) {
                    have = entry.path();
                    matched = true;
                    break;
                }
            }
        }
        if (!matched) return path;   // give up; let the failure name the intended path
    }
    return have.string();
}

// True for a path that names a place on its own. ⚠️ A `pt://` URI is absolute: read as relative, it
// would be joined onto a base dir into a string that resolves nowhere.
inline bool path_is_absolute(const std::string& path) {
    if (path.empty()) return false;
    return path[0] == '/' || path[0] == '\\' ||
           (path.size() > 1 && path[1] == ':') ||   // C:\… on Windows
           pt_path_is_uri(path.c_str());
}

// Absolute wins; relative resolves against base_dir (portable projects and the test data store
// relative paths). No <filesystem>: it needs a separate link library on some toolchains.
//
// ⚠️ A project MOVED between installs holds absolute paths under the authoring install's root. When
// such a path does not exist here, its app-root-relative tail is re-rooted onto `app_root`. A sample
// kept outside the app tree is left as written, so the failure names the real path. The stored string
// is never rewritten, so a re-save stays portable.
// `app_root` empty skips the re-rooting entirely — the host tools' default.
inline std::string resolve_media_path(const std::string& path, const std::string& base_dir,
                                      const std::string& app_root) {
    if (path.empty()) return path;
    const bool absolute = path_is_absolute(path);

    std::string resolved = (!absolute && !base_dir.empty()) ? base_dir + "/" + path : path;

    // Absolute wins — unless it points nowhere here and has an app-tree tail.
    if (absolute && !app_root.empty() && !path_exists(resolved)) {
        const std::string tail = app_root_relative_tail(resolved);
        if (!tail.empty()) resolved = app_root + "/" + tail;
    }

    // Last: fix any case drift. A no-op when the path exists exactly.
    return resolve_case_insensitive(resolved);
}

// The two folders a stored media path resolves against. Both empty ⇒ paths are used as written.
struct MediaRoots {
    std::string baseDir;   // the project file's folder — a relative path joins onto it
    std::string appRoot;   // this install's app folder — where a foreign absolute path is re-rooted
};

inline std::string resolve_media_path(const std::string& path, const MediaRoots& roots) {
    return resolve_media_path(path, roots.baseDir, roots.appRoot);
}

}  // namespace songcore

#endif  // POCKETTRACKER_SONGCORE_MEDIA_PATH_H
