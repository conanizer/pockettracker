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
// The ONLY platform header in pt-ui, and it buys exactly one thing: the answer to "where is Documents",
// which no portable API can give (see documents_directory below). NOMINMAX because <windows.h> defines
// `min`/`max` as macros and this file uses std::min.
#define NOMINMAX
#include <windows.h>
#include <shlobj.h>
#endif

namespace fs = std::filesystem;

namespace pt::ui {

namespace {

/**
 * `file_time_type` → milliseconds since the Unix epoch.
 *
 * C++17 gives no way to ask what `file_time_type`'s epoch IS (that arrives with C++20's `clock_cast`),
 * so the standard workaround is to measure the offset between the two clocks *now* and shift by it.
 * It is exact to within the time the two `now()` calls take to run, which is nanoseconds — and this
 * value is used for a SORT KEY and a `dd-MM-yy` label, both of which are indifferent to that.
 */
int64_t to_unix_millis(fs::file_time_type t) {
    using namespace std::chrono;
    const auto shifted = time_point_cast<system_clock::duration>(
        t - fs::file_time_type::clock::now() + system_clock::now());
    return duration_cast<milliseconds>(shifted.time_since_epoch()).count();
}

/** Forward slashes, always — Kotlin builds every path with them and the browser DRAWS the path. */
std::string generic(const fs::path& p) { return p.generic_string(); }

/** `[^a-zA-Z0-9_-.]` → '_', which is AndroidFileSystem.renameFile's rule. `allow_dot` is its
 *  createFolder rule, which does NOT permit one (a folder named "a.b" would read as a file). */
std::string sanitize(const std::string& name, bool allow_dot) {
    std::string out;
    out.reserve(name.size());
    for (char c : name) {
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                        c == '_' || c == '-' || (allow_dot && c == '.');
        out.push_back(ok ? c : '_');
    }
    return out;
}

const char* env_or_null(const char* key) {
    const char* v = std::getenv(key);
    return (v && *v) ? v : nullptr;
}

#if defined(_WIN32)

/**
 * UTF-16 → the Windows NARROW encoding, which is the one thing on this path that must not be guessed.
 *
 * ⚠️ `CP_ACP`, deliberately, NOT `CP_UTF8`. Everything downstream of this string takes it as narrow
 * `char`: `fs::path(std::string)` and `std::ifstream(const char*)` both decode with the process's ACTIVE
 * code page on MSVC. Hand them UTF-8 bytes on a machine whose ACP is still legacy and a user called
 * "José" gets a path that does not exist — silently, because every one of those calls reports failure by
 * returning an empty listing.
 *
 * CP_ACP is also the choice that stays right AFTER the fix: an app manifest with `ActiveCodePage=UTF-8`
 * (Windows 10 1903+) makes the ACP *be* UTF-8, and this line then produces UTF-8 without being touched.
 * That manifest is the real repair for names the legacy ACP cannot represent at all (Cyrillic on a
 * Western-European box, where the conversion below substitutes '?'), and it belongs with the packaging —
 * it is A3 on the convergence plan, not this function.
 */
std::string narrow(const wchar_t* wide) {
    if (!wide || !*wide) return {};
    const int n = ::WideCharToMultiByte(CP_ACP, 0, wide, -1, nullptr, 0, nullptr, nullptr);
    if (n <= 1) return {};                       // 1 == the terminator alone, i.e. it converted to nothing
    std::string out(static_cast<size_t>(n - 1), '\0');
    ::WideCharToMultiByte(CP_ACP, 0, wide, -1, out.data(), n, nullptr, nullptr);
    return out;
}

/**
 * `Documents`, as EXPLORER resolves it. Empty if Windows will not say.
 *
 * ⚠️ Not `%USERPROFILE%\Documents`, and the difference is not pedantry — it is most Windows 11 machines.
 * With OneDrive's "back up your folders" on (the default on a consumer install), Explorer's Documents is
 * `…\OneDrive\Documents`, while `%USERPROFILE%\Documents` is a leftover empty directory beside it. Guess
 * the second and the app writes songs into a folder its owner is not looking at — which is the exact
 * failure the Documents decision was taken to avoid.
 */
std::string documents_directory() {
    PWSTR         wide = nullptr;
    const HRESULT hr   = ::SHGetKnownFolderPath(FOLDERID_Documents, KF_FLAG_CREATE, nullptr, &wide);
    std::string   out;
    if (SUCCEEDED(hr)) out = narrow(wide);
    ::CoTaskMemFree(wide);   // documented as required even on failure, where it is handed nullptr
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

std::string to_lower(std::string s) {
    for (char& c : s)
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c + 32);
    return s;
}

std::string default_app_root() {
    // An explicit override wins on every platform, and on the handheld it is the only answer that ever
    // runs: a PortMaster launch script exports it to point the app at the SD card's ports directory.
    if (const char* home = env_or_null("POCKETTRACKER_HOME")) return home;

#if defined(_WIN32)
    // ⚠️ BEFORE the XDG/HOME chain rather than after it, and that ordering is most of the point of this
    // branch. MSYS2, Git Bash and Cygwin all export HOME on Windows, so the fall-through would send a
    // shell launched from any of those to `C:/Users/<name>/.local/share/PocketTracker` — a directory no
    // Windows user would think to look in — while the same binary double-clicked from Explorer answered
    // differently. One binary, one root, whatever started it.
    //
    // Documents, NOT %APPDATA%: projects, samples and renders are the USER's documents, which is the
    // same argument that put Android's under /Documents/PocketTracker. A song saved into AppData is a
    // song its owner cannot find from Explorer.
    if (const std::string docs = documents_directory(); !docs.empty())
        return generic(fs::path(docs) / "PocketTracker");
    if (const char* profile = env_or_null("USERPROFILE"))
        return generic(fs::path(profile) / "Documents" / "PocketTracker");
#elif defined(__APPLE__)
    // The same argument as Windows, so the same answer: ~/Documents, not ~/Library/Application Support.
    if (const char* home = env_or_null("HOME")) return std::string(home) + "/Documents/PocketTracker";
#else
    if (const char* xdg = env_or_null("XDG_DATA_HOME")) return std::string(xdg) + "/PocketTracker";
    if (const char* home = env_or_null("HOME"))
        return std::string(home) + "/.local/share/PocketTracker";
#endif

    // Every branch above missed — a stripped-down CFW with no HOME, a Windows install that will not name
    // its own Documents folder. Relative to the CWD is a poor root (it scatters an app tree wherever the
    // binary was run from, which is how a `PocketTracker/` folder once appeared inside the source
    // checkout) but it is a WRITABLE one, and the alternative is an app that cannot save at all.
    return "PocketTracker";
}

// ─── StdFileSystem ───────────────────────────────────────────────────────────────────────────────

std::string StdFileSystem::ensure_dir(const char* sub) {
    const fs::path dir = fs::path(root_) / sub;
    std::error_code ec;
    fs::create_directories(dir, ec);   // already-exists is not an error; a failure leaves it absent
    return generic(dir);               // …and the browser then reports an empty listing, which is true
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

    // The non-throwing overloads throughout: an unreadable directory, or an entry that vanishes
    // between the readdir and the stat, must yield a short listing — never an exception across a UI
    // frame. (An SD card pulled mid-browse does exactly this.)
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
    // A filesystem root is its own parent — report "no parent" rather than looping the browser's ".."
    // entry back onto the directory it is already showing.
    if (parent.empty() || parent == p) return "";
    return generic(parent);
}

bool StdFileSystem::write_file(const std::string& path, const std::string& content) {
    return write_bytes(path, content.data(), content.size());
}

bool StdFileSystem::write_bytes(const std::string& path, const void* data, size_t len) {
    std::error_code ec;
    fs::create_directories(fs::path(path).parent_path(), ec);   // a save into a folder that is not there

    // Write to `<path>.tmp` and rename over the target, which is AndroidFileSystem.writeFile's dance.
    // A handheld is a device people switch off with a hardware slider; a half-written project must not
    // be able to replace a whole one.
    const std::string tmp = path + ".tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f) return false;
        if (len > 0) f.write(static_cast<const char*>(data), static_cast<std::streamsize>(len));
        if (!f) return false;
    }

    fs::rename(tmp, path, ec);
    if (ec) {
        // rename(2) fails across filesystems. Copy, then drop the temp — the same fallback Android has.
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
    // remove_all covers both a file and a whole tree (`deleteFileOrFolder`), and returns the count.
    return fs::remove_all(path, ec) > 0 && !ec;
}

bool StdFileSystem::rename_file(const std::string& path, const std::string& new_base_name) {
    std::error_code ec;
    const bool dir = is_directory(path);
    const std::string ext = dir ? std::string() : path_extension(path);

    std::string safe = sanitize(new_base_name, /*allow_dot=*/true);
    if (safe.empty()) return false;

    // Kotlin keeps the original extension, unless the typed name already ends in it.
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
    const std::string safe = sanitize(folder_name, /*allow_dot=*/false);
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
    // `recursive` and no overwrite option: a folder copies as a folder, and an existing target FAILS
    // rather than being merged into. The caller has already de-duplicated the name (`_2`, `_3`, …).
    fs::copy(from, to, fs::copy_options::recursive, ec);
    return !ec;
}

}  // namespace pt::ui
