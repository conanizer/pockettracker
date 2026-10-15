#ifndef POCKETTRACKER_SHELL_ANDROID_JNI_H
#define POCKETTRACKER_SHELL_ANDROID_JNI_H

// The JNI attach/local-ref helper the Android shell shares.

#ifdef __ANDROID__

#include <SDL.h>
#include <jni.h>

#include <string>

namespace ptshell {

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

    // The activity's method by name, or null (exception cleared) — R8 renamed it, or it is not built.
    jmethodID method(const char* name, const char* sig) const {
        jclass    cls = env->GetObjectClass(activity);
        jmethodID m   = env->GetMethodID(cls, name, sig);
        if (env->ExceptionCheck()) { env->ExceptionClear(); m = nullptr; }
        env->DeleteLocalRef(cls);
        return m;
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

}  // namespace ptshell

#endif  // __ANDROID__
#endif  // POCKETTRACKER_SHELL_ANDROID_JNI_H
