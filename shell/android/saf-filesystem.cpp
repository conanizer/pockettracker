#include "android/saf-filesystem.h"

#include "android/android-jni.h"
#include "common/byte_source.h"

#include <SDL.h>

#include <android/log.h>
#include <jni.h>
#include <unistd.h>        // close() — the writers own the descriptor `safOpenFd` detached

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <iterator>

namespace ptshell {
namespace {

constexpr const char* kLogTag   = "PocketTrackerSDL";
constexpr const char* kScheme   = "pt://";
constexpr size_t      kSchemeLen = 5;
constexpr const char* kRootsPath = "pt://roots";

// The ACTION row in the roots directory. Cannot collide: the roots' children are `pt://<id>`.
constexpr const char* kAddRootPath = "pt://roots/add";

bool is_pt_path(const std::string& s) {
    return s.compare(0, kSchemeLen, kScheme) == 0;
}

// ─── the JNI seam ────────────────────────────────────────────────────────────────────────────────
//
// By name and exception-safe, like `android-main.cpp`: a method R8 renamed in a release build costs
// one log line and an empty answer, not a crash.

struct Env : Attached {
    jmethodID method(const char* name, const char* sig) const {
        const jmethodID m = Attached::method(name, sig);
        if (!m) {
            __android_log_print(ANDROID_LOG_WARN, kLogTag,
                                "saf: %s%s not found on the activity - R8 renamed it, or it is not built",
                                name, sig);
        }
        return m;
    }

