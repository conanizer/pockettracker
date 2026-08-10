#ifndef POCKETTRACKER_SONGCORE_MEDIA_PATH_H
#define POCKETTRACKER_SONGCORE_MEDIA_PATH_H

// ─── Making a stored path mean something on THIS device ──────────────────────────────────────────
//
// A path written into a project — or into config.json — is a place named by a human, or by an earlier
// install, that has to resolve here. The three ways it can be wrong are all handled below: it may be
// RELATIVE to the project rather than to us, it may be ABSOLUTE under an app root this device does not
// have, and its CASE may not match a case-sensitive disk.
//
// ⚠️ **Two callers, one rule.** `resolve_media_path` is a project's sample/SF2 paths; `pt::ui::
// resolve_browse_dir` (ui/folder_config.h) is config.json's browse folders. Both re-root through
// `app_root_relative_tail`, and they must keep doing it through the SAME function: the day the two
// disagree is the day a project finds its samples in a folder the browser will not open.
//
// Nothing here links a library beyond <filesystem>, and nothing here knows what a Project is — which is
// why it is its own header rather than part of engine_setup.h, whose job is pushing a project into an
// engine.

#include <filesystem>   // resolve_case_insensitive — Android storage is case-insensitive, the SD card is not
#include <cstdio>
#include <string>
#include <vector>

#include "../byte_source.h"  // pt_fopen — path_exists probes through the same opener as a real load

