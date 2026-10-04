// PocketTracker — the ANDROID entry point. Everything in this file is platform residue; `app.cpp`
// boots, runs and tears down identically on both platforms. Side by side with `main.cpp`:
//
//   main.cpp (desktop/handheld)              android-main.cpp (this file)
//   ─────────────────────────────────────    ──────────────────────────────────────────────────────
//   argv: project, media dir, app root       argv: app root, then filesDir — both from SDLActivity
//   SIGTERM/SIGINT → a flag the loop polls   nothing (see the terminate_requested note below)
//   SDL_Init / SDL_Quit, here                SDL_Init here too; SDLActivity owns the surface, not this
//   SdlAudioEngine                           OboeAudioEngine
//   StdFileSystem over the app root          SafFileSystem — the app holds no storage permission
//   default_app_root(), one root twice       two roots from Java: media tree + app-private files
//   PlatformCaps::sdl(debug), console on     PlatformCaps::converged(...) — see where it is set below
//
// ⚠️ NO `SDL_MAIN_HANDLED` HERE, THE OPPOSITE OF `main.cpp`: on `__ANDROID__` SDL_main.h does
// `#define main SDL_main`, the symbol `SDLActivity` looks up with `dlsym`. Define SDL_MAIN_HANDLED
// and the app dies at start-up with a missing-entry-point message.

// <cmath> before <SDL.h> — see the note in sdl-audio-engine.h (M_PI, _USE_MATH_DEFINES, C4005).
#include <cmath>

#include <SDL.h>

#include "audio-engine.h"
#include "byte_source.h"       // pt_fopen — the log file's tee follows the app into a granted tree
#include "oboe-audio-engine.h"
#include "ui/platform_caps.h"

#include "app.h"
#include "button_feedback.h"
#include "saf-filesystem.h"    // the ONLY ui::FileSystem here; Android-only by construction
#include "midi-in-android.h"    // the MIDI INPUT port;    compiles to nothing elsewhere
#include "midi-out-android.h"   // the EXTERNAL MIDI port; compiles to nothing elsewhere

#include <android/log.h>
#include <jni.h>
#include <pthread.h>
#include <unistd.h>

#include <atomic>
#include <cstdio>
#include <cstdlib>     // atoi — the AudioManager properties come back as decimal strings
#include <memory>
#include <mutex>
#include <string>

namespace ui = pt::ui;

