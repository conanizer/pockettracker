#include "ui/std_filesystem.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <system_error>

#if defined(_WIN32)
// The ONLY platform header in pt-ui: "where is Documents" has no portable answer. NOMINMAX because
// <windows.h> defines `min`/`max` as macros.
#define NOMINMAX
#include <windows.h>
#include <shlobj.h>
#endif

namespace fs = std::filesystem;

namespace pt::ui {

namespace {

/**
 * `file_time_type` → milliseconds since the Unix epoch. C++17 cannot name that clock's epoch, so the
 * offset between the two clocks is measured now — exact to nanoseconds, plenty for a sort key and a
 * `dd-MM-yy` label.
 */
int64_t to_unix_millis(fs::file_time_type t) {
    using namespace std::chrono;
    const auto shifted = time_point_cast<system_clock::duration>(
        t - fs::file_time_type::clock::now() + system_clock::now());
    return duration_cast<milliseconds>(shifted.time_since_epoch()).count();
}

/** Forward slashes always — the browser DRAWS the path. */
std::string generic(const fs::path& p) { return p.generic_string(); }

const char* env_or_null(const char* key) {
    const char* v = std::getenv(key);
    return (v && *v) ? v : nullptr;
}

#if defined(_WIN32)

/**
 * UTF-16 → the Windows NARROW encoding.
 * ⚠️ `CP_ACP`, NOT `CP_UTF8`: everything downstream (`fs::path(std::string)`, `std::ifstream`) decodes
 * narrow strings with the ACTIVE code page on MSVC, and UTF-8 bytes on a legacy-ACP machine make "José"'s
 * paths silently not exist. An `ActiveCodePage=UTF-8` app manifest makes the ACP UTF-8 and this line
 * follows — that manifest is the real fix for names the legacy ACP cannot represent.
 */
std::string narrow(const wchar_t* wide) {
    if (!wide || !*wide) return {};
    const int n = ::WideCharToMultiByte(CP_ACP, 0, wide, -1, nullptr, 0, nullptr, nullptr);
    if (n <= 1) return {};                       // 1 == the terminator alone: nothing converted
    std::string out(static_cast<size_t>(n - 1), '\0');
    ::WideCharToMultiByte(CP_ACP, 0, wide, -1, out.data(), n, nullptr, nullptr);
    return out;
}

/**
 * `Documents` as EXPLORER resolves it; empty if Windows will not say.
 * ⚠️ Not `%USERPROFILE%\Documents`: with OneDrive folder backup (the consumer default) Explorer's
 * Documents is `…\OneDrive\Documents`, and the other is an empty leftover the user never opens.
 */
std::string documents_directory() {
    PWSTR         wide = nullptr;
    const HRESULT hr   = ::SHGetKnownFolderPath(FOLDERID_Documents, KF_FLAG_CREATE, nullptr, &wide);
    std::string   out;
    if (SUCCEEDED(hr)) out = narrow(wide);
    ::CoTaskMemFree(wide);   // required even on failure (where it is handed nullptr)
    return out;
}

#endif  // _WIN32

}  // namespace

// ─── Path helpers ────────────────────────────────────────────────────────────────────────────────

std::string path_name(const std::string& path) {
    const size_t slash = path.find_last_of("/\\");
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

std::string path_extension(const std::string& path) {
    const std::string name = path_name(path);
    const size_t      dot  = name.find_last_of('.');
    return dot == std::string::npos ? std::string() : name.substr(dot + 1);
}

std::string path_stem(const std::string& path) {
    const std::string name = path_name(path);
    const size_t      dot  = name.find_last_of('.');
    return dot == std::string::npos ? name : name.substr(0, dot);
}

std::string path_sanitize(const std::string& name, bool allow_dot) {
    std::string out;
    out.reserve(name.size());
    for (char c : name) {
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                        c == '_' || c == '-' || (allow_dot && c == '.');
        out.push_back(ok ? c : '_');
    }
    return out;
}

std::string to_lower(std::string s) {
    for (char& c : s)
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c + 32);
    return s;
}

std::string default_app_root() {
    // An explicit override wins everywhere — a PortMaster launch script points it at the SD card.
    if (const char* home = env_or_null("POCKETTRACKER_HOME")) return home;

#if defined(_WIN32)
    // ⚠️ BEFORE the XDG/HOME chain: MSYS2, Git Bash and Cygwin export HOME on Windows, and the same
    // binary must find one root however it was started.
    // Documents, not %APPDATA%: projects, samples and renders are the USER's documents, findable from
    // Explorer.
    if (const std::string docs = documents_directory(); !docs.empty())
        return generic(fs::path(docs) / "PocketTracker");
    if (const char* profile = env_or_null("USERPROFILE"))
        return generic(fs::path(profile) / "Documents" / "PocketTracker");
#elif defined(__APPLE__)
    // The same argument as Windows: ~/Documents.
    if (const char* home = env_or_null("HOME")) return std::string(home) + "/Documents/PocketTracker";
#else
    if (const char* xdg = env_or_null("XDG_DATA_HOME")) return std::string(xdg) + "/PocketTracker";
    if (const char* home = env_or_null("HOME"))
        return std::string(home) + "/.local/share/PocketTracker";
#endif

    // Every branch missed (no HOME, no Documents). Relative to the CWD is a poor root — it scatters an
    // app tree wherever the binary ran — but a WRITABLE one; the alternative cannot save at all.
    return "PocketTracker";
}

// ─── StdFileSystem ───────────────────────────────────────────────────────────────────────────────

std::string StdFileSystem::ensure_dir(const char* sub) {
    const fs::path dir = fs::path(root_) / sub;
    std::error_code ec;
    fs::create_directories(dir, ec);   // already-exists is not an error; a failure leaves it absent
    return generic(dir);               // …and the browser then shows an empty listing, which is true
}

bool StdFileSystem::read_file(const std::string& path, std::string& out) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    out.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    return true;
}

