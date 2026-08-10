#include "saf-filesystem.h"

#include "byte_source.h"

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

// ─── construction, and the hooks ─────────────────────────────────────────────────────────────────

// ⚠️ The hooks are bare function pointers with no user-data argument (byte_source.h), so the
// instance has to be reachable from free functions. One per process is the only shape this app has
// ever needed; a second construction is logged rather than left to steal the hooks silently.
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
    // ⚠️ NOT `call_string`: `safDelete` returns a boolean, and reading a `Z` return through
    // `CallObjectMethod` is undefined behaviour rather than a wrong answer.
    Env e;
    if (!e.ok()) return false;
    jmethodID m = e.method("safDelete", "(Ljava/lang/String;)Z");
    if (!m) return false;
    jstring    ju  = e.str(uri);
    const bool ok  = e.env->CallBooleanMethod(e.activity, m, ju) == JNI_TRUE;
    const bool bad = e.threw();
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
    const bool bad = e.threw();
    e.env->DeleteLocalRef(ju);
    e.env->DeleteLocalRef(jm);
    return bad ? -1 : static_cast<int>(fd);
}

bool SafFileSystem::write_uri(const std::string& uri, const void* data, size_t len) {
    // "wt" — write and TRUNCATE. Plain "w" leaves whatever was longer than the new content in place,
    // so overwriting a big project with a small one would leave the tail of the big one behind and the
    // file would still parse as far as the JSON goes.
    const int fd = open_uri_fd(uri, "wt");
    if (fd < 0) return false;

    std::FILE* f = fdopen(fd, "wb");
    if (!f) { close(fd); return false; }
    const bool wrote = len == 0 || std::fwrite(data, 1, len, f) == len;
    // ⚠️ `fclose` IS A WRITE and it is checked, for `std_filesystem.cpp`'s reason exactly: a payload
    // smaller than the stdio buffer reaches the provider for the FIRST time in this flush, so a full
    // disk arrives here and nowhere earlier. Unchecked, a truncated save returns true.
    const bool closed = std::fclose(f) == 0;
    return wrote && closed;
}

// ─── the open hook ───────────────────────────────────────────────────────────────────────────────

