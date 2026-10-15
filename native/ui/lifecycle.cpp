#include "ui/lifecycle.h"

#include "songcore/project_io.h"   // serialize_project

namespace pt::ui {

bool autosave_exists(FileSystem& fs) {
    return fs.file_exists(fs.autosave_file_path());
}

bool autosave_write(const songcore::SongcoreHost& host, FileSystem& fs) {
    return fs.write_file(fs.autosave_file_path(), songcore::serialize_project(host.project()));
}

bool autosave_clear(FileSystem& fs) {
    const std::string path = fs.autosave_file_path();
    return fs.file_exists(path) ? fs.delete_path(path) : true;
}

bool autosave_load(songcore::SongcoreHost& host, FileSystem& fs, const std::string& mediaBaseDir) {
    const std::string path = fs.autosave_file_path();
    if (!fs.file_exists(path)) return false;

    // A mangled autosave fails the parse with the previous project intact.
    return host.load_project_file(path, mediaBaseDir);
}

}  // namespace pt::ui
