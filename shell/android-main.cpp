// PocketTracker — the ANDROID entry point. Everything in this file is platform residue.
//
// Convergence C3, and the file `app.h`'s diagram has had a placeholder for since C0.2. It is the
// sibling of `main.cpp`: the shared shell (`app.cpp`) boots, runs and tears down identically on both,
// and what differs is exactly the list C0.2 named. Set the two side by side and the difference IS the
// port:
//
//   main.cpp (desktop/handheld)              android-main.cpp (this file)
//   ─────────────────────────────────────    ──────────────────────────────────────────────────────
//   argv: project, media dir, app root       argv: the app root, handed down by SDLActivity
//   SIGTERM/SIGINT → a flag the loop polls   nothing (⚠️ C4 — see the terminate_requested note below)
//   SDL_Init / SDL_Quit, here                SDL_Init here too; SDLActivity owns the surface, not this
//   SdlAudioEngine                           OboeAudioEngine  ← the whole of C3's audio work
//   default_app_root() + StdFileSystem       the root from Java + StdFileSystem (⚠️ C5, see below)
//   PlatformCaps::sdl(debug), console on     PlatformCaps::sdl(debug) for now (⚠️ C6 replaces it)
//
// ⚠️ **NO `SDL_MAIN_HANDLED` HERE, AND THAT IS THE OPPOSITE OF `main.cpp`.** SDL_main.h defines
// `SDL_MAIN_NEEDED` on `__ANDROID__` and with it `#define main SDL_main`, so the `main` below is
// compiled as `SDL_main` — which is the symbol `SDLActivity` looks up by name (`getMainFunction()`)
// with `dlsym` in the last library `getLibraries()` names. Define SDL_MAIN_HANDLED as the desktop
// does and the rename does not happen, the symbol is not there, and the app dies at start-up with a
// message about a missing entry point rather than anything about this file.

// <cmath> before <SDL.h> — see the note in sdl-audio-engine.h (M_PI, _USE_MATH_DEFINES, C4005).
#include <cmath>

#include <SDL.h>

#include "audio-engine.h"
#include "oboe-audio-engine.h"
#include "ui/platform_caps.h"
#include "ui/std_filesystem.h"

#include "app.h"
#include "button_feedback.h"
#include "midi-in-android.h"    // the MIDI INPUT port  (MIDI plan E5);  compiles to nothing elsewhere
#include "midi-out-android.h"   // the EXTERNAL MIDI port (MIDI plan B2b); compiles to nothing elsewhere

#include <android/log.h>
#include <jni.h>
#include <pthread.h>
#include <unistd.h>

#include <cstdio>
#include <memory>
#include <string>

namespace ui = pt::ui;

