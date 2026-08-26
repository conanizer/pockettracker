#include "app.h"

// <cmath> before <SDL.h> — see the note in sdl-audio-engine.h (M_PI, _USE_MATH_DEFINES, C4005).
#include <cmath>

#include <SDL.h>

#include "audio-backend.h"
#include "audio-engine.h"
#include "songcore/host.h"
#include "ui/app_state.h"
#include "ui/button_mapper.h"
#include "ui/buttons.h"
#include "ui/canvas.h"
#include "ui/engine_feed.h"
#include "ui/input_dispatcher.h"
#include "ui/layout.h"
#include "ui/modules/oscilloscope.h"   // WAVEFORM_SIZE — the C7 audible test
#include "ui/settings_store.h"

#include "device_skin.h"
#include "skin.h"
#include "overlay.h"
#include "font.h"
#include "portrait2.h"

#include "midi-in.h"      // the platform's IMidiIn, or nothing (E2: Windows only)
#include "midi-sender.h"
#include "sdl-input.h"
#include "sdl-touch.h"
#include "sdl-video.h"

#include <cstdio>

using namespace songcore;
namespace ui = pt::ui;

namespace ptshell {

namespace {

// ─── THE BACKGROUND WATCHER (C4) ─────────────────────────────────────────────────────────────────
//
// Everything the "the platform is taking the app away" handler needs, borrowed from `run`'s frame.
// ⚠️ It lives on `run`'s STACK, so the watcher MUST be removed before `run` returns — see the
// SDL_DelEventWatch below the loop. A watcher outliving its userdata is a use-after-free that fires
// during teardown, which is the least debuggable moment available.
// ─── THE MIDI-IN CONSOLE (phase E2) ──────────────────────────────────────────────────────────────
//
// ⭐⭐ **THE INSTRUMENT FOR A COMPONENT WHOSE CORRECT BEHAVIOUR IS SILENCE.** A MIDI input that is
// working and one that never received a byte look identical from every seat in the app: no sound, no
// screen, no log. So every stage of the path counts, and the counts are printed beside the verdict at
// exit — bytes at the PORT (`MidiInBase`), bytes and messages at the QUEUE and PARSER
// (`SongcoreHost`), records at the ROUTER, and here the split between messages that produced a record
// and messages that produced none.
//
// ⚠️ That last split is the one nothing else can make, and it is the difference between "the cable is
// dead" and "no track is listening on that channel" — two problems whose fixes have nothing in common.
//
// It runs on the FRAME LOOP's thread (`SongcoreHost::poll` drains, this is called from there), so it
// may print. The backend's own callback may not — see midi-in-base.h.
struct MidiInConsole : songcore::IMidiInObserver {
    bool     trace    = false;   // POCKETTRACKER_MIDI_IN_TRACE=1
    uint64_t messages = 0;
    uint64_t records  = 0;
    uint64_t silent   = 0;       // messages that produced no bus record at all