namespace {

constexpr const char* kLogTag = "PocketTrackerSDL";

// ⚠️ THE APP ROOT COMES FROM JAVA: `ui::default_app_root()` walks POCKETTRACKER_HOME → XDG_DATA_HOME
// → HOME, all of which miss on Android, falling through to a RELATIVE path beside the cwd. Only Java
// knows where DIRECTORY_DOCUMENTS is, so the activity passes it through `getArguments()`. The fallback
// keeps a bring-up going and SAYS SO in the log — a silently wrong root reads as "my projects are gone".
constexpr const char* kFallbackAppRoot = "/storage/emulated/0/Documents/PocketTracker";

// ─── stdout/stderr → logcat ───────────────────────────────────────────────────────────────────────
//
// The boot banner and status line are the bring-up instrument, and on Android stdout is /dev/null
// unless `log.redirect-stdio` is set (root). A pipe-and-pump sends them to logcat, unbuffered.
//
// ⚠️ AND TO A FILE, because logcat needs a PC and developer mode; a user reporting a boot problem can
// reach `pockettracker-log.txt` with any file manager. TRUNCATED at start and capped.
//
// ⚠️⚠️ WRITTEN TO TWO PLACES, BOTH REQUIRED: at the first line the only certainly writable directory
// is `filesDir` (unreachable to the user); the granted tree (reachable) is known only later in boot.
// So the log opens in `filesDir` and RELOCATES into the granted tree once there is one, replaying
// what was written (`relocate_log_file`). `getExternalFilesDir()` is no answer: Android 11+ hides
// `Android/data`.
std::mutex  g_logMutex;           // g_logFile is swapped on the SDL thread and written on the pump's
FILE*       g_logFile     = nullptr;   // null = logcat alone
size_t      g_logFileSize = 0;
std::string g_logPath;            // what g_logFile is open on; "" = nothing open
constexpr size_t      kLogFileCap  = 512 * 1024;
constexpr const char* kLogFileName = "pockettracker-log.txt";

// Everything written so far, held so the relocation can carry it across. ⚠️ In memory, not re-read
// off the first file: the lines printed just before the relocation are still in the pipe, and a
// re-read would race the pump. Appended under the write's lock, so each line is carried or goes to
// the new file — never neither, never both.
std::string      g_logCarry;
bool             g_logCarrying = true;
constexpr size_t kLogCarryCap  = 64 * 1024;   // the boot is a few KB; this is slack, not a budget

void log_line(const char* s) {
    __android_log_write(ANDROID_LOG_INFO, kLogTag, s);
    std::lock_guard<std::mutex> lock(g_logMutex);
    if (g_logCarrying && g_logCarry.size() < kLogCarryCap) {
        g_logCarry.append(s);
        g_logCarry.push_back('\n');
    }
    if (!g_logFile || g_logFileSize >= kLogFileCap) return;
    // ⚠️ Flushed per line, for main.cpp's `setvbuf` reason exactly: a buffer that dies with the
    // process takes the boot with it, and the boot is what this file exists to record.
    // ⚠️ `fprintf` returns NEGATIVE on error, and this counter is unsigned — adding it raw would wrap
    // to a huge value and silently stop the log at the first hiccup (a full disk, a revoked grant).
    const int written = std::fprintf(g_logFile, "%s\n", s);
    if (written > 0) g_logFileSize += static_cast<size_t>(written);
    std::fflush(g_logFile);
    if (g_logFileSize >= kLogFileCap)
        std::fprintf(g_logFile, "--- log capped at %zu bytes ---\n", kLogFileCap);
}

/** The destination is settled — stop carrying and give the buffer back. */
void log_carry_done() {
    std::lock_guard<std::mutex> lock(g_logMutex);
    g_logCarrying = false;
    std::string().swap(g_logCarry);
}

// Move the tee into `dir` (a granted `pt://` tree), carrying everything already written across.
// Returns the path it is now writing to, or "" if it could not move — in which case the first
// destination still stands and is still being written to.
//
// ⚠️ **Through `pt_fopen`, so it must run AFTER the hooks are installed** — `std::fopen` cannot see a
// `pt://` string, and a silent failure here is precisely the failure this function exists to end.
//
// ⚠️ The `filesDir` copy is NOT deleted, and not truncated either. On the run where the relocation is
// itself what went wrong it is the only copy there is, and it is bounded by the same cap.
std::string relocate_log_file(const std::string& dir) {
    const std::string path = dir + "/" + kLogFileName;

    FILE* next = pt_fopen(path.c_str(), "w");
    if (!next) return std::string();

    std::lock_guard<std::mutex> lock(g_logMutex);
    if (!g_logCarry.empty()) std::fwrite(g_logCarry.data(), 1, g_logCarry.size(), next);
    std::fflush(next);
    if (g_logFile) std::fclose(g_logFile);
    g_logFile     = next;
    g_logPath     = path;
    g_logFileSize = g_logCarry.size();
    g_logCarrying = false;
    std::string().swap(g_logCarry);
    return path;
}

void* log_pump(void* arg) {
    const int   fd = static_cast<int>(reinterpret_cast<intptr_t>(arg));
    std::string line;
    char        buf[256];
    ssize_t     n;
    while ((n = read(fd, buf, sizeof(buf))) > 0) {
        for (ssize_t i = 0; i < n; ++i) {
            if (buf[i] == '\n') {
                log_line(line.c_str());
                line.clear();
            } else if (buf[i] != '\r') {
                line.push_back(buf[i]);
            }
        }
        // A status line that never ends in '\n' would otherwise accumulate forever. Flush long
        // fragments rather than growing without bound.
        if (line.size() > 1024) {
            log_line(line.c_str());
            line.clear();
        }
    }
    return nullptr;
}

// ─── button feedback → the thin Kotlin managers ──────────────────────────────────────────────────
//
// The shared touch layer decides WHEN a virtual button clicks (sdl-touch.cpp); this shim carries it
// to Java, where SoundPool and the Vibrator live. Runs on the SDL thread; `SDL_AndroidGetJNIEnv`
// attaches it. The method is looked up by NAME, so a mismatch degrades to silence with one log line.
class AndroidButtonFeedback : public ptshell::ButtonFeedback {
public:
    void play(pt::ui::Button button, bool down, const ptshell::ButtonFeedbackSettings& s) override {
        // Nothing enabled → nothing worth a JNI round trip. The Kotlin side re-checks too; this is just
        // the cheap early-out for the common "both off" case.
        if (!s.soundEnabled && !s.vibroEnabled) return;

        JNIEnv* env = static_cast<JNIEnv*>(SDL_AndroidGetJNIEnv());
        if (!env) return;
        jobject activity = static_cast<jobject>(SDL_AndroidGetActivity());  // a LOCAL ref — delete below
        if (!activity) return;

        jmethodID mid = method_id(env, activity);
        if (mid) {
            env->CallVoidMethod(activity, mid, static_cast<jint>(button),
                                static_cast<jboolean>(down),
                                static_cast<jboolean>(s.soundEnabled), static_cast<jint>(s.soundVolume),
                                static_cast<jboolean>(s.vibroEnabled), static_cast<jint>(s.vibroPower));
            if (env->ExceptionCheck()) { env->ExceptionDescribe(); env->ExceptionClear(); }
        }
        env->DeleteLocalRef(activity);
    }

private:
    // The method id, resolved once. Method ids stay valid for the class's lifetime and the activity's
    // class is never unloaded, so the lookup is a one-time cost; `looked_up_` also stops a missing
    // method from re-logging on every tap.
    jmethodID method_id(JNIEnv* env, jobject activity) {
        if (looked_up_) return mid_;
        looked_up_ = true;
        jclass cls = env->GetObjectClass(activity);
        mid_ = env->GetMethodID(cls, "onButtonFeedback", "(IZZIZI)V");
        if (env->ExceptionCheck()) { env->ExceptionClear(); mid_ = nullptr; }
        if (!mid_) {
            __android_log_print(ANDROID_LOG_WARN, kLogTag,
                                "onButtonFeedback(IZZIZI)V not found on the activity - "
                                "button feedback disabled");
        }
        env->DeleteLocalRef(cls);
        return mid_;
    }