int SafFileSystem::open_fd(const std::string& path, const char* mode) {
    if (!is_pt_path(path)) return -1;   // a plain path never reaches here — pt_fopen took the fopen branch

    const bool append = mode && std::strchr(mode, 'a');
    const bool write  = mode && (std::strchr(mode, 'w') || std::strchr(mode, '+'));

    // ⚠️ APPEND is refused rather than emulated. `openFileDescriptor` has an "wa" mode, but nothing
    // below the UI opens for append today, and a mode nobody exercises is a mode nobody would notice
    // breaking. It says so out loud rather than returning a bare -1, which reads as "not found".
    if (append) {
        __android_log_print(ANDROID_LOG_WARN, kLogTag,
                            "saf: open('%s','%s') - append is not implemented", path.c_str(), mode);
        return -1;
    }

    // See the header: the create is derivable because a `pt://` path is composable. `ensure_file` is
    // find-or-create, and the "t" below is what makes the found case an overwrite rather than a patch.
    const std::string uri = write ? ensure_file(path) : resolve(path);
    if (uri.empty()) return -1;

    // ⚠️ **"rwt", not "wt", and the extra "r" is load-bearing.** The only thing that opens a `pt://`
    // path for writing is `WavStreamWriter`, which streams a render past the point of no return and
    // then SEEKS BACK TO ZERO to patch the RIFF header with the length it turned out to be. "rwt" is
    // the mode a backward seek over a provider descriptor was measured on; a write-only descriptor
    // has never been asked to do it here. `write_bytes` does not come through this path — it opens
    // "wt" for a single forward pass, which is all it needs.
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

    // A different parent is a MOVE, and `move_file` already knows how to ask the provider for one and
    // how to fall back when it will not. Nothing below the UI renames across directories today — the
    // two writers publish `<path>.tmp` onto `<path>` — so this arm is the general answer, not the
    // exercised one.
    if (parent_path(from) != parent_path(to)) return move_file(from, to) ? 0 : -1;

    const std::string name = leaf_name(to);
    if (name.empty()) return -1;

    const std::string uri = resolve(from);
    if (uri.empty()) return -1;

    // ⚠️ The target must be gone, exactly as in `write_bytes`: `renameDocument` onto a taken name
    // de-duplicates the way `createDocument` does. Both callers already `pt_remove` the target first,
    // so this is the belt to that pair of braces — and it is cheap, because `resolve` on a name the
    // caller just deleted answers out of a listing this class invalidated when it deleted it.
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

    // ⚠️ **The temp-then-rename dance survives, and which failure it trades for which matters.**
    // Writing straight over the target with "wt" would mean a process killed mid-save leaves a
    // truncated file where the whole project was — the exact failure `StdFileSystem` refuses. Here the
    // window is between the delete and the rename, and what sits on disk during it is the COMPLETE new
    // content under `<name>.tmp`: recoverable by hand, where a truncated `.ptp` is not.
    const std::string tmpPath = path + ".tmp";
    const std::string tmpUri  = ensure_file(tmpPath);
    if (tmpUri.empty()) return false;

    if (!write_uri(tmpUri, data, len)) {
        delete_uri(tmpUri);   // free the space the next attempt needs, and drop the partial
        forget(tmpPath);
        return false;
    }

    // The target has to GO before the rename: `renameDocument` onto a taken name de-duplicates the
    // way `createDocument` does, so skipping this leaves the old file plus a `song (1).ptp`.
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

bool SafFileSystem::delete_path(const std::string& path) {
    if (!is_pt_path(path)) return priv_.delete_path(path);

    const std::string uri = resolve(path);
    if (uri.empty()) return false;
    if (!delete_uri(uri)) return false;
    forget(path);
    return true;
}

bool SafFileSystem::rename_file(const std::string& path, const std::string& new_base_name) {
    if (!is_pt_path(path)) return priv_.rename_file(path, new_base_name);

    // Byte-for-byte `StdFileSystem::rename_file`'s rule, through the shared `path_sanitize`: keep the
    // extension unless the typed name already ends in it, and never clobber.
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

    // ⚠️ The document's URI may have CHANGED — `ExternalStorageProvider` encodes the display name into
    // the id — so the OLD path and everything under it is forgotten and the NEW one takes the returned
    // URI. Caching the old string against the new path is how a renamed folder's children go stale.
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
    // ⚠️ "" when it already exists, which is the interface's contract and NOT what `safCreateDir`
    // does on its own — that one is find-or-create, because the seven app folders ask for themselves
    // on every launch. The check belongs here, where the contract is.
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
    // Whole-file, because that is what the seam is shaped for (`read_file` returns a string) and what
    // the callers copy: projects, presets, themes and samples. The largest of them is a sample, and one
    // already costs twice this much resident once it is loaded.
    std::string blob;
    if (!read_file(from, blob)) return false;
    return write_bytes(to, blob.data(), blob.size());
}

bool SafFileSystem::move_file(const std::string& from, const std::string& to) {
    if (!is_pt_path(from) && !is_pt_path(to)) return priv_.move_file(from, to);

    // Same tree, both ends SAF: ask the provider to move the document, which costs nothing and keeps
    // the bytes where they are. "" means it will not — a missing FLAG_SUPPORTS_MOVE, or two different
    // providers — and that is a fallback rather than a failure, exactly as a cross-filesystem
    // `rename(2)` is for `StdFileSystem`.
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
                    const bool bad = e.threw();
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
    // ⚠️ The copy is what must not be lost, so a delete that fails leaves BOTH rather than neither and
    // still reports success — a move that half-worked with the source gone is unrecoverable, one with
    // the source still there is a duplicate the user can see and remove.
    delete_path(from);
    return true;
}

bool SafFileSystem::copy_file(const std::string& from, const std::string& to) {
    if (!is_pt_path(from) && !is_pt_path(to)) return priv_.copy_file(from, to);
    if (file_exists(to)) return false;   // the caller has already de-duplicated the name
    return copy_tree(from, to);
}

}  // namespace ptshell