    void on_midi_in(const songcore::MidiInMessage& m, const songcore::Event* ev, int n) override {
        ++messages;
        records += static_cast<uint64_t>(n < 0 ? 0 : n);
        if (n <= 0) ++silent;
        if (!trace) return;

        // ⚠️ The bytes are RECONSTRUCTED from the message rather than remembered from the wire, so a
        // trace line that disagrees with the cable is a parser bug this can actually show. A channel
        // message's status byte is the nibble plus the channel; a system one is the whole byte.
        const unsigned status = m.is_channel() ? (m.status | m.channel) : m.status;
        std::printf("midi ->  %02X", status);
        if (m.len > 1) std::printf(" %02X", m.data1);
        if (m.len > 2) std::printf(" %02X", m.data2);
        std::printf("%*s-> %d event(s)", (m.len > 2 ? 3 : (m.len > 1 ? 6 : 9)), "", n);
        for (int i = 0; i < n; ++i)
            std::printf("  [track %d inst %d]", ev[i].track, static_cast<int>(ev[i].instrument));
        std::printf("\n");
        std::fflush(stdout);
    }
};

struct BackgroundContext {
    SongcoreHost*        host       = nullptr;
    ui::InputDispatcher* dispatch   = nullptr;
    ui::FileSystem*      filesystem = nullptr;
    ui::AppState*        state      = nullptr;
    bool                 console    = false;
};

/**
 * `SDL_APP_WILLENTERBACKGROUND` — the Android Home press, and the last moment this process is
 * guaranteed to run.
 *
 * ⚠️⚠️ **THE PLAN SAID THIS FIRES ON THE JAVA ACTIVITY THREAD. IT DOES NOT, AND THE DIFFERENCE IS
 * THE WHOLE DESIGN OF THIS FUNCTION.** Read out of the vendored SDL 2.30.9 rather than remembered:
 *
 *   1. `SDLActivity.onPause()` → `nativePause()` (SDL_android.c:1264) does exactly ONE thing:
 *      `SDL_SemPost(Android_PauseSem)`. It sends no event and calls nothing of ours.
 *   2. The NATIVE thread discovers that semaphore inside its own `SDL_PollEvent` →
 *      `Android_PumpEvents_Blocking` (SDL_androidevents.c:156), which is what sends
 *      `SDL_APP_WILLENTERBACKGROUND` — and `SDL_PushEvent` dispatches event watchers SYNCHRONOUSLY
 *      (SDL_events.c:1184) before the event is ever queued.
 *   3. Only on the NEXT pump does `isPaused` send the thread into `SDL_SemWait(Android_ResumeSem)`
 *      and freeze it.
 *
 * So this runs **on the native thread, inside the frame loop's own `SDL_PollEvent` call**, one full
 * frame before anything freezes. There is no concurrency with the loop because it IS the loop — which
 * is why this touches `host`, `state` and the project directly with no lock, and why a lock would in
 * fact have been the bug (the loop is not running to release it).
 *
 * ⚠️ A poll-loop `case SDL_APP_WILLENTERBACKGROUND:` would ALSO work here, for a reason worth writing
 * down: `SDL_PollEvent` pumps only when no poll sentinel is pending (SDL_events.c:1092), so a
 * `while (SDL_PollEvent(...))` drain pumps exactly once and cannot block partway through. The watcher
 * is still the right mechanism — it does not depend on that property surviving a future refactor of
 * the loop, and SDL's own `nativeSendQuit` says in as many words that "the user should have handled
 * state storage in SDL_APP_WILLENTERBACKGROUND", because after a quit it FLUSHES the event queue.
 *
 * ⚠️ Inert everywhere else, by construction rather than by `#ifdef`: SDL sends this event on Android
 * and iOS only, so on desktop this function is installed and never called.
 *
 * ⚠️ Every step is IDEMPOTENT, because background→foreground→background fires it again: `stop()` on a
 * stopped transport does nothing, `flush_autosave` is a no-op on a clean document, and
 * `save_settings_if_changed` compares against the bytes on disk.
 */
int SDLCALL on_app_event(void* userdata, SDL_Event* e) {
    if (e->type != SDL_APP_WILLENTERBACKGROUND) return 0;

    auto* c = static_cast<BackgroundContext*>(userdata);

    // ⚠️ **UNCONDITIONAL, AND IT IS THE ONLY LINE HERE THAT ALWAYS PRINTS — ON PURPOSE.** Every step
    // below is a no-op in the common case (nothing playing, document clean, settings unmoved), so a
    // watcher that ran perfectly and a watcher that never fired at all produce EXACTLY the same empty
    // log. That ambiguity is not hypothetical: it is what the first device test of this function ran
    // into — the autosave was on disk afterwards and there was no way to tell whether this code had
    // written it or the 3 s debounce had beaten it there. A lifecycle handler whose correct behaviour
    // is silence needs one line saying it woke up, or it cannot be told from a handler that is never
    // installed.
    if (c->console) std::printf("lifecycle: entering background\n");

    // ── 1. Playback stops. THE USER'S DECISION, and the alternative is not "it keeps playing" ──
    //
    // SDL freezes the native thread on the next pump, and `host.poll()` — the lookahead pump — rides
    // this loop. Oboe's callback thread is NOT frozen (SDL pauses its OWN audio devices in
    // Android_PumpEvents_Blocking, and it has never heard of Oboe), so the engine would keep pulling
    // samples against a scheduler that has stopped filling the buffer: ~4 s of lookahead drains and
    // the song dies mid-phrase. That is P4c's shape exactly — the bus keeps running, the notes stop
    // arriving — and the only difference between the two options is whether the user hears a defined
    // stop or a song rotting away in the background. Stopping is the honest one.
    if (c->host->is_playing()) {
        c->host->stop();
        if (c->console) std::printf("lifecycle: backgrounded - playback stopped\n");
    }

    // ── 2. The autosave. THE REASON THIS FUNCTION EXISTS ──
    //
    // A backgrounded Android app is killed without further notice, and the 3 s autosave debounce may
    // not have fired. `MainActivity.kt:1054` has flushed on ON_STOP since long before this port for
    // exactly this reason; until C4 the SDL build had NOTHING here, so every Home press risked the
    // last few edits and no crash recovery existed at all on this platform.
    c->dispatch->flush_autosave();

    // ── 3. Settings. THE BUG THE USER REPORTED ──
    //
    // `save_settings_if_changed` is called once, below the frame loop — and on Android the loop is
    // only left on a clean destroy (`nativeSendQuit` → SDL_QUIT). A Home press never reaches it, so
    // settings.json was never written and every setting was back to factory on the next launch.
    switch (ui::save_settings_if_changed(*c->filesystem, c->state->settings, c->state->theme)) {
        using SW = ui::SettingsWrite;
        case SW::UNCHANGED: break;
        case SW::SAVED:
            if (c->console) std::printf("lifecycle: backgrounded - settings saved\n");
            break;
        case SW::FAILED:
            std::printf("lifecycle: backgrounded - settings SAVE FAILED - %s\n",
                        c->filesystem->settings_path().c_str());
            break;
    }
    std::fflush(stdout);

    return 0;  // watchers do not consume; the event still reaches the queue
}

// ─── C7: IS ANYTHING ON SCREEN ACTUALLY MOVING? ──────────────────────────────────────────────────
//
// A DIRECT PORT OF `PixelPerfectRenderer.kt:151-191`, which solved this on Android long before the
// port existed and whose own comment names unconditional repainting "the dominant battery drain on
// the handheld". Kotlin gets two redraw sources: Compose recomposes on a real state change (a cursor
// move, an edit, a new playback row), and an `oscilloscopeTicker` drives ANIMATION — and the loop
// bumps that ticker only while audio is audible, polling cheaply at 20 Hz when it is not.
//
// ⚠️ **THE ONE THING THAT COULD NOT BE COPIED, AND COPYING IT WOULD HAVE ADDED INPUT LAG.** Kotlin's
// visualizer loop is a separate coroutine, so its idle `delay(50L)` slows nothing but itself. Here
// input polling, `host.poll()` and drawing are ALL ONE LOOP: dropping it to 20 Hz would put 50 ms of
// latency on every keypress, on a tracker whose whole point is that a note sounds when you press it.
// So the LOOP stays at 60 Hz and only the DRAW is skipped — the frame still polls input, still pumps
// the scheduler, still runs the watcher. That is the shape of the port, not an approximation of it.
//
// What counts as audible is Kotlin's test exactly: the transport playing, OR any master-waveform
// sample above the silence floor (a one-shot preview still ringing after STOP), OR the preview lane
// active. The threshold is its constant, not a new one.
//
// ⚠️ AUDIBLE IS NOT ANIMATING, and this test answers only the first. Everything that keeps moving
// after the sound has gone — the mixer's markers, the spectrum's bars — is the `metersFalling` term
// below. Kotlin held the same frames open with a 75-frame release tail it re-armed while audible;
// asking the modules whether they are at rest is that, without a number that has to stay true.
constexpr float SCOPE_SILENCE_THRESHOLD = 0.002f;   // PixelPerfectRenderer.kt:101

bool audio_is_audible(const ui::AppState& s) {
    if (s.isPlaying || s.previewLaneActive) return true;

    // ⚠️ Null is SILENCE, not "unknown" — `engine_feed` leaves these null when there is nothing to
    // show, and `ptshot` draws whole screens with no engine at all. Treating null as audible would
    // make the idle path unreachable in exactly the configuration that is idle.
    if (s.waveform) {
        for (int i = 0; i < ui::WAVEFORM_SIZE; ++i) {
            if (std::fabs(s.waveform[i]) > SCOPE_SILENCE_THRESHOLD) return true;
        }
    }
    return false;
}

}  // namespace

int run(const AppConfig& cfg) {
    // The version, in the log, on every platform — a bug report arrives as a log file and a sentence,
    // and nothing else in this program says which build produced it. It sits in the SHARED shell
    // rather than in each platform's entry point, which costs it the first line (the audio and MIDI
    // backends are already open by the time run() is called) and buys it being impossible to have on
    // one platform and not another.
    //
    // PT_VERSION_STRING comes from native/cmake/pt_version.cmake, which reads app/build.gradle.kts.
    // No fallback here on purpose: a tree that forgets the define must fail to COMPILE rather than
    // print a version that is quietly a lie.
    std::printf("PocketTracker %s\n", PT_VERSION_STRING);

    AudioEngine&  engineRef  = *cfg.engine;
    AudioBackend& audio      = *cfg.audio;
    ui::FileSystem& filesystem = *cfg.filesystem;

    // The engine asks for its stream back without knowing what a stream is. Unwired at teardown
    // below, before the backend is closed — a callback firing into a dead lambda is the one ordering
    // mistake this pair can make.
    engineRef.onResumeRequested = [&audio] { audio.resumeStream(); };

    // ⚠️ **DECLARED BEFORE THE HOST, AND THE ORDER IS A LIFETIME FIX, NOT A STYLE CHOICE.** The
    // ExternalConsumer inside `host` calls this observer, and its DESTRUCTOR panics — which emits
    // messages. Declared after the host it would be destroyed first, and that final panic would call
    // into a dead object. It is also what makes the early `return 1` below (a boot project that does not
    // parse) safe: there is no teardown path on which the observer outlives its caller.
    // See the sender-thread block after the port is attached for what fills it.
    MidiJitterRecorder midiJitter;

    // ⚠️ **ALSO BEFORE THE HOST, AND FOR THE SAME KIND OF REASON**: the host calls this observer from
    // `poll()`, and `~SongcoreHost` runs before any object declared after it. Attached unconditionally
    // below — the counters are what make a silent input path readable, so they must exist on the runs
    // where nobody asked for a trace.
    MidiInConsole midiInConsole;

    SongcoreHost host(&engineRef, audio.sampleRate());
    host.set_midi_in_observer(&midiInConsole);
    if (const char* t = SDL_getenv("POCKETTRACKER_MIDI_IN_TRACE"); t && t[0] == '1')
        midiInConsole.trace = true;

    // THIS install's app root — where Projects/ Samples/ Soundfonts/… live (also the FileSystem's root).
    // Handed to the host BEFORE the first load: a project opened at boot may have been authored on
    // another install (a phone's Documents/PocketTracker) whose absolute media paths are dead here until
    // re-rooted onto ours. See set_app_root.
    host.set_app_root(cfg.appRoot);

    // The EXTERNAL MIDI port, if the platform opened one (MIDI plan phase B). Set BEFORE the project
    // loads, so an EXTERNAL instrument in a boot project is routed from its very first note.
    host.set_midi_out(cfg.midiOut);
    host.set_midi_offset_ms(cfg.midiOffsetMs);

    // ── The MIDI sender thread (phase B3) ────────────────────────────────────────────────────────
    //
    // Started on EVERY platform and whether or not a port is open: the queue must be released either
    // way (a LEN gate and a panic's note-offs are owed after the last note was scheduled), and starting
    // it only when a cable is attached would mean the timing path a user exercises is not the one that
    // was measured. `poll()` stops pumping while it runs — see set_midi_pump_external.
    //
    // ⚠️ DEV OVERRIDES, both of them, and the first is B3's own negative control:
    //   POCKETTRACKER_MIDI_SENDER=0   don't start it; the 60 Hz frame loop keeps the job (pre-B3)
    //   POCKETTRACKER_MIDI_JITTER=1   measure every released message and print the numbers on exit
    const bool jitterOn = [] {
        const char* j = SDL_getenv("POCKETTRACKER_MIDI_JITTER");
        return j && j[0] == '1';
    }();
    if (jitterOn) host.midi_out().set_send_observer(&midiJitter);

    // ⚠️ …and the SENDER is declared AFTER the host, for the mirror-image reason: it pumps `host`, so it
    // must be destroyed (and its thread joined — see the destructor) while the host is still alive. That
    // is also what makes the early `return 1` safe on this side.
    MidiSender  midiSender(engineRef, host, audio.sampleRate());
    const char* senderEnv = SDL_getenv("POCKETTRACKER_MIDI_SENDER");
    bool        senderOn  = false;
    if (senderEnv && senderEnv[0] == '0') {
        std::printf("midi:    sender thread DISABLED (POCKETTRACKER_MIDI_SENDER=0) - the 60 Hz frame "
                    "loop releases the queue, as it did before phase B3\n");
    } else {
        senderOn = midiSender.start();
    }

    // A REQUESTED project is one with a path; its bytes are already in `projectBlob`, read by the
    // platform before any window existed to hide a bad path behind.
    const bool hasProject = !cfg.projectPath.empty();

    if (hasProject) {
        if (!host.push_project(cfg.projectBlob)) {
            std::fprintf(stderr, "%s did not parse as a .ptp\n", cfg.projectPath.c_str());
            return 1;
        }

        const MediaLoadResult media = host.load_media(cfg.mediaBaseDir);
        std::printf("project: %s\nmedia:   %d loaded, %d failed (base dir: %s)\n",
                    cfg.projectPath.c_str(), media.loaded, media.failed, cfg.mediaBaseDir.c_str());
        if (media.failed > 0) {
            // ASCII, like every other line this program prints — see the banner below. (This one was NOT:
            // it carried an em-dash, on the one path you most want legible when something has already gone
            // wrong on a device with no screen. Found by grepping the file against its own stated rule.)
            std::fprintf(stderr,
                         "warning: %d sample(s)/SoundFont(s) failed to load - those instruments will be "
                         "silent\n",
                         media.failed);
        }
    } else {
        // The same document NEW PROJECT builds. There is no media to load: a blank project references
        // no samples, so load_media would walk an empty instrument pool and report 0/0.
        host.new_project();
        std::printf("project: (none given) - starting on a blank document\n");
    }

    // ⚠️ **Load the media, then push the PARAMS.** The engine holds a great deal of state on its own
    // behalf that no note ever carries — the mixer, the master bus, reverb, delay, the 128-slot EQ bank,
    // and every instrument's drive / crush / filter / sample window / loop — and until Phase 3 S4 the
    // only thing in the whole tree that pushed any of it was the offline renderer. So this shell would
    // RENDER a project correctly and PLAY the same project on the engine's factory defaults. See
    // songcore::push_live_params: it survived Phase 2 and three Phase-3 sessions because it is invisible
    // to every conformance tool (none of it is an event, none of it is made by a note, and the one tool
    // that compares audio renders — through the path that was already right).
    host.push_params();

    // ── The file system (S6a) ────────────────────────────────────────────────────────────────────
    //
    // ⚠️ Create the app directories NOW, at boot, rather than lazily on the first browse. `ensure_dir`
    // runs inside each getter, so the folders would otherwise not exist until the user opened the
    // browser once — and on a handheld that is exactly backwards. The first thing anyone does with a new
    // port is plug the SD card into a PC and copy their samples in, and they cannot do that if the app
    // has not yet said where. (Android gets this for free: it calls the getters all through start-up.)
    //
    // WHICH filesystem is the platform's business — the root is the only per-platform fact about files,
    // and `pt::ui::FileSystem` has been the seam for it since S6a. The SUB-directory names are identical
    // on every platform on purpose: a user who copies their `PocketTracker/` folder off a phone onto an
    // SD card must find their projects where the app looks.
    filesystem.projects_directory();
    filesystem.samples_directory();
    filesystem.renders_directory();
    filesystem.instruments_directory();
    filesystem.soundfonts_directory();
    filesystem.themes_directory();
    std::printf("files:   %s\n", cfg.appRoot.c_str());

    // ⚠️ The app's OWN files need not live in the tree just named — on Android they do not, because
    // settings/template/autosave are read at boot and the media tree may not be readable then. Say so,
    // and say it by comparing the path the filesystem actually returns against the one the old
    // single-root layout would have produced: a platform that later changes its mind cannot leave this
    // line asserting something stale, and there is no flag to keep in step.
    if (filesystem.settings_path() != cfg.appRoot + "/settings.json")
        std::printf("files:   app files: %s\n", filesystem.settings_path().c_str());

    // ⚠️ **THE COUNT, not just the path.** Every other line of this banner is satisfied by a storage
    // backend that resolves a directory and then lists nothing — the browser comes up empty and looks
    // exactly like an empty folder. One number tells the two apart, and it is the same reason
    // `media: N loaded, M failed` prints a number rather than "media ok".
    {
        const std::string projects = filesystem.projects_directory();
        std::printf("files:   projects: %s (%zu entries)\n", projects.c_str(),
                    filesystem.list_files(projects).size());
    }

    SdlVideo video;
    if (!video.open("PocketTracker", ui::DESIGN_W, ui::DESIGN_H, /*fullscreen=*/false,
                    /*resizable=*/cfg.windowed)) {
        return 1;
    }

    SdlInput input;

    // ⚠️ POCKETTRACKER_INPUT_TRACE=1 — the bring-up instrument for a NEW device or CFW, and the only
    // eye the port has on the layer between the hardware and `ButtonEvent`. ptinput, ptdispatch and
    // ptmapper all start one layer BELOW this: none can see SDL, the CFW's controller mapping, or the
    // launch script — which is precisely the layer P4b's bug lived in.
    //
    // An env var rather than a flag because PortMaster invokes the binary with NO arguments, so a
    // flag would be unreachable on the device this exists for. Off unless asked: it prints per event.
    const char* inputTrace = SDL_getenv("POCKETTRACKER_INPUT_TRACE");
    if (inputTrace && inputTrace[0] == '1') {
        input.set_trace(true);
        std::printf("input:   TRACE ON - every event prints, with what it mapped to (or did not)\n");
    }

    input.open_controllers();

    // ── The on-screen gamepad (Phase D) ────────────────────────────────────────────────────────────
    //
    // Drawn only where the hardware is a touchscreen AND no physical controller is attached — the SDL
    // reading of DeviceAdapter's "does this device have game buttons?". A phone with no pad gets the
    // panels; a handheld (a pad, built-in or plugged) gets FULL and full-bleed, exactly as the Kotlin
    // app decides. `touch.layout()` in the loop below then draws them only if the letterbox bars are
    // actually wide enough (`active()`), so a narrow window shows none regardless. The trace shares the
    // input trace's env var — it is the same P4b blind channel, one input source over.
    //
    // ⚠️ `set_enabled` is NOT called here: the gate is recomputed EACH FRAME in the loop (see `useTouch`)
    // so a controller plugged or unplugged mid-session moves the app between the on-screen gamepad and a
    // bare fullscreen frame. Setting it once at boot is what let a controller connected AFTER launch (or
    // the portrait skin, which used to read `cfg.touchCapable` alone) disagree with this gate.
    SdlTouch touch;
    if (inputTrace && inputTrace[0] == '1') touch.set_trace(true);

    // The click/haptic sink (convergence D). Null on desktop/handheld (no feedback); Android hands in
    // a JNI shim. Installed once — the per-frame `set_feedback_settings` below is what tracks the live
    // BTN SOUND / BTN VIBRO rows. See button_feedback.h.
    touch.set_feedback(cfg.buttonFeedback);

    // ── The touch skin's textures (D7 asset seam → D2 decode → texture), consumed by PORTRAIT2 ─────
    //
    // Decoded only where there is a touchscreen to skin — the same gate as the panels — so desktop and
    // the handhelds decode nothing. The `skin:` lines it prints are the on-device account of whether a
    // real PNG came out of the APK — the log line IS the assertion there, there being no console test on
    // a phone. The D7 walking skeleton blitted one piece in a corner to prove the pipeline reached the
    // screen; the PORTRAIT2 renderer (below, `portrait`) composites the whole set into the band geometry.
    // ⚠️ `unload()` is called before video.close() below, because these textures belong to that renderer
    // and outliving it is a use-after-free.
    //
    // ⚠️ The theme is no longer hardcoded: `SETTINGS > LAYOUT`'s skin column (NORM/DARK) picks it, the
    // choice persists as `portrait_skin` and resolves through `device_skin.h`. The actual `skin.load` /
    // `portrait.set_skin` happens in the loop below (the `loadedSkinIdx` sync), so a live switch reloads
    // the PNGs once, on the deliberate user action, rather than every frame.
    Skin skin;

    // The button-label font (convergence D): Helvetica, decoded once, owned by the renderer like the
    // skin, loaded on the same touchscreen gate — so desktop and the handhelds parse nothing. A load
    // failure is NOT fatal: PortraitSkin::draw_buttons falls back to the shared 5×5 font, so a missing
    // asset degrades to blocky labels, not none. ⚠️ `unload()` before video.close() below, with skin's.
    Font helvFont;
    if (cfg.touchCapable) helvFont.load(video.renderer(), "fonts/helvetica_regular.otf", cfg.console);

    // The D-pad ARROW font (convergence D-theme): Linux Biolinum (SIL OFL), bundled for the four arrow
    // glyphs (↑↓←→) Helvetica lacks — on Android those came from the system-font fallback, which one
    // .otf has no equivalent of. `PortraitSkin::draw_buttons` blits its real glyphs; a load failure is
    // NOT fatal — it falls back to the shell-drawn line arrow — so a missing asset degrades, not breaks.
    // Same touchscreen gate and renderer-owned lifetime as helvFont; `unload()` before video.close().
    Font arrowFont;
    if (cfg.touchCapable) arrowFont.load(video.renderer(), "fonts/LinBiolinum_Rah.ttf", cfg.console);

    // ── The screen overlay (convergence D6): the CRT filter drawn OVER the frame ─────────────────
    //
    // One texture, decoded through the same D7 asset seam / D2 decoder as the skin, owned by the
    // renderer (unload() before video.close()). It is NOT gated on a touchscreen like the skin — it is
    // a screen filter, so it applies wherever the OVERLAY row exists (caps.skinOverlay, Android/debug).
    // The actual `screenOverlay.load` happens in the loop below (the `loadedOverlayIdx` sync), so a live
    // selection change decodes the PNG once, on the deliberate user action, not every frame.
    ScreenOverlay screenOverlay;

    // ── The PORTRAIT2 device-skin renderer (convergence D) ───────────────────────────────────────
    //
    // Owns no resources — it composites `skin`'s textures into the band geometry each frame — so it
    // just needs to exist. `portrait.layout()` in the loop decides per frame whether the output is
    // portrait and this should present the skin instead of the centred landscape frame; `active()` is
    // that decision, derived from the output aspect, so a live rotation switches modes with nothing to
    // keep in sync. Inert where there is no skin to draw (desktop/handheld, and any landscape frame).
    PortraitSkin portrait;

    // The UI state points at the host's live project — one document, edited in place. The boot
    // screen is AppState's own default (SONG, as Android): restating it here would be a second
    // place for it to rot, which is exactly how it sat on PHRASE for four phases.
    ui::AppState state;
    state.project = &host.edit_project();

    // Which SETTINGS rows exist, and whether PROJECT has an EXIT. A VALUE, not an #ifdef — see
    // ui/platform_caps.h. The platform chose it; this file only installs it.
    state.caps = cfg.caps;

    // ── settings.json ────────────────────────────────────────────────────────────────────────────
    // SharedPreferences, as a file. No file = first launch = the factory settings, which is not an
    // error and gets no complaint.
    //
    // ⚠️ But `load_settings` returns the same `false` for "no file" and "the file is there and would
    // not read or would not parse", and the second one silently DISCARDS the user's settings — the
    // quit-time save then writes the defaults over what was there. Asking the filesystem separately
    // is what tells the two apart, so the one case worth a complaint gets one.
    if (ui::load_settings(filesystem, state.settings, state.theme)) {
        std::printf("settings: %s\n", filesystem.settings_path().c_str());
    } else if (filesystem.file_exists(filesystem.settings_path())) {
        std::fprintf(stderr,
                     "warning: %s exists but could not be read or parsed - starting on the factory "
                     "settings, and they will be saved over it at quit\n",
                     filesystem.settings_path().c_str());
    }

    // ── config.json — the hand-edited configuration file ──────────────────────────────────────────
    //
    // Read once at boot, on EVERY platform and in every build. It was debug-gated while the shape was
    // being tested in 0.9.4; from this release the gate is gone from the read, the seed and the
    // dispatcher alike, so a file that works on Windows works identically on Android and on a handheld.
    //
    // Seed a starter template FIRST (only when the file is absent — it never clobbers the user's), so the
    // feature is discoverable: the app used to never create the file, so there was nothing to find or
    // edit. The template states every section at its current value, so the file as seeded changes
    // nothing; the loads right after pick it up.
    //
    // ⚠️ The keyboard defaults are handed to the seeder BY the input layer rather than restated in it —
    // pt-ui cannot name an SDL key, and a template that lies about your current bindings is worse than
    // no template. This is the whole reason `default_keyboard_bindings()` exists.
    if (ui::seed_config_template(filesystem, SdlInput::default_keyboard_bindings()))
        std::printf("config:   seeded template %s\n", filesystem.config_path().c_str());

    // `folders` → the dispatcher's browse start dirs. An override is root-relative unless absolute,
    // is re-rooted when it was authored under another install's root, and falls back to the built-in
    // folder when it cannot be read here — the whole rule is `ui::resolve_browse_dir`.
    if (ui::load_folder_config(filesystem, state.folderConfig))
        std::printf("config:   %s\n", filesystem.config_path().c_str());

    // `controller` + `keyboard` → this shell's own input layer. Every rejection is printed: a
    // hand-edited file that looks applied and is not is the most expensive way for this to fail.
    {
        ui::InputConfig                     inputCfg;
        std::vector<ui::InputConfigWarning> warnings;
        ui::load_input_config(filesystem, inputCfg, warnings);
        for (const ui::InputConfigWarning& w : warnings) std::printf("config:   %s\n", w.text.c_str());
        input.apply_input_config(inputCfg);

        // config.json SEEDS the ABXY row rather than competing with it. A user who wrote a value
        // into the file before the row existed keeps it; once the row says anything but AUTO, the
        // row wins and the file is ignored. Two controls for one value is how one of them becomes a
        // lie, and this is the cheapest way to have only one.
        if (state.settings.abxyIndex == 0 && inputCfg.abxy != ui::AbxyLayout::AUTO)
            state.settings.abxyIndex = static_cast<int>(inputCfg.abxy);
    }

    // Resolve the PERSISTED skin id (a stable string, `portrait_skin`) to the runtime index the SETTINGS
    // skin column edits. device_skin.h falls back to DARK for an unknown id, so a missing/mangled key
    // keeps the shell's shipped look. The loop's `loadedSkinIdx` sync turns this index into the loaded
    // textures + PortraitSkin's scalars on the first frame, and again whenever the user changes it.
    state.settings.skinIndex = device_skin_index(state.settings.portraitSkin);

    // And the persisted OVERLAY selection (`overlay_name`, "OFF" or a stable id) to its cycle index —
    // 0 = OFF, 1.. = a shipped overlay. Same "stable string, not an index" contract as the skin above
    // (shell/overlay.h). The loop's `loadedOverlayIdx` sync turns this into the decoded texture on the
    // first frame the OVERLAY row is live, and again whenever the user cycles it.
    state.settings.overlayIndex = screen_overlay_index(state.settings.overlayName);

    // ── The MIDI port (plan §8.1, B4.3) ──────────────────────────────────────────────────────────
    //
    // The backend goes into AppState so the MIDI screen can ENUMERATE — the one option list in the app
    // that comes from the operating system and changes while the app is running. It stays a borrowed,
    // nullable pointer: a build with no backend (Linux and Android until B2b) simply has none, and
    // every use of it is guarded.
    //
    // The two env-var overrides land here, over whatever settings.json said, BEFORE anything opens —
    // see app.h for why they become settings rather than a second, invisible source of truth.
    state.midiOut = cfg.midiOut;
    // The INPUT backend (E2) rides in the same way and on the same terms — the list is the OS's, the
    // choice is a NAME, and the override becomes a setting rather than a second source of truth.
    state.midiIn  = cfg.midiIn;
    if (!cfg.midiInDevice.empty())  state.settings.midiInDevice  = cfg.midiInDevice;
    if (!cfg.midiOutDevice.empty()) state.settings.midiOutDevice = cfg.midiOutDevice;
    if (cfg.midiOffsetMs != 0)      state.settings.midiOffsetMs  = cfg.midiOffsetMs;
    if (cfg.midiSyncOut >= 0)       state.settings.midiSyncOut   = cfg.midiSyncOut != 0;

    ui::Canvas        canvas;
    ui::TrackerLayout layout;
    ui::EngineFeed    feed;

    // The whole input layer, in one object. It edits `host.edit_project()` — the SAME Project the
    // Sequencer is reading — so an edit is live the instant it is made.
    ui::InputDispatcher dispatch(state, host, filesystem);
    ui::MapperState     mapper;

    // Open the port the settings name — and push the OFFSET, which the consumer needs whether or not a
    // port ever opens. Deliberately the DISPATCHER's call and not six lines of resolve-and-open here:
    // the MIDI screen's OUTPUT row runs the same two functions, so boot and UI cannot drift apart about
    // which device is open. See InputDispatcher::boot_midi_port.
    dispatch.boot_midi_port();
    if (cfg.midiOut) {
        std::printf("midi:    OUT %s (offset %+d ms, sync %s)\n",
                    cfg.midiOut->is_open() ? state.settings.midiOutDevice.c_str() : "OFF",
                    state.settings.midiOffsetMs,
                    state.settings.midiSyncOut ? "ON 24 PPQN" : "off");
    }

    // The INPUT port, opened the same way and by the same rule: the dispatcher owns the open BECAUSE it
    // owns the sink wiring, and the two may never come apart (input_dispatcher.h).
    //
    // ⭐ **THE LINE IS UNCONDITIONAL ON A BUILD THAT HAS A BACKEND, and that is the guardrail's own
    // "give it one 'I woke up' line".** An input path is the purest case of a component whose correct
    // behaviour is silence: with no line here, "MIDI in is not compiled into this build" and "your
    // keyboard is not plugged in" and "the port refused to open" are one indistinguishable nothing.
    dispatch.boot_midi_in_port();
    // Latched at BOOT, and read by the exit report. Not asked again at teardown for `senderOn`'s reason
    // (the B3 block above): by then the port has been closed, so the object would answer "no" on every
    // run and the report would print on none of them.
    const bool midiInPortWasOpen = cfg.midiIn && cfg.midiIn->is_open();
    if (cfg.midiIn) {
        std::printf("midi:    IN  %s (%d port(s) enumerated)\n",
                    cfg.midiIn->is_open() ? state.settings.midiInDevice.c_str() : "OFF",
                    static_cast<int>(state.midiInDeviceNames.size()) - 1);
    } else {
        // ⚠️ Since E5 every shipping platform HAS a backend (winmm, ALSA rawmidi, `MidiManager`), so
        // this arm now means "this build is none of the three" and nothing else. A Linux box whose
        // libasound is missing does NOT come here — it has a backend object that answers 0 devices, and
        // said why on its own line — and keeping the two states distinguishable is the whole point.
        std::printf("midi:    IN  no input backend compiled into this build\n");
    }

    // ⭐ **THE THRU VERDICT (E4), PRINTED WHETHER OR NOT IT IS THE INTERESTING ONE.** A live key on an
    // EXTERNAL instrument goes out on the cable; on a LOOPBACK port that would be an amplifying feedback
    // loop, so the dispatcher turns it off when both ports name the same device. Both outcomes print,
    // because the suppression is invisible from anywhere else — a silent thru and an EXTERNAL instrument
    // that simply is not responding look exactly alike from the keyboard.
    bool midiThru = host.midi_in_thru();
    std::printf("midi:    THRU %s%s\n", midiThru ? "on" : "OFF",
                midiThru ? " (a live key on an EXTERNAL instrument reaches the cable)"
                         : " - IN and OUT are the SAME port, and thru on a loopback is a feedback loop");

    // ── What a RENDER needs from the shell (S7) ──────────────────────────────────────────────────
    //
    // The render is SYNCHRONOUS — the frame loop stops and renders. Android hands it to a coroutine
    // because Compose would ANR; there is nothing here to hand it to, and stopping is the safer
    // answer anyway, because the ONE thing that must not touch the engine while an offline render is
    // driving it is the audio callback.
    //
    // ⚠️ So the device is PAUSED, not merely stopped. Kotlin stops PLAYBACK and leaves its Oboe stream
    // open and idle, which is a race it happens to win (an idle callback reads a silent engine). A
    // paused device is a guarantee instead of a coincidence, and it costs one call.
    // ── The present branch, shared by the frame loop and the synchronous repaint hook ─────────────
    //
    // Composites whatever THIS orientation's skin is around the just-drawn 640x480 `canvas`: the
    // PORTRAIT2 device skin (present_skinned — chrome behind the frame, button cluster in front) or the
    // landscape LEFT/RIGHT touch panels (present), plus the CRT screen overlay (D6) over the frame in
    // either. Returns whether a frame actually reached the screen — the C7 pixel gate skips one identical
    // to the last, and the loop counts the rest.
    //
    // ⚠️ It lives in ONE place for a reason. When this was inline in the loop only, the dispatcher's
    // `repaint` hook — the path a BLOCKING render uses to push its "RESAMPLING... 43%" readout on screen
    // while the loop runs no frames — did a bare fullscreen `video.present`, flashing the PORTRAIT2 skin
    // away for the whole length of a resample/export and snapping it back when the loop resumed. Both
    // callers now go through here, so that divergence cannot reopen. Assumes layout.draw() already
    // refreshed `canvas`.
    const auto present_current = [&]() -> bool {
        // The CRT overlay: a translucent PNG over the tracker's on-screen box at STR/255 alpha, on the
        // FRAME rect (portrait.frame_rect() in the bezel, video.frame_rect() centred in landscape),
        // drawn AFTER the frame and BEFORE the buttons so it filters the screen but not the gamepad.
        // Off (index 0 / STR 0 / nothing decoded — every non-Android build) draws nothing.
        const int  ovStr = state.settings.overlayStrength;
        const bool ovOn  = state.settings.overlayIndex > 0 && ovStr > 0 && screenOverlay.loaded();

        // A CRT change (selection OR strength) must force one repaint even on an otherwise-still screen —
        // the canvas need not change when only the overlay does. Bits 48-60 are disjoint from the
        // touch/portrait signatures (buttons low, geometry 16-47, active markers 62/63), so an XOR into
        // either cannot collide. Zero when off, leaving those signatures untouched.
        const uint64_t crtSig = ovOn
            ? ((1ull << 60) | (static_cast<uint64_t>(state.settings.overlayIndex & 0xF) << 56) |
               (static_cast<uint64_t>(ovStr & 0xFF) << 48))
            : 0;

        if (portrait.active()) {
            // PORTRAIT2 (convergence D): the retro-device skin. The 640x480 frame lands in the bezel
            // (portrait.frame_rect), NOT window-centred; the casing is the clear, the chrome bands the
            // underlay behind the frame, the button cluster the overlay in front. The buttons are
            // pressable: touch.layout_portrait2 (in the loop) hit-tests this same cluster.
            const uint32_t bg = state.theme.background;   // the tracker's own bg fills the bezel gap
            const auto chrome = [&portrait, &skin, bg](SDL_Renderer* r) {
                portrait.draw_chrome(r, skin, bg);
            };
            const auto buttons = [&portrait, &skin, &helvFont, &arrowFont, &input,
                                  &screenOverlay, ovOn, ovStr](SDL_Renderer* r) {
                if (ovOn) screenOverlay.draw(r, portrait.frame_rect(), ovStr);
                portrait.draw_buttons(r, skin, helvFont, arrowFont, input);
            };
            // ⚠️ B4 scrims ONLY the bezel's inner GLASS on PORTRAIT2, never the casing or the button
            // cluster: dimming those would make the whole skin go dark around a bright cluster, which
            // reads as a bug, not a modal. The modal's own MODAL_BACKDROP dims the tracker inside the
            // frame; this extends the SAME dim to the bright gap AROUND the frame when INTEGER scaling
            // leaves the frame smaller than the glass (screen_rect()). FIT fills the glass, so the gap —
            // and therefore this scrim — is empty there. Off entirely when no full-canvas modal is up.
            const uint32_t scrim = ui::modal_backdrop_active(state) ? ui::MODAL_BACKDROP : 0;
            return video.present_skinned(canvas, portrait.casing_argb(), portrait.frame_rect(),
                                         chrome, buttons, portrait.signature(input) ^ crtSig, scrim,
                                         portrait.screen_rect());
        }
        // Landscape / desktop: the centred frame, the LEFT/RIGHT touch panels in the bars beside it —
        // inert (drawing nothing, signature 0) when there is no touchscreen.
        const auto overlay = [&touch, &input, &screenOverlay, ovOn, ovStr,
                              fr = video.frame_rect()](SDL_Renderer* r) {
            if (ovOn) screenOverlay.draw(r, fr, ovStr);
            touch.draw(r, input);
        };
        return video.present(canvas, state.theme.background, overlay, touch.signature(input) ^ crtSig,
                             ui::modal_backdrop_active(state) ? ui::MODAL_BACKDROP : 0);
    };

    ui::InputDispatcher::RenderHooks hooks;
    hooks.suspend_audio = [&audio](bool suspend) { audio.setPaused(suspend); };
    // A SYNCHRONOUS repaint — the "RENDERING... 43%" / "RESAMPLING..." readout the dispatcher pushes on
    // screen from INSIDE a blocking render (the frame loop runs no frames for its duration). It presents
    // through present_current, the SAME branch the loop uses, so it keeps the PORTRAIT2 skin instead of
    // flashing the bare frame fullscreen for the render's duration. See present_current above.
    hooks.repaint       = [&]() {
        layout.draw(canvas, state);
        present_current();
    };
    dispatch.set_render_hooks(std::move(hooks));

    // ── THE LIFECYCLE (S10) ──────────────────────────────────────────────────────────────────────
    //
    // Where a RELATIVE sample path resolves. Absolute paths (everything the browser loads) ignore it;
    // a portable project — every golden, and anything this build ships — stores its media relative, and
    // recovering one of those against the wrong folder brings the song back looking perfect and playing
    // silence. So the dispatcher is TOLD the session's media dir rather than guessing one.
    //
    // ⚠️ It must never arrive EMPTY: an empty base resolves every relative path against the process's
    // cwd, which on a handheld is whatever the launch script last cd'd to. The platform resolves it —
    // the project's own directory when there is one, the app root when there is not, because that is
    // where Samples/ lives and therefore where a recovered autosave's relative media actually is.
    dispatch.set_media_base_dir(cfg.mediaBaseDir);

    // An autosave that survived to launch means the last session did not end cleanly — a launcher's
    // kill, a flat battery, a crash. SETTINGS → RESUME decides what happens next: ASK raises the
    // RECOVER WORK? dialog, AUTO restores in silence.
    //
    // ⚠️ AFTER load_settings (RESUME is the setting being read) and AFTER push_params (a recovery
    // re-pushes everything anyway). If there is no autosave — the common case, and the one that means
    // everything went fine last time — this does nothing at all.
    // Said out loud for the same reason the once-a-second status line below is: during a handheld
    // bring-up there is no screen yet, and "did it find my crash file?" is not a question you can answer
    // by looking at a window that is not there.
    //
    // ⚠️ ASCII, and that is this file's own rule being obeyed rather than a preference — the help banner
    // below states it: the console's encoding is not ours to choose (a handheld's serial console, an ssh
    // session, a Windows box on a legacy code page), and an em-dash arrives there as mojibake. S10 wrote
    // one into this very line and watched it come back as `вЂ”` on the first run.
    switch (dispatch.boot_recovery()) {
        using BR = ui::InputDispatcher::BootRecovery;
        case BR::NONE:     break;   // the common case, and it deserves no line of its own
        case BR::ASKED:    std::printf("autosave: FOUND - asking (SETTINGS > RESUME = ASK)\n"); break;
        case BR::RESTORED: std::printf("autosave: FOUND - restored (SETTINGS > RESUME = AUTO)\n"); break;
        case BR::DROPPED:  std::printf("autosave: FOUND but UNREADABLE - dropped\n"); break;
    }

    // ── The lifecycle watcher (C4) ───────────────────────────────────────────────────────────────
    // Installed AFTER boot_recovery, so a backgrounding that arrives during start-up cannot flush an
    // autosave over the one still being decided about. Removed below the loop — `bg` is a stack
    // object and the watcher must not outlive it. See on_app_event for why this is not a thread
    // boundary despite what the plan assumed.
    BackgroundContext bg{&host, &dispatch, &filesystem, &state, cfg.console};
    SDL_AddEventWatch(on_app_event, &bg);

    // The banner and the once-a-second status line below are the two HIGH-VOLUME things this file
    // prints, and they are the bring-up instrument for a platform whose screen is not up yet. A
    // platform whose stdout goes nowhere — an APK's does — turns them off and pays nothing; the
    // handful of one-line boot diagnostics above stay unconditional, being both cheap and the answers
    // to "did my samples load?" and "where did it put its folders?".
    if (cfg.console) {
        std::printf("\nWASD/arrows move   K/Enter = A   J/Esc = B   U/I = L/R   LShift = SELECT   SPACE = START   F10 quit\n");
        std::printf("A+LEFT/RIGHT edit   A+UP/DOWN edit fast   A+B clear   A,A insert next unused\n");
        std::printf("B+LEFT/RIGHT change WHICH phrase/chain/table   B+UP/DOWN page the song\n");
        std::printf("L+B select (tap again to widen)   B copies   L+A cut/paste   L+R deselect   L+B+A clone\n");
        // ASCII only, deliberately: this goes to a console whose encoding is not ours to choose (a
        // handheld's serial/ssh terminal, a Windows box on a legacy code page), and a stray em-dash
        // arrives there as mojibake.
        std::printf("A+UP on an FX-TYPE column opens the effect picker - release A to choose\n");
        std::printf("R+DPAD moves between screens: SONG CHAIN PHRASE INSTRUMENT TABLE MODS INST.POOL\n");
        std::printf("                             GROOVE MIXER EFFECTS PROJECT SETTINGS\n");
        std::printf("PROJECT: A on SAVE/LOAD/NEW, on EXPORT MIX/STEMS, on COMPACT SEQ/INST, on SETTINGS>, on EXIT\n");
        std::printf("         A on NAME opens the keyboard; A+LEFT/RIGHT edits one character in place\n");
        std::printf("         a confirm asks A=YES B=NO before anything destructive\n");
        std::printf("START auditions the instrument on INSTRUMENT/POOL/MODS/TABLE - any button silences it\n");
        std::printf("SELECT on the EFFECTS TIME row toggles delay sync (free ms <-> note divisions)\n");
        std::printf("\nFILE BROWSER (A on INSTRUMENT's LOAD, or on the pool's NAME of an empty slot):\n");
        std::printf("  A opens a folder or LOADS the file   B goes back   START auditions the file\n");
        std::printf("  R+LEFT = up a directory   R+UP/DOWN = sort (name/date/size)   DPAD L/R = page\n");
        std::printf("  SELECT+A rename   SELECT+B delete   SELECT+R new folder\n");
        std::printf("  L+B select (again within 500ms = all)   B copies   L+A cut/paste   L+R cancel\n");
        std::printf("KEYBOARD: DPAD picks a key   A types   B deletes   R+UP/DOWN = ABC/123 layout\n");
        std::printf("          R+LEFT/RIGHT moves the text cursor   SELECT aborts   START applies\n");
        std::printf("\nEQ EDITOR (A on any EQ cell: INSTRUMENT/POOL/MIXER master/EFFECTS REV+DLY/SAMPLE FX):\n");
        std::printf("  DPAD UP/DOWN picks the param, LEFT/RIGHT the band   A+LEFT/RIGHT and A+UP/DOWN dial it\n");
        std::printf("  A+B resets it   B+LEFT/RIGHT changes the EQ SLOT   B or SELECT closes\n");
        std::printf("  START still auditions underneath, so you can sweep a band across a ringing note\n\n");
    }

    bool   running    = true;
    Uint64 lastStatus = 0;

    // ── ⚠️ DEV BRING-UP ONLY: the two hooks that make a TIMED run scriptable (phase B3) ───────────
    //
    //   POCKETTRACKER_AUTOPLAY_MS=<n>   press START once, n ms after the first frame
    //   POCKETTRACKER_QUIT_AFTER_MS=<n> push SDL_QUIT n ms after the first frame
    //
    // They exist because B3's claim is about MILLISECONDS and a human pressing SPACE cannot be a
    // control run. Neither invents a path: autoplay pushes a `Button::START` PRESSED event through
    // `ui::handle_button` — the identical call the SDL input layer makes, so the mapper, the
    // preview-silencing rule and `on_start` all run exactly as they do for a finger. (What it does NOT
    // exercise is `sdl-input.cpp` itself, which remains the tree's one untested layer; that is a
    // different gap and this does not pretend to close it.)
    //
    // ⚠️ Not a general scripted-input facility, deliberately. One button, no sequence, no file: the
    // moment this grows a script format it becomes a second input path to keep in step with the real
    // one, and the guardrails have a name for a test that drives a path the user cannot.
    const auto env_ms = [](const char* name) -> Uint64 {
        const char* v = SDL_getenv(name);
        if (!v || !v[0]) return 0;
        const long n = SDL_strtol(v, nullptr, 10);
        return n > 0 ? static_cast<Uint64>(n) : 0;
    };
    const Uint64 autoplayMs  = env_ms("POCKETTRACKER_AUTOPLAY_MS");
    const Uint64 quitAfterMs = env_ms("POCKETTRACKER_QUIT_AFTER_MS");
    Uint64       firstFrameMs = 0;   // 0 until the first frame — the clock both hooks count from
    bool         autoplayFired = false;
    if (autoplayMs || quitAfterMs) {
        std::printf("dev:     AUTOPLAY_MS=%llu  QUIT_AFTER_MS=%llu (scripted run)\n",
                    static_cast<unsigned long long>(autoplayMs),
                    static_cast<unsigned long long>(quitAfterMs));
    }

    // The last output size the PORTRAIT2 log line reported, so it prints once per rotation/resize rather
    // than every frame — the portrait analogue of sdl-video's describe-on-change (which reports the
    // centred landscape frame and would misdescribe this mode; see present() in sdl-video.cpp).
    int lastPortraitW = 0, lastPortraitH = 0;

    // The last LAYOUT-GATE verdict reported, on the same print-on-change discipline. `lastUseTouch`
    // starts at the value `useTouch` can never legitimately hold at boot on a touch device, and the
    // sizes at -1, so the first frame always prints — the line's whole job is to exist at boot.
    bool lastUseTouch = false;
    int  lastGateW = -1, lastGateH = -1;

    // The device skin currently decoded into `skin` + adopted by `portrait`. -1 = none loaded yet, so
    // the first touch frame decodes the persisted choice; thereafter it reloads only when the SETTINGS
    // skin column moves `skinIndex`. Decoding ~10 PNGs is a deliberate-action cost, never a per-frame one.
    int loadedSkinIdx = -1;

    // The screen overlay currently decoded into `screenOverlay`. -1 = nothing decoded yet, so the first
    // frame the OVERLAY row is live loads the persisted choice (or nothing, for OFF); thereafter it
    // reloads only when the SETTINGS OVERLAY column moves `overlayIndex`. One PNG, a deliberate-action
    // cost, never per frame.
    int loadedOverlayIdx = -1;

    // ── IS THERE A PHYSICAL PAD? (the touch vs FULL gate) ──────────────────────────────────────────
    //
    // `physicalGamepadPresent` is the platform's answer when it has one — Android's, because SDL's joystick
    // count is not the truth there (it opens the emulator's keyboard as a controller; see app.h). Desktop
    // and the handhelds leave it null and fall back to `controller_count()`, which IS the truth on those.
    // Cached, not asked every frame: on Android that call is a JNI round trip, so it is refreshed only when
    // SDL reports a controller add/remove (`padDirty`, set in the event loop) — which is exactly when the
    // answer can change, so hot-plug still moves the app between the on-screen gamepad and a bare frame.
    auto compute_has_pad = [&]() -> bool {
        return cfg.physicalGamepadPresent ? cfg.physicalGamepadPresent()
                                          : (input.controller_count() > 0);
    };
    bool physicalPad = compute_has_pad();
    bool padDirty    = false;

    // Does `layoutIndex` currently carry the FULL/PORTRAIT choice? The list is one entry long
    // without a pad and two with, so the same index means different things in the two states — and
    // reading it as a choice while it means the other thing is how the choice gets cleared. Set
    // from `padChoice` at the end of each gate pass; see the read-back there.
    bool padLayoutLive = false;

    // The last rotation permission handed to the platform, so it is pushed on CHANGE rather than
    // every frame (each call is a JNI round trip that touches the activity). -1 = nothing sent yet,
    // a value neither branch can produce, so the first gate pass always states the boot answer.
    int lastLandscapeAllowed = -1;

    // DEV BRING-UP ONLY, and the same knob POCKETTRACKER_TOUCH is: PT_TOP_ANCHOR=1 forces the
    // clip-on-gamepad frame placement on a desktop, where the gate below can never fire (no
    // touchscreen). Drag the window tall and the frame moves into the top half. It is how the
    // placement is looked at without a phone and a controller in hand.
    const char* topAnchorEnv   = SDL_getenv("PT_TOP_ANCHOR");
    const bool  forceTopAnchor = topAnchorEnv && topAnchorEnv[0] == '1';

    // ── C7 state ─────────────────────────────────────────────────────────────────────────────────
    // `sawInput`     — anything happened this frame that could have changed what is on screen.
    // `audibleEdge`  — audio was audible LAST frame, so the first silent frame is still drawn once
    //                  (Kotlin's active→idle bump; without it the scope freezes mid-wave).
    // `drewOnce`     — the first frame always draws, or the window comes up empty until a keypress.
    bool sawInput    = false;
    bool audibleEdge = true;
    bool drewOnce    = false;

    // ⚠️ **THE NUMBERS BESIDE THE VERDICT, AND C7 IS UNVERIFIABLE WITHOUT THEM.** A working idle skip
    // and a skip that never fires look IDENTICAL on screen — that is the whole point of it, the
    // picture does not change either way. So the status line carries the frame accounting: `drew` is
    // frames actually sent to the display, `skip` frames the animation gate dropped, `same` frames
    // that were drawn and then found byte-identical to what was already there (gate 1 being
    // conservative, gate 2 catching it). On a still screen `drew` should stop climbing while the
    // frame counter keeps going; if `skip` stays 0 the feature is not working, whatever it looks like.
    long long drawn = 0, presented = 0, skipped = 0;

    // ── ROTATION / RESIZE SETTLING (C7's blind spot on Android) ──────────────────────────────────
    //
    // A rotation on Android resizes the SurfaceView over SEVERAL frames, and SDL does not send a
    // WINDOWEVENT for every intermediate size — so the event-driven redraw (SDL_WINDOWEVENT_SIZE_CHANGED
    // above) forces ONE frame, which may land on a half-transitioned surface, and then the C7 idle-skip
    // freezes THAT half-drawn frame until the next input. That is the reported bug: rotate and the screen
    // is half casing / half bezel-background (or half a stale landscape frame), and it only corrects when
    // a virtual button is touched. The fix is to POLL the output size every frame and force a full redraw
    // for a few frames after it LAST changed — so the settled geometry always gets a clean repaint with
    // no input. `lastOutW/H` start at -1 so the very first frame is not counted as a resize.
    constexpr int RESIZE_SETTLE_FRAMES = 4;
    int lastOutW = -1, lastOutH = -1, resizeSettle = 0;

    // ⚠️ `terminate_requested` is the launcher's kill, read once per frame. On desktop it reports a FLAG
    // set by a signal handler that does nothing else — so this loop condition is where a SIGTERM actually
    // takes effect, and the flush below the loop is where the work is saved, on the main thread, with a
    // heap to do it with.
    //
    // ⚠️⚠️ **A PLATFORM THAT FREEZES THIS LOOP CANNOT USE THIS HOOK, AND ANDROID IS ONE.** SDL blocks the
    // native thread while the activity is paused (SDL_HINT_ANDROID_BLOCK_ON_PAUSE, on by default), which
    // is exactly when the process is most likely to be killed — so a flag consumed here would be P4d's
    // never-armed write in a new body. Convergence C4 flushes in an `SDL_AddEventWatch` watcher instead,
    // which fires synchronously on the Java activity thread. This hook is nullable for that reason.
    //
    // ⚠️ One honest limit, stated rather than discovered later: a kill arriving while the app is inside
    // the SYNCHRONOUS export render is not seen until the render finishes, because the frame loop is not
    // running. The exposure is small — the autosave for everything up to that point fired 3 s after the
    // last edit, long before the user navigated to EXPORT and pressed A — but it is not zero.
    while (running && !state.shouldQuit &&
           !(cfg.terminate_requested && cfg.terminate_requested())) {
        // One clock reading per frame, handed to everything that needs it. The input layer's repeat
        // is a function of time, so it takes the clock rather than reaching for it.
        const Uint64 now = SDL_GetTicks64();

        // Lay the touch panels into the CURRENT letterbox bars before polling, so a finger arriving
        // this frame hits the geometry that is actually on screen. A rotate or resize is absorbed the
        // next frame; the call is a handful of int ops and a no-op when there is no touchscreen.
        {
            int outW = 0, outH = 0;
            video.output_size(outW, outH);

            // A change in the polled output size (a rotation, a resize, the system bars appearing) arms a
            // short run of forced full redraws — see RESIZE_SETTLE_FRAMES. Re-armed every frame the size
            // is still moving, so the countdown only starts once it has SETTLED, which is when the final
            // geometry gets its clean repaint even though no input arrived.
            if (outW != lastOutW || outH != lastOutH) {
                resizeSettle = RESIZE_SETTLE_FRAMES;
                lastOutW = outW;
                lastOutH = outH;
            }

            // ── THE LAYOUT SELECTOR, recomputed each frame (DeviceAdapter.hasPhysicalGameButtons) ────
            //
            // ONE gate, read by BOTH `touch` and `portrait`, so they can never disagree about whether
            // this is a touch device. A touchscreen with NO controller gets the virtual buttons (the
            // PORTRAIT2 skin held portrait, the landscape panels held landscape); a device WITH a
            // controller (built-in or plugged) gets FULL/fullscreen with no skin, in either orientation
            // — exactly as Kotlin's hasPhysicalGameButtons()→FULL decides. Recomputed per frame because
            // `controller_count()` tracks hot-plug: plug a pad and the skin disappears next frame.
            // ⚠️ This is what fixed the split gate — the portrait skin used to read `cfg.touchCapable`
            // alone and would activate on a controller-equipped phone whose buttons then did nothing.
            //
            // ⚠️ **NOT `controller_count()` DIRECTLY — see `physicalPad` above.** On Android SDL opens the
            // emulator's keyboard as a game controller, so the raw count is wrong; the platform hook answers
            // instead. `padDirty` is armed by a controller add/remove in the event loop below, so the JNI
            // answer is refreshed the frame after the hardware changes, never every frame.
            if (padDirty) { physicalPad = compute_has_pad(); padDirty = false; }
            // ── On-screen buttons: automatic, EXCEPT where the device has both ────────────────
            //
            // A pad turning the on-screen buttons off is the right default and was the whole rule
            // until a phone could have both at once: a clip-on controller leaves the touchscreen
            // exactly where it was, and only its owner knows whether they want the buttons too.
            // So where both exist the LAYOUT row offers the choice, and this reads it.
            //
            // ⚠️ `touchButtonsWithPad` DEFAULTS FALSE, which is what keeps every existing install on
            // the behaviour it has: a pad still means fullscreen until somebody says otherwise.
            const bool padChoice = cfg.touchCapable && physicalPad;

            // ⚠️ **THE ROW'S WRITE IS CONSUMED HERE, BEFORE THE INDEX IS RE-DERIVED BELOW — and the
            // order is the whole mechanism.** Input is polled AFTER this block, so a press lands on
            // `layoutIndex` once the frame's derive has already run; reading the index back further
            // down the SAME frame reads only what that derive wrote, and the next frame's derive
            // overwrites the edit before anything sees it. Reading it at the top instead means the
            // choice takes effect on the very frame after the press, gate included.
            //
            // `padLayoutLive` says the index CURRENTLY means FULL/PORTRAIT. Without it the first
            // frame under a freshly-plugged pad would read an index that meant the one-entry
            // touch-only list (always 0) as a deliberate "FULL", silently clearing the choice.
            if (padChoice && padLayoutLive)
                state.settings.touchButtonsWithPad = (state.settings.layoutIndex == 1);
            padLayoutLive = padChoice;

            const bool useTouch  = cfg.touchCapable &&
                                   (!physicalPad || state.settings.touchButtonsWithPad);
            touch.set_enabled(useTouch);

            // ── Rotation follows the layout that is actually in force ────────────────────────────
            //
            // Landscape is allowed EXACTLY where the FULL layout is — a pad is driving and the frame
            // is centred with no on-screen buttons, which is the one landscape presentation the app
            // has. Every other state (no pad, or a pad with the buttons kept) would rotate into the
            // letterboxed touch panels, a layout release deliberately does not ship: nothing in
            // SETTINGS names it and the only way back out is to turn the phone.
            //
            // ⚠️ **THE BOOT HINT CANNOT DO THIS ON ITS OWN.** `SDL_HINT_ORIENTATIONS` is read once at
            // window creation, so a pad UNPLUGGED mid-session left the activity free to stay
            // landscape while the app had already switched back to the portrait skin. The platform
            // re-applies it live; desktop and the handhelds leave the hook null, and it is a no-op.
            if (cfg.allowLandscape && static_cast<int>(!useTouch) != lastLandscapeAllowed) {
                lastLandscapeAllowed = !useTouch ? 1 : 0;
                cfg.allowLandscape(!useTouch);
            }

            // ── The frame moves up when a clip-on gamepad is covering the bottom of the phone ─────
            //
            // A GameSir Pocket Taco or an 8BitDo FlipPad grips a phone held in PORTRAIT and its two
            // halves sit over the bottom third of the screen. That configuration is exactly the one
            // where nothing else moves the frame: a physical pad turns the on-screen buttons off, so
            // the fullscreen layout centres the tracker squarely behind the controller.
            //
            // All three terms are needed and each excludes a real device:
            //   touchCapable   a phone or tablet - never a desktop window dragged tall, and never a
            //                  Linux handheld, whose cap is already false
            //   physicalPad    a pad is actually attached (hot-plug tracked, see above)
            //   outH > outW    held in portrait; landscape is what a handheld is and it is centred
            //
            // Read per frame from the live gate, so unclipping the pad or turning the phone puts the
            // frame back with no restart and no setting to find.
            // ⚠️ `!useTouch` IS PART OF THE GATE now that the buttons can be kept under a pad. With
            // them up the portrait skin places the frame itself (present_skinned) and this value is
            // never read - so without this term the layout line below would report an anchor that is
            // not in force, which is the lying instrument its own comment block exists to prevent.
            const bool topAnchor = forceTopAnchor ||
                                   (!useTouch && cfg.touchCapable && physicalPad && outH > outW);
            video.set_top_anchor(topAnchor);

            // ⚠️ **THE DECISION, SAID OUT LOUD — because a phone that lands on FULL and one that has no
            // touchscreen at all look identical from here.** A user reporting "it opened without the
            // virtual buttons" leaves nothing behind to tell a wrong `physicalPad` from a wrong aspect
            // ratio from a `touchCapable` that never got set; the portrait2 line below only prints once
            // the skin is ALREADY up, so it is silent in exactly the case that needs explaining.
            // Printed on every CHANGE (and therefore once at boot), not per frame — hot-plugging a pad
            // is a transition worth a line too.
            if (cfg.console && (useTouch != lastUseTouch || outW != lastGateW || outH != lastGateH)) {
                std::printf("layout:  %s  (touchCapable=%d physicalPad=%d output=%dx%d %s%s)\n",
                            useTouch ? (outH > outW ? "PORTRAIT2 skin" : "landscape touch panels")
                                     : "FULL - no on-screen buttons",
                            cfg.touchCapable ? 1 : 0, physicalPad ? 1 : 0, outW, outH,
                            outH > outW ? "portrait" : "landscape",
                            topAnchor ? ", frame anchored TOP HALF - clip-on pad" : "");
                std::fflush(stdout);
                lastUseTouch = useTouch;
                lastGateW    = outW;
                lastGateH    = outH;
            }

            // ⚠️ **THE SETTINGS > LAYOUT ROW FOLLOWS THE TOUCH GATE, NOT JUST THE STATIC CAP.**
            // `caps.touchLayouts` is true on every Android build, but on a device WITH physical buttons —
            // a handheld like the AYANEO, which auto-selects FULL — there is no touch layout to configure,
            // so the row would draw a header with no options under it. Gate the row's very EXISTENCE on
            // `useTouch` (a touchscreen AND no pad), ANDed with the platform's static cap so desktop and
            // the Linux handhelds (cap already false) are untouched. `settings_row_visible`,
            // `settings_row_offset_y`, the cursor wrap and the cursor context all read this one flag, so
            // hiding it here removes the row everywhere consistently — platform_caps.h's "a setting that
            // configures nothing is a lie", applied at runtime because the fact it rests on (a controller)
            // is a runtime one. Recomputed with `useTouch`, so unplugging a pad brings the row back.
            // ⚠️ AND IT IS NO LONGER GATED ON `useTouch`, which would hide the row in exactly the
            // state it now exists for: a pad is attached, the buttons are off, and this row is how
            // they are turned back on. It always has something to say on a touchscreen device - the
            // skin when the buttons are up, the FULL / PORTRAIT choice when a pad is on.
            state.caps.touchLayouts = cfg.caps.touchLayouts && cfg.touchCapable;

            // BTN SOUND and BTN VIBRO sound and shake the ON-SCREEN buttons, so they follow whether
            // those are DRAWN rather than whether the device could draw them - platform_caps.h's
            // "a setting that configures nothing is a lie", one more row than it used to reach.
            state.caps.buttonFeedback = cfg.caps.buttonFeedback && useTouch;

            // The face-button swap is for a pad that misreports itself; with nothing plugged in there
            // is nothing to swap. Hot-plugged, like the two above.
            state.caps.padAttached = physicalPad;

            // Push the user's live BTN SOUND / BTN VIBRO scalars so the next tap plays with whatever
            // SETTINGS currently shows — read here, in the loop, because they can change live. A no-op
            // where there is no feedback sink (button_feedback.h).
            // The face-button swap, read live so the row takes effect on the next press.
            input.set_abxy(static_cast<ui::AbxyLayout>(
                std::clamp(state.settings.abxyIndex, 0, 2)));

            touch.set_feedback_settings({state.settings.buttonSoundEnabled,
                                         state.settings.buttonSoundVolume,
                                         state.settings.buttonVibroEnabled,
                                         state.settings.vibroPower});

            // ── The device skin: which theme paints the chrome, and the SETTINGS row that picks it ────
            //
            // The skin column (NORM/DARK) is the real control on this UI's LAYOUT row; the mode column is
            // a single "PORTRAIT" (matching release Kotlin's touch-only layout list, which has one entry
            // — the shell already auto-selects portrait/landscape by aspect and fullscreen by controller
            // presence, so there is no mode for the user to override). `skinCount > 0` is what makes the
            // editor draw the second column at all; the display strings are the platform's to supply.
            // The mode column has two entries only where there is a choice to make - a touchscreen
            // with a pad on it. Everywhere else it is the single "PORTRAIT" it always was, because a
            // touch-only device offered FULL would have no way left to press anything.
            //
            // FULL is index 0 and PORTRAIT index 1, and the INDEX IS DERIVED from the persisted
            // boolean rather than persisted itself: the list is one entry long without a pad and two
            // with, so a stored index would mean different things in the two states and unplugging
            // would forget the choice.
            if (padChoice) {
                state.settings.layoutCount = 2;
                state.settings.layoutIndex = state.settings.touchButtonsWithPad ? 1 : 0;
                state.layoutText           = state.settings.touchButtonsWithPad ? "PORTRAIT" : "FULL";
            }
            if (useTouch) {
                if (state.settings.skinIndex < 0 || state.settings.skinIndex >= kDeviceSkinCount)
                    state.settings.skinIndex = device_skin_index(state.settings.portraitSkin);
                state.settings.skinCount   = kDeviceSkinCount;
                if (!padChoice) {
                    state.settings.layoutCount = 1;
                    state.settings.layoutIndex = 0;
                }
                const DeviceSkinDef& d = kDeviceSkins[state.settings.skinIndex];
                if (!padChoice) state.layoutText = "PORTRAIT";
                state.skinText              = d.displayName;
                state.settings.portraitSkin = d.id;   // keep the persisted id in step with the choice

                // Reload the PNGs + adopt the scalars only when the choice actually changed (or on the
                // first frame). This is where the D7 asset seam is finally driven by a setting.
                if (state.settings.skinIndex != loadedSkinIdx) {
                    skin.load(video.renderer(), d.id, cfg.console);
                    portrait.set_skin(d.casingFillArgb, d.labelRgb, d.bezelThicknessX);
                    loadedSkinIdx = state.settings.skinIndex;
                }
            } else {
                state.settings.skinCount = 0;   // no skin column on a fullscreen (controller) layout
                if (!padChoice) { state.settings.layoutCount = 1; state.settings.layoutIndex = 0; }
            }

            // ── The screen overlay (D6): the CRT filter's SETTINGS row + its texture ──────────────
            //
            // Independent of the touch skin — it is a screen filter, gated on `caps.skinOverlay` (the
            // OVERLAY row, Android/debug) rather than a touchscreen. The row edits `overlayIndex`; here
            // the shell fills in what only it knows — the choice COUNT and the display TEXT — and keeps
            // the persisted `overlayName` in step with the index, exactly as the skin block does with
            // `portraitSkin`. The PNG is (re)decoded only when the choice actually changes.
            if (cfg.caps.skinOverlay) {
                state.settings.overlayCount = screen_overlay_choice_count();
                if (state.settings.overlayIndex < 0 ||
                    state.settings.overlayIndex >= state.settings.overlayCount)
                    state.settings.overlayIndex = screen_overlay_index(state.settings.overlayName);
                state.settings.overlayName = screen_overlay_id(state.settings.overlayIndex);
                state.overlayText          = screen_overlay_text(state.settings.overlayIndex);

                if (state.settings.overlayIndex != loadedOverlayIdx) {
                    screenOverlay.load(video.renderer(), state.settings.overlayIndex, cfg.console);
                    loadedOverlayIdx = state.settings.overlayIndex;
                }
            }

            // PORTRAIT2 first — its active() decides whether `touch` hit-tests the skinned cluster
            // (portrait) or the landscape letterbox bars. Both are a handful of int ops and a no-op with
            // no touchscreen. The cluster geometry is PortraitSkin's, handed straight to `touch` so the
            // hit-test and the draw share ONE source of truth: a press can never highlight a cell the
            // finger is not over, because both read the same rects. `scalingBilinear` picks INTEGER vs
            // FIT for where the frame lands in the bezel (see PortraitSkin::layout).
            portrait.layout(outW, outH, useTouch, state.settings.scalingBilinear);
            if (portrait.active())
                touch.layout_portrait2(portrait.cluster_rect(), portrait.button_rects(), outW, outH);
            else
                touch.layout(video.frame_rect(), outW, outH);

            // A one-line account of the PORTRAIT2 skin when it activates or the output rotates — the
            // portrait analogue of sdl-video's describe(). On console + on change only, like the status
            // line: on a phone with no console this reads back out of logcat, and it is how "did it go
            // into portrait, and where did the frame land?" is answered when the screen is the thing
            // under test.
            if (cfg.console && portrait.active() &&
                (outW != lastPortraitW || outH != lastPortraitH)) {
                const SDL_Rect fr = portrait.frame_rect();
                std::printf("portrait2: output=%dx%d  frame=%dx%d at %d,%d  skin composited in the bezel\n",
                            outW, outH, fr.w, fr.h, fr.x, fr.y);
                std::fflush(stdout);
                lastPortraitW = outW;
                lastPortraitH = outH;
            }
        }

        // ⚠️ BEFORE the event loop, or a B pressed THIS frame would arm against last frame's answer.
        // B repeats only while the qwerty overlay is up, where it is a backspace and holding it to
        // erase a word is the expected gesture; everywhere else B is COPY / BACK / CANCEL and the
        // modifier of B+DPAD, none of which may fire on a timer. Restated every frame from the live
        // flag rather than pushed when the overlay opens and closes — the state cannot come apart from
        // the overlay if nothing has to remember to say so. (`sdl-input.h::set_b_repeatable`.)
        input.set_b_repeatable(state.qwerty.isOpen);

        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            // ⚠️ C7: ANY event counts, not just the ones that map to a button. A window resize, an
            // expose, a focus change, a rotation and the system bars appearing are all events that
            // change what should be on screen while producing no ButtonEvent at all — and being
            // over-inclusive here costs one redrawn frame, while being under-inclusive costs a stale
            // window. The gate is allowed to be conservative; that is what the pixel compare in
            // `present` is for.
            sawInput = true;

            // A controller plugged or unplugged changes the answer to "is there a physical pad?", so
            // re-ask it next frame (see physicalPad). Cheap here — just a flag; the recompute is gated on
            // it. Covers both the game-controller and raw-joystick add/remove SDL emits.
            if (e.type == SDL_CONTROLLERDEVICEADDED || e.type == SDL_CONTROLLERDEVICEREMOVED ||
                e.type == SDL_JOYDEVICEADDED || e.type == SDL_JOYDEVICEREMOVED)
                padDirty = true;

            // ⚠️ **THE BLACK-SCREEN-ON-RESUME FIX.** `sawInput` above makes this frame DRAW, but C7's
            // pixel gate in `present` still SKIPS the upload when the drawn frame is byte-identical to
            // the last one — and on an Android sleep→resume nothing has changed, so it is identical,
            // while the PLATFORM has blanked the real surface behind that gate's back. The frame stays
            // black until the next real state change (the reported "renders only after a button press").
            // Re-expose and a renderer reset are the same shape. So force the next present here; a DEVICE
            // reset (GL context loss) also loses the streaming texture and must recreate it. ONE
            // unconditional log line, because a forced present that fired and one that never installed
            // look identical on an idle screen — the C4 silent-handler lesson.
            const char* redrawReason = nullptr;
            if (e.type == SDL_RENDER_DEVICE_RESET) {
                video.invalidate_backbuffer(/*texture_lost=*/true);
                redrawReason = "device reset - texture recreated";
            } else if (e.type == SDL_RENDER_TARGETS_RESET || e.type == SDL_APP_WILLENTERFOREGROUND ||
                       e.type == SDL_APP_DIDENTERFOREGROUND ||
                       (e.type == SDL_WINDOWEVENT &&
                        (e.window.event == SDL_WINDOWEVENT_EXPOSED ||
                         e.window.event == SDL_WINDOWEVENT_SIZE_CHANGED ||
                         e.window.event == SDL_WINDOWEVENT_RESIZED))) {
                // ⚠️ **A ROTATION IS A SURFACE SWAP, AND ON A PORTRAIT-NATIVE PHONE THE FIRST ONE
                // HAPPENS AT BOOT.** The window opens in the device's portrait, then SDL requests
                // SENSOR_LANDSCAPE (windowed=false) and Android rotates — replacing the SurfaceView's
                // buffer with a landscape one. The frames drawn before that settle are presented to the
                // old surface, and once the app goes idle the C7 pixel gate skips re-presenting the
                // NEW (blank) one because the canvas has not changed: a black screen until the first
                // input redraws it. The landscape-native AYANEO never rotated, so it never showed this
                // — the assumption "the surface we drew to is the one on screen", true there, broken
                // here. A SIZE_CHANGED/RESIZED is exactly that swap announcing itself, so it forces the
                // next present the same way a re-expose does. The texture is 640×480 regardless of
                // window size, so it is not lost — only the "already on screen" assumption is.
                video.invalidate_backbuffer(/*texture_lost=*/false);
                redrawReason = "foreground / re-expose / resize / targets reset";
            }
            if (redrawReason && cfg.console) {
                std::printf("video:   backbuffer no longer ours (%s) - forcing a redraw\n", redrawReason);
                std::fflush(stdout);
            }

            // ⚠️ **A redraw is not a re-read, and the file browser needs the second one.** The arm
            // above puts the same pixels back on a surface the platform took away; it does not ask the
            // disk anything. While we were in the background the directory on screen may have been
            // changed by a file manager or a download — and on Android by this app's own ADD FOLDER…,
            // which grants through a picker activity and therefore ALWAYS lands while we are the
            // backgrounded one. Inert on desktop: SDL sends this event on Android and iOS only.
            if (e.type == SDL_APP_DIDENTERFOREGROUND) dispatch.refresh_browser_on_foreground();

            if (e.type == SDL_QUIT) {
                // The OTHER way a kill arrives: a window manager's close button, and — where SDL was
                // built with HAVE_SIGNAL_H and nobody set SDL_NO_SIGNAL_HANDLERS — SDL's own SIGINT /
                // SIGTERM translation. Both land here as an ordinary event.
                //
                // ⚠️ It is NOT the guarantee, and S10 assumed it was until it measured. On Windows SDL's
                // signal code is `#undef HAVE_SIGNAL_H`'d out entirely, and on Linux it is gated on an
                // ENVIRONMENT variable. So the desktop shell installs its own handler (see main.cpp) and
                // this arm is the belt to that handler's braces — an UNCLEAN exit either way, so the
                // flush below the loop keeps the work.
                running = false;
            } else if (e.type == SDL_KEYDOWN && e.key.keysym.sym == SDLK_F10) {
                // Dev-only quit, and still NOT part of the button model — it is the desktop escape
                // hatch, and it bypasses the dirty check on purpose (a dev killing a test run does not
                // want to be asked).
                //
                // ⚠️ Not being ASKED is not the same as CHOOSING TO DISCARD, so F10 is an UNCLEAN exit
                // and the flush below keeps the work. That is also the more useful behaviour for the
                // person pressing it: F10 out of a test session, come back, and the session is still
                // there. The one exit that throws work away is the one that says so first.
                //
                // The REAL exit is PROJECT → EXIT (S7): a handheld launcher offers no window chrome to
                // close, so the app has to be able to give the process back from inside. It asks when
                // there is unsaved work, and its YES is the app's ONE clean death — the only path that
                // deletes the autosave rather than writing one.
                running = false;
            } else if (e.type == SDL_FINGERDOWN || e.type == SDL_FINGERUP ||
                       e.type == SDL_FINGERMOTION) {
                // A virtual-button touch feeds SdlInput's OWN press/release, so downstream it is
                // indistinguishable from a key or a pad — combos, key-repeat and held-de-dup all apply
                // with nothing rewritten for fingers. A finger outside a button (on the frame, in a
                // gap) is ignored.
                touch.handle_finger(e, input, now);
            } else {
                input.handle_event(e, now);
            }
        }
        input.tick(now);

        // The frame's clock, handed to the dispatcher once. It feeds the L+B multi-tap window — a
        // class whose behaviour is a function of time must be GIVEN the time, not go looking for it.
        dispatch.set_now(static_cast<long long>(now));

        ui::ButtonEvent be;
        while (input.poll(be)) {
            // ⚠️ Also here, and it is not redundant with the SDL loop above: KEY REPEAT is generated
            // by `input.tick()` from the clock, so a held A+UP produces a ButtonEvent — and an edit —
            // on frames where SDL delivered no event at all. Without this the screen would freeze
            // while a held button was still changing the document underneath it.
            sawInput = true;
            ui::handle_button(be, dispatch, mapper, now);
        }

        // ── The scripted-run hooks (dev only; both are 0 unless an env var set them) ─────────────
        if ((autoplayMs || quitAfterMs) && firstFrameMs == 0) firstFrameMs = now;
        if (autoplayMs && !autoplayFired && now - firstFrameMs >= autoplayMs) {
            autoplayFired = true;
            // The user's path, not a shortcut around it: this is the same call the input layer makes.
            ui::handle_button({ui::Button::START, ui::ButtonAction::PRESSED, ui::ButtonMods{}}, dispatch,
                              mapper, now);
            std::printf("dev:     AUTOPLAY pressed START at %llu ms\n",
                        static_cast<unsigned long long>(now - firstFrameMs));
            std::fflush(stdout);
        }
        if (quitAfterMs && now - firstFrameMs >= quitAfterMs) {
            std::printf("dev:     QUIT_AFTER reached at %llu ms\n",
                        static_cast<unsigned long long>(now - firstFrameMs));
            running = false;
        }

        // Which instrument a live MIDI key plays on a track the sequencer has not touched (E2). Pushed
        // every frame rather than on change, because the user moves the cursor between frames and there
        // is no change notification to hook — the same reason SCALING is polled sixty lines below.
        //
        // ⚠️ It is the instrument the UI is SHOWING, which is the one the A-button already auditions, and
        // it exists so a correctly configured keyboard is not silent on a stopped song. `TrackInstruments`
        // wins the moment the sequencer plays a note on that track (midi_in.h).
        host.set_midi_in_instrument(state.currentInstrument);

        // ⚠️ The thru verdict can CHANGE mid-session — picking a port on the MIDI screen is exactly how
        // a user arrives at a loopback — and the boot line above would then be a lie for the rest of the
        // run. One line per transition, never per frame: the screen says `THRU OFF: LOOP` where the pick
        // was made, and this is the console's copy of the same news.
        if (host.midi_in_thru() != midiThru) {
            midiThru = host.midi_in_thru();
            std::printf("midi:    THRU %s (device pick)\n", midiThru ? "on" : "OFF - loopback");
            std::fflush(stdout);
        }

        // ⚠️ **IMMEDIATELY ABOVE THE DRAIN, and E5's Android backend is why there is a call here at
        // all.** A POLLED input backend (`MidiManager`, which delivers to Kotlin on a binder thread)
        // fetches its bytes here; the winmm and ALSA backends push from their own threads and this is a
        // no-op for them. Move it below `host.poll()` and every Android MIDI byte waits an extra frame —
        // invisible on the two platforms that ignore the call, which is exactly why the ordering is
        // written down in midi-in-base.h as well.
        if (cfg.midiIn) cfg.midiIn->pump();

        // The lookahead pump — the same call, on the same 60 Hz cadence, that PixelPerfectRenderer's
        // loop makes on Android. ⚠️ Since B3 it no longer releases the MIDI queue when the sender
        // thread is running (host.h's set_midi_pump_external) — the scheduler's lookahead is still all
        // its own. ⚠️ Since E2 it also DRAINS the MIDI-in queue, which is why a live key's latency is
        // this loop's period and not something a backend chose.
        host.poll();

        state.isPlaying = host.is_playing();
        // Eight asks, one per track, because there is no ninth answer that covers them all. A track
        // with no position hands back −1 in every field and the screens draw no marker for it —
        // which is what makes auditioning a phrase leave CHAIN and SONG alone (ui/playhead.h).
        for (int t = 0; t < 8; ++t) {
            const PlaybackPosition p = host.playheads(t);
            state.playheads[t] = {p.songRow, p.chainId, p.chainRow, p.phraseId, p.phraseStep};
            const songcore::LiveSlot q = host.live_queue(t);
            state.liveQueue[t] = {q.targetRow, q.stop, q.immediate, q.armed()};
        }
        // ⚠️ READ BACK FROM THE HOST, not trusted from the toggle that set it: `stop()` and a project
        // load can both end a take, and a screen still drawing LIVE over a transport that has left it
        // would blink markers at a queue nothing is holding.
        state.liveMode     = host.live_mode();
        // The blink phase, handed to the drawing layer rather than reached for by it — see AppState.
        state.blinkPhaseMs = static_cast<int>(now % 1000);
        state.trackMask = host.track_mask();

        // Everything the UI reads back OUT of the engine: the scope's samples, the eight monitored
        // notes, the table's playing row, the SF2 preset list, the mixer's meters. AFTER the transport
        // fields above — the waveform decay is a function of isPlaying, and the table row is only
        // resolved on TABLE. `now` because the meters poll on their own 60 ms cadence, not per frame.
        feed.poll(engineRef, host, state, static_cast<long long>(now));

        // ⚠️ **SETTINGS > SCALING, APPLIED — AND UNTIL C4 NOTHING APPLIED IT.** `scalingBilinear` was
        // read from settings.json, written back to it, and drawn as the `SCALING: BILINEAR/INT` row,
        // and `SdlVideo::set_scaling` had ZERO call sites in the entire tree: the video stayed on its
        // `ScalingMode::INTEGER` default for the life of the process, on every platform. That is
        // exactly what platform_caps.h calls "a setting which configures nothing is a lie told in the
        // user's own UI", shipped on desktop and PortMaster as well as here.
        //
        // Polled rather than pushed on change, because there is no change notification to hook and
        // there are two ways in (the SETTINGS row and a settings.json written by hand). `set_scaling`
        // early-returns when the mode is unchanged, so the steady-state cost is one comparison per
        // frame — and the texture recreation it does otherwise is why this cannot go in `present`.
        video.set_scaling(state.settings.scalingBilinear ? ScalingMode::FIT : ScalingMode::INTEGER);

        // ── C7: THE IDLE FRAME ───────────────────────────────────────────────────────────────────
        //
        // The shell used to draw and present unconditionally, 60 times a second, whether or not one
        // pixel had moved. On a handheld that was a known, accepted cost; on a PHONE it is a straight
        // regression against the Compose app being replaced, which repaints only on real state
        // changes precisely to keep from burning battery — users would have felt it as warmth.
        //
        // TWO GATES, and they answer different questions:
        //
        //   1. ANIMATION (here). While audio is audible the visualizer is genuinely moving, so the
        //      screen must be redrawn every frame even though no input arrived. When nothing is
        //      audible nothing animates, and a redraw can only reproduce what is already there —
        //      EXCEPT after an input, which may have changed something. `audibleEdge` is Kotlin's
        //      "one final bump on the active→idle edge": the frame where sound stops must still be
        //      drawn once, or the scope freezes mid-wave instead of settling flat.
        //   2. PIXELS (`video.present`). The safety net under gate 1 — if the frame is byte-identical
        //      to what is on screen it is not sent. That is what makes gate 1 safe to be WRONG: a
        //      state change this loop failed to anticipate still gets drawn, and merely costs a
        //      comparison. ⚠️ It is also why gate 1 may be conservative and never has to be clever.
        //
        // ⚠️ The LOOP does not slow down — only the DRAWING stops. Input, `host.poll()` and the
        // lifecycle watcher all still run every frame at 60 Hz. Kotlin could afford `delay(50L)` when
        // idle because its visualizer was a separate coroutine; here that would be 50 ms of input lag.
        // ⚠️ `has_pending_timed_work()` is the third term and it is NOT covered by the pixel net: the
        // status line clears itself 5 s after it is set, with no input, and a frame that is never
        // drawn is never compared. Without it a "PROJECT SAVED" would sit on a still screen forever.
        //
        // ⚠️ `resizeSettle` is the FOURTH, and it is a rotation/resize term the pixel net cannot cover
        // either: a settling surface must be repainted at its NEW geometry with no input, or the C7 skip
        // freezes a half-transitioned frame (the reported "rotate → half black / half theme bg, fixed
        // only by touching a button"). While it counts down we both DRAW and clear the pixel-skip
        // (invalidate_backbuffer), so each settle frame is genuinely re-uploaded at the current size.
        //
        // ⚠️ `metersFalling` is the FIFTH, and it is the one place where "audible" and "animating" come
        // apart. The MIXER's peak markers age one segment per 60 ms peak poll and the SPECTRUM strip's
        // bars and peak dots age one step per FRAME, but both age INSIDE the draw — so gate 1 closing is
        // what stops them, and the pixel net cannot help because a frame that is never drawn is never
        // compared. Stop the transport and each hangs for the fall it still owes (the markers ~5 s, the
        // spectrum ~1 s), stepping down once per input instead — any input, mapped or not, because every
        // SDL event sets `sawInput` and buys exactly one frame. The EQ editor's spectrum panel is a third
        // case and a different one: nothing in it ages, but it is only ever as current as the last frame
        // drawn, so a closed gate strands whatever was on screen. See TrackerLayout::has_falling_meters:
        // every term is gated there on its module being drawn at all, since nothing off-screen ages and
        // nothing would ever bring this term back to false.
        const bool metersFalling = layout.has_falling_meters(state);
        const bool audible = audio_is_audible(state);
        const bool settling = resizeSettle > 0;
        if (settling) {
            video.invalidate_backbuffer(/*texture_lost=*/false);   // the settled frame must not be skipped
            --resizeSettle;
        }
        if (audible || audibleEdge || sawInput || !drewOnce || settling || metersFalling ||
            dispatch.has_pending_timed_work()) {
            layout.draw(canvas, state);
            ++drawn;

            // ── The frame, and around it whatever this orientation's skin is ──────────────────────
            //
            // ⚠️ The clear colour is passed per present rather than set once — the live theme's
            // background in landscape (so the 4:3 frame on a 16:9 window reads as one surface, not a
            // picture on a black wall), the device CASING in PORTRAIT2 — because a value that must be
            // re-pushed on change is a value some future screen forgets to re-push (the theme editor can
            // change the first mid-session). The on-screen controls' fingerprint joins the C7 gate so a
            // press highlight is not skipped as an unchanged canvas. The portrait/landscape branch, the
            // CRT overlay (D6) and the C7 signatures all live in `present_current` (declared above the
            // render hooks), so the dispatcher's synchronous repaint takes the identical path.
            if (present_current()) ++presented;
            drewOnce = true;
        } else {
            ++skipped;
            // Nothing drawn — but the frame must still take its 16 ms, or the loop spins hot and the
            // idle path costs MORE than the busy one. See SdlVideo::pace().
            video.idle_frame();
        }
        audibleEdge = audible;   // so the first silent frame is still drawn (the flattened scope)
        sawInput    = false;

        // A status line once a second, kept from the Phase 2 shell and kept for the same reason: on a
        // headless box, over ssh, or during a handheld bring-up where you cannot yet see or hear
        // anything, these numbers are what tell you the chain is alive. The FRAME COUNTER means the
        // audio device is calling back, the PLAYHEAD means the sequencer is advancing, and VOICES
        // means events are reaching the engine and turning into sound. Any one stuck at zero names the
        // broken link. A window on screen does not answer that question when there is no screen.
        //
        // TRACK 0 of the eight, and it is the right one to print: a PHRASE or CHAIN audition plays
        // track 0 by default, and under SONG any track advancing proves the clock is alive, which is
        // the whole job of this line. −1 shows as such, and means that track has no position at all.
        if (cfg.console && now - lastStatus >= 1000) {
            lastStatus = now;
            const ui::TrackPlayhead& t0 = state.playheads[0];
            std::printf(
                "%s  frame %-10lld  song %3d  chain %2d  step %2d   voices %2d   %-10s cursor %X,%d"
                "   drew %lld skip %lld same %lld\n",
                host.is_playing() ? "play" : "stop",
                static_cast<long long>(engineRef.getCurrentFrame()), t0.songRow, t0.chainRow,
                t0.step, engineRef.getActiveVoiceCount(), ui::screen_label(state.currentScreen),
                state.cursorRow, state.cursorColumn, presented, skipped, drawn - presented);
            std::fflush(stdout);  // block-buffered to a pipe otherwise, and then it says nothing
        }
    }