    bool      looked_up_ = false;
    jmethodID mid_       = nullptr;
};

// ─── does a physical game controller exist? → SdlActivity ────────────────────────────────────────
//
// On Android SDL counts the emulator's keyboard as a controller (app.h `physicalGamepadPresent`);
// only `InputDevice.getSources()` tells them apart, so this asks `SdlActivity.hasPhysicalGameButtons()`
// (SOURCE_GAMEPAD/SOURCE_JOYSTICK). By name, null/exception-safe, degrading to "no pad" (the touch
// UI). Asked only at boot and on a controller add/remove.
bool android_has_physical_gamepad() {
    JNIEnv* env = static_cast<JNIEnv*>(SDL_AndroidGetJNIEnv());
    if (!env) return false;
    jobject activity = static_cast<jobject>(SDL_AndroidGetActivity());  // LOCAL ref — deleted below
    if (!activity) return false;

    bool      result = false;
    jclass    cls    = env->GetObjectClass(activity);
    jmethodID mid    = env->GetMethodID(cls, "hasPhysicalGameButtons", "()Z");
    if (env->ExceptionCheck()) { env->ExceptionClear(); mid = nullptr; }
    if (mid) {
        result = env->CallBooleanMethod(activity, mid) == JNI_TRUE;
        if (env->ExceptionCheck()) { env->ExceptionDescribe(); env->ExceptionClear(); result = false; }
    } else {
        __android_log_print(ANDROID_LOG_WARN, kLogTag,
                            "hasPhysicalGameButtons()Z not found - assuming no pad (touch UI)");
    }
    env->DeleteLocalRef(cls);
    env->DeleteLocalRef(activity);
    return result;
}

// ─── may the screen rotate into landscape? → SdlActivity ─────────────────────────────────────────
//
// The live half of the orientation lock: `SDL_HINT_ORIENTATIONS` is read once, so a pad unplugged
// mid-session would leave the activity in landscape over the portrait skin. The layout gate calls
// this on every change and Java sets `requestedOrientation`; Android re-orients when the current
// one stops being allowed. A missing method logs and changes nothing.
void android_set_landscape_allowed(bool allowed) {
    JNIEnv* env = static_cast<JNIEnv*>(SDL_AndroidGetJNIEnv());
    if (!env) return;
    jobject activity = static_cast<jobject>(SDL_AndroidGetActivity());
    if (!activity) return;

    jclass    cls = env->GetObjectClass(activity);
    jmethodID mid = env->GetMethodID(cls, "setLandscapeAllowed", "(Z)V");
    if (env->ExceptionCheck()) { env->ExceptionClear(); mid = nullptr; }
    if (mid) {
        env->CallVoidMethod(activity, mid, allowed ? JNI_TRUE : JNI_FALSE);
        if (env->ExceptionCheck()) { env->ExceptionDescribe(); env->ExceptionClear(); }
        else std::printf("orient:  %s\n", allowed ? "landscape allowed (FULL layout)"
                                                  : "portrait only (on-screen buttons in force)");
        std::fflush(stdout);
    } else {
        __android_log_print(ANDROID_LOG_WARN, kLogTag,
                            "setLandscapeAllowed(Z)V not found - orientation stays as launched");
    }
    env->DeleteLocalRef(cls);
    env->DeleteLocalRef(activity);
}

// ─── PLAYING ON IN THE BACKGROUND ────────────────────────────────────────────────────────────────
//
// The three facts that cross between the frame loop and Java, as atomics: Java reads and writes them
// on its UI thread, the loop on the native thread. The SERVICE is Java's — started in `onPause`, where
// Android still allows it (MainActivity.kt) — and these only say what each side needs to know.
std::atomic<bool> g_bgPlaying{false};          // the transport, published by the loop every tick
std::atomic<bool> g_bgServiceStarted{false};   // onPause brought the playback service up
std::atomic<bool> g_bgStopRequested{false};    // the notification's STOP, waiting for the loop

// The song stopped in the background (or never kept playing): Java takes the service down. Same
// by-name, exception-safe shape as the hooks above.
void android_stop_playback_service() {
    g_bgServiceStarted.store(false);
    JNIEnv* env = static_cast<JNIEnv*>(SDL_AndroidGetJNIEnv());
    if (!env) return;
    jobject activity = static_cast<jobject>(SDL_AndroidGetActivity());
    if (!activity) return;
    jclass    cls = env->GetObjectClass(activity);
    jmethodID mid = env->GetMethodID(cls, "stopPlaybackService", "()V");
    if (env->ExceptionCheck()) { env->ExceptionClear(); mid = nullptr; }
    if (mid) {
        env->CallVoidMethod(activity, mid);
        if (env->ExceptionCheck()) { env->ExceptionDescribe(); env->ExceptionClear(); }
    } else {
        __android_log_print(ANDROID_LOG_WARN, kLogTag, "stopPlaybackService()V not found");
    }
    env->DeleteLocalRef(cls);
    env->DeleteLocalRef(activity);
}

// ─── the input-device enumeration, once at boot ──────────────────────────────────────────────────
//
// ⚠️ THE ONE THING A LAYOUT BUG REPORT NEEDS: `useTouch` is `touchCapable && !physicalPad`, and
// `hasPhysicalGameButtons()` logs only when it FINDS a pad. This prints the whole enumeration
// through `printf`, so it reaches the log file (Kotlin's `Log.i` reaches logcat only).
void android_log_input_devices() {
    JNIEnv* env = static_cast<JNIEnv*>(SDL_AndroidGetJNIEnv());
    if (!env) { std::printf("input:   no JNI env - cannot enumerate\n"); return; }
    jobject activity = static_cast<jobject>(SDL_AndroidGetActivity());
    if (!activity) { std::printf("input:   no activity - cannot enumerate\n"); return; }

    jclass    cls = env->GetObjectClass(activity);
    jmethodID mid = env->GetMethodID(cls, "describeInputDevices", "()Ljava/lang/String;");
    if (env->ExceptionCheck()) { env->ExceptionClear(); mid = nullptr; }
    if (!mid) {
        std::printf("input:   describeInputDevices() not found - R8 renamed it, or it is not built\n");
    } else {
        jobject s = env->CallObjectMethod(activity, mid);
        if (env->ExceptionCheck()) { env->ExceptionDescribe(); env->ExceptionClear(); s = nullptr; }
        if (s) {
            const char* utf = env->GetStringUTFChars(static_cast<jstring>(s), nullptr);
            if (utf) {
                std::printf("%s\n", utf);
                env->ReleaseStringUTFChars(static_cast<jstring>(s), utf);
            }
            env->DeleteLocalRef(s);
        }
    }
    std::fflush(stdout);
    env->DeleteLocalRef(cls);
    env->DeleteLocalRef(activity);
}

// `privateRoot` is `context.filesDir` — the ONE directory this process can certainly write to before
// any folder has been granted, which is why the tee starts there and moves later. Empty skips the file
// entirely and leaves logcat as the only sink: a degraded bring-up, not a failure to launch.
void redirect_stdio_to_logcat(const std::string& privateRoot) {
    // Unbuffered for the same reason main.cpp is: a buffer that dies with the process takes the only
    // record of the boot with it.
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::setvbuf(stderr, nullptr, _IONBF, 0);

    // Opened BEFORE the pump starts, so the very first banner line is already teed. A failure here
    // leaves the pointer null and costs nothing.
    if (!privateRoot.empty()) {
        const std::string path = privateRoot + "/" + kLogFileName;
        g_logFile     = std::fopen(path.c_str(), "w");
        g_logFileSize = 0;
        if (g_logFile) g_logPath = path;
    }

    int pfd[2];
    if (pipe(pfd) != 0) return;  // No console is a degraded bring-up, not a failure to launch.

    // ⚠️ KEEP THE ORIGINALS, so the redirect is UNDOABLE: with no pump, stdout is a pipe nobody
    // reads, which fills its 64 KB in minutes and blocks the SDL thread's next `printf` forever.
    const int savedOut = dup(STDOUT_FILENO);
    const int savedErr = dup(STDERR_FILENO);

    dup2(pfd[1], STDOUT_FILENO);
    dup2(pfd[1], STDERR_FILENO);
    close(pfd[1]);

    pthread_t t;
    if (pthread_create(&t, nullptr, log_pump, reinterpret_cast<void*>((intptr_t)pfd[0])) == 0) {
        pthread_detach(t);
        if (savedOut >= 0) close(savedOut);
        if (savedErr >= 0) close(savedErr);
        return;
    }

    // The pump never ran. Put the descriptors back and drop the pipe, so writing to stdout is
    // harmless again — degraded to "no console", not to a deadlock.
    if (savedOut >= 0) { dup2(savedOut, STDOUT_FILENO); close(savedOut); }
    if (savedErr >= 0) { dup2(savedErr, STDERR_FILENO); close(savedErr); }
    close(pfd[0]);
    __android_log_print(ANDROID_LOG_WARN, kLogTag,
                        "log pump thread did not start - console goes to logcat only");
}

// ─── What the speaker actually runs at ───────────────────────────────────────────────────────────

/**
 * Read one `AudioManager` property as an int, or 0 if the platform will not say.
 *
 * ⚠️ **THE PROPERTY NAME IS READ OFF THE FRAMEWORK CLASS, NOT SPELLED OUT HERE.** Both constants are
 * public static Strings on `android.media.AudioManager`, so taking them from the field costs one JNI
 * lookup and cannot drift from the platform. Spelling the value in a literal would compile forever
 * and be wrong in silence.
 *
 * ⚠️ Every name below belongs to the FRAMEWORK, so R8 has nothing to rename and this needs no
 * `-keep` — unlike a callback that resolves into our own Kotlin, which does.
 */
int audio_manager_property(JNIEnv* env, jobject audioManager, jclass amClass,
                           const char* constantField) {
    const jfieldID fid = env->GetStaticFieldID(amClass, constantField, "Ljava/lang/String;");
    if (fid == nullptr) { env->ExceptionClear(); return 0; }
    const jstring key = (jstring)env->GetStaticObjectField(amClass, fid);
    if (key == nullptr) return 0;

    const jmethodID getProperty = env->GetMethodID(amClass, "getProperty",
                                                   "(Ljava/lang/String;)Ljava/lang/String;");
    if (getProperty == nullptr) { env->ExceptionClear(); env->DeleteLocalRef(key); return 0; }

    const jstring val = (jstring)env->CallObjectMethod(audioManager, getProperty, key);
    env->DeleteLocalRef(key);
    if (env->ExceptionCheck()) { env->ExceptionClear(); return 0; }
    if (val == nullptr) return 0;

    const char* chars = env->GetStringUTFChars(val, nullptr);
    const int   out   = chars ? std::atoi(chars) : 0;
    if (chars) env->ReleaseStringUTFChars(val, chars);
    env->DeleteLocalRef(val);
    return out;
}

/**
 * The device's own output rate and burst size, for Oboe to open at instead of guessing.
 *
 * ⚠️ ONLY JAVA KNOWS THESE: `AudioManager` names the rate the HAL mixes at and the block size it
 * hands out; asking for anything else costs a resampler, which commonly loses the fast path. Both
 * 0 when the platform declines; the backend then leaves Oboe's defaults alone.
 */
void query_device_audio_defaults(int& sampleRate, int& framesPerBurst) {
    sampleRate = framesPerBurst = 0;

    JNIEnv* env      = (JNIEnv*)SDL_AndroidGetJNIEnv();
    jobject activity = (jobject)SDL_AndroidGetActivity();   // ⚠️ a LOCAL ref SDL leaves us to release
    if (env == nullptr || activity == nullptr) return;

    // ⚠️ EVERY LOOKUP IS CLEARED THE MOMENT IT MISSES: a pending exception makes the next JNI call
    // a programming error that CheckJNI aborts for.
    jclass  actClass = env->GetObjectClass(activity);
    if (env->ExceptionCheck()) env->ExceptionClear();
    jclass  ctxClass = env->FindClass("android/content/Context");
    if (env->ExceptionCheck()) env->ExceptionClear();
    jclass  amClass  = env->FindClass("android/media/AudioManager");
    if (env->ExceptionCheck()) env->ExceptionClear();
    jobject am       = nullptr;

    if (actClass && ctxClass && amClass) {
        const jfieldID audioSvc = env->GetStaticFieldID(ctxClass, "AUDIO_SERVICE",
                                                        "Ljava/lang/String;");
        if (env->ExceptionCheck()) env->ExceptionClear();
        const jmethodID getService = env->GetMethodID(actClass, "getSystemService",
                                                      "(Ljava/lang/String;)Ljava/lang/Object;");
        if (env->ExceptionCheck()) env->ExceptionClear();

        if (audioSvc && getService) {
            jstring name = (jstring)env->GetStaticObjectField(ctxClass, audioSvc);
            am = env->CallObjectMethod(activity, getService, name);
            if (env->ExceptionCheck()) { env->ExceptionClear(); am = nullptr; }
            if (name) env->DeleteLocalRef(name);
        }
    }

    if (am != nullptr) {
        sampleRate     = audio_manager_property(env, am, amClass, "PROPERTY_OUTPUT_SAMPLE_RATE");
        framesPerBurst = audio_manager_property(env, am, amClass, "PROPERTY_OUTPUT_FRAMES_PER_BUFFER");
        env->DeleteLocalRef(am);
    }

    if (actClass) env->DeleteLocalRef(actClass);
    if (ctxClass) env->DeleteLocalRef(ctxClass);
    if (amClass)  env->DeleteLocalRef(amClass);
    env->DeleteLocalRef(activity);

    __android_log_print(ANDROID_LOG_INFO, kLogTag,
                        "device audio: rate=%d burst=%d%s", sampleRate, framesPerBurst,
                        (sampleRate == 0 && framesPerBurst == 0)
                                ? " (platform would not say - Oboe keeps its own defaults)" : "");
}

}  // namespace

