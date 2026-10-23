// The Android entry point. `app.cpp` boots, runs and tears down as on desktop; what differs from
// `main.cpp`: argv is the media root then `filesDir` (from SDLActivity), no signal handlers,
// SDLActivity owns the surface, Oboe for audio, `SafFileSystem` (no storage permission), and
// `PlatformCaps::converged`.
//
// ⚠️ No `SDL_MAIN_HANDLED`, unlike `main.cpp`: SDL renames `main` to `SDL_main`, the symbol
// SDLActivity looks up. Defined, the app dies at start-up with a missing entry point.

// <cmath> before <SDL.h> — see the note in sdl-audio-engine.h (M_PI, _USE_MATH_DEFINES, C4005).
#include <cmath>

#include <SDL.h>

#include "audio-engine.h"
#include "common/byte_source.h"       // pt_fopen — the log file's tee follows the app into a granted tree
#include "oboe-audio-engine.h"
#include "ui/platform_caps.h"

#include "app.h"
#include "skin/button_feedback.h"
#include "android/android-jni.h"
#include "android/saf-filesystem.h"    // the ONLY ui::FileSystem here; Android-only by construction
#include "android/midi-in-android.h"    // the MIDI INPUT port;    compiles to nothing elsewhere
#include "android/midi-out-android.h"   // the EXTERNAL MIDI port; compiles to nothing elsewhere

#include <android/log.h>
#include <jni.h>
#include <pthread.h>
#include <sys/system_properties.h>
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

// Only Java knows where Documents is; the activity passes it in `getArguments()`. The fallback keeps
// a bring-up going, and says so in the log.
constexpr const char* kFallbackAppRoot = "/storage/emulated/0/Documents/PocketTracker";

// ─── stdout/stderr → logcat ───────────────────────────────────────────────────────────────────────
//
// On Android stdout goes nowhere, so a pipe and a pump send it to logcat — and to
// `pockettracker-log.txt`, which a user can reach without a PC. Truncated at start, capped.
// ⚠️ The file starts in `filesDir`, the only directory certainly writable at the first line, and
// moves into the granted tree once boot knows it (`relocate_log_file`), carrying what was written.
std::mutex  g_logMutex;           // g_logFile is swapped on the SDL thread and written on the pump's
FILE*       g_logFile     = nullptr;   // null = logcat alone
size_t      g_logFileSize = 0;
std::string g_logPath;            // what g_logFile is open on; "" = nothing open
constexpr size_t      kLogFileCap  = 512 * 1024;
constexpr const char* kLogFileName = "pockettracker-log.txt";

// Everything written so far, for the relocation to carry across. ⚠️ Held in memory, not re-read
// from the first file (that would race the pump); appended under the write's lock, so no line is
// lost or doubled.
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
    // Flushed per line: a buffer that dies with the process takes the boot with it.
    // ⚠️ `fprintf` returns negative on error; added raw to the unsigned size it would stop the log.
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

// Moves the tee into `dir` (a granted `pt://` tree) with everything written so far; returns the new
// path, or "" if it could not (the first file then stands). ⚠️ Through `pt_fopen`: after the hooks.
// The `filesDir` copy is kept — on the run where the move is what failed, it is the only one.
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
        // A line with no '\n' would grow for ever; flush long fragments.
        if (line.size() > 1024) {
            log_line(line.c_str());
            line.clear();
        }
    }
    return nullptr;
}

// The activity's method `name`, or null with a warning saying what its absence costs.
jmethodID activity_method(const ptshell::Attached& a, const char* name, const char* sig,
                          const char* otherwise) {
    const jmethodID mid = a.method(name, sig);
    if (!mid) __android_log_print(ANDROID_LOG_WARN, kLogTag, "%s%s not found - %s", name, sig, otherwise);
    return mid;
}

// ─── button feedback → the thin Kotlin managers ──────────────────────────────────────────────────
//
// The touch layer decides when a button clicks (sdl-touch.cpp); this carries it to Kotlin. Runs on
// the SDL thread; looked up by name, so a mismatch is silence plus one log line.
class AndroidButtonFeedback : public ptshell::ButtonFeedback {
public:
    void play(pt::ui::Button button, bool down, const ptshell::ButtonFeedbackSettings& s) override {
        // Both off → no JNI round trip.
        if (!s.soundEnabled && !s.vibroEnabled) return;

        ptshell::Attached a;
        if (!a.ok()) return;
        if (const jmethodID mid = method_id(a)) {
            a.env->CallVoidMethod(a.activity, mid, static_cast<jint>(button),
                                  static_cast<jboolean>(down),
                                  static_cast<jboolean>(s.soundEnabled), static_cast<jint>(s.soundVolume),
                                  static_cast<jboolean>(s.vibroEnabled), static_cast<jint>(s.vibroPower));
            a.failed();
        }
    }

private:
    // Resolved once (the activity's class is never unloaded); also keeps a missing method from
    // logging on every tap.
    jmethodID method_id(const ptshell::Attached& a) {
        if (looked_up_) return mid_;
        looked_up_ = true;
        mid_ = activity_method(a, "onButtonFeedback", "(IZZIZI)V", "button feedback disabled");
        return mid_;
    }

