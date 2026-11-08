#ifndef POCKETTRACKER_SONGCORE_PATH_PARTS_H
#define POCKETTRACKER_SONGCORE_PATH_PARTS_H

// A path's parts, split at the last `/` or `\` and at the name's last `.` — for songcore and the UI
// alike. ⚠️ Not `std::filesystem::path`: here `.bashrc` has the extension "bashrc", and the browser
// filters on it.

#include <string>

namespace songcore {

inline std::string to_lower_ascii(std::string s) {
    for (char& c : s) if (c >= 'A' && c <= 'Z') c = static_cast<char>(c + 32);
    return s;
}

// "/a/b/c.wav" → "/a/b"; "c.wav" → "".
inline std::string path_dir(const std::string& path) {
    const size_t slash = path.find_last_of("/\\");
    return slash == std::string::npos ? std::string() : path.substr(0, slash);
}

// "/a/b/c.wav" → "c.wav".
inline std::string path_name(const std::string& path) {
    const size_t slash = path.find_last_of("/\\");
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

// "/a/b/c.wav" → "c".
inline std::string path_stem(const std::string& path) {
    const std::string name = path_name(path);
    const size_t      dot  = name.find_last_of('.');
    return dot == std::string::npos ? name : name.substr(0, dot);
}

// "c.wav" → "wav"; "README" → "".
inline std::string path_extension(const std::string& path) {
    const std::string name = path_name(path);
    const size_t      dot  = name.find_last_of('.');
    return dot == std::string::npos ? std::string() : name.substr(dot + 1);
}

}  // namespace songcore

#endif  // POCKETTRACKER_SONGCORE_PATH_PARTS_H