    // A jstring the caller deletes.
    jstring str(const std::string& s) const { return env->NewStringUTF(s.c_str()); }
};

std::string call_string(const char* name, const char* sig, const std::string& a, const std::string* b) {
    Env e;
    if (!e.ok()) return std::string();
    jmethodID m = e.method(name, sig);
    if (!m) return std::string();
    jstring ja = e.str(a);
    jstring jb = b ? e.str(*b) : nullptr;
    jobject r  = b ? e.env->CallObjectMethod(e.activity, m, ja, jb)
                   : e.env->CallObjectMethod(e.activity, m, ja);
    const bool bad = e.failed();
    e.env->DeleteLocalRef(ja);
    if (jb) e.env->DeleteLocalRef(jb);
    if (bad) { if (r) e.env->DeleteLocalRef(r); return std::string(); }
    return e.take(r);
}

// A no-argument String method. ⚠️ Not `call_string` with "": passing an argument to a
// `()Ljava/lang/String;` signature is undefined behaviour.
std::string call_string0(const char* name) {
    Env e;
    if (!e.ok()) return std::string();
    jmethodID m = e.method(name, "()Ljava/lang/String;");
    if (!m) return std::string();
    jobject r = e.env->CallObjectMethod(e.activity, m);
    if (e.failed()) { if (r) e.env->DeleteLocalRef(r); return std::string(); }
    return e.take(r);
}

}  // namespace

// ─── construction, and the hooks ─────────────────────────────────────────────────────────────────

// ⚠️ The hooks take no user data, so the instance is reached through this pointer; a second
// construction is logged.
SafFileSystem* g_instance = nullptr;

SafFileSystem::SafFileSystem(std::string private_root) : priv_(private_root, private_root) {}

extern "C" int saf_open_hook_trampoline(const char* path, const char* mode) {
    if (!g_instance || !path || !mode) return -1;
    return g_instance->open_fd(path, mode);
}

extern "C" int saf_remove_hook_trampoline(const char* path) {
    if (!g_instance || !path) return -1;
    return g_instance->hook_remove(path);
}

extern "C" int saf_rename_hook_trampoline(const char* from, const char* to) {
    if (!g_instance || !from || !to) return -1;
    return g_instance->hook_rename(from, to);
}

void SafFileSystem::install_file_hooks() {
    if (g_instance && g_instance != this) {
        __android_log_print(ANDROID_LOG_ERROR, kLogTag,
                            "saf: a second SafFileSystem is taking the file hooks - the first one's "
                            "URIs will stop resolving");
    }
    g_instance = this;

    PtFileHooks hooks;
    hooks.open   = &saf_open_hook_trampoline;
    hooks.remove = &saf_remove_hook_trampoline;
    hooks.rename = &saf_rename_hook_trampoline;
    pt_set_file_hooks(hooks);
    std::printf("saf:     pt_fopen/pt_remove/pt_rename hooks installed\n");
}

// ─── roots ───────────────────────────────────────────────────────────────────────────────────────

const std::vector<SafFileSystem::Root>& SafFileSystem::roots(bool refresh) {
    if (rootsLoaded_ && !refresh) return roots_;
    rootsLoaded_ = true;
    roots_.clear();
    homeId_.clear();

    int count = 0;
    {
        Env e;
        if (e.ok()) {
            if (jmethodID m = e.method("safRootCount", "()I")) {
                count = e.env->CallIntMethod(e.activity, m);
                if (e.failed()) count = 0;
            }
        }
    }

    for (int i = 0; i < count; ++i) {
        Env e;
        if (!e.ok()) break;
        jmethodID m = e.method("safRootInfo", "(I)Ljava/lang/String;");
        if (!m) break;
        jobject r = e.env->CallObjectMethod(e.activity, m, static_cast<jint>(i));
        if (e.failed()) { if (r) e.env->DeleteLocalRef(r); continue; }
        const std::string info = e.take(r);

        // `<id>\t<name>\t<docUri>\t<live>`; a row missing any field — the liveness flag included —
        // is skipped, never read as "dead".
        const size_t t1 = info.find('\t');
        if (t1 == std::string::npos) continue;
        const size_t t2 = info.find('\t', t1 + 1);
        if (t2 == std::string::npos) continue;
        const size_t t3 = info.find('\t', t2 + 1);
        if (t3 == std::string::npos) continue;
        roots_.push_back(Root{info.substr(0, t1), info.substr(t1 + 1, t2 - t1 - 1),
                              info.substr(t2 + 1, t3 - t2 - 1), info[t3 + 1] == '1'});
    }

    // ⚠️ Asked once per load, not per `ensure_dir`: it is a JNI round trip, and the app folders ask
    // on every browser open. The Kotlin side stamps its choice as it answers.
    homeId_ = call_string0("safHomeRootId");

    return roots_;
}

bool SafFileSystem::has_grant() { return !roots().empty(); }

int SafFileSystem::root_count() { return static_cast<int>(roots().size()); }

std::string SafFileSystem::home_root_path() {
    const auto& r = roots();
    // ⚠️ `pt://roots`, not "", when nothing is granted: "" opens an empty browser with no way out;
    // the roots directory offers ADD FOLDER…. Every mutation refuses the roots path.
    if (r.empty()) return kRootsPath;

    // The stored home, if still LIVE — a grant survives its folder, and a dead home would answer ""
    // for ever.
    if (!homeId_.empty()) {
        for (const Root& root : r)
            if (root.id == homeId_ && root.live) return std::string(kScheme) + root.id;
    }

    // Else the lowest live id.
    for (const Root& root : r) {
        if (!root.live) continue;
        if (!homeId_.empty())
            __android_log_print(ANDROID_LOG_WARN, kLogTag,
                                "saf: home root '%s' is gone or unreadable - falling back to '%s'",
                                homeId_.c_str(), root.id.c_str());
        return std::string(kScheme) + root.id;
    }

    // Granted, but none resolves (a card pulled, every folder deleted): the roots directory says what
    // is granted and offers ADD FOLDER….
    __android_log_print(ANDROID_LOG_ERROR, kLogTag,
                        "saf: %d granted folder(s) and NONE of them resolve - the documents are gone",
                        static_cast<int>(r.size()));
    return kRootsPath;
}

// ─── path arithmetic ─────────────────────────────────────────────────────────────────────────────

std::string SafFileSystem::parent_path(const std::string& path) {
    if (!is_pt_path(path)) return priv_.parent_path(path);
    if (path == kRootsPath) return std::string();          // the top; the browser's ".." stops here

    const size_t slash = path.rfind('/');
    // "pt://<id>": the last '/' is the scheme's, so the parent is the roots directory.
    if (slash == std::string::npos || slash <= kSchemeLen) return kRootsPath;
    return path.substr(0, slash);
}

// ─── resolution: a composable path → an opaque document URI ──────────────────────────────────────

std::string SafFileSystem::resolve(const std::string& path) {
    if (!is_pt_path(path) || path == kRootsPath) return std::string();

    auto cached = uriCache_.find(path);
    if (cached != uriCache_.end()) return cached->second;

    const std::string tail = path.substr(kSchemeLen);
    const size_t      cut  = tail.find('/');
    if (cut == std::string::npos) {                        // "pt://<id>" — a granted tree itself
        for (const Root& r : roots()) {
            if (r.id == tail) {
                uriCache_[path] = r.docUri;
                return r.docUri;
            }
        }
        return std::string();
    }

    // Deeper: list the parent, which caches every sibling's URI — 8 samples in one folder, one query.
    listing(parent_path(path));
    cached = uriCache_.find(path);
    return cached != uriCache_.end() ? cached->second : std::string();
}

const std::vector<pt::ui::FileInfo>* SafFileSystem::listing(const std::string& dirPath) {
    std::vector<pt::ui::FileInfo> out;

    // ⚠️ The roots directory is never cached (checked before the lookup): ADD FOLDER… adds a grant
    // while running. Others are cached until `forget`/`invalidate`; files from outside the app are
    // why the browser's refresh drops entries.
    if (dirPath == kRootsPath) {
        for (const Root& r : roots(/*refresh=*/true)) {
            pt::ui::FileInfo fi;
            fi.path        = std::string(kScheme) + r.id;
            fi.isDirectory = true;
            fi.isRoot      = true;   // ⚠️ not a folder: no rename, no delete, and SEL+A means SET HOME

            // Which tree is home, and which grant lost its folder: the name is the only place to say so.
            fi.name = r.name;
            if (!r.live)                 fi.name += " (MISSING)";
            else if (r.id == homeId_)    fi.name += " (HOME)";

            out.push_back(fi);
            uriCache_[fi.path] = r.docUri;
        }

        // The way out of the empty state: with nothing granted this is the only row.
        pt::ui::FileInfo add;
        add.path     = kAddRootPath;
        add.name     = "ADD FOLDER...";   // the 5x5 font has no ellipsis glyph; three dots is the glyph
        add.isAction = true;
        out.push_back(add);

        auto& slot = listCache_[dirPath];
        slot = std::move(out);
        return &slot;
    }

    auto hit = listCache_.find(dirPath);
    if (hit != listCache_.end()) return &hit->second;

    const std::string dirUri = resolve(dirPath);
    if (dirUri.empty()) return nullptr;

    const std::string blob = call_string("safListChildren", "(Ljava/lang/String;)Ljava/lang/String;",
                                         dirUri, nullptr);
    size_t pos = 0;
    while (pos < blob.size()) {
        size_t nl = blob.find('\n', pos);
        if (nl == std::string::npos) nl = blob.size();
        const std::string line = blob.substr(pos, nl - pos);
        pos = nl + 1;
        if (line.empty()) continue;

        // docUri \t name \t isDir \t size \t lastModified
        size_t f[4];
        f[0] = line.find('\t');                    if (f[0] == std::string::npos) continue;
        f[1] = line.find('\t', f[0] + 1);          if (f[1] == std::string::npos) continue;
        f[2] = line.find('\t', f[1] + 1);          if (f[2] == std::string::npos) continue;
        f[3] = line.find('\t', f[2] + 1);          if (f[3] == std::string::npos) continue;

        pt::ui::FileInfo fi;
        const std::string docUri = line.substr(0, f[0]);
        fi.name        = line.substr(f[0] + 1, f[1] - f[0] - 1);
        fi.isDirectory = line[f[1] + 1] == '1';
        fi.size        = std::strtoll(line.c_str() + f[2] + 1, nullptr, 10);
        fi.lastModified = std::strtoll(line.c_str() + f[3] + 1, nullptr, 10);
        fi.path        = dirPath + "/" + fi.name;
            // `File.extension` semantics, as `std_filesystem.h` — the browser filters on it.
        if (!fi.isDirectory) {
            const size_t dot = fi.name.rfind('.');
            if (dot != std::string::npos && dot + 1 < fi.name.size())
                fi.extension = fi.name.substr(dot + 1);
        }
        uriCache_[fi.path] = docUri;
        out.push_back(fi);
    }

    auto& slot = listCache_[dirPath];
    slot = std::move(out);
    return &slot;
}

void SafFileSystem::invalidate(const std::string& dirPath) {
    listCache_.erase(dirPath);
}

// ─── the seven app folders ───────────────────────────────────────────────────────────────────────

std::string SafFileSystem::ensure_dir(const char* sub) {
    const std::string home = home_root_path();
    // Nothing granted: every accessor answers the roots directory, where a folder can be added.
    if (home == kRootsPath) return home;

    std::string cur = home;
    const std::string subs(sub);
    size_t pos = 0;
    while (pos <= subs.size()) {
        size_t slash = subs.find('/', pos);
        if (slash == std::string::npos) slash = subs.size();
        const std::string seg = subs.substr(pos, slash - pos);
        pos = slash + 1;
        if (seg.empty()) break;

        const std::string want = cur + "/" + seg;
        if (!resolve(want).empty()) { cur = want; continue; }

        const std::string parentUri = resolve(cur);
        if (parentUri.empty()) return std::string();
        const std::string made = call_string("safCreateDir",
                                             "(Ljava/lang/String;Ljava/lang/String;)Ljava/lang/String;",
                                             parentUri, &seg);
        if (made.empty()) return std::string();
        uriCache_[want] = made;
        invalidate(cur);
        cur = want;
    }
    return cur;
}

std::string SafFileSystem::projects_directory()    { return ensure_dir("Projects"); }
std::string SafFileSystem::samples_directory()     { return ensure_dir("Samples"); }
std::string SafFileSystem::renders_directory()     { return ensure_dir("Renders"); }
std::string SafFileSystem::resampled_directory()   { return ensure_dir("Samples/Resampled"); }
std::string SafFileSystem::instruments_directory() { return ensure_dir("Instruments"); }
std::string SafFileSystem::soundfonts_directory()  { return ensure_dir("Soundfonts"); }
std::string SafFileSystem::themes_directory()      { return ensure_dir("Themes"); }
std::string SafFileSystem::scales_directory()      { return ensure_dir("Scales"); }
std::string SafFileSystem::grooves_directory()     { return ensure_dir("Grooves"); }
std::string SafFileSystem::reverbs_directory()     { return ensure_dir("Reverbs"); }
std::string SafFileSystem::delays_directory()      { return ensure_dir("Delays"); }

bool SafFileSystem::activate(const std::string& path) {
    if (path != kAddRootPath) return false;

    Env e;
    if (!e.ok()) return false;
    jmethodID m = e.method("safRequestRoot", "()Z");
    if (!m) return false;
    const bool launched = e.env->CallBooleanMethod(e.activity, m) == JNI_TRUE;
    if (e.failed()) return false;

    // ⚠️ `launched` = the picker is on screen, not that a folder was granted. Nothing waits for the
    // grant: the browser re-lists when the app comes back.
    std::printf("saf:     ADD FOLDER - picker %s\n", launched ? "launched" : "COULD NOT BE OPENED");
    return launched;
}

bool SafFileSystem::set_home_directory(const std::string& path) {
    // Only a granted tree can be home: not the roots, not a folder inside a tree.
    if (!is_granted_tree(path)) return false;

    const std::string id = path.substr(kSchemeLen);
    Env e;
    if (!e.ok()) return false;
    jmethodID m = e.method("safSetHomeRoot", "(Ljava/lang/String;)Z");
    if (!m) return false;
    jstring jid = e.str(id);
    const bool set = e.env->CallBooleanMethod(e.activity, m, jid) == JNI_TRUE;
    const bool bad = e.failed();
    e.env->DeleteLocalRef(jid);
    if (bad || !set) return false;

    // ⚠️ `homeId_` is read once per roots load; without this the accessors keep answering the old
    // tree until relaunch.
    roots(/*refresh=*/true);

    // The roots listing carries the "(HOME)" marker, which has just moved.
    invalidate(kRootsPath);

    std::printf("saf:     home folder is now %s\n", path.c_str());
    return true;
}

bool SafFileSystem::revoke_access(const std::string& path) {
    if (!is_granted_tree(path)) return false;

    const std::string id = path.substr(kSchemeLen);
    Env e;
    if (!e.ok()) return false;
    jmethodID m = e.method("safForgetRoot", "(Ljava/lang/String;)Z");
    if (!m) return false;
    jstring jid = e.str(id);
    const bool gone = e.env->CallBooleanMethod(e.activity, m, jid) == JNI_TRUE;
    const bool bad  = e.failed();
    e.env->DeleteLocalRef(jid);
    if (bad || !gone) return false;

    // ⚠️ Every cached URI under the tree is dead with the permission. `forget` also drops the roots
    // listing, since that is the tree's parent.
    forget(path);
    roots(/*refresh=*/true);   // re-read the grant list, and the home with it — Java may have cleared it
    invalidate(kRootsPath);

    std::printf("saf:     forgot %s (%d granted folder(s) left)\n", path.c_str(),
                static_cast<int>(roots_.size()));
    return true;
}

std::string SafFileSystem::config_path() {
    const std::string home = home_root_path();
    // ⚠️ "" with nothing granted — `load_folder_config` reads that as "no config", its common case.
    // Tested against the roots path, which is what `home_root_path` answers then.
    return home == kRootsPath ? std::string() : home + "/config.json";
}

// ─── reading ─────────────────────────────────────────────────────────────────────────────────────

bool SafFileSystem::read_file(const std::string& path, std::string& out) {
    if (!is_pt_path(path)) return priv_.read_file(path, out);

    const std::string uri = resolve(path);
    if (uri.empty()) return false;

    Env e;
    if (!e.ok()) return false;
    jmethodID m = e.method("safReadFile", "(Ljava/lang/String;)[B");
    if (!m) return false;
    jstring ju = e.str(uri);
    jobject r  = e.env->CallObjectMethod(e.activity, m, ju);
    const bool bad = e.failed();
    e.env->DeleteLocalRef(ju);
    if (bad || !r) { if (r) e.env->DeleteLocalRef(r); return false; }

    jbyteArray arr = static_cast<jbyteArray>(r);
    const jsize n  = e.env->GetArrayLength(arr);
    out.resize(static_cast<size_t>(n));
    if (n > 0) e.env->GetByteArrayRegion(arr, 0, n, reinterpret_cast<jbyte*>(&out[0]));
    e.env->DeleteLocalRef(r);
    return true;
}

std::vector<pt::ui::FileInfo> SafFileSystem::list_files(const std::string& directory) {
    if (!is_pt_path(directory)) return priv_.list_files(directory);
    const std::vector<pt::ui::FileInfo>* l = listing(directory);
    return l ? *l : std::vector<pt::ui::FileInfo>();
}

void SafFileSystem::forget_listing(const std::string& directory) {
    if (is_pt_path(directory)) forget(directory);
}

bool SafFileSystem::file_exists(const std::string& path) {
    if (!is_pt_path(path)) return priv_.file_exists(path);
    if (path == kRootsPath) return true;
    return !resolve(path).empty();
}

bool SafFileSystem::is_directory(const std::string& path) {
    if (!is_pt_path(path)) return priv_.is_directory(path);
    if (path == kRootsPath) return true;
    if (parent_path(path) == kRootsPath) return !resolve(path).empty();   // a granted tree

    const std::vector<pt::ui::FileInfo>* l = listing(parent_path(path));
    if (!l) return false;
    for (const pt::ui::FileInfo& fi : *l)
        if (fi.path == path) return fi.isDirectory;
    return false;
}

// ─── the pieces the write half is built from ─────────────────────────────────────────────────────

std::string SafFileSystem::leaf_name(const std::string& path) {
    if (!is_pt_path(path) || path == kRootsPath) return std::string();
    const size_t slash = path.rfind('/');
    if (slash == std::string::npos || slash <= kSchemeLen) return std::string();  // a bare tree
    return path.substr(slash + 1);
}

void SafFileSystem::forget(const std::string& path) {
    const std::string prefix = path + "/";
    for (auto it = uriCache_.begin(); it != uriCache_.end();)
        it = (it->first == path || it->first.compare(0, prefix.size(), prefix) == 0)
                 ? uriCache_.erase(it) : std::next(it);
    for (auto it = listCache_.begin(); it != listCache_.end();)
        it = (it->first == path || it->first.compare(0, prefix.size(), prefix) == 0)
                 ? listCache_.erase(it) : std::next(it);
    invalidate(parent_path(path));   // the parent's listing no longer describes its children
}

std::string SafFileSystem::ensure_file(const std::string& path) {
    const std::string existing = resolve(path);
    if (!existing.empty()) return existing;

    const std::string name = leaf_name(path);
    if (name.empty()) return std::string();
    const std::string parentUri = resolve(parent_path(path));
    if (parentUri.empty()) return std::string();   // ⚠️ the create does NOT make intermediate folders

    const std::string made = call_string("safCreateFile",
                                         "(Ljava/lang/String;Ljava/lang/String;)Ljava/lang/String;",
                                         parentUri, &name);
    if (made.empty()) return std::string();
    invalidate(parent_path(path));
    uriCache_[path] = made;
    return made;
}

bool SafFileSystem::delete_uri(const std::string& uri) {
    // ⚠️ Not `call_string`: `safDelete` returns a boolean, and reading `Z` through `CallObjectMethod`
    // is undefined behaviour.
    Env e;
    if (!e.ok()) return false;
    jmethodID m = e.method("safDelete", "(Ljava/lang/String;)Z");
    if (!m) return false;
    jstring    ju  = e.str(uri);
    const bool ok  = e.env->CallBooleanMethod(e.activity, m, ju) == JNI_TRUE;
    const bool bad = e.failed();
    e.env->DeleteLocalRef(ju);
    return ok && !bad;
}

int SafFileSystem::open_uri_fd(const std::string& uri, const char* mode) {
    Env e;
    if (!e.ok()) return -1;
    jmethodID m = e.method("safOpenFd", "(Ljava/lang/String;Ljava/lang/String;)I");
    if (!m) return -1;
    jstring ju = e.str(uri);
    jstring jm = e.str(mode);
    const jint fd  = e.env->CallIntMethod(e.activity, m, ju, jm);
    const bool bad = e.failed();
    e.env->DeleteLocalRef(ju);
    e.env->DeleteLocalRef(jm);
    return bad ? -1 : static_cast<int>(fd);
}

bool SafFileSystem::write_uri(const std::string& uri, const void* data, size_t len) {
    // "wt" — write and TRUNCATE; plain "w" leaves the tail of a longer old file in place.
    const int fd = open_uri_fd(uri, "wt");
    if (fd < 0) return false;

    std::FILE* f = fdopen(fd, "wb");
    if (!f) { close(fd); return false; }
    const bool wrote = len == 0 || std::fwrite(data, 1, len, f) == len;
    // ⚠️ `fclose` is a write: a small payload reaches the provider first here, so a full disk shows up
    // only now. Unchecked, a truncated save returns true.
    const bool closed = std::fclose(f) == 0;
    return wrote && closed;
}

// ─── the open hook ───────────────────────────────────────────────────────────────────────────────

int SafFileSystem::open_fd(const std::string& path, const char* mode) {
    if (!is_pt_path(path)) return -1;   // a plain path never reaches here — pt_fopen took the fopen branch

    const bool append = mode && std::strchr(mode, 'a');
    const bool write  = mode && (std::strchr(mode, 'w') || std::strchr(mode, '+'));

    // ⚠️ Append is refused out loud: nothing opens for append, and a bare -1 reads as "not found".
    if (append) {
        __android_log_print(ANDROID_LOG_WARN, kLogTag,
                            "saf: open('%s','%s') - append is not implemented", path.c_str(), mode);
        return -1;
    }

    // `ensure_file` finds or creates; the "t" makes a found file an overwrite.
    const std::string uri = write ? ensure_file(path) : resolve(path);
    if (uri.empty()) return -1;

    // ⚠️ "rwt", not "wt": `WavStreamWriter` seeks back to patch the RIFF header, and that seek was
    // verified on "rwt". `write_bytes` opens "wt" for one forward pass.
    return open_uri_fd(uri, write ? "rwt" : "r");
}

// ─── the remove and rename hooks ─────────────────────────────────────────────────────────────────

int SafFileSystem::hook_remove(const std::string& path) {
    if (!is_pt_path(path)) return -1;   // a plain path never reaches here — pt_remove took libc's branch
    return delete_path(path) ? 0 : -1;
}

int SafFileSystem::hook_rename(const std::string& from, const std::string& to) {
    // `pt_rename` refuses a mixed pair, so both ends are `pt://` by the time this is called.
    if (!is_pt_path(from) || !is_pt_path(to)) return -1;

    // A different parent is a MOVE. Nothing below the UI does that — both writers publish
    // `<path>.tmp` onto `<path>`.
    if (parent_path(from) != parent_path(to)) return move_file(from, to) ? 0 : -1;

    const std::string name = leaf_name(to);
    if (name.empty()) return -1;

    const std::string uri = resolve(from);
    if (uri.empty()) return -1;

    // ⚠️ The target must be gone: a rename onto a taken name de-duplicates. Callers `pt_remove` it
    // first; this is the backstop.
    const std::string targetUri = resolve(to);
    if (!targetUri.empty()) {
        delete_uri(targetUri);
        forget(to);
    }

    const std::string renamed = call_string("safRename",
                                            "(Ljava/lang/String;Ljava/lang/String;)Ljava/lang/String;",
                                            uri, &name);
    forget(from);
    if (renamed.empty()) return -1;
    uriCache_[to] = renamed;
    invalidate(parent_path(to));
    return 0;
}

// ─── writing ─────────────────────────────────────────────────────────────────────────────────────

bool SafFileSystem::write_file(const std::string& path, const std::string& content) {
    return write_bytes(path, content.data(), content.size());
}

bool SafFileSystem::write_bytes(const std::string& path, const void* data, size_t len) {
    if (!is_pt_path(path)) return priv_.write_bytes(path, data, len);

    const std::string name = leaf_name(path);
    if (name.empty()) return false;

    // ⚠️ Temp then rename: a death mid-save leaves the complete new content in `<name>.tmp`, never a
    // truncated project.
    const std::string tmpPath = path + ".tmp";
    const std::string tmpUri  = ensure_file(tmpPath);
    if (tmpUri.empty()) return false;

    if (!write_uri(tmpUri, data, len)) {
        delete_uri(tmpUri);   // free the space the next attempt needs, and drop the partial
        forget(tmpPath);
        return false;
    }

    // The target goes first, or the rename de-duplicates into `song (1).ptp`.
    const std::string targetUri = resolve(path);
    if (!targetUri.empty()) {
        delete_uri(targetUri);
        forget(path);
    }

    const std::string renamed = call_string("safRename",
                                            "(Ljava/lang/String;Ljava/lang/String;)Ljava/lang/String;",
                                            tmpUri, &name);
    forget(tmpPath);
    if (renamed.empty()) return false;
    uriCache_[path] = renamed;
    invalidate(parent_path(path));
    return true;
}

bool SafFileSystem::is_granted_tree(const std::string& path) {
    return is_pt_path(path) && path != kRootsPath && parent_path(path) == kRootsPath;
}

bool SafFileSystem::delete_path(const std::string& path) {
    if (!is_pt_path(path)) return priv_.delete_path(path);

    // ⚠️ A granted tree is not the app's to delete: `pt://<id>` is the user's whole folder. The browser
    // refuses it too (`FileInfo::isRoot`).
    if (is_granted_tree(path)) return false;

    const std::string uri = resolve(path);
    if (uri.empty()) return false;
    if (!delete_uri(uri)) return false;
    forget(path);
    return true;
}

bool SafFileSystem::rename_file(const std::string& path, const std::string& new_base_name) {
    if (!is_pt_path(path)) return priv_.rename_file(path, new_base_name);
    if (is_granted_tree(path)) return false;   // see delete_path — the tree itself is the user's

    // `StdFileSystem::rename_file`'s rule via `path_sanitize`: keep the extension, never clobber.
    const bool        dir = is_directory(path);
    const std::string ext = dir ? std::string() : pt::ui::path_extension(path);

    const std::string safe = pt::ui::path_sanitize(new_base_name, /*allow_dot=*/true);
    if (safe.empty()) return false;

    std::string finalName = safe;
    if (!ext.empty()) {
        const std::string suffix = "." + ext;
        const bool hasSuffix = safe.size() > suffix.size() &&
                               safe.compare(safe.size() - suffix.size(), suffix.size(), suffix) == 0;
        if (!hasSuffix) finalName = safe + suffix;
    }

    const std::string parent = parent_path(path);
    const std::string target = parent + "/" + finalName;
    if (!resolve(target).empty()) return false;   // the browser reports the failure

    const std::string uri = resolve(path);
    if (uri.empty()) return false;

    const std::string renamed = call_string("safRename",
                                            "(Ljava/lang/String;Ljava/lang/String;)Ljava/lang/String;",
                                            uri, &finalName);
    if (renamed.empty()) return false;

    // ⚠️ The URI may change (the provider may encode the name in the id), so the old path and all
    // under it are forgotten, and the new path takes the returned URI.
    forget(path);
    uriCache_[target] = renamed;
    invalidate(parent);
    return true;
}

std::string SafFileSystem::create_folder(const std::string& parent, const std::string& folder_name) {
    if (!is_pt_path(parent)) return priv_.create_folder(parent, folder_name);

    const std::string safe = pt::ui::path_sanitize(folder_name, /*allow_dot=*/false);
    if (safe.empty()) return std::string();

    const std::string target = parent + "/" + safe;
    // ⚠️ "" if it exists — the interface's contract; `safCreateDir` alone would find-or-create.
    if (!resolve(target).empty()) return std::string();

    const std::string parentUri = resolve(parent);
    if (parentUri.empty()) return std::string();

    const std::string made = call_string("safCreateDir",
                                         "(Ljava/lang/String;Ljava/lang/String;)Ljava/lang/String;",
                                         parentUri, &safe);
    if (made.empty()) return std::string();
    uriCache_[target] = made;
    invalidate(parent);
    return target;
}

bool SafFileSystem::copy_tree(const std::string& from, const std::string& to) {
    if (is_directory(from)) {
        if (create_folder(parent_path(to), leaf_name(to)).empty()) return false;
        for (const pt::ui::FileInfo& child : list_files(from))
            if (!copy_tree(child.path, to + "/" + child.name)) return false;
        return true;
    }
    // Whole-file, as the seam is shaped (`read_file` returns a string); the largest is a sample.
    std::string blob;
    if (!read_file(from, blob)) return false;
    return write_bytes(to, blob.data(), blob.size());
}

bool SafFileSystem::move_file(const std::string& from, const std::string& to) {
    if (!is_pt_path(from) && !is_pt_path(to)) return priv_.move_file(from, to);
    if (is_granted_tree(from)) return false;   // see delete_path — and a move ends in a delete

    // Both ends in one tree: the provider moves it for free. "" means it will not (no
    // FLAG_SUPPORTS_MOVE, two providers) — then copy and delete, as `rename(2)` across filesystems.
    if (is_pt_path(from) && is_pt_path(to)) {
        const std::string srcUri       = resolve(from);
        const std::string fromParent   = resolve(parent_path(from));
        const std::string toParent     = resolve(parent_path(to));
        if (!srcUri.empty() && !fromParent.empty() && !toParent.empty()) {
            Env e;
            if (e.ok()) {
                if (jmethodID m = e.method(
                        "safMove",
                        "(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;)Ljava/lang/String;")) {
                    jstring a = e.str(srcUri), b = e.str(fromParent), c = e.str(toParent);
                    jobject r = e.env->CallObjectMethod(e.activity, m, a, b, c);
                    const bool bad = e.failed();
                    e.env->DeleteLocalRef(a);
                    e.env->DeleteLocalRef(b);
                    e.env->DeleteLocalRef(c);
                    if (bad && r) e.env->DeleteLocalRef(r);   // `take` is what frees it on the good path
                    const std::string moved = bad ? std::string() : e.take(r);
                    if (!moved.empty()) {
                        forget(from);
                        uriCache_[to] = moved;
                        invalidate(parent_path(to));
                        return true;
                    }
                }
            }
        }
    }

    if (!copy_tree(from, to)) return false;
    // ⚠️ A delete that fails leaves both and still reports success: a duplicate the user can remove
    // beats a lost source.
    delete_path(from);
    return true;
}

bool SafFileSystem::copy_file(const std::string& from, const std::string& to) {
    if (!is_pt_path(from) && !is_pt_path(to)) return priv_.copy_file(from, to);
    if (file_exists(to)) return false;   // the caller has already de-duplicated the name
    return copy_tree(from, to);
}

}  // namespace ptshell