std::vector<FileInfo> StdFileSystem::list_files(const std::string& directory) {
    std::vector<FileInfo> out;
    std::error_code ec;

    // The non-throwing overloads throughout: an unreadable directory or an entry vanishing mid-walk (an
    // SD card pulled while browsing) yields a short listing, never an exception across a UI frame.
    fs::directory_iterator it(directory, ec);
    if (ec) return out;

    for (const fs::directory_entry& e : it) {
        std::error_code ec2;
        const bool dir = e.is_directory(ec2);
        if (ec2) continue;

        FileInfo info;
        info.path        = generic(e.path());
        info.name        = path_name(info.path);
        info.extension   = dir ? std::string() : path_extension(info.name);
        info.isDirectory = dir;
        info.size        = dir ? 0 : static_cast<int64_t>(fs::file_size(e.path(), ec2));
        if (ec2) info.size = 0;
        info.lastModified = to_unix_millis(fs::last_write_time(e.path(), ec2));
        if (ec2) info.lastModified = 0;

        out.push_back(std::move(info));
    }
    return out;
}

bool StdFileSystem::file_exists(const std::string& path) {
    std::error_code ec;
    return fs::exists(path, ec) && !ec;
}

bool StdFileSystem::is_directory(const std::string& path) {
    std::error_code ec;
    return fs::is_directory(path, ec) && !ec;
}

std::string StdFileSystem::parent_path(const std::string& path) {
    const fs::path p      = fs::path(path);
    const fs::path parent = p.parent_path();
    // A filesystem root is its own parent — report "no parent" rather than loop ".." onto itself.
    if (parent.empty() || parent == p) return "";
    return generic(parent);
}

bool StdFileSystem::write_file(const std::string& path, const std::string& content) {
    return write_bytes(path, content.data(), content.size());
}