    // ── Leaving ──────────────────────────────────────────────────────────────────────────────────
    //
    // ⚠️ The watcher goes FIRST, and it is not tidiness: `bg` is a stack object in this frame and
    // every line below can push an SDL event. A watcher left installed past this point is a
    // use-after-free during teardown.
    //
    // ⚠️ Removing it does NOT make the Android exit path unsafe. A real destroy (`nativeSendQuit`)
    // injects SDL_QUIT and unblocks the loop, so control arrives here and the two saves below run
    // normally — the watcher's job was the Home press that never reaches this line at all.
    SDL_DelEventWatch(on_app_event, &bg);

    // ⚠️ **THE SENDER THREAD GOES BEFORE `host.stop()`, AND THE ORDER IS THE POINT.** Once it has
    // joined, `poll()` owns the release again and the panic below runs on the only thread left — so the
    // note-offs a stop owes are sent by definition rather than by winning a race against a thread that
    // is being torn down. It also must be joined before `host` (its `pump` target) leaves scope, and
    // before `audio.closeStream()` takes away the frame counter it reads.
    midiSender.stop();

    // ⚠️⚠️ **THE INPUT PORT IS CLOSED HERE, AND IT IS A LIFETIME FIX RATHER THAN POLITENESS.** The port
    // object belongs to the platform's `main` and OUTLIVES this function; the queue it delivers into is
    // inside `host`, which does not. Leave it open and a byte arriving during teardown — a key released
    // as the user quits, a controller's own all-notes-off — is a backend thread writing into a destroyed
    // object. Unwire first, then close: `close()` is what waits for a callback already in flight.
    if (cfg.midiIn) {
        cfg.midiIn->set_sink(nullptr);
        cfg.midiIn->close();
    }