// ─── called by Java, so OUTSIDE the anonymous namespace — a name in there is not exported ────────
extern "C" JNIEXPORT jboolean JNICALL
Java_com_conanizer_pockettracker_MainActivity_nativeIsPlaying(JNIEnv*, jobject) {
    return g_bgPlaying.load() ? JNI_TRUE : JNI_FALSE;
}

extern "C" JNIEXPORT void JNICALL
Java_com_conanizer_pockettracker_MainActivity_nativeSetServiceStarted(JNIEnv*, jobject, jboolean started) {
    g_bgServiceStarted.store(started == JNI_TRUE);
}

extern "C" JNIEXPORT void JNICALL
Java_com_conanizer_pockettracker_PlaybackService_nativeRequestStop(JNIEnv*, jclass) {
    g_bgStopRequested.store(true);
}

int main(int argc, char** argv) {
    // ⚠️ THE ROOTS FIRST, then the redirect: the pump tees into a file and must know where before
    // the first line. The lines below use `__android_log_print` directly.
    // argv[0] is the application name SDLActivity supplies; the root is the first real argument.
    std::string appRoot = (argc > 1 && argv[1] && argv[1][0]) ? argv[1] : std::string();
    if (appRoot.empty()) {
        __android_log_print(ANDROID_LOG_ERROR, kLogTag,
                            "no app root in argv - the activity's getArguments() should pass one. "
                            "Falling back to %s",
                            kFallbackAppRoot);
        appRoot = kFallbackAppRoot;
    }

    // ⚠️ argv[2] IS `context.filesDir`, NOT A SECOND COPY OF THE ROOT: settings.json, template.ptp
    // and autosave.ptp are read during boot, before any grant, so they live in app-private storage —
    // or the quit-time save would write defaults over unreadable settings. `config.json` stays in the
    // media tree. Missing, this falls back to the media root, as on desktop.
    const std::string privateRoot =
        (argc > 2 && argv[2] && argv[2][0]) ? std::string(argv[2]) : appRoot;

    // ⚠️ THE PRIVATE ROOT, NOT THE MEDIA ROOT: the media tree is unwritable until a folder is
    // granted, and the refusal shows only under MediaProvider's own logcat tag. `relocate_log_file`
    // moves it later.
    redirect_stdio_to_logcat(privateRoot);

    // ⚠️ THE BACK BUTTON, TRAPPED. Untrapped, back runs `SDLActivity.onBackPressed()` → `finish()`,
    // closing the activity mid-edit. Trapped, the key reaches native as `SDLK_AC_BACK`, mapped to B
    // (sdl-input.cpp). Java reads the hint when back is pressed; setting it before `SDL_Init` is
    // belt-and-braces.
    // ⚠️ It works only while `android:enableOnBackInvokedCallback` is unset (the targetSdk 34
    // default): a targetSdk bump that opts into predictive back silently un-traps it.
    SDL_SetHint(SDL_HINT_ANDROID_TRAP_BACK_BUTTON, "1");

    // ⚠️ **THE NATIVE THREAD IS NOT FROZEN OFF THE SCREEN**, so a song left playing goes on being fed
    // by the lookahead pump (app.cpp, on_app_event). Read ONCE, when the video device is created, so
    // BEFORE `SDL_Init` — it cannot be switched per pause. The price is the loop's: it must draw nothing
    // while backgrounded (SDL has backed up the GL context) and must wait long when nothing plays.
    SDL_SetHint(SDL_HINT_ANDROID_BLOCK_ON_PAUSE, "0");

    // ⚠️ NO `SDL_INIT_AUDIO`: Oboe owns the device here, and two libraries must not share an output
    // stream. `SdlAudioEngine::openStream` initialises the subsystem itself where it is used.
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER) != 0) {
        __android_log_print(ANDROID_LOG_ERROR, kLogTag, "SDL_Init failed: %s", SDL_GetError());
        return 1;
    }

    // ⚠️ HEAP, not a local: AudioEngine's members blow a stack — and this runs on SDLActivity's
    // thread, whose stack is smaller than a desktop main's.
    auto engine = std::make_unique<AudioEngine>();

    OboeAudioEngine audio(engine.get());

    // ⚠️ BEFORE openStream, not after: these are what the stream opens AT. Asked here because SDL is
    // up by now and the query needs its JNI env and the activity.
    int deviceRate = 0, deviceBurst = 0;
    query_device_audio_defaults(deviceRate, deviceBurst);
    audio.setPlatformDefaults(deviceRate, deviceBurst);
    if (!privateRoot.empty()) audio.setSlowOpenMarker(privateRoot + "/audio-slow-open");

    if (!audio.openStream()) {
        __android_log_print(ANDROID_LOG_ERROR, kLogTag, "openStream failed - no audio device");
        SDL_Quit();
        return 1;
    }

    // ⚠️ THE STORAGE ACCESS FRAMEWORK IS THE ONLY WAY TO USER FILES: the app declares no storage
    // permission, so a `StdFileSystem` over the media tree would list nothing. It still OWNS a
    // `StdFileSystem` over `privateRoot` for the boot-time files. Constructed here to outlive `run()`.
    // ⚠️ The hooks go in before anything can open a file: un-installed, a render writes every byte
    // and produces no file.
    ptshell::SafFileSystem filesystem(privateRoot);
    filesystem.install_file_hooks();

    // The COUNT, unconditionally: an empty browser under a grant and under none look alike. Zero is
    // the fresh install — the browser opens on the roots directory's ADD FOLDER… row.
    std::printf("saf:     %d granted folder(s)\n", filesystem.root_count());

    // ⚠️ THE MEDIA ROOT IS THE GRANTED TREE, NOT `appRoot` (argv[1], unreadable to this process).
    // `pt://roots` when nothing is granted, so paths built on it fail to open rather than resolve wrong.
    const std::string mediaRoot = filesystem.home_root_path();

    // ⚠️ The log moves NOW, not earlier: it goes through `pt_fopen`, which needs the hooks.
    // Unconditional either way — a log with no reachable destination looks fine from inside the app.
    {
        const std::string moved = filesystem.has_grant() ? relocate_log_file(mediaRoot) : std::string();
        if (moved.empty()) log_carry_done();   // it stays where it is; give the carry buffer back

        if (!moved.empty())
            std::printf("log:     %s\n", moved.c_str());
        else if (!g_logPath.empty())
            std::printf("log:     %s (app-private - grant a folder for a copy you can reach)\n",
                        g_logPath.c_str());
        else
            std::printf("log:     logcat only - no file could be opened\n");
    }

    ptshell::AppConfig cfg;
    cfg.engine     = engine.get();
    cfg.audio      = &audio;
    cfg.appRoot    = mediaRoot;
    cfg.filesystem = &filesystem;

    // No command line: the app opens the blank NEW PROJECT document, and the browser reaches the
    // songs. ⚠️ mediaBaseDir must be the tree the samples are actually in, or a portable project
    // loads looking correct and plays silence.
    cfg.mediaBaseDir = mediaRoot;

    // ⚠️ `converged()`, not `sdl()` or `android()` — the profile this app runs: the touch layouts,
    // BTN SOUND/VIBRO and CRT overlay rows on, `sdl()`'s `appExit` and RESUME row kept
    // (platform_caps.h::converged).
