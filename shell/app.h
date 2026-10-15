// The shared shell: boot, frame loop, teardown. A platform's `main` fills an `AppConfig`; the
// tracker it gets is the same everywhere. Platform differences arrive as values and borrowed
// objects in `AppConfig`, never as #ifdefs here.

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

// The outputs SETTINGS > AUDIO OUT switches between; index 0 is the system output. Only Windows has
// one (ASIO); elsewhere `AppConfig::audioOutputs` is null and the row is hidden.
class AudioOutputSelector {
  public:
    virtual ~AudioOutputSelector() = default;
    virtual const std::vector<std::string>& names() const = 0;
    // The output playing now — not necessarily the one last asked for.
    virtual int active() const = 0;
    // Switch at the engine's current rate. On failure the previous output is back and `error` says why.
    virtual bool select(int index, std::string& error) = 0;
};

// What the shared shell is given rather than decides; filled by the platform's `main` before `run()`.
struct AppConfig {
    // Both borrowed; `audio->openStream()` has already succeeded. ⚠️ `AudioEngine` must be
    // heap-allocated — its scratch buffers blow a 1 MB stack.
    AudioEngine*  engine = nullptr;
    AudioBackend* audio  = nullptr;
    // Null where the platform has only one output; when set, it is the same object as `audio`.
    AudioOutputSelector* audioOutputs = nullptr;

    // The data root (Projects/, Samples/, …): the host takes it as a string to re-root projects from
    // another install, the UI reaches disk only through `filesystem`.
    std::string         appRoot;
    pt::ui::FileSystem* filesystem = nullptr;

    // The project to open at boot, already read; empty = a blank NEW PROJECT (PortMaster passes no
    // arguments). `projectPath` is for the console only; `mediaBaseDir` is where relative sample
    // paths resolve, and must not be empty (see `set_media_base_dir` in the .cpp).
    std::string projectBlob;
    std::string projectPath;
    std::string mediaBaseDir;

    // Which SETTINGS rows exist and whether PROJECT has an EXIT (ui/platform_caps.h).
    pt::ui::PlatformCaps caps;

    // The help banner and the once-a-second status line; off where stdout goes nowhere (an APK).
    bool console = true;

    // A window the user can resize; it opens at the largest integer scale that fits the display.
    // ⚠️ Android must leave it false: it becomes `SDL_WINDOW_RESIZABLE`, which lets the activity
    // rotate into portrait.
    bool windowed = false;

    // The hardware can be touched, so the virtual gamepad is drawn when no physical pad is present.
    // Not `PlatformCaps::touchLayouts`, which is the SETTINGS row.
    bool touchCapable = false;

    // Click + haptic for a virtual button. Borrowed, nullable — only Android has one.
    ButtonFeedback* buttonFeedback = nullptr;

    // The external MIDI port, borrowed and nullable. ⚠️ Null is not "MIDI off": an EXTERNAL
    // instrument is silent. Attached even with no port open — the MIDI screen needs the enumerator.
    // `midiOffsetMs` / `midiOutDevice` are env-var overrides copied over settings.json, so they
    // persist on quit like any setting.
    songcore::IMidiOut* midiOut = nullptr;
    int                 midiOffsetMs = 0;
    std::string         midiOutDevice;   // empty = no override; else the resolved device NAME

    // The MIDI input port, borrowed and nullable. ⚠️ The shell never opens it, not even for the env
    // override: bytes would arrive before `SongcoreHost` exists. `InputDispatcher::boot_midi_in_port`
    // opens the resolved NAME. A `MidiInBase*` so the exit report can ask how many bytes it received.
    MidiInBase* midiIn = nullptr;
    std::string midiInDevice;    // empty = no override; else the resolved device NAME

    // POCKETTRACKER_MIDI_SYNC: −1 = no override, 0/1 = force; persists on quit like `midiOutDevice`.
    int                 midiSyncOut = -1;

    // Android only, nullable: is a physical pad present? ⚠️ SDL's joystick list lies on a phone — it
    // counts the emulator's keyboard (KEYBOARD | DPAD) as a pad. Elsewhere `SdlInput::controller_count()`
    // answers. Re-asked on every controller add/remove.
    std::function<bool()> physicalGamepadPresent;

    // Android only, nullable, called on change: may the screen rotate into landscape (true while the
    // FULL layout is in force)? ⚠️ The boot orientation hint is read once, so this re-applies it live.
    std::function<void(bool)> allowLandscape;

    // Polled once a frame; true ends the session as an unclean exit, keeping the autosave. Desktop's
    // SIGTERM/SIGINT flag. Null on Android, which flushes from an `SDL_AddEventWatch` watcher instead
    // (SDL freezes the native thread on pause).
    std::function<bool()> terminate_requested;

    // Android: a song playing when the app leaves the screen keeps playing under a notification.
    // Null elsewhere. ⚠️ Java starts the service in `onPause` (Android refuses one started from the
    // background); the shell only publishes whether it plays and asks whether the service came up.
    struct BackgroundPlayback {
        std::function<void(bool)> publishPlaying;   // every tick: is the transport running
        std::function<bool()>     serviceStarted;   // did onPause bring the playback service up
        std::function<bool()>     takeStopRequest;  // the notification's STOP, once per press
        std::function<void()>     end;              // the song has stopped: take the service down
    } background;
};

// Boot, run until asked to stop, save, tear down; returns the exit code. Every way out reaches the
// same flush (see the .cpp for why an asked-for exit leaves no autosave).
int run(const AppConfig& cfg);

}  // namespace ptshell

#endif  // POCKETTRACKER_APP_H