    // The E2 verdict, and it prints on every run that opened a port — the counters ARE the instrument
    // for a path whose failure is silence. Four stages, so a break can be located rather than guessed:
    // the port received nothing (no cable, or the device is not sending), the queue dropped (the drain
    // is not keeping up), the parser saw orphans (the stream was joined mid-message), or the router
    // routed nothing (nothing is mapped — and its four counters say which of the four reasons).
    if (cfg.midiIn && (host.midi_in_bytes() > 0 || midiInPortWasOpen)) {
        MidiInBase* base = cfg.midiIn;
        const songcore::MidiInputRouter& r = host.midi_in_router();
        std::printf("midi in: %llu bytes at the port (%llu callbacks, %llu port errors), %llu drained, "
                    "%llu messages\n"
                    "         routed %llu, dropped: %llu non-channel, %llu unmapped, %llu no-instrument, "
                    "%llu unsupported\n"
                    "         queue overflow %llu bytes, parser orphans %llu, messages with no record %llu\n"
                    // ⭐ THE E4 LINE, and it is the one that says whether anything was HEARD. `routed`
                    // above counts records the router made; these count records the ENGINE and the
                    // CABLE were actually handed — the two differ by exactly the thru suppression, so a
                    // silent keyboard on an EXTERNAL instrument names itself here instead of looking
                    // like a dead cable three lines up.
                    "         injected %llu into the engine; thru %s: %llu to the cable, %llu suppressed\n",
                    static_cast<unsigned long long>(base->bytes_received()),
                    static_cast<unsigned long long>(base->callbacks()),
                    static_cast<unsigned long long>(base->port_errors()),
                    static_cast<unsigned long long>(host.midi_in_bytes()),
                    static_cast<unsigned long long>(host.midi_in_messages()),
                    static_cast<unsigned long long>(r.routed()),
                    static_cast<unsigned long long>(r.nonChannel()),
                    static_cast<unsigned long long>(r.unmapped()),
                    static_cast<unsigned long long>(r.noInstrument()),
                    static_cast<unsigned long long>(r.unsupported()),
                    static_cast<unsigned long long>(host.midi_in_sink().dropped()),
                    static_cast<unsigned long long>(host.midi_in_parser().orphan_bytes()),
                    static_cast<unsigned long long>(midiInConsole.silent),
                    static_cast<unsigned long long>(host.midi_in_injected()),
                    host.midi_in_thru() ? "on" : "OFF (loopback)",
                    static_cast<unsigned long long>(host.midi_in_thru_sent()),
                    static_cast<unsigned long long>(host.midi_in_thru_suppressed()));
    }

