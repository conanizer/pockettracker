#include "ui/lifecycle.h"

#include "songcore/project_io.h"   // serialize_project — the kotlinx-byte-exact emitter (S2)

namespace pt::ui {

bool autosave_exists(FileSystem& fs) {
    return fs.file_exists(fs.autosave_file_path());
}

bool autosave_write(const songcore::SongcoreHost& host, FileSystem& fs) {
    // ⚠️ `fs.write_file`, NOT `host.save_project_file` — the temp+rename is the point. See lifecycle.h.
    return fs.write_file(fs.autosave_file_path(), songcore::serialize_project(host.project()));
}

bool autosave_clear(FileSystem& fs) {
    const std::string path = fs.autosave_file_path();
    // Nothing to delete is not a failure — it is the post-condition, already true (Kotlin's
    // clearAutosave). Every caller is a clean transition saying "there is now nothing to recover".
    return fs.file_exists(path) ? fs.delete_path(path) : true;
}

bool autosave_load(songcore::SongcoreHost& host, FileSystem& fs, const std::string& mediaBaseDir) {
    const std::string path = fs.autosave_file_path();
    if (!fs.file_exists(path)) return false;

    // The guard rail S4 paid 84.4% of a render for: parse → push → load_media → push_params, in ONE
    // call, so a new caller cannot forget the two that come after the parse. This is a new caller.
    //
    // ⚠️ A half-written or hand-mangled autosave lands here as a parse failure and returns false with
    // the previous project INTACT (push_project's own contract: "a malformed blob must leave the
    // previous project intact"). The caller then drops the file rather than offering it again — an
    // autosave that cannot be read is not a recovery, it is a prompt that can never succeed.
    return host.load_project_file(path, mediaBaseDir);
}

}  // namespace pt::ui
