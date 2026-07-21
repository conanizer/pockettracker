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
// active. The threshold and the spectrum release are its constants, not new ones.
constexpr float SCOPE_SILENCE_THRESHOLD = 0.002f;   // PixelPerfectRenderer.kt:101
constexpr int   SPECTRUM_RELEASE_FRAMES = 75;       // PixelPerfectRenderer.kt:106

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
    AudioEngine&  engineRef  = *cfg.engine;
    AudioBackend& audio      = *cfg.audio;
    ui::FileSystem& filesystem = *cfg.filesystem;

    // The engine asks for its stream back without knowing what a stream is. Unwired at teardown
    // below, before the backend is closed — a callback firing into a dead lambda is the one ordering
    // mistake this pair can make.
    engineRef.onResumeRequested = [&audio] { audio.resumeStream(); };

    SongcoreHost host(&engineRef, audio.sampleRate());

    // THIS install's app root — where Projects/ Samples/ Soundfonts/… live (also the FileSystem's root).
    // Handed to the host BEFORE the first load: a project opened at boot may have been authored on
    // another install (a phone's Documents/PocketTracker) whose absolute media paths are dead here until
    // re-rooted onto ours. See set_app_root.
    host.set_app_root(cfg.appRoot);

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
        std::printf("input:   TRACE ON — every event prints, with what it mapped to (or did not)\n");
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
    SdlTouch touch;
    touch.set_enabled(cfg.touchCapable && input.controller_count() == 0);
    if (inputTrace && inputTrace[0] == '1') touch.set_trace(true);

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
    if (ui::load_settings(filesystem, state.settings, state.theme))
        std::printf("settings: %s\n", filesystem.settings_path().c_str());

    ui::Canvas        canvas;
    ui::TrackerLayout layout;
    ui::EngineFeed    feed;

    // The whole input layer, in one object. It edits `host.edit_project()` — the SAME Project the
    // Sequencer is reading — so an edit is live the instant it is made.
    ui::InputDispatcher dispatch(state, host, filesystem);
    ui::MapperState     mapper;

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
    ui::InputDispatcher::RenderHooks hooks;
    hooks.suspend_audio = [&audio](bool suspend) { audio.setPaused(suspend); };
    hooks.repaint       = [&]() {
        layout.draw(canvas, state);
        video.present(canvas, state.theme.background);
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
        std::printf("A+UP/DOWN edit   A+LEFT/RIGHT edit fast   A+B clear   A,A insert next unused\n");
        std::printf("B+LEFT/RIGHT change WHICH phrase/chain/table   B+UP/DOWN page the song\n");
        std::printf("L+B select (tap again to widen)   B copies   L+A cut/paste   L+R deselect   L+B+A clone\n");
        // ASCII only, deliberately: this goes to a console whose encoding is not ours to choose (a
        // handheld's serial/ssh terminal, a Windows box on a legacy code page), and a stray em-dash
        // arrives there as mojibake.
        std::printf("A+UP on an FX-TYPE column opens the effect picker - release A to choose\n");
        std::printf("R+DPAD moves between screens: SONG CHAIN PHRASE INSTRUMENT TABLE MODS INST.POOL\n");
        std::printf("                             GROOVE MIXER EFFECTS PROJECT SETTINGS\n");
        std::printf("PROJECT: A on SAVE/LOAD/NEW, on EXPORT MIX/STEMS, on COMPACT SEQ/INST, on SETTINGS>, on EXIT\n");
        std::printf("         A on NAME opens the keyboard; A+UP/DOWN edits one character in place\n");
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
        std::printf("  DPAD UP/DOWN picks the param, LEFT/RIGHT the band   A+UP/DOWN and A+LEFT/RIGHT dial it\n");
        std::printf("  A+B resets it   B+LEFT/RIGHT changes the EQ SLOT   B or SELECT closes\n");
        std::printf("  START still auditions underneath, so you can sweep a band across a ringing note\n\n");
    }

    bool   running    = true;
    Uint64 lastStatus = 0;

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
            touch.layout(video.frame_rect(), outW, outH);
        }

        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            // ⚠️ C7: ANY event counts, not just the ones that map to a button. A window resize, an
            // expose, a focus change, a rotation and the system bars appearing are all events that
            // change what should be on screen while producing no ButtonEvent at all — and being
            // over-inclusive here costs one redrawn frame, while being under-inclusive costs a stale
            // window. The gate is allowed to be conservative; that is what the pixel compare in
            // `present` is for.
            sawInput = true;

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

        // The lookahead pump — the same call, on the same 60 Hz cadence, that PixelPerfectRenderer's
        // loop makes on Android.
        host.poll();

        const PlaybackPosition pos = host.playheads();
        state.isPlaying        = host.is_playing();
        state.playbackRow      = pos.phraseStep;
        state.playbackChainRow = pos.chainRow;
        state.playbackSongRow  = pos.songRow;
        state.trackMask        = host.track_mask();

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
        const bool audible = audio_is_audible(state);
        if (audible || audibleEdge || sawInput || !drewOnce || dispatch.has_pending_timed_work()) {
            layout.draw(canvas, state);
            ++drawn;

            // ⚠️ The letterbox is painted the LIVE theme's background, so the 4:3 frame on a 16:9
            // window reads as one surface instead of a picture on a black wall. Passed per present
            // rather than set once — the theme editor can change it mid-session, and a value that
            // must be re-pushed on change is a value some future screen forgets to re-push.
            //
            // The touch panels ride the same present: drawn into the bars after the frame, and their
            // fingerprint joins the C7 gate so a press highlight is not skipped as an unchanged canvas.
            // Both are inert when there is no touchscreen — the overlay draws nothing, the sig is 0.
            const auto overlay = [&touch, &input](SDL_Renderer* r) { touch.draw(r, input); };
            if (video.present(canvas, state.theme.background, overlay, touch.signature(input)))
                ++presented;
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
        if (cfg.console && now - lastStatus >= 1000) {
            lastStatus = now;
            std::printf(
                "%s  frame %-10lld  song %3d  chain %2d  step %2d   voices %2d   %-10s cursor %X,%d"
                "   drew %lld skip %lld same %lld\n",
                host.is_playing() ? "play" : "stop",
                static_cast<long long>(engineRef.getCurrentFrame()), pos.songRow, pos.chainRow,
                pos.phraseStep, engineRef.getActiveVoiceCount(), ui::screen_label(state.currentScreen),
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
    engineRef.onResumeRequested = nullptr;
    audio.closeStream();
    input.close_controllers();
    video.close();
    return 0;
}

}  // namespace ptshell
