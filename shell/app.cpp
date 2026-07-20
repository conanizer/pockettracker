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
#include "ui/settings_store.h"

#include "sdl-input.h"
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
    if (!video.open("PocketTracker", ui::DESIGN_W, ui::DESIGN_H)) {
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
        video.present(canvas);
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

        SDL_Event e;
        while (SDL_PollEvent(&e)) {
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

        layout.draw(canvas, state);
        video.present(canvas);

        // A status line once a second, kept from the Phase 2 shell and kept for the same reason: on a
        // headless box, over ssh, or during a handheld bring-up where you cannot yet see or hear
        // anything, these numbers are what tell you the chain is alive. The FRAME COUNTER means the
        // audio device is calling back, the PLAYHEAD means the sequencer is advancing, and VOICES
        // means events are reaching the engine and turning into sound. Any one stuck at zero names the
        // broken link. A window on screen does not answer that question when there is no screen.
        if (cfg.console && now - lastStatus >= 1000) {
            lastStatus = now;
            std::printf(
                "%s  frame %-10lld  song %3d  chain %2d  step %2d   voices %2d   %-10s cursor %X,%d\n",
                host.is_playing() ? "play" : "stop",
                static_cast<long long>(engineRef.getCurrentFrame()), pos.songRow, pos.chainRow,
                pos.phraseStep, engineRef.getActiveVoiceCount(), ui::screen_label(state.currentScreen),
                state.cursorRow, state.cursorColumn);
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