bool StdFileSystem::write_bytes(const std::string& path, const void* data, size_t len) {
    std::error_code ec;
    fs::create_directories(fs::path(path).parent_path(), ec);   // a save into a folder that is not there

    // Write `<path>.tmp` and rename over the target: a handheld switched off mid-save must not lose the
    // whole project to a half-written one.
    const std::string tmp = path + ".tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f) return false;
        if (len > 0) f.write(static_cast<const char*>(data), static_cast<std::streamsize>(len));

        // ⚠️ CLOSED HERE AND CHECKED — never left to the destructor. A small payload first reaches the disk
        // in this flush, so a FULL DISK (ENOSPC) surfaces here; `~ofstream` would swallow it, and the
        // rename would put an empty file where the project was while returning true. Dropping the temp
        // also frees space for the next attempt.
        f.close();
        if (!f) {
            std::error_code ignored;
            fs::remove(tmp, ignored);
            return false;
        }
    }

    fs::rename(tmp, path, ec);
    if (ec) {
        // rename(2) fails across filesystems: copy, then drop the temp.
        ec.clear();
        fs::copy_file(tmp, path, fs::copy_options::overwrite_existing, ec);
        std::error_code ignored;
        fs::remove(tmp, ignored);
        if (ec) return false;
    }
    return true;
}

bool StdFileSystem::delete_path(const std::string& path) {
    std::error_code ec;
    // remove_all covers a file and a whole tree.
    return fs::remove_all(path, ec) > 0 && !ec;
}

bool StdFileSystem::rename_file(const std::string& path, const std::string& new_base_name) {
    std::error_code ec;
    const bool dir = is_directory(path);
    const std::string ext = dir ? std::string() : path_extension(path);

    std::string safe = path_sanitize(new_base_name, /*allow_dot=*/true);
    if (safe.empty()) return false;

    // Keep the original extension, unless the typed name already ends in it.
    std::string final_name = safe;
    if (!ext.empty()) {
        const std::string suffix = "." + ext;
        const bool has_suffix = safe.size() > suffix.size() &&
                                safe.compare(safe.size() - suffix.size(), suffix.size(), suffix) == 0;
        if (!has_suffix) final_name = safe + suffix;
    }

    const fs::path target = fs::path(path).parent_path() / final_name;
    if (fs::exists(target, ec)) return false;   // never clobber — the browser reports the failure

    ec.clear();
    fs::rename(path, target, ec);
    return !ec;
}

std::string StdFileSystem::create_folder(const std::string& parent, const std::string& folder_name) {
    const std::string safe = path_sanitize(folder_name, /*allow_dot=*/false);
    if (safe.empty()) return "";

    const fs::path target = fs::path(parent) / safe;
    std::error_code ec;
    if (fs::exists(target, ec)) return "";

    ec.clear();
    if (!fs::create_directories(target, ec) || ec) return "";
    return generic(target);
}

bool StdFileSystem::move_file(const std::string& from, const std::string& to) {
    std::error_code ec;
    fs::create_directories(fs::path(to).parent_path(), ec);

    ec.clear();
    fs::rename(from, to, ec);
    if (!ec) return true;

    // Cross-filesystem (SD card → internal): copy the whole thing, then drop the source.
    ec.clear();
    fs::copy(from, to, fs::copy_options::recursive, ec);
    if (ec) return false;

    std::error_code ignored;
    fs::remove_all(from, ignored);
    return true;
}

bool StdFileSystem::copy_file(const std::string& from, const std::string& to) {
    std::error_code ec;
    fs::create_directories(fs::path(to).parent_path(), ec);

    ec.clear();
    // `recursive` with no overwrite: a folder copies as a folder, and an existing target FAILS rather
    // than merging. The caller has de-duplicated the name (`_2`, `_3`, …).
    fs::copy(from, to, fs::copy_options::recursive, ec);
    return !ec;
}

}  // namespace pt::ui
