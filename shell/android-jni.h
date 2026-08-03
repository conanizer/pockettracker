#ifndef POCKETTRACKER_SHELL_ANDROID_JNI_H
#define POCKETTRACKER_SHELL_ANDROID_JNI_H

// android-jni.h — the one JNI attach/local-ref helper the Android backends share (MIDI plan E5).
//
// It was `midi-out-android.cpp`'s private helper until the INPUT backend needed the identical thing.
// Written twice, the two copies are two chances to get local-reference lifetime wrong in a file nobody
// looks at again; written once, there is one place that knows the rule.

#ifdef __ANDROID__

#include <SDL.h>
#include <jni.h>

namespace ptshell {

/**
 * The env + a LOCAL ref to the running activity, released on scope exit.
 *
 * Local refs are not free — the JNI spec only guarantees 16 slots without an explicit frame — and the
 * MIDI backends reach for the activity per message (out) or per frame (in), so leaking one per call
 * would eventually abort the VM with a local reference table overflow. RAII rather than a
 * DeleteLocalRef at every return.
 *
 * ⚠️ `SDL_AndroidGetJNIEnv` attaches the CALLING thread if it is not attached already, which is what
 * makes this safe from the MIDI sender thread (an SDL thread) as well as from the frame loop.
 */
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

    /** Clear any pending exception so the NEXT JNI call is not refused. Returns true if there was one. */
    bool failed() const {
        if (!env->ExceptionCheck()) return false;
        env->ExceptionDescribe();
        env->ExceptionClear();
        return true;
    }
};

}  // namespace ptshell

#endif  // __ANDROID__
#endif  // POCKETTRACKER_SHELL_ANDROID_JNI_H