    //
    // ⚠️ Settings are written HERE, not on every keystroke. Holding A+UP on a hex-byte setting fires
    // an edit every 100 ms (the key-repeat interval), and one file write per repeat is an SD card
    // being hammered for a value that is still moving.
    //
    // ⚠️⚠️ …but NOT behind a dirty FLAG, and that distinction cost a real bug. This used to read
    // `if (state.settingsDirty)`, and the only thing that ever SET that flag was the SETTINGS screen's
    // edit arm — so a palette dialled in the THEME EDITOR (which mutates the theme directly, having no
    // CursorContext to route through) armed nothing and was silently thrown away on quit. The verb below
    // asks the DATA instead: it writes only when the bytes on disk differ from what memory holds, so it
    // is still one write per session at most, and there is no longer anything for the next screen that
    // touches the theme to forget. See ui/settings_store.h, and ptdispatch §27(c) — which fails if the
    // arming ever comes apart again.
    switch (ui::save_settings_if_changed(filesystem, state.settings, state.theme)) {
        using SW = ui::SettingsWrite;
        case SW::UNCHANGED: break;   // nothing moved this session; the file already says so
        case SW::SAVED:     std::printf("settings: saved\n"); break;
        // A full SD card, a read-only mount. S9's lesson, one file over: the only save in the app with
        // no result at all was a dropped error return, and it read as success.
        case SW::FAILED:
            std::printf("settings: SAVE FAILED - %s\n", filesystem.settings_path().c_str());
            break;
    }