    bool      looked_up_ = false;
    jmethodID mid_       = nullptr;
};

// ─── does a physical game controller exist? → SdlActivity ────────────────────────────────────────
//
// SDL counts the emulator's keyboard as a controller; Kotlin tells them apart. Asked at boot and on
// a controller add/remove; a missing method means "no pad" (the touch UI).
bool android_has_physical_gamepad() {
    ptshell::Attached a;
    if (!a.ok()) return false;
    const jmethodID mid = activity_method(a, "hasPhysicalGameButtons", "()Z", "assuming no pad (touch UI)");
    if (!mid) return false;
    const bool result = a.env->CallBooleanMethod(a.activity, mid) == JNI_TRUE;
    return !a.failed() && result;
}

// ─── may the screen rotate into landscape? → SdlActivity ─────────────────────────────────────────
//
// The live half of the orientation lock: `SDL_HINT_ORIENTATIONS` is read once, so a pad unplugged
// mid-session would leave landscape over the portrait skin. Kotlin sets `requestedOrientation`.
void android_set_landscape_allowed(bool allowed) {
    ptshell::Attached a;
    if (!a.ok()) return;
    const jmethodID mid = activity_method(a, "setLandscapeAllowed", "(Z)V", "orientation stays as launched");
    if (!mid) return;
    a.env->CallVoidMethod(a.activity, mid, allowed ? JNI_TRUE : JNI_FALSE);
    if (!a.failed())
        std::printf("orient:  %s\n", allowed ? "landscape allowed (FULL layout)"
                                             : "portrait only (on-screen buttons in force)");
    std::fflush(stdout);
}

// ─── PLAYING ON IN THE BACKGROUND ────────────────────────────────────────────────────────────────
//
// What crosses between the frame loop and Kotlin's UI thread. The service itself is Kotlin's,
// started in `onPause` (MainActivity.kt).
std::atomic<bool> g_bgPlaying{false};          // the transport, published by the loop every tick
std::atomic<bool> g_bgServiceStarted{false};   // onPause brought the playback service up
std::atomic<bool> g_bgStopRequested{false};    // the notification's STOP, waiting for the loop

// A screen recording shows the app (Android 15+); the audio backend reopens its stream around one.
std::atomic<bool> g_playbackCaptured{false};

// The song stopped in the background (or never played on): Kotlin takes the service down.
void android_stop_playback_service() {
    g_bgServiceStarted.store(false);
    ptshell::Attached a;
    if (!a.ok()) return;
    if (const jmethodID mid = activity_method(a, "stopPlaybackService", "()V", "the service stays up")) {
        a.env->CallVoidMethod(a.activity, mid);
        a.failed();
    }
}

// ─── the input-device enumeration, once at boot ──────────────────────────────────────────────────
//
// ⚠️ What a layout bug report needs: `hasPhysicalGameButtons()` logs only a find. Through `printf`,
// so it reaches the log file (Kotlin's `Log` reaches logcat only).
void android_log_input_devices() {
    ptshell::Attached a;
    if (!a.env)      { std::printf("input:   no JNI env - cannot enumerate\n"); return; }
    if (!a.activity) { std::printf("input:   no activity - cannot enumerate\n"); return; }

    if (const jmethodID mid = a.method("describeInputDevices", "()Ljava/lang/String;")) {
        jobject s = a.env->CallObjectMethod(a.activity, mid);
        if (!a.failed() && s) std::printf("%s\n", a.take(s).c_str());
    } else {
        std::printf("input:   describeInputDevices() not found - R8 renamed it, or it is not built\n");
    }
    std::fflush(stdout);
}

