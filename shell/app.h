// app.{h,cpp} — the SHARED shell: the boot sequence, the frame loop, the teardown. A platform's
// `main` builds an `AppConfig`, and the tracker it gets is the same on every platform.
//
//     ┌─────────────────────────────────────────────┐
//     │  native/ui       the screens, the canvas     │  ← portable, no SDL
//     │  native/songcore the sequencer, the project  │  ← portable, no SDL
//     │  native/         the engine, the DSP         │  ← portable, no SDL
//     ├─────────────────────────────────────────────┤
//     │  app.{h,cpp}     boot · frame loop · exit    │  ← SDL, but the SAME on every platform
//     │  sdl-video · sdl-input                       │  ← SDL, and likewise shared
//     ├─────────────────────────────────────────────┤
//     │  main.cpp        argv · signals · console    │  ← the desktop's platform residue
//     │  android-main.cpp                            │  ← Android's
//     └─────────────────────────────────────────────┘
//
// Platform differences arrive as VALUES and borrowed objects in `AppConfig`: the audio backend
// (`audio-backend.h`), the filesystem (`pt::ui::FileSystem`), the caps, and nullable hooks.
// ⚠️ The lifecycle source is one nullable callback (`terminate_requested`). Android does not use
// it: SDL freezes the native thread when the activity pauses, so its autosave flushes in an
// `SDL_AddEventWatch` watcher instead.

#ifndef POCKETTRACKER_APP_H
#define POCKETTRACKER_APP_H

#include "ui/filesystem.h"
#include "ui/platform_caps.h"

#include <functional>
#include <string>
#include <vector>

class AudioBackend;
class AudioEngine;

namespace songcore { struct IMidiOut; }

namespace ptshell {

class ButtonFeedback;
class MidiInBase;

/**
 * The outputs a platform can switch between while running — SETTINGS > AUDIO OUT. Index 0 is always
 * the system output. Only Windows has one (its ASIO drivers); elsewhere `AppConfig::audioOutputs` is
 * null and the row is hidden.
 */
class AudioOutputSelector {
  public:
    virtual ~AudioOutputSelector() = default;
    virtual const std::vector<std::string>& names() const = 0;
    /** The output that is playing now — not necessarily the one last asked for (see `select`). */
    virtual int active() const = 0;
    /** Switch to `index`, at the engine's current rate. On failure the previous output is back on and
     *  `error` says why, in a few words. */
    virtual bool select(int index, std::string& error) = 0;
};

/**
 * Everything the shared shell is GIVEN rather than decides. Every field is filled by the platform's
 * `main` before `run()` is called; nothing here is read after it returns.
 */
struct AppConfig {
    /**
     * The portable engine and the platform's device for it, both borrowed. ⚠️ `AudioEngine` must be
     * heap-allocated — its per-block scratch, spectrum rings and table pool blow a 1 MB stack.
     *
     * `audio->openStream()` must already have SUCCEEDED; the platform reports that failure on its
     * own console, before a window exists.
     */
    AudioEngine*  engine = nullptr;
    AudioBackend* audio  = nullptr;
    /** Null where the platform has only one output. When set, `audio` is the same object. */
    AudioOutputSelector* audioOutputs = nullptr;

    /**
     * Where Projects/ Samples/ Soundfonts/ Instruments/ Renders/ Themes live, and the filesystem
     * rooted at it. Two fields rather than one because the HOST is told the root as a string
     * (`set_app_root`, for re-rooting a project authored on another install) while the UI reaches
     * disk only through the interface — see ui/filesystem.h.
     */
    std::string         appRoot;
    pt::ui::FileSystem* filesystem = nullptr;

    /**
     * The project to open at boot, ALREADY READ. Empty blob = start on the blank document
     * `NEW PROJECT` makes, which is what the shipping handheld target always does (PortMaster invokes
     * the binary with no arguments at all).
     *
     * `projectPath` is for the console line only; `mediaBaseDir` is where the project's RELATIVE
     * sample paths resolve — see the note on `set_media_base_dir` in the .cpp, which is why an empty
     * one is not allowed to mean "the process's cwd".
     */
    std::string projectBlob;
    std::string projectPath;
    std::string mediaBaseDir;

    /**
     * Which SETTINGS rows exist, and whether PROJECT has an EXIT — a VALUE, not an #ifdef
     * (ui/platform_caps.h), chosen by the caller.
     */
    pt::ui::PlatformCaps caps;

    /**
     * The multi-line help banner and the once-a-second status line. On by default because they are
     * the bring-up instrument for a device with no screen yet; a platform whose stdout goes nowhere
     * (an APK's does) turns them off and pays nothing.
     */
    bool console = true;

    /**
     * A WINDOWED host — one whose window the user can drag to any size. The window opens at the
     * largest integer multiple of the design that fits the display, which also gives SETTINGS >
     * SCALING somewhere to show.
     *
     * ⚠️⚠️ ANDROID MUST LEAVE THIS FALSE — FOR ORIENTATION, NOT RESIZING: it becomes
     * `SDL_WINDOW_RESIZABLE`, which lets the activity rotate into PORTRAIT (sdl-video.h::open).
     * PortMaster is unaffected: a 640×480 panel computes 1×, and KMSDRM has no window manager.
     */
    bool windowed = false;

    /**
     * This platform has a touchscreen, so the shell draws the virtual gamepad when there is no
     * physical one and room for it. A phone sets it; desktop and handhelds leave it false. ⚠️ Not
     * `PlatformCaps::touchLayouts` (the SETTINGS row that picks a layout) — this just says the
     * hardware can be touched.
     */
    bool touchCapable = false;

