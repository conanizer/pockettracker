#include "saf-filesystem.h"

#include "byte_source.h"

#include <SDL.h>

#include <android/log.h>
#include <jni.h>

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace ptshell {
namespace {

constexpr const char* kLogTag   = "PocketTrackerSDL";
constexpr const char* kScheme   = "pt://";
constexpr size_t      kSchemeLen = 5;
constexpr const char* kRootsPath = "pt://roots";

bool is_pt_path(const std::string& s) {
    return s.compare(0, kSchemeLen, kScheme) == 0;
}

// ─── the JNI seam ────────────────────────────────────────────────────────────────────────────────
//
// The same by-name, exception-safe shape `android-main.cpp` and both MIDI backends already use: a
// missing method degrades to one log line and an empty answer rather than a crash, because the only
// way it can go missing is R8 renaming it in a RELEASE build — the one build nobody is attached to.

struct Env {
    JNIEnv* env      = nullptr;
    jobject activity = nullptr;   // LOCAL ref
    jclass  cls      = nullptr;   // LOCAL ref

    Env() {
        env = static_cast<JNIEnv*>(SDL_AndroidGetJNIEnv());
        if (!env) return;
        activity = static_cast<jobject>(SDL_AndroidGetActivity());
        if (!activity) return;
        cls = env->GetObjectClass(activity);
    }
    ~Env() {
        if (!env) return;
        if (cls) env->DeleteLocalRef(cls);
        if (activity) env->DeleteLocalRef(activity);
    }
    bool ok() const { return env && activity && cls; }

    jmethodID method(const char* name, const char* sig) {
        jmethodID m = env->GetMethodID(cls, name, sig);
        if (env->ExceptionCheck()) { env->ExceptionClear(); m = nullptr; }
        if (!m) {
            __android_log_print(ANDROID_LOG_WARN, kLogTag,
                                "saf: %s%s not found on the activity - R8 renamed it, or it is not built",
                                name, sig);
        }
        return m;
    }

    /** A jstring that is deleted with the scope. */
    jstring str(const std::string& s) { return env->NewStringUTF(s.c_str()); }

    std::string take(jobject o) {
        if (!o) return std::string();
        const char* utf = env->GetStringUTFChars(static_cast<jstring>(o), nullptr);
        std::string out = utf ? utf : "";
        if (utf) env->ReleaseStringUTFChars(static_cast<jstring>(o), utf);
        env->DeleteLocalRef(o);
        return out;
    }

    bool threw() {
        if (!env->ExceptionCheck()) return false;
        env->ExceptionDescribe();
        env->ExceptionClear();
        return true;
    }
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
    const bool bad = e.threw();
    e.env->DeleteLocalRef(ja);
    if (jb) e.env->DeleteLocalRef(jb);
    if (bad) { if (r) e.env->DeleteLocalRef(r); return std::string(); }
    return e.take(r);
}

}  // namespace

// ─── construction, and the hook ──────────────────────────────────────────────────────────────────

// ⚠️ `PtOpenHook` is a bare function pointer with no user-data argument (byte_source.h), so the
// instance has to be reachable from a free function. One per process is the only shape this app has
// ever needed; a second construction is logged rather than left to steal the hook silently.
SafFileSystem* g_instance = nullptr;

SafFileSystem::SafFileSystem(std::string private_root) : priv_(private_root, private_root) {}

extern "C" int saf_open_hook_trampoline(const char* path, const char* mode) {
    if (!g_instance || !path || !mode) return -1;
    return g_instance->open_fd(path, mode);
}

void SafFileSystem::install_open_hook() {
    if (g_instance && g_instance != this) {
        __android_log_print(ANDROID_LOG_ERROR, kLogTag,
                            "saf: a second SafFileSystem is taking the open hook - the first one's "
                            "URIs will stop resolving");
    }
    g_instance = this;
    pt_set_open_hook(&saf_open_hook_trampoline);
    std::printf("saf:     pt_fopen hook installed\n");
}

// ─── roots ───────────────────────────────────────────────────────────────────────────────────────

const std::vector<SafFileSystem::Root>& SafFileSystem::roots(bool refresh) {
    if (rootsLoaded_ && !refresh) return roots_;
    rootsLoaded_ = true;
    roots_.clear();

    int count = 0;
    {
        Env e;
        if (e.ok()) {
            if (jmethodID m = e.method("safRootCount", "()I")) {
                count = e.env->CallIntMethod(e.activity, m);
                if (e.threw()) count = 0;
            }
        }
    }

    for (int i = 0; i < count; ++i) {
        Env e;
        if (!e.ok()) break;
        jmethodID m = e.method("safRootInfo", "(I)Ljava/lang/String;");
        if (!m) break;
        jobject r = e.env->CallObjectMethod(e.activity, m, static_cast<jint>(i));
        if (e.threw()) { if (r) e.env->DeleteLocalRef(r); continue; }
        const std::string info = e.take(r);

        // `<id>\t<name>\t<docUri>` — a row with a missing field is skipped rather than half-read.
        const size_t t1 = info.find('\t');
        if (t1 == std::string::npos) continue;
        const size_t t2 = info.find('\t', t1 + 1);
        if (t2 == std::string::npos) continue;
        roots_.push_back(Root{info.substr(0, t1), info.substr(t1 + 1, t2 - t1 - 1), info.substr(t2 + 1)});
    }
    return roots_;
}

bool SafFileSystem::has_grant() { return !roots().empty(); }

int SafFileSystem::root_count() { return static_cast<int>(roots().size()); }

std::string SafFileSystem::home_root_path() {
    const auto& r = roots();
    if (r.empty()) return std::string();
    // Java already sorted by id; taking the lowest is what makes the choice stable across boots.
    // ⚠️ Scaffolding: granting a second tree whose id sorts lower would move the app's folders. A
    // designated home root persisted in settings.json is owed before P4 — saf-migration-plan.md §5b.
    return std::string(kScheme) + r.front().id;
}

// ─── path arithmetic — the whole point of §5a ────────────────────────────────────────────────────

std::string SafFileSystem::parent_path(const std::string& path) {
    if (!is_pt_path(path)) return priv_.parent_path(path);
    if (path == kRootsPath) return std::string();          // the top; the browser's ".." stops here

    const size_t slash = path.rfind('/');
    // "pt://<id>" — the last '/' is the scheme's own, so the parent is the roots directory. This is
    // §7's whole reason for making the roots listing a DIRECTORY: the browser needs no new state.
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

    // Anything deeper is found by listing its parent, which caches every sibling's URI on the way —
    // so resolving 8 samples in one folder costs one query, not eight. Recurses to the root.
    listing(parent_path(path));
    cached = uriCache_.find(path);
    return cached != uriCache_.end() ? cached->second : std::string();
}

const std::vector<pt::ui::FileInfo>* SafFileSystem::listing(const std::string& dirPath) {
    auto hit = listCache_.find(dirPath);
    if (hit != listCache_.end()) return &hit->second;

    std::vector<pt::ui::FileInfo> out;

    if (dirPath == kRootsPath) {
        for (const Root& r : roots()) {
            pt::ui::FileInfo fi;
            fi.path        = std::string(kScheme) + r.id;
            fi.name        = r.name;
            fi.isDirectory = true;
            out.push_back(fi);
            uriCache_[fi.path] = r.docUri;
        }
        auto& slot = listCache_[dirPath];
        slot = std::move(out);
        return &slot;
    }

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
        // `File.extension` semantics, as `std_filesystem.h` insists: the browser FILTERS on this
        // string, so the two implementations must answer identically.
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
    if (home.empty()) return std::string();   // nothing granted — the browser shows the empty state

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

std::string SafFileSystem::config_path() {
    const std::string home = home_root_path();
    // ⚠️ Empty when nothing is granted, and that is correct rather than a degradation:
    // `load_folder_config` treats "no file" as its common case, so "no tree yet" reads exactly like
    // "no config was ever written" — which is the argument that kept this file in the tree at all.
    return home.empty() ? std::string() : home + "/config.json";
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
    const bool bad = e.threw();
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

// ─── the open hook ───────────────────────────────────────────────────────────────────────────────

int SafFileSystem::open_fd(const std::string& path, const char* mode) {
    if (!is_pt_path(path)) return -1;   // a plain path never reaches here — pt_fopen took the fopen branch

    // ⚠️ A document must EXIST before it can be opened for write (byte_source.h says so, and it is
    // why creating one belongs to `ui::FileSystem`). P3a resolves reads; the write half is P3b.
    if (mode && (std::strchr(mode, 'w') || std::strchr(mode, 'a'))) {
        __android_log_print(ANDROID_LOG_WARN, kLogTag,
                            "saf: open('%s','%s') - write modes arrive in P3b", path.c_str(), mode);
        return -1;
    }

    const std::string uri = resolve(path);
    if (uri.empty()) return -1;

    Env e;
    if (!e.ok()) return -1;
    jmethodID m = e.method("safOpenFd", "(Ljava/lang/String;Ljava/lang/String;)I");
    if (!m) return -1;
    jstring ju = e.str(uri);
    jstring jm = e.str("r");
    const jint fd  = e.env->CallIntMethod(e.activity, m, ju, jm);
    const bool bad = e.threw();
    e.env->DeleteLocalRef(ju);
    e.env->DeleteLocalRef(jm);
    return bad ? -1 : static_cast<int>(fd);
}

// ─── writing — P3a serves plain paths and refuses URIs out loud ──────────────────────────────────

namespace {
bool refuse(const char* what, const std::string& path) {
    __android_log_print(ANDROID_LOG_WARN, kLogTag, "saf: %s on '%s' arrives in P3b", what, path.c_str());
    return false;
}
}  // namespace

bool SafFileSystem::write_file(const std::string& path, const std::string& content) {
    if (!is_pt_path(path)) return priv_.write_file(path, content);
    return refuse("write_file", path);
}

bool SafFileSystem::write_bytes(const std::string& path, const void* data, size_t len) {
    if (!is_pt_path(path)) return priv_.write_bytes(path, data, len);
    return refuse("write_bytes", path);
}

bool SafFileSystem::delete_path(const std::string& path) {
    if (!is_pt_path(path)) return priv_.delete_path(path);
    return refuse("delete_path", path);
}

bool SafFileSystem::rename_file(const std::string& path, const std::string& new_base_name) {
    if (!is_pt_path(path)) return priv_.rename_file(path, new_base_name);
    return refuse("rename_file", path);
}

std::string SafFileSystem::create_folder(const std::string& parent, const std::string& folder_name) {
    if (!is_pt_path(parent)) return priv_.create_folder(parent, folder_name);
    refuse("create_folder", parent);
    return std::string();
}

bool SafFileSystem::move_file(const std::string& from, const std::string& to) {
    if (!is_pt_path(from) && !is_pt_path(to)) return priv_.move_file(from, to);
    return refuse("move_file", from);
}

bool SafFileSystem::copy_file(const std::string& from, const std::string& to) {
    if (!is_pt_path(from) && !is_pt_path(to)) return priv_.copy_file(from, to);
    return refuse("copy_file", from);
}

}  // namespace ptshell