// `privateRoot` is `filesDir`, the one place certainly writable before any grant. Empty = logcat only.
void redirect_stdio_to_logcat(const std::string& privateRoot) {
    // Unbuffered, as on desktop: a buffer that dies with the process takes the boot with it.
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::setvbuf(stderr, nullptr, _IONBF, 0);

    // Opened before the pump starts, so the first line is teed. A failure costs nothing.
    if (!privateRoot.empty()) {
        const std::string path = privateRoot + "/" + kLogFileName;
        g_logFile     = std::fopen(path.c_str(), "w");
        g_logFileSize = 0;
        if (g_logFile) g_logPath = path;
    }

    int pfd[2];
    if (pipe(pfd) != 0) return;  // No console is a degraded bring-up, not a failure to launch.

    // ⚠️ Keep the originals so the redirect can be undone: a pipe nobody reads fills in minutes and
    // blocks the next `printf` for ever.
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

    // The pump never ran: put the descriptors back — no console, but no deadlock.
    if (savedOut >= 0) { dup2(savedOut, STDOUT_FILENO); close(savedOut); }
    if (savedErr >= 0) { dup2(savedErr, STDERR_FILENO); close(savedErr); }
    close(pfd[0]);
    __android_log_print(ANDROID_LOG_WARN, kLogTag,
                        "log pump thread did not start - console goes to logcat only");
}

// ─── What the speaker actually runs at ───────────────────────────────────────────────────────────

// One `AudioManager` property as an int, or 0 if the platform will not say. The property name is
// read off the framework's constant, so it cannot drift; framework names need no `-keep`.
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

// The device's own output rate (printed only) and burst (Oboe's default for OpenSL ES). Both 0 when
// the platform declines; Oboe keeps its defaults.
void query_device_audio_defaults(int& sampleRate, int& framesPerBurst) {
    sampleRate = framesPerBurst = 0;

    ptshell::Attached a;
    if (!a.ok()) return;
    JNIEnv* env      = a.env;
    jobject activity = a.activity;

    // ⚠️ Every miss is cleared at once: under CheckJNI a pending exception aborts the next JNI call.
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

extern "C" JNIEXPORT void JNICALL
Java_com_conanizer_pockettracker_MainActivity_nativeSetPlaybackCaptured(JNIEnv*, jobject, jboolean captured) {
    g_playbackCaptured.store(captured == JNI_TRUE);
}

int main(int argc, char** argv) {
    // The roots first: the log pump tees into a file and must know where. argv[0] is the app name.
    std::string appRoot = (argc > 1 && argv[1] && argv[1][0]) ? argv[1] : std::string();
    if (appRoot.empty()) {
        __android_log_print(ANDROID_LOG_ERROR, kLogTag,
                            "no app root in argv - the activity's getArguments() should pass one. "
                            "Falling back to %s",
                            kFallbackAppRoot);
        appRoot = kFallbackAppRoot;
    }

    // argv[2] is `filesDir`: settings, template and autosave are read at boot, before any grant.
    // Missing, this falls back to the media root, as on desktop.
    const std::string privateRoot =
        (argc > 2 && argv[2] && argv[2][0]) ? std::string(argv[2]) : appRoot;

    // The private root: the media tree is unwritable until a folder is granted.
    redirect_stdio_to_logcat(privateRoot);

    // `adb shell setprop debug.pockettracker.latency 1` turns the latency probe on (there is no
    // environment here). Before the first frame: the probe reads its switch once.
    char latencyProp[PROP_VALUE_MAX] = {};
    if (__system_property_get("debug.pockettracker.latency", latencyProp) > 0 && latencyProp[0] == '1')
        setenv("POCKETTRACKER_LATENCY", "1", 1);

    // ⚠️ Trap the back button, or SDLActivity finishes the activity mid-edit; trapped, it arrives as
    // `SDLK_AC_BACK`, mapped to B. Works only while `enableOnBackInvokedCallback` is unset — a
    // targetSdk bump into predictive back silently un-traps it.
    SDL_SetHint(SDL_HINT_ANDROID_TRAP_BACK_BUTTON, "1");

    // ⚠️ The native thread keeps running off the screen, so a song left playing is still fed. Read
    // once, before `SDL_Init`. The loop must then draw nothing while backgrounded, and sleep when idle.
    SDL_SetHint(SDL_HINT_ANDROID_BLOCK_ON_PAUSE, "0");

    // No `SDL_INIT_AUDIO`: Oboe owns the output here.
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER) != 0) {
        __android_log_print(ANDROID_LOG_ERROR, kLogTag, "SDL_Init failed: %s", SDL_GetError());
        return 1;
    }

    // ⚠️ Heap: AudioEngine blows a stack, and SDLActivity's thread has a small one.
    auto engine = std::make_unique<AudioEngine>();

    OboeAudioEngine audio(engine.get());

    // Before openStream: the stream opens at these. Needs SDL up, for the JNI env.
    int deviceRate = 0, deviceBurst = 0;
    query_device_audio_defaults(deviceRate, deviceBurst);
    audio.setPlatformDefaults(deviceRate, deviceBurst);
    if (!privateRoot.empty()) audio.setSlowOpenMarker(privateRoot + "/audio-slow-open");
    audio.setPlaybackCaptured(&g_playbackCaptured);

    if (!audio.openStream()) {
        __android_log_print(ANDROID_LOG_ERROR, kLogTag, "openStream failed - no audio device");
        SDL_Quit();
        return 1;
    }

    // The only way to user files (no storage permission); it owns a `StdFileSystem` over
    // `privateRoot` for the boot files. ⚠️ Hooks in before anything opens a file, or a render
    // produces no file.
    ptshell::SafFileSystem filesystem(privateRoot);
    filesystem.install_file_hooks();

    // Printed always: an empty browser looks the same with a grant and without.
    std::printf("saf:     %d granted folder(s)\n", filesystem.root_count());

    // ⚠️ The media root is the granted tree, not argv[1] (unreadable here); `pt://roots` with no grant.
    const std::string mediaRoot = filesystem.home_root_path();

    // ⚠️ The log moves now, through `pt_fopen`, which needs the hooks. Where it went is always printed.
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

    // ⚠️ The tree the samples are in, or a portable project loads fine and plays silence.
    cfg.mediaBaseDir = mediaRoot;

    // `converged()`: the touch layouts, BTN SOUND/VIBRO and overlay rows, plus `sdl()`'s exit and
    // RESUME row.