    /**
     * The click + haptic a VIRTUAL button gives back. BORROWED and NULLABLE: only Android constructs
     * one (a JNI shim into the Kotlin managers); null means no feedback, so the shared touch path
     * never learns the word `jni`. Paired with `caps.buttonFeedback`, which shows the BTN rows.
     */
    ButtonFeedback* buttonFeedback = nullptr;

    /**
     * The EXTERNAL MIDI port. BORROWED and NULLABLE — opening a port is the per-platform part of MIDI
     * (winmm, ALSA rawmidi, `MidiManager` over JNI); everything above it is songcore/midi_out.h.
     *
     * ⚠️ NULL is not "MIDI off": an EXTERNAL instrument is then silent on both sides. Attached
     * whether or not a port is open — the MIDI screen needs the ENUMERATOR, and `ExternalConsumer`
     * checks `is_open()` before every byte.
     *
     * `midiOffsetMs` and `midiOutDevice` are ENV-VAR OVERRIDES: `app.cpp` copies a non-empty one
     * over settings.json's value, so there is ONE answer to "which port is open". ⚠️ An override
     * therefore PERSISTS on quit, like any other setting.
     */
    songcore::IMidiOut* midiOut = nullptr;
    int                 midiOffsetMs = 0;
    std::string         midiOutDevice;   // empty = no override; else the resolved device NAME

    /**
     * The MIDI INPUT port. Borrowed and nullable on the same terms as `midiOut`.
     *
     * ⚠️ NULL IS NOT "MIDI IN OFF": the host owns the queue, parser and router unconditionally, so
     * "no cable" is distinguishable from "no track listening on that channel".
     *
     * ⚠️ THE SHELL NEVER OPENS THIS ONE, not even for the env override: an open port delivers bytes
     * at once, into a `SongcoreHost` that does not exist yet. `POCKETTRACKER_MIDI_IN` resolves to a
     * NAME (`MidiInBase::resolve_spec`); `InputDispatcher::boot_midi_in_port` opens it, wiring the sink.
     *
     * ⚠️ `MidiInBase*`, not the songcore interface: the exit report asks the PORT how many bytes it
     * received — "nothing was sent" versus "received and lost later".
     */
    MidiInBase* midiIn = nullptr;
    std::string midiInDevice;    // empty = no override; else the resolved device NAME

    /**
     * SYNC OUT override — `POCKETTRACKER_MIDI_SYNC`. −1 = no override, 0/1 = force. An `int` so "the
     * user said nothing" exists (a false default would overwrite a SYNC the user turned on); persists
     * on quit like `midiOutDevice`. It makes the clock jitter measurement scriptable.
     */
    int                 midiSyncOut = -1;

    /**
     * Is a PHYSICAL game controller present right now? NULLABLE and Android-only. The layout gate
     * (app.cpp `useTouch`) asks it to choose the on-screen gamepad vs a full-bleed frame; elsewhere
     * it is null and the gate uses `SdlInput::controller_count()`.
     *
     * ⚠️ ANDROID NEEDS IT BECAUSE SDL's JOYSTICK LIST LIES ON A PHONE: SDL opens any device with a
     * GAMEPAD or a bare DPAD source, so the emulator's built-in keyboard (`KEYBOARD | DPAD`) counts as
     * a pad and the touch layout vanishes. `SdlActivity.hasPhysicalGameButtons()` counts only
     * `SOURCE_GAMEPAD`/`SOURCE_JOYSTICK`. The gate re-asks on every controller add/remove.
     */
    std::function<bool()> physicalGamepadPresent;

    /**
     * May the screen rotate into LANDSCAPE right now? NULLABLE and Android-only, called on CHANGE
     * from the layout gate: true exactly while the FULL layout is in force (a pad, no on-screen
     * buttons).
     *
     * ⚠️ THE BOOT HINT IS NOT ENOUGH: `SDL_HINT_ORIENTATIONS` is read once, so a pad unplugged later
     * would leave the activity free to sit in landscape over the portrait skin. Re-applied live.
     */
    std::function<void(bool)> allowLandscape;

    /**
     * Polled once a frame; true ends the session as an UNCLEAN exit, so the autosave is kept.
     *
     * Desktop hands its SIGTERM/SIGINT flag through here. ⚠️ May be null, and Android's is (see the
     * header note).
     */
    std::function<bool()> terminate_requested;

    /**
     * Android: a song that is PLAYING when the app leaves the screen keeps playing, under a
     * notification with STOP. Null everywhere else, and then leaving the screen stops it as before.
     *
     * ⚠️ The service is started by JAVA, in `onPause`, while the activity still counts as in front —
     * Android refuses a foreground service started from the background. So the shell does not ask
     * for it; it publishes whether it is playing, and asks afterwards whether the service came up.
     */
    struct BackgroundPlayback {
        std::function<void(bool)> publishPlaying;   // every tick: is the transport running
        std::function<bool()>     serviceStarted;   // did onPause bring the playback service up
        std::function<bool()>     takeStopRequest;  // the notification's STOP, once per press
        std::function<void()>     end;              // the song has stopped: take the service down
    } background;
};

/**
 * Boot, run until something asks to stop, save, and tear down. Returns a process exit code.
 *
 * ⚠️ Every way out of the loop arrives at the same flush — a launcher's kill, a window close, F10
 * and PROJECT → EXIT. See the flush comment in the .cpp for why the one exit that ASKED leaves no
 * autosave.
 */
int run(const AppConfig& cfg);

}  // namespace ptshell

#endif  // POCKETTRACKER_APP_H