#ifdef NDEBUG
    cfg.caps = ui::PlatformCaps::converged(/*debug_build=*/false);
#else
    cfg.caps = ui::PlatformCaps::converged(/*debug_build=*/true);
#endif

    // On by default and worth it: with the pump above, the banner and the status line land in logcat,
    // which is the only console this platform has.
    cfg.console = true;

    // This is a phone: draw the on-screen gamepad when no physical controller is plugged and there
    // is room (the shell decides both). Not `PlatformCaps::touchLayouts`, the row that picks one.
    cfg.touchCapable = true;

    // ⚠️ "Is there a real pad?" is answered by Java, NOT SDL's joystick count, which counts the
    // emulator's keyboard as a pad (app.h physicalGamepadPresent).
    cfg.physicalGamepadPresent = android_has_physical_gamepad;

    // The evidence behind the line above, written down once. See android_log_input_devices: the
    // layout gate's inputs are otherwise unrecoverable from a user's report.
    android_log_input_devices();

    // ⚠️ `cfg.windowed = true` IS AN ORIENTATION DECISION: it becomes `SDL_WINDOW_RESIZABLE`, which
    // makes the activity FULL_USER (free to rotate) instead of SENSOR_LANDSCAPE. Held landscape the
    // phone keeps its 2x window; held portrait it gets the PORTRAIT2 skin (app.cpp picks by aspect).
    // ⚠️ It also makes a landscape-native handheld (the AYANEO) rotatable — landscape stays 2x (a
    // function of the output size), but a deliberate rotate shows PORTRAIT2 there too.
    cfg.windowed = true;

    // ⚠️ A RELEASE BUILD ON A PHONE IS PINNED TO PORTRAIT, which makes the line above safe: FULL_USER
    // would let the sensor reach the landscape touch panels, a layout SETTINGS cannot name or leave.
    // DEBUG leaves the hint empty so those panels stay drivable. A device with a PHYSICAL pad (the
    // AYANEO) takes the FULL layout and is not pinned.
    // ⚠️ LAUNCH-TIME ONLY: the hint is read at window creation; `cfg.allowLandscape` below is the live
    // half. "Portrait PortraitUpsideDown" → SENSOR_PORTRAIT with a resizable window.
