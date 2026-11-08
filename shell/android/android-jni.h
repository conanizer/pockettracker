#ifndef POCKETTRACKER_SHELL_ANDROID_JNI_H
#define POCKETTRACKER_SHELL_ANDROID_JNI_H

// The JNI attach/local-ref helper the Android shell shares, and the one way it calls the activity.

#ifdef __ANDROID__

#include <SDL.h>
#include <android/log.h>
#include <jni.h>

#include <string>
#include <type_traits>

namespace ptshell {

namespace jni_detail {

// A call argument in JNI form. A string becomes a local jstring that lives until the call returns.
class LocalString {
  public:
    LocalString(JNIEnv* env, const char* s) : env_(env), ref_(env->NewStringUTF(s)) {}
    ~LocalString() {
        if (ref_) env_->DeleteLocalRef(ref_);
    }
    LocalString(const LocalString&)            = delete;
    LocalString& operator=(const LocalString&) = delete;
    jstring get() const { return ref_; }

  private:
    JNIEnv* env_;
    jstring ref_;
};

template <class T> struct Plain {
    T v;
    T get() const { return v; }
};

inline LocalString     arg(JNIEnv* e, const std::string& s) { return LocalString(e, s.c_str()); }
inline LocalString     arg(JNIEnv* e, const char* s) { return LocalString(e, s); }
inline Plain<jint>     arg(JNIEnv*, int v) { return {v}; }
inline Plain<jobject>  arg(JNIEnv*, jobject o) { return {o}; }
// ⚠️ Exactly bool: a `uint8_t` would otherwise convert to jboolean before it promoted to int.
template <class B, std::enable_if_t<std::is_same_v<B, bool>, int> = 0>
Plain<jboolean> arg(JNIEnv*, B v) { return {static_cast<jboolean>(v ? JNI_TRUE : JNI_FALSE)}; }

}  // namespace jni_detail

// What a call answers when it cannot be made or it threw: nothing, false, -1, "", null.
template <class R> R jni_failure() {
    if constexpr (std::is_same_v<R, int>) return -1;
    else if constexpr (!std::is_void_v<R>) return R{};
}

// The env and a LOCAL ref to the running activity, released on scope exit — the MIDI backends reach
// for it per message, and leaked local refs overflow the VM's table. `SDL_AndroidGetJNIEnv` attaches
// the calling thread, so this works from any SDL thread.
struct Attached {
    JNIEnv* env      = nullptr;
    jobject activity = nullptr;

    Attached() {
        env = static_cast<JNIEnv*>(SDL_AndroidGetJNIEnv());
        if (env) activity = static_cast<jobject>(SDL_AndroidGetActivity());
    }
    ~Attached() {
        if (env && activity) env->DeleteLocalRef(activity);
    }
    Attached(const Attached&)            = delete;
    Attached& operator=(const Attached&) = delete;

    bool ok() const { return env != nullptr && activity != nullptr; }

    // Clears a pending exception so the next JNI call is not refused; true if there was one.
    bool failed() const {
        if (!env->ExceptionCheck()) return false;
        env->ExceptionDescribe();
        env->ExceptionClear();
        return true;
    }

    // The activity's method by name, or null — R8 renamed it, or it is not built. A miss is cleared
    // at once (under CheckJNI a pending exception aborts the next call) and logged with `otherwise`,
    // what its absence costs.
    jmethodID method(const char* name, const char* sig, const char* otherwise) const {
        jclass    cls = env->GetObjectClass(activity);
        jmethodID m   = env->GetMethodID(cls, name, sig);
        if (env->ExceptionCheck()) { env->ExceptionClear(); m = nullptr; }
        env->DeleteLocalRef(cls);
        if (!m) __android_log_print(ANDROID_LOG_WARN, "PocketTrackerSDL", "%s%s not found - %s", name, sig, otherwise);
        return m;
    }

    // One call on the activity, as `R` (void, bool, int, std::string, or a jobject the caller
    // deletes); `jni_failure<R>()` if it threw. Strings go in as local jstrings, freed after.
    template <class R, class... A> R call(jmethodID m, const A&... args) const {
        if constexpr (std::is_void_v<R>) {
            env->CallVoidMethod(activity, m, jni_detail::arg(env, args).get()...);
            failed();
        } else if constexpr (std::is_same_v<R, bool>) {
            const jboolean r = env->CallBooleanMethod(activity, m, jni_detail::arg(env, args).get()...);
            return !failed() && r == JNI_TRUE;
        } else if constexpr (std::is_same_v<R, int>) {
            const jint r = env->CallIntMethod(activity, m, jni_detail::arg(env, args).get()...);
            return failed() ? -1 : static_cast<int>(r);
        } else {
            static_assert(std::is_same_v<R, std::string> || std::is_same_v<R, jobject>, "no such JNI return");
            jobject r = env->CallObjectMethod(activity, m, jni_detail::arg(env, args).get()...);
            if (failed()) {
                if (r) env->DeleteLocalRef(r);
                return R{};
            }
            if constexpr (std::is_same_v<R, std::string>) return take(r);
            else return r;
        }
    }

    // A returned Java String as UTF-8, its local ref deleted; "" for null.
    std::string take(jobject s) const {
        if (!s) return std::string();
        const char* utf = env->GetStringUTFChars(static_cast<jstring>(s), nullptr);
        std::string out = utf ? utf : "";
        if (utf) env->ReleaseStringUTFChars(static_cast<jstring>(s), utf);
        env->DeleteLocalRef(s);
        return out;
    }
};

// Attach, look up and call in one: `jni_failure<R>()` if any step fails.
template <class R, class... A>
R call_activity(const char* name, const char* sig, const char* otherwise, const A&... args) {
    Attached a;
    if (!a.ok()) return jni_failure<R>();
    const jmethodID m = a.method(name, sig, otherwise);
    if (!m) return jni_failure<R>();
    return a.call<R>(m, args...);
}

}  // namespace ptshell

#endif  // __ANDROID__
#endif  // POCKETTRACKER_SHELL_ANDROID_JNI_H