namespace {

constexpr const char* kLogTag = "PocketTrackerSDL";

// ⚠️ **THE APP ROOT COMES FROM JAVA, AND IT IS NOT A STYLE CHOICE.** `ui::default_app_root()` walks
// `POCKETTRACKER_HOME` → `XDG_DATA_HOME` → `HOME`, and on Android all three miss — so it would fall
// through to the RELATIVE path "PocketTracker", i.e. beside whatever the process's cwd happens to be.
// That is character-for-character the A1 bug, which was found on Windows for the same reason: the
// platform nobody resolved a root for is already broken. Only Java knows where
// `Environment.getExternalStoragePublicDirectory(DIRECTORY_DOCUMENTS)` actually is on this device and
// this OS version, so the activity resolves it and passes it down through `getArguments()`.
//
// The fallback below exists so a bring-up cannot be blocked by a missing argument, and it SAYS SO in
// the log rather than quietly guessing — a silently wrong root would present as "all my projects are
// gone", which is the worst possible way to discover an argv change.
constexpr const char* kFallbackAppRoot = "/storage/emulated/0/Documents/PocketTracker";

// ─── stdout/stderr → logcat ───────────────────────────────────────────────────────────────────────
//
// The shared shell's boot banner and its once-a-second status line are THE bring-up instrument — the
// half of this app that answers "did my samples load?", "where did it put its folders?", "did it find
// my crash file?". On Android they go to a stdout that is `/dev/null` unless somebody has set
// `log.redirect-stdio`, which needs root on most devices. So the two lessons this project has already
// paid for both apply here and neither is satisfied by default: `main.cpp`'s `setvbuf` note (a
// buffered stdout loses everything when the process is killed, which is how a bring-up ends) and P4a's
// (an instrument that is not pointed at the thing tells you nothing about it).
//
// Twenty lines of pipe-and-pump fixes both, and it is platform residue in the strictest sense —
// nothing above this file knows it happened.
//
// ⚠️ **AND THE SAME LINES GO TO A FILE, because logcat is unreachable to the person who has the bug.**
// Reading logcat needs a PC, developer mode and USB debugging; a user reporting "it opened without the
// on-screen buttons" has none of those, and that report is about the boot itself — the one moment
// nobody can be talked through capturing live. So the pump tees into `<appRoot>/pockettracker-log.txt`,
// which the user reaches with any file manager and attaches to a mail. It is TRUNCATED at start (a
// session log, not a history) and capped, so it cannot grow into the user's storage.
FILE*  g_logFile     = nullptr;   // pump thread only, after the pump starts; null = logcat alone
size_t g_logFileSize = 0;
constexpr size_t kLogFileCap = 512 * 1024;

void log_line(const char* s) {
    __android_log_write(ANDROID_LOG_INFO, kLogTag, s);
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

// ─── button feedback → the surviving thin Kotlin managers (convergence D) ───────────────────────────
//
// The ONE outward JNI hook the Phase-E plan names: the shared touch layer decides WHEN a virtual
// button clicks (sdl-touch.cpp), and this shim carries that decision across to Java, where the
// SoundPool and the Vibrator live. It calls a single method on the running `SdlActivity` — `pt-ui` and
// the shared shell never learn the word `jni`; only this file, which is already the platform residue.
//
// ⚠️ Runs on the SDL thread (the frame loop), NOT the Java UI thread. `SDL_AndroidGetJNIEnv` attaches
// this thread to the JVM and hands back its env; the Kotlin side is what marshals the haptic to the UI
// thread where it needs to be (SoundPool is thread-safe and stays put for lowest latency). The method
// is looked up by NAME, so it does not exist at C++ compile time and a mismatch degrades to silence
// with one log line rather than a crash — hence the null-and-exception handling on every JNI call.
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

// ─── does a physical game controller exist? → SdlActivity (convergence D) ────────────────────────────
//
// The shared layout gate (app.cpp `useTouch`) must answer "is there a real pad?" the way the Compose app
// did — and on Android SDL cannot, because `isDeviceSDLJoystick` counts the emulator's keyboard as a
// controller (see app.h `physicalGamepadPresent`). Only `InputDevice.getSources()` tells them apart, and
// it is Java-only, so this JNIs into `SdlActivity.hasPhysicalGameButtons()`, which runs Kotlin's exact
// SOURCE_GAMEPAD/SOURCE_JOYSTICK test. Resolved by name each call — null/exception-safe, degrading to
// "no pad" (the touch UI) rather than a crash, exactly like AndroidButtonFeedback above. Cheap: the frame
// loop only asks at boot and when SDL reports a controller add/remove.
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

// `appRoot` may be empty — then the tee is skipped and logcat is the only sink, which is the
// pre-existing behaviour rather than a failure.
// ─── the input-device enumeration, once at boot ──────────────────────────────────────────────────
//
// ⚠️ **THE ONE THING A LAYOUT BUG REPORT NEEDS, AND THE ONE THING NOTHING RECORDED.** `useTouch` is
// `touchCapable && !physicalPad`, and when a phone lands on FULL there is no way to tell which half
// was wrong — `hasPhysicalGameButtons()` logs only when it FINDS a pad, so the failing case is the
// silent one. This prints the whole enumeration Java saw, through `printf` so the pipe tees it into
// `pockettracker-log.txt` (a `Log.i` from Kotlin reaches logcat only, and logcat needs a PC).
//
// Once, at boot, on the same by-name/exception-safe pattern as the hook above: a missing method
// degrades to one line saying so, never a crash.
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

void redirect_stdio_to_logcat(const std::string& appRoot) {
    // Unbuffered for the same reason main.cpp is: a buffer that dies with the process takes the only
    // record of the boot with it.
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::setvbuf(stderr, nullptr, _IONBF, 0);

    // Opened BEFORE the pump starts, so the very first banner line is already teed. A failure here
    // (no permission yet, no such directory) leaves the pointer null and costs nothing.
    if (!appRoot.empty()) {
        const std::string path = appRoot + "/pockettracker-log.txt";
        g_logFile     = std::fopen(path.c_str(), "w");
        g_logFileSize = 0;
    }

    int pfd[2];
    if (pipe(pfd) != 0) return;  // No console is a degraded bring-up, not a failure to launch.
    dup2(pfd[1], STDOUT_FILENO);
    dup2(pfd[1], STDERR_FILENO);
    close(pfd[1]);

    pthread_t t;
    if (pthread_create(&t, nullptr, log_pump, reinterpret_cast<void*>((intptr_t)pfd[0])) == 0) {
        pthread_detach(t);
    }
}

}  // namespace

int main(int argc, char** argv) {
    // ⚠️ THE ROOT IS RESOLVED FIRST, and the redirect follows it — not the other way round. The pump
    // tees the console into `<appRoot>/pockettracker-log.txt`, so it has to know the root before the
    // first line is written or the banner lands in logcat alone. The two lines below use
    // `__android_log_print` directly and so do not need the redirect to be up.
    // argv[0] is the application name SDLActivity supplies; the root is the first real argument.
    std::string appRoot = (argc > 1 && argv[1] && argv[1][0]) ? argv[1] : std::string();
    if (appRoot.empty()) {
        __android_log_print(ANDROID_LOG_ERROR, kLogTag,
                            "no app root in argv - the activity's getArguments() should pass one. "
                            "Falling back to %s",
                            kFallbackAppRoot);
        appRoot = kFallbackAppRoot;
    }

    redirect_stdio_to_logcat(appRoot);

    // ⚠️ **THE BACK BUTTON, TRAPPED BEFORE SDL_Init (C4).** Untrapped, Android's back runs
    // `SDLActivity.onBackPressed()` → `finish()`, which closes the activity out from under the frame
    // loop mid-edit — and it is the easiest button on a phone to hit by accident. Set, the activity
    // ignores it (SDLActivity.java:623 returns early) and the key still reaches native as
    // `SDLK_AC_BACK`, which sdl-input.cpp maps to B, the app's own cancel.
    //
    // ⚠️ This is read by JAVA, through `nativeGetHintBoolean`, at the moment back is pressed — so it
    // is a hint about the activity's behaviour rather than about any subsystem, and setting it before
    // `SDL_Init` is belt-and-braces rather than a requirement.
    //
    // ⚠️ It works because `android:enableOnBackInvokedCallback` is NOT set in the manifest: at
    // targetSdk 34 that defaults to false, so the legacy `onBackPressed` path SDL hooks is still the
    // one Android uses. A future targetSdk bump that opts into predictive back silently un-traps this
    // — the symptom being the app closing on back again, with nothing here having changed.
    SDL_SetHint(SDL_HINT_ANDROID_TRAP_BACK_BUTTON, "1");

    // ⚠️ NO `SDL_INIT_AUDIO`, and on this platform it is load-bearing rather than tidy: Oboe owns the
    // device here. Asking SDL for the audio subsystem as well would put two libraries on one output
    // stream — convergence-plan §1's "SDL and Oboe are not a choice". `SdlAudioEngine::openStream`
    // initialises the subsystem itself on the platforms that use it, which is what lets this line be
    // identical to the desktop's; see native/audio-backend.h.
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER) != 0) {
        __android_log_print(ANDROID_LOG_ERROR, kLogTag, "SDL_Init failed: %s", SDL_GetError());
        return 1;
    }

    // ⚠️ HEAP, not a local — the same 0xC00000FD the desktop would hit, and worse here: Android's
    // default thread stack is smaller than a desktop's, and this runs on SDLActivity's thread rather
    // than a process main. AudioEngine's per-block DSP scratch, spectrum rings and 256-slot table pool
    // are members.
    auto engine = std::make_unique<AudioEngine>();

    OboeAudioEngine audio(engine.get());
    if (!audio.openStream()) {
        __android_log_print(ANDROID_LOG_ERROR, kLogTag, "openStream failed - no audio device");
        SDL_Quit();
        return 1;
    }

    // ⚠️ **THIS LINE IS C5's SPIKE, ANSWERED BY RUNNING RATHER THAN BY READING.** The open question
    // the plan wanted settled in week one is whether `std::filesystem` can reach
    // `/storage/emulated/0` from native code with MANAGE_EXTERNAL_STORAGE granted. `StdFileSystem` is
    // the same implementation the desktop uses, so if the file browser lists projects on device the
    // answer is yes and `AndroidFileSystem.kt` (324 lines) dies with the rest of the Kotlin in Phase
    // E; if it does not, this one line becomes a JNI-backed `pt::ui::FileSystem` and NOTHING ELSE IN
    // THE TREE CHANGES — the interface has been abstract since S6a precisely so that the worst case
    // is a second implementation rather than a redesign.
    //
    // ⚠️ And that is also the SAF contingency. If the fdroiddata review forces Storage Access
    // Framework, `std::filesystem` is off the table regardless of what this spike measures, because
    // SAF is a Java-only API — the answer would then be the same second implementation, arrived at
    // for a different reason. The seam is what makes both outcomes cheap.
    ui::StdFileSystem filesystem(appRoot);

    ptshell::AppConfig cfg;
    cfg.engine     = engine.get();
    cfg.audio      = &audio;
    cfg.appRoot    = appRoot;
    cfg.filesystem = &filesystem;

    // No command line, so no project and no media dir of its own: the app opens the blank document
    // NEW PROJECT makes and the file browser is how the user reaches their songs — exactly as the
    // shipping handheld target already behaves (PortMaster invokes the binary with no arguments).
    // ⚠️ mediaBaseDir is never empty: an empty base resolves relative sample paths against the
    // process's cwd, and the app root is where Samples/ actually lives.
    cfg.mediaBaseDir = appRoot;

    // ⚠️ **`converged()`, NOT `sdl()` OR `android()` — the profile the converged Android app RUNS.**
    // Its three device rows (touch layouts, BTN SOUND/VIBRO, the CRT overlay) are all on because
    // their FEATURES now exist in the shell (Phases D–D6); it keeps `sdl()`'s `appExit` and RESUME
    // row and leaves `engineToggle` off (no Kotlin sequencer left to switch to). See
    // `platform_caps.h::converged` for why it is neither of the other two profiles. This replaces the
    // three hand-flipped `cfg.caps.X = true` overrides those phases added one at a time — the value
    // is byte-identical, now named. ⚠️ ptinput's goldens are unaffected: they compare against
    // `PlatformCaps::android()`, Kotlin's row map, not this runtime choice.