#ifdef NDEBUG
    cfg.caps = ui::PlatformCaps::converged(/*debug_build=*/false);
#else
    cfg.caps = ui::PlatformCaps::converged(/*debug_build=*/true);
#endif

    // With the pump above, the banner and status line reach logcat.
    cfg.console = true;

    // A phone: the on-screen gamepad when no controller is plugged and there is room.
    cfg.touchCapable = true;

    // ⚠️ Kotlin answers, not SDL's joystick count, which counts the emulator's keyboard.
    cfg.physicalGamepadPresent = android_has_physical_gamepad;

    // The evidence behind that answer, once.
    android_log_input_devices();

    // ⚠️ An orientation decision: a resizable window lets the activity rotate. Landscape keeps its 2×
    // window; portrait gets the PORTRAIT2 skin — on a landscape handheld too, if rotated on purpose.
    cfg.windowed = true;

    // ⚠️ A release build on a phone is pinned to portrait, or the sensor could reach the landscape
    // touch panels, which no SETTINGS row names. Debug leaves them reachable; a device with a pad
    // takes FULL and is not pinned. Launch-time only — `cfg.allowLandscape` is the live half.
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

    // The live half (app.h `allowLandscape`), in debug too: the landscape panels reached by
    // unplugging a pad confuse there as well. On desktop: POCKETTRACKER_TOUCH=1 and a wide window.
    cfg.allowLandscape = [](bool allowed) { android_set_landscape_allowed(allowed); };

    cfg.background.publishPlaying  = [](bool playing) { g_bgPlaying.store(playing); };
    cfg.background.serviceStarted  = [] { return g_bgServiceStarted.load(); };
    cfg.background.takeStopRequest = [] { return g_bgStopRequested.exchange(false); };
    cfg.background.end             = [] { android_stop_playback_service(); };

    // ⚠️ Null: off the screen the process is frozen or killed without notice, so a polled flag never
    // runs. The autosave is an event watcher instead (app.cpp).
    cfg.terminate_requested = nullptr;

    // Outlives `run()`.
    AndroidButtonFeedback buttonFeedback;
    cfg.buttonFeedback = &buttonFeedback;

    // ── EXTERNAL MIDI out ────────────────────────────────────────────────────────────────────────
    //
    // Attached always — the MIDI screen tells "no devices" from "no backend" by it. The saved device
    // is opened from settings.json at boot.
    ptshell::AndroidMidiOut midiOut;
    cfg.midiOut = &midiOut;

    // ── MIDI IN ──────────────────────────────────────────────────────────────────────────────────
    //
    // The same; ⚠️ it must outlive the `SongcoreHost` it delivers into (`run()` closes it). Polled.
    ptshell::AndroidMidiIn midiIn;
    cfg.midiIn = &midiIn;

    const int rc = ptshell::run(cfg);

    SDL_Quit();
    return rc;
}