#ifdef NDEBUG
    if (!android_has_physical_gamepad()) {
        SDL_SetHint(SDL_HINT_ORIENTATIONS, "Portrait PortraitUpsideDown");
        std::printf("orient:  portrait only (release, no physical pad)\n");
    } else {
        std::printf("orient:  free (release, physical pad present)\n");
    }
#else
    std::printf("orient:  free (debug build)\n");
#endif

    // The live half of the same rule (app.h `allowLandscape`). ⚠️ UNCONDITIONAL, unlike the boot
    // hint: the landscape panels reached by unplugging a pad are as confusing in debug as in release.
    // They stay drivable on a desktop with POCKETTRACKER_TOUCH=1 and a wide window.
    cfg.allowLandscape = [](bool allowed) { android_set_landscape_allowed(allowed); };

    cfg.background.publishPlaying  = [](bool playing) { g_bgPlaying.store(playing); };
    cfg.background.serviceStarted  = [] { return g_bgServiceStarted.load(); };
    cfg.background.takeStopRequest = [] { return g_bgStopRequested.exchange(false); };
    cfg.background.end             = [] { android_stop_playback_service(); };

    // ⚠️ NULL: off the screen the process is frozen or killed without notice (unless the playback
    // service holds it), so a flag polled by this loop would never run. The autosave flushes in an
    // `SDL_AddEventWatch` watcher on the Java activity thread instead.
    cfg.terminate_requested = nullptr;

    // The button-feedback sink, constructed here so it outlives `run()` (desktop leaves it null).
    AndroidButtonFeedback buttonFeedback;
    cfg.buttonFeedback = &buttonFeedback;

    // ── EXTERNAL MIDI out ────────────────────────────────────────────────────────────────────────
    //
    // Attached UNCONDITIONALLY — the MIDI screen needs the ENUMERATOR to tell "no devices" from "no
    // backend". Constructed here so it outlives `run()`. No env-var block: the device pick comes from
    // settings.json via `InputDispatcher::boot_midi_port()`, as for the OUTPUT row.
    ptshell::AndroidMidiOut midiOut;
    cfg.midiOut = &midiOut;

    // ── MIDI IN ──────────────────────────────────────────────────────────────────────────────────
    //
    // Same terms: attached unconditionally, constructed here — ⚠️ it must outlive the `SongcoreHost`
    // whose queue it delivers into, and `run()` closes it before returning — and opened from
    // settings.json via `InputDispatcher::boot_midi_in_port()`. POLLED once a frame (midi-in-android.cpp).
    ptshell::AndroidMidiIn midiIn;
    cfg.midiIn = &midiIn;

    const int rc = ptshell::run(cfg);

    SDL_Quit();
    return rc;
}