#ifdef NDEBUG
    cfg.caps = ui::PlatformCaps::converged(/*debug_build=*/false);
#else
    cfg.caps = ui::PlatformCaps::converged(/*debug_build=*/true);
#endif

    // On by default and worth it: with the pump above, the banner and the status line land in logcat,
    // which is the only console this platform has.
    cfg.console = true;

    // ⚠️ **PHASE D: this is a phone, so draw the on-screen gamepad** — when no physical controller is
    // plugged and the letterbox bars have room (the shell decides both). This is NOT the same as
    // flipping `PlatformCaps::touchLayouts`: that is the SETTINGS row that lets the user PICK a layout,
    // and it stays off until PORTRAIT and the skinned grid exist to be picked, so the picker never
    // offers a mode that does nothing (platform_caps.h's own rule). Desktop and the handhelds leave
    // this false — main.cpp says nothing, so the default (false) is the safe answer there.
    cfg.touchCapable = true;

    // ⚠️ **HOW "IS THERE A REAL PAD?" IS ANSWERED ON ANDROID — NOT by SDL's joystick count.** SDL opens
    // the emulator's keyboard (`qwerty2`, a DPAD source with a BUTTON_A keylayout) as a game controller, so
    // `SdlInput::controller_count()` reads 1 with no pad attached and the shell would drop the touch UI to
    // a bare fullscreen frame with an empty LAYOUT row. Only Android's InputDevice source flags tell the
    // keyboard from a pad, so the gate asks Java. Desktop/handheld leave this null and fall back to the SDL
    // count, which IS the truth there. See app.h physicalGamepadPresent and android_has_physical_gamepad.
    cfg.physicalGamepadPresent = android_has_physical_gamepad;

    // The evidence behind the line above, written down once. See android_log_input_devices: the
    // layout gate's inputs are otherwise unrecoverable from a user's report.
    android_log_input_devices();

    // ⚠️ **`cfg.windowed = true` UNLOCKS PORTRAIT, AND THAT IS AN ORIENTATION DECISION, NOT A COSMETIC
    // ONE.** It becomes `SDL_WINDOW_RESIZABLE`, which SDL hands straight to
    // `SDLActivity.setOrientationBis` (SDL_androidwindow.c:52): a NON-resizable window takes its
    // orientation from `w > h` and locks to SENSOR_LANDSCAPE for the 640x480 design, while a RESIZABLE
    // one becomes SCREEN_ORIENTATION_FULL_USER, free to follow the sensor into PORTRAIT. Through C4 this
    // was deliberately FALSE, because a rotation into portrait had no layout to land on and would have
    // shown a broken letterboxed screen. Phase D's PORTRAIT2 device skin is that layout, so it flips to
    // true here: held LANDSCAPE the phone still gets C4's pixel-exact 2x window (FULL_USER stays
    // landscape while the device is), and held PORTRAIT it now gets the skin — app.cpp switches on the
    // output aspect, with nothing to keep in sync. Read out of the vendored SDL source, not remembered.
    //
    // ⚠️ **THIS ALSO MAKES A LANDSCAPE-NATIVE HANDHELD (the AYANEO) ROTATABLE**, where C4 proved its
    // geometry with the flag false. Landscape is preserved — the 2x integer scale is a function of the
    // OUTPUT SIZE, not this flag — but a deliberate rotate would now show PORTRAIT2 there too. That is
    // the one behaviour change this slice makes to a C4-proven config, and it is worth a re-check on
    // that device.
    cfg.windowed = true;

    // ⚠️ **NULL, AND C4 IS WHERE THIS GETS ITS ANSWER — NOT HERE.** The desktop polls a SIGTERM flag
    // through this hook once a frame. Android must not: SDL freezes the native thread when the
    // activity pauses (`SDL_HINT_ANDROID_BLOCK_ON_PAUSE`, on by default), which is precisely when the
    // process is most likely to be killed, so a flag consumed by this loop is P4d's never-armed write
    // in a new body — it would read correct and never run. C4's autosave flushes in an
    // `SDL_AddEventWatch` watcher, which fires synchronously on the Java activity thread and does not
    // touch this loop at all. Leaving it null is the honest state: nothing asks this app to
    // terminate yet.
    cfg.terminate_requested = nullptr;

    // The button-feedback sink (convergence D). Constructed HERE so it outlives `run()`, and only on
    // Android — desktop's `main.cpp` leaves `cfg.buttonFeedback` null and the shared touch path treats
    // that as "no feedback". See button_feedback.h and AndroidButtonFeedback above.
    AndroidButtonFeedback buttonFeedback;
    cfg.buttonFeedback = &buttonFeedback;

    // ── EXTERNAL MIDI out (MIDI plan phase B2b) ──────────────────────────────────────────────────
    //
    // Attached UNCONDITIONALLY, and — read app.h — null is NOT "MIDI off". `SongcoreHost` attaches its
    // `ExternalConsumer` either way; what this pointer decides is whether the bytes have anywhere to
    // go. The MIDI screen needs the ENUMERATOR even on a phone with nothing plugged in, or its OUTPUT
    // row cannot tell "no devices" from "no backend" — which is the state the whole row exists to make
    // visible. Constructed HERE so it outlives `run()`, like the feedback sink above.
    //
    // ⚠️ NO ENV-VAR BLOCK, unlike shell/main.cpp. `POCKETTRACKER_MIDI_OUT` and friends are a desktop
    // bring-up console; on Android there is no shell to set them from, and the device pick comes from
    // settings.json through `InputDispatcher::boot_midi_port()` — which `ptshell::run` calls below,
    // and which is the same path the OUTPUT row uses. One owner of "which port is open".
    ptshell::AndroidMidiOut midiOut;
    cfg.midiOut = &midiOut;

    // ── MIDI IN (MIDI plan phase E5) ─────────────────────────────────────────────────────────────
    //
    // The same terms as the output port above, and the same three rules: attached UNCONDITIONALLY (the
    // MIDI screen's INPUT row needs the enumerator even on a phone with nothing plugged in), constructed
    // HERE so it outlives `run()` — ⚠️ which is not a style point but the E2 lifetime rule: the queue
    // this port delivers into lives inside the `SongcoreHost` that `run()` owns, so the port must be the
    // longer-lived of the two and `run()` closes it before returning — and NO env-var block, because the
    // device pick comes from settings.json through `InputDispatcher::boot_midi_in_port()`.
    //
    // ⚠️ Unlike the other two backends this one is POLLED, once a frame, from inside `run()`. See
    // midi-in-android.cpp: `MidiManager` delivers on a binder thread to the Kotlin side and the frame
    // loop fetches, which costs nothing because the drain is on that loop either way.
    ptshell::AndroidMidiIn midiIn;
    cfg.midiIn = &midiIn;

    const int rc = ptshell::run(cfg);

    SDL_Quit();
    return rc;
}
