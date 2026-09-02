#pragma once

// ─── .pts — a scale, as a file ───────────────────────────────────────────────────────────────────
//
// What `ui/theme_io.h` is to a palette, this is to one of the project's sixteen scale slots: a small
// human-readable document a user can save, rename, hand to someone else, or drop onto an SD card.
//
// ⚠️ IT ALWAYS WRITES `enabled`, where every other writer in the tree omits a field equal to its
// default. A `.ptp` omits the scale pool precisely so that a project which never opened the SCALE
// screen stays byte-identical, and a `.ptt` omits a colour that matches CLASSIC — both are files whose
// subject is something else. A `.pts` has exactly one subject, and a Chromatic scale written under that
// rule would be an empty object: a file that says nothing about the only thing it is for. So the twelve
// degrees are unconditional here, and the deferred microtuning `offset` is not (it is written only once
// somebody has moved it, which is the same terms `project_io` writes it on).
//
// ⚠️ The reader is TOLERANT in the same way `parse_theme` is — an unknown key is ignored, a missing or
// wrong-typed one keeps the current value — and it fails only when the text is not a JSON object at
// all. That one case is what the caller reports as LOAD FAILED, rather than silently blanking a slot.
//
// ⚠️ A LOAD NEVER TOUCHES THE SLOT'S `id`. The id is *which of the sixteen this is*; the file is a
// shape. Copying the id out of a file would let a scale saved from slot 3 renumber slot 9 on the way in.

#include <cctype>
#include <string>
#include <vector>

#include "songcore/model.h"
#include "songcore/project_io.h"     // JsonWriter + JsonLayout, and the pool parser's tolerances
#include "songcore/scale_bank.h"
#include "ui/filesystem.h"
#include "vendor/nlohmann/json.hpp"

namespace pt::ui {

/** The extension, in one place — the browser's filter, the save path and the seed all read it. */
inline constexpr const char* SCALE_FILE_EXT = "pts";

/**
 * A scale → `.pts` bytes. Pretty-printed: this is a file a person may open.
 *
 * ⚠️ The array writer and the two readers below are `project_io`'s own (`songcore::detail`), reached
 * into deliberately rather than reimplemented here. How an int array and a tolerant key read are
 * SPELLED is one answer for this project's files; a second copy in this header is a second answer, and
 * the first divergence between them would be a `.pts` a `.ptp` reader could not agree with.
 */
inline std::string serialize_scale(const songcore::Scale& s) {
    songcore::JsonWriter w{songcore::JsonLayout::Pretty};
    w.begin_object();
    if (!s.name.empty()) w.field_string("name", s.name);
    songcore::detail::emit_int_array(w, "enabled", s.enabled);
    if (s.offset != std::vector<int>(12, 0))
        songcore::detail::emit_int_array(w, "offset", s.offset);
    w.end_object();
    return std::move(w.out);
}

/**
 * `.pts` bytes → the shape half of a scale. `out` keeps its `id`; everything else the file names is
 * replaced, and everything it does not name keeps what `out` already held.
 *
 * Returns false only when the text is not a JSON object.
 */
inline bool parse_scale_text(const std::string& text, songcore::Scale& out) {
    const nlohmann::json j = nlohmann::json::parse(text, nullptr, /*allow_exceptions=*/false);
    if (!j.is_object()) return false;

    songcore::Scale s = out;                     // the id, and any field the file is silent about
    s.name    = songcore::detail::get_str(j, "name", s.name);
    s.enabled = songcore::detail::parse_int_array(j, "enabled", s.enabled);
    s.offset  = songcore::detail::parse_int_array(j, "offset",  s.offset);

    // A hand-edited or truncated array must not leave a 12-degree consumer reading past its end — the
    // same resize `parse_scale` does inside a project, and for the same reason.
    s.enabled.resize(12, 1);
    s.offset.resize(12, 0);

    out = s;
    return true;
}

/** Write `scale` to `path`. */
inline bool save_scale_file(FileSystem& fs, const std::string& path, const songcore::Scale& scale) {
    return fs.write_file(path, serialize_scale(scale));
}

/** Read a scale from `path` into `out`, keeping `out.id`. */
inline bool load_scale_file(FileSystem& fs, const std::string& path, songcore::Scale& out) {
    std::string text;
    if (!fs.read_file(path, text)) return false;
    return parse_scale_text(text, out);
}

/**
 * A scale name, as a FILENAME — the sanitizer the theme save uses, on the same terms: anything outside
 * `[A-Za-z0-9_]` becomes `_`, so a name survives a FAT32 card. The SPACE in "Phrygian Dominant" is what
 * makes this matter for the factory bank, which is the one caller that hits it every time.
 *
 * ⚠️ It does not supply the empty fallback — the callers do, because `<Scales>/.pts` is a dotfile the
 * browser does not list, and a save that produces an invisible file is a save that reports success by
 * silence.
 */
inline std::string sanitize_scale_filename(const std::string& name) {
    std::string out;
    out.reserve(name.size());
    for (const char c : name) {
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                        (c >= '0' && c <= '9') || c == '_';
        out += ok ? c : '_';
    }
    return out;
}

/**
 * Write the compiled-in factory bank to the Scales folder, so the shapes exist as files that can be
 * edited, renamed and shared — the same job `seed_config_template` does for config.json.
 *
 * ⚠️ THE TEST IS "DOES THIS FOLDER HOLD ANY `.pts` AT ALL", not "is each file missing". A user who
 * deletes the thirty they never use has made a decision, and a per-file seed would undo it on every
 * launch; a user who has emptied the folder, or has never had one, gets the whole bank. Either way the
 * cycle on the SCALE screen is unaffected — that reads the compiled-in list and never these files.
 *
 * ⚠️ AND IT NEVER OVERWRITES. A name that collides with a file already present is skipped, so an edited
 * `Major.pts` is safe even in the empty-folder case (where it cannot be present) and, more to the
 * point, cannot be clobbered if this ever gains a second caller.
 *
 * Returns how many files it wrote — 0 when the folder already had scales in it.
 */
inline int seed_scale_bank(FileSystem& fs) {
    const std::string dir = fs.scales_directory();

    // ⚠️ Lower-cased before it is compared: `FileInfo::extension` is the case that is ON DISK, and a
    // card carrying `MAJOR.PTS` holds scales however the filesystem chose to spell it.
    for (const FileInfo& f : fs.list_files(dir)) {
        if (f.isDirectory) continue;
        std::string ext = f.extension;
        for (char& c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (ext == SCALE_FILE_EXT) return 0;
    }

    const std::vector<songcore::ScaleBankEntry>& bank = songcore::scale_bank();
    int written = 0;
    for (int i = 0; i < static_cast<int>(bank.size()); ++i) {
        const std::string safe = sanitize_scale_filename(bank[static_cast<size_t>(i)].name);
        const std::string path = dir + "/" + (safe.empty() ? std::string("SCALE") : safe) + ".pts";
        if (fs.file_exists(path)) continue;

        songcore::Scale s;
        songcore::scale_apply_bank(s, i);
        if (save_scale_file(fs, path, s)) ++written;
    }
    return written;
}

}  // namespace pt::ui