namespace songcore {

// A cheap "is this file actually here?" — an open probe, NOT <filesystem>, so it stays inside the
// no-extra-link-library rule the path helpers below keep. Used only to decide whether an absolute path
// needs relocating; a false negative (a file that exists but cannot be opened) at worst re-roots to the
// same-or-a-worse guess, and the load fails either way, so it costs nothing it did not already cost.
//
// ⚠️ **The probe goes through pt_fopen, and this is the site where getting that wrong is SILENT.**
// Every other opener reports a failure. This one only answers a question, and a "no" here sends
// resolve_media_path off to re-root a path that was already correct: the project loads, the
// instrument is empty, and nothing anywhere says so.
inline bool path_exists(const std::string& path) {
    if (path.empty()) return false;
    FILE* f = pt_fopen(path.c_str(), "rb");
    if (!f) return false;
    std::fclose(f);
    return true;
}

// The app-root-relative tail of an absolute media path authored under ANOTHER install — the portable
// part naming where UNDER the app root a file lives ("Samples/Pads/kick.wav"), with the foreign root
// stripped. Empty when the path is under no recognisable app sub-tree (a sample the user kept elsewhere).
//
// ⭐ **A URI is deliberately NOT excluded, and anchor 2 is why.** Android's paths are `pt://<root-id>/…`
// where the id is derived from the granted tree URI — so re-granting the same folder, or moving the home
// root, leaves a project full of absolute paths that name a tree this device no longer has. Those still
// carry "/Samples/", so they re-root onto the current home exactly as a phone-authored plain path does.
// The anchor only ever runs on a path that has already failed to open (see resolve_media_path).
//
// ⚠️ Anchor 2's list is the MEDIA sub-trees, not all seven app folders, because its other caller is a
// sample path. A browse folder under a foreign root NOT called PocketTracker — ".../data/Themes" — is
// therefore not re-rooted, and falls back to the category default rather than to a wrong guess.
inline std::string app_root_relative_tail(const std::string& path) {
    // 1) An Android phone hard-codes its root to ".../PocketTracker" (AndroidFileSystem), so everything
    //    after the LAST "/PocketTracker/" is exactly the tail. This is the case a user copying a project
    //    off a phone hits, and the anchor the user themselves named. rfind, so a user sub-folder that
    //    happens to be called "PocketTracker" loses to the real root above it.
    static const std::string kPtAnchor = "/PocketTracker/";
    const size_t pt = path.rfind(kPtAnchor);
    if (pt != std::string::npos) return path.substr(pt + kPtAnchor.size());

    // 2) A root NOT named PocketTracker (another handheld whose $POCKETTRACKER_HOME is e.g. ".../data")
    //    has no such anchor — fall back to the media sub-trees themselves, keeping the sub-dir IN the
    //    tail so it re-roots whole ("Samples/x.wav" onto <root> → <root>/Samples/x.wav).
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

// Resolve a path whose stored CASE may not match the disk. A project authored on Android references
// "Samples/Breaks/x.wav", but Android storage is case-INsensitive (FAT/sdcardfs) while a Linux SD card is
// case-SENSITIVE — so the real file is "Samples/breaks/x.wav" and the exact path is dead. Walk the path a
// component at a time from the longest existing prefix; where an exact child is missing, take the entry
// whose name matches case-insensitively. Returns the ORIGINAL path when no such chain exists, so a
// genuinely-absent file (a sample the user never copied) still fails naming what the project asked for.
//
// ⭐ It returns IMMEDIATELY when the exact path exists — which is every host-tool/golden case (their paths
// match the disk exactly) — so nothing there moves, and the only directory listing ever done is on a real
// miss. This is the one place songcore reaches for <filesystem>.
//
// ⚠️ **A URI IS NOT A `std::filesystem::path` AND IS RETURNED UNTOUCHED.** `fs::exists("pt://…")` is
// false, so without this the walk below would split the URI on its own slashes, fail to find "pt:" in the
// process's current directory, and return the path anyway — the right answer reached by listing a
// directory that has nothing to do with the file. Case drift is a disk property; a document provider
// resolves its own names.
inline std::string resolve_case_insensitive(const std::string& path) {
    if (path.empty() || pt_path_is_uri(path.c_str())) return path;
    namespace fs = std::filesystem;
    std::error_code ec;
    if (fs::exists(fs::path(path), ec)) return path;   // exact hit — the overwhelmingly common case

    // Split on both separators OURSELVES rather than via fs::path's relative_path(): the real device app
    // root is "//mnt/SDCARD/Roms/PORTS//ports/…" and fs::path treats a leading "//" in an
    // implementation-defined way. Dropping empty components collapses every redundant slash cleanly.
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
        have = fs::path(parts[0] + "/");   // a Windows drive ("C:") anchors the walk (dev box)
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

// True for a path that names a place on its own rather than relative to something — the test both
// resolvers make, in one place so they cannot drift apart.
//
// ⚠️ **A URI IS ABSOLUTE**, and none of the three character tests says so: `pt://…` starts with a letter
// and its second character is not ':'. Missed, it reads as RELATIVE and is joined onto a base dir —
// `pt://home/pt://other/Samples/kick.wav`, a string that resolves nowhere, from a project that named its
// sample perfectly well.
inline bool path_is_absolute(const std::string& path) {
    if (path.empty()) return false;
    return path[0] == '/' || path[0] == '\\' ||
           (path.size() > 1 && path[1] == ':') ||   // C:\… on Windows
           pt_path_is_uri(path.c_str());
}

// Project media paths are absolute on device, but a portable project (the /tools/testdata goldens, anything
// the Linux build ships) stores them RELATIVE to the project file. Absolute wins; relative resolves
// against base_dir. Deliberately not <filesystem>: it drags in a separate link library on some
// toolchains, for a job that is one string concat.
//
// ⚠️ THE ONE EXCEPTION — a project MOVED between installs. Its paths were written absolute under the
// AUTHORING install's root (an Android phone's ".../Documents/PocketTracker/", another handheld's own
// $POCKETTRACKER_HOME); copied onto THIS device they point nowhere. `app_root` — the folder THIS install
// keeps Samples/, Soundfonts/… directly under — is the anchor that fixes it: an absolute path that does
// not exist here but carries an app-root-relative tail is re-rooted onto our own root. Bounded to the app
// tree on purpose (app_root_relative_tail returns "" otherwise) — a sample parked OUTSIDE PocketTracker/
// is genuinely unfindable and is left as-authored so the failure names the real path. Resolution only:
// the Project's stored string is NOT rewritten, so a re-save stays portable back to the phone.
//
// ⭐ app_root EMPTY ⇒ the whole exception is skipped ⇒ byte-for-byte the old two-line behaviour. Every
// host TOOL leaves it empty (SongcoreHost::appRoot_ defaults to ""), which is why not one golden moves;
// only the SDL shell, which calls set_app_root() at boot, ever re-roots.
inline std::string resolve_media_path(const std::string& path, const std::string& base_dir,
                                      const std::string& app_root) {
    if (path.empty()) return path;
    const bool absolute = path_is_absolute(path);

    std::string resolved = (!absolute && !base_dir.empty()) ? base_dir + "/" + path : path;

    // Absolute wins — UNLESS it points nowhere here and was authored under another install's app tree.
    if (absolute && !app_root.empty() && !path_exists(resolved)) {
        const std::string tail = app_root_relative_tail(resolved);
        if (!tail.empty()) resolved = app_root + "/" + tail;
    }

    // Last: fix any case drift (an Android-authored "Breaks/" vs the card's "breaks/"). A no-op when the
    // path already exists exactly, so goldens/tools — whose paths match the disk — are byte-for-byte
    // unchanged; it only ever lists a directory on a real miss.
    return resolve_case_insensitive(resolved);
}

}  // namespace songcore

#endif  // POCKETTRACKER_SONGCORE_MEDIA_PATH_H