    // ⚠️ **THE FLUSH — and every way out of the loop above arrives here, which is the design.**
    //
    // The 3 s debounce can lose the last few edits if the process is taken away before it fires, and on
    // a handheld it very often is: the CFW menu kills the port, the battery goes, the power slider is a
    // switch and not a request. So the exit path flushes synchronously, on the main thread, while there
    // is still a heap and a filesystem to do it with.
    //
    // ⚠️ It is a NO-OP when the document is clean, and that is what keeps the file's meaning intact:
    // "an autosave exists" must mean "the last session ended badly and there is work in it". A confirmed
    // PROJECT → EXIT has already DELETED the autosave (confirm_accept) and made the project clean, so
    // this writes nothing — the one exit the user was asked about is the one exit that leaves no trace.
    // Everything else — SIGTERM, SIGINT, the window's close button, F10 — never asked, so it keeps the
    // work, and the next launch says so.
    dispatch.flush_autosave();

    host.stop();

    // The B3 verdict, after the last message has left (the panic in `host.stop()` is part of the run).
    if (jitterOn) {
        host.midi_out().set_send_observer(nullptr);
        // ⚠️ `senderOn` is latched at START, not read here: `midiSender.stop()` above has already
        // cleared `running()`, so asking the object would label every run "60 Hz" — a mislabelled
        // measurement, which is worse than no measurement.
        // The TEMPO is phase C's anchor: the clock block derives BPM from a wall clock and from the due
        // frames, and this is the third number they are both checked against. Read from the live
        // project, which is the same value the clock's period came from.
        midiJitter.report(senderOn ? "B3 sender thread" : "60 Hz frame loop (pre-B3)", audio.sampleRate(),
                          host.project().tempo);
    }

    engineRef.onResumeRequested = nullptr;
    audio.closeStream();
    input.close_controllers();
    arrowFont.unload();  // ⚠️ same reason as skin/helvFont: glyph textures belong to the renderer
    helvFont.unload();  // ⚠️ same reason as skin: its glyph textures belong to the renderer being destroyed
    skin.unload();   // ⚠️ before video.close(): the skin's textures belong to the renderer it destroys
    screenOverlay.unload();  // ⚠️ same reason: the CRT overlay's texture belongs to that renderer
    video.close();
    return 0;
}

}  // namespace ptshell
