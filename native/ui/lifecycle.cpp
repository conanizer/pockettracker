#include "ui/lifecycle.h"

#include "songcore/project_io.h"   // serialize_project

namespace pt::ui {

bool autosave_exists(FileSystem& fs) {
    return fs.file_exists(fs.autosave_file_path());
}

bool autosave_write(const songcore::SongcoreHost& host, FileSystem& fs) {
    // ⚠️ `fs.write_file` — its temp+rename matters most for this file (lifecycle.h).
    return fs.write_file(fs.autosave_file_path(), songcore::serialize_project(host.project()));
}

bool autosave_clear(FileSystem& fs) {
    const std::string path = fs.autosave_file_path();
    // Nothing to delete is not a failure — "nothing to recover" already holds.
    return fs.file_exists(path) ? fs.delete_path(path) : true;
}

bool autosave_load(songcore::SongcoreHost& host, FileSystem& fs, const std::string& mediaBaseDir) {
    const std::string path = fs.autosave_file_path();
    if (!fs.file_exists(path)) return false;

    // parse → push → load_media → push_params in ONE call, so this caller cannot skip the last two.
    // ⚠️ A half-written or mangled autosave fails the parse with the previous project INTACT; the caller
    // then drops the file rather than offering a prompt that can never succeed.
    return host.load_project_file(path, mediaBaseDir);
}

}  // namespace pt::ui
