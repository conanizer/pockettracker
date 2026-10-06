#include "app.h"

// <cmath> before <SDL.h> — see the note in sdl-audio-engine.h (M_PI, _USE_MATH_DEFINES, C4005).
#include <cmath>

#include <SDL.h>

#include "audio-backend.h"
#include "audio-engine.h"
#include "common/load_progress.h"   // set_load_tick
#include "songcore/host.h"
#include "ui/app_state.h"
#include "ui/button_mapper.h"
#include "ui/buttons.h"
#include "ui/canvas.h"
#include "ui/engine_feed.h"
#include "ui/input_dispatcher.h"
#include "ui/layout.h"
#include "ui/modules/oscilloscope.h"   // WAVEFORM_SIZE
#include "ui/scale_io.h"              // seed_scale_bank
#include "ui/groove_io.h"             // seed_groove_bank
#include "ui/settings_store.h"

#include "skin/device_skin.h"
#include "skin/skin.h"
#include "skin/overlay.h"
#include "skin/font.h"
#include "skin/portrait2.h"

#include "latency_probe.h"   // POCKETTRACKER_LATENCY=1
#include "midi/midi-in.h"      // the platform's IMidiIn, or nothing
#include "midi/midi-sender.h"
#include "sdl/sdl-input.h"
#include "sdl/sdl-touch.h"
#include "sdl/sdl-video.h"

#include <algorithm>
#include <cstdio>

using namespace songcore;
namespace ui = pt::ui;

namespace ptshell {

namespace {

// ─── The MIDI-in console ─────────────────────────────────────────────────────────────────────────
//
// Counts what MIDI input delivered, printed at exit. A working input and one that never received a
// byte look identical inside the app, so these counts are how the two are told apart. `silent`
// separates "the cable is dead" from "no track is listening on that channel".
//
// Runs on the frame loop's thread (SongcoreHost::poll calls it), so it may print.
struct MidiInConsole : songcore::IMidiInObserver {
    bool     trace    = false;   // POCKETTRACKER_MIDI_IN_TRACE=1
    uint64_t messages = 0;
    uint64_t records  = 0;
    uint64_t silent   = 0;       // messages that produced no bus record at all

    // How fast CCs arrive while a knob turns: per poll (`ccMaxPerDrain` says whether batching CCs
    // into one apply could gain anything), per 16 ms frame, and the peak per second.
    // ⚠️ The maxima count every controller number, so two knobs at once read as one stream.
    // ⚠️ Messages from one poll share the poll's timestamp, so a window never splits a poll.
    uint64_t ccTotal   = 0;
    uint64_t ccFirstMs = 0;      // wall clock of the first and last CC — the span the rate is over
    uint64_t ccLastMs  = 0;
    uint64_t ccDrains  = 0;      // drains that carried at least one CC
    uint64_t ccMaxPerDrain = 0;
    uint64_t drain     = 0;      // the poll that took the messages back: bumped above host.poll()

    /** Most CCs inside one fixed window of `width` ms. A burst across a boundary counts as two, so
     *  this is a floor. */
    struct Window {
        uint64_t width;
        uint64_t id  = ~0ull;    // a value no window index can have, so the first CC opens one
        uint64_t n   = 0;
        uint64_t max = 0;

        void add(uint64_t nowMs) {
            const uint64_t w = nowMs / width;
            if (w != id) { id = w; n = 0; }
            if (++n > max) max = n;
        }
    };
    Window w16{16}, w100{100}, w1000{1000};

    void count_cc(uint64_t nowMs) {
        if (ccTotal++ == 0) ccFirstMs = nowMs;
        ccLastMs = nowMs;

        if (drain != ccDrain_) { ccDrain_ = drain; ccInDrain_ = 0; ++ccDrains; }
        ++ccInDrain_;
        if (ccInDrain_ > ccMaxPerDrain) ccMaxPerDrain = ccInDrain_;

        w16.add(nowMs);
        w100.add(nowMs);
        w1000.add(nowMs);
    }

    void on_midi_in(const songcore::MidiInMessage& m, const songcore::Event* ev, int n) override {
        ++messages;
        records += static_cast<uint64_t>(n < 0 ? 0 : n);
        if (n <= 0) ++silent;
        if (m.status == songcore::EV_CC) count_cc(static_cast<uint64_t>(SDL_GetTicks64()));
        if (!trace) return;

        // The bytes are rebuilt from the parsed message, not copied from the wire, so a trace line
        // that disagrees with the cable shows a parser bug.
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

  private:
    // Starts at a value no drain index can have, so the first CC opens a run of its own.
    uint64_t ccDrain_ = ~0ull, ccInDrain_ = 0;
};

// ─── The background watcher ──────────────────────────────────────────────────────────────────────
//
// What on_app_event needs, borrowed from run()'s stack.
// ⚠️ The watcher must be removed before run() returns — the first step of its teardown.
struct BackgroundContext {
    SongcoreHost*        host       = nullptr;
    ui::InputDispatcher* dispatch   = nullptr;
    ui::FileSystem*      filesystem = nullptr;
    ui::AppState*        state      = nullptr;
    AudioBackend*        audio      = nullptr;
    bool                 console    = false;
    const AppConfig::BackgroundPlayback* background = nullptr;

    // Set by the watcher, cleared by the loop once it has reopened the device. No lock: the watcher
    // runs on the loop's own thread (see on_app_event).
    bool audioClosed = false;

    // Off the screen: nothing may be drawn (the GL context is backed up). Cleared on
    // SDL_APP_DIDENTERFOREGROUND.
    bool backgrounded = false;
    // …and a song was left playing there, under the playback service.
    bool playingInBackground = false;
};

/**
 * SDL_APP_WILLENTERBACKGROUND — the Android Home press, and the last moment this process is sure to
 * run. Saves what would otherwise be lost and hands the audio device back.
 *
 * ⚠️ Runs on the NATIVE thread, inside the loop's own SDL_PollEvent: SDL's onPause only posts a
 * semaphore, the native pump then sends this event, and SDL calls watchers synchronously. So it may
 * touch `host`, `state` and the project with no lock. A watcher rather than a case in the poll loop
 * because SDL flushes the event queue after a quit.
 *
 * Never called on desktop (SDL sends it on Android and iOS only). Every step is idempotent, since
 * background → foreground → background fires it again.
 */
int SDLCALL on_app_event(void* userdata, SDL_Event* e) {
    if (e->type != SDL_APP_WILLENTERBACKGROUND) return 0;

    auto* c = static_cast<BackgroundContext*>(userdata);

    // Always printed: every step below is usually a no-op, so without this line a watcher that ran
    // and one that never fired leave the same log.
    if (c->console) std::printf("lifecycle: entering background\n");

    c->backgrounded = true;

    // 1. A playing song keeps playing, if Android let the playback service start. ⚠️ Without the
    // service Android freezes the process and the audio server silently retires the stream — so stop.
    const AppConfig::BackgroundPlayback* bp = c->background;
    const bool keep = c->host->is_playing() && bp && bp->serviceStarted && bp->serviceStarted();
    if (keep) {
        c->playingInBackground = true;
        if (c->console) std::printf("lifecycle: backgrounded - still playing, under the service\n");
    } else {
        if (bp && bp->end) bp->end();   // a service onPause started for a song that has since stopped
        if (c->host->is_playing()) {
            c->host->stop();
            if (c->console) std::printf("lifecycle: backgrounded - playback stopped\n");
        }
    }

    // 2. The autosave: a backgrounded app can be killed without notice, before the 3 s debounce fires.
    c->dispatch->flush_autosave();

    // 3. Settings: on Android the loop is left only on a real destroy, so the save below the loop
    // never runs after a Home press.
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

    // 4. Hand the audio device back. ⚠️ A stream left open across backgrounding comes back dead: the
    // frozen process stops answering its pulls and the audio server retires it without telling us.
    // Last, because the close waits for the callback in flight. The loop reopens it on return.
    if (!keep) {
        c->audio->closeStream();
        c->audioClosed = true;
        if (c->console) std::printf("lifecycle: backgrounded - audio device released\n");
    }

    std::fflush(stdout);

    return 0;  // watchers do not consume; the event still reaches the queue
}

// ─── Is anything on screen moving? ───────────────────────────────────────────────────────────────
//
// The loop always runs at full rate; only the DRAW is skipped when nothing changes, to save battery.
// Audible = the transport playing, the preview lane active, or the master waveform above the silence
// floor (a preview still ringing after STOP). Meters still falling after the sound stops are a
// separate term in the loop.
constexpr float SCOPE_SILENCE_THRESHOLD = 0.002f;

bool audio_is_audible(const ui::AppState& s) {
    if (s.isPlaying || s.previewLaneActive) return true;

    // ⚠️ Null means SILENCE, not "unknown": engine_feed leaves it null when there is nothing to show,
    // and the headless screenshot tool draws with no engine at all.
    if (s.waveform) {
        for (int i = 0; i < ui::WAVEFORM_SIZE; ++i) {
            if (std::fabs(s.waveform[i]) > SCOPE_SILENCE_THRESHOLD) return true;
        }
    }
    return false;
}

// ─── Boot steps ──────────────────────────────────────────────────────────────────────────────────

/** Opens the project the platform asked for, or the blank document. False = it did not parse. */
bool open_boot_project(const AppConfig& cfg, SongcoreHost& host) {
    // A requested project's bytes were read by the platform before any window existed.
    if (!cfg.projectPath.empty()) {
        if (!host.push_project(cfg.projectBlob)) {
            std::fprintf(stderr, "%s did not parse as a .ptp\n", cfg.projectPath.c_str());
            return false;
        }

        const MediaLoadResult media = host.load_media(cfg.mediaBaseDir);
        std::printf("project: %s\nmedia:   %d loaded, %d failed (base dir: %s)\n",
                    cfg.projectPath.c_str(), media.loaded, media.failed, cfg.mediaBaseDir.c_str());
        if (media.failed > 0) {
            std::fprintf(stderr,
                         "warning: %d sample(s)/SoundFont(s) failed to load - those instruments will be "
                         "silent\n",
                         media.failed);
        }
    } else {
        // The same blank document NEW PROJECT builds; it references no media.
        host.new_project();
        std::printf("project: (none given) - starting on a blank document\n");
    }
    return true;
}

/**
 * Creates the app folders at boot, not on the first browse: on a handheld the user's first move is
 * copying samples onto the SD card, and the folders must already be there. The names are the same on
 * every platform, so a PocketTracker/ folder copied off a phone is found where the app looks.
 */
void make_app_folders(const AppConfig& cfg, ui::FileSystem& filesystem) {
    filesystem.projects_directory();
    filesystem.samples_directory();
    filesystem.renders_directory();
    filesystem.instruments_directory();
    filesystem.soundfonts_directory();
    filesystem.themes_directory();
    filesystem.scales_directory();
    filesystem.grooves_directory();
    std::printf("files:   %s\n", cfg.appRoot.c_str());

    // The app's own files (settings, template, autosave) may live elsewhere — on Android they do.
    // Derived by comparing paths, so there is no flag to keep in step.
    if (filesystem.settings_path() != cfg.appRoot + "/settings.json")
        std::printf("files:   app files: %s\n", filesystem.settings_path().c_str());

    // The count, not just the path: a storage backend that lists nothing looks like an empty folder.
    const std::string projects = filesystem.projects_directory();
    std::printf("files:   projects: %s (%zu entries)\n", projects.c_str(),
                filesystem.list_files(projects).size());
}

/** settings.json, then config.json and the factory banks it sits beside. */
void load_settings_and_config(ui::FileSystem& filesystem, ui::AppState& state, SdlInput& input) {
    // No file means a first launch and the factory settings. ⚠️ load_settings also returns false for
    // a file that will not read or parse, and the save at quit would then overwrite it — so that case
    // is reported.
    if (ui::load_settings(filesystem, state.settings, state.theme)) {
        std::printf("settings: %s\n", filesystem.settings_path().c_str());
    } else if (filesystem.file_exists(filesystem.settings_path())) {
        std::fprintf(stderr,
                     "warning: %s exists but could not be read or parsed - starting on the factory "
                     "settings, and they will be saved over it at quit\n",
                     filesystem.settings_path().c_str());
    }

    // ── config.json, the hand-edited configuration ───────────────────────────────────────────────
    //
    // Read at boot on every platform. A starter template is seeded only when the file is absent; it
    // states every current value, so seeding changes nothing.
    // ⚠️ The binding defaults come from the input layer: pt-ui cannot name an SDL key or pad button.
    if (ui::seed_config_template(filesystem, SdlInput::default_keyboard_bindings(),
                                 SdlInput::default_gamepad_bindings()))
        std::printf("config:   seeded template %s\n", filesystem.config_path().c_str());

    // The same text again as config.example.json, so settings newer than the user's file can be
    // found. Not on the Miyoo Mini: its launcher copies the Miyoo template there, and ours would
    // name the desktop keys.
    if (!SdlInput::on_miyoo_mini() &&
        ui::write_config_example(filesystem, SdlInput::default_keyboard_bindings(),
                                 SdlInput::default_gamepad_bindings()))
        std::printf("config:   wrote %s\n", ui::config_example_path(filesystem).c_str());

    // The factory scales as editable files, written only when the folder has no .pts at all, so a
    // user's pruning is kept. The count is printed: a seed that wrote nothing is otherwise invisible.
    if (const int seeded = ui::seed_scale_bank(filesystem); seeded > 0)
        std::printf("files:   seeded %d factory scales in %s\n", seeded,
                    filesystem.scales_directory().c_str());

    // The factory grooves, on the same terms.
    if (const int seeded = ui::seed_groove_bank(filesystem); seeded > 0)
        std::printf("files:   seeded %d factory grooves in %s\n", seeded,
                    filesystem.grooves_directory().c_str());

    // `folders` → the browser's start folders. The whole rule is ui::resolve_browse_dir.
    if (ui::load_folder_config(filesystem, state.folderConfig))
        std::printf("config:   %s\n", filesystem.config_path().c_str());

    // `controller`, `keyboard`, `gamepad`, `repeat` → this shell's input layer. Every rejected entry is printed: a
    // hand-edited file that looks applied and is not is the worst way for this to fail.
    {
        ui::InputConfig                     inputCfg;
        std::vector<ui::InputConfigWarning> warnings;
        ui::load_input_config(filesystem, inputCfg, warnings);
        for (const ui::InputConfigWarning& w : warnings) std::printf("config:   %s\n", w.text.c_str());
        input.apply_input_config(inputCfg);

        // config.json only SEEDS the ABXY row: once the row says anything but AUTO, the row wins.
        if (state.settings.abxyIndex == 0 && inputCfg.abxy != ui::AbxyLayout::AUTO)
            state.settings.abxyIndex = static_cast<int>(inputCfg.abxy);
    }

    // The saved skin id (a stable string) → the index the SETTINGS skin column edits. An unknown id
    // falls back to DARK.
    state.settings.skinIndex = device_skin_index(state.settings.portraitSkin);

    // The saved overlay name → its index (0 = OFF), on the same stable-string terms (shell/skin/overlay.h).
    state.settings.overlayIndex = screen_overlay_index(state.settings.overlayName);
}

/**
 * The backends go into AppState so the MIDI screen can list devices. Either may be null, and every use
 * is guarded. The env-var overrides land as settings before anything opens (see app.h).
 */
void apply_midi_overrides(const AppConfig& cfg, ui::AppState& state) {
    state.midiOut = cfg.midiOut;
    state.midiIn  = cfg.midiIn;
    if (!cfg.midiInDevice.empty())  state.settings.midiInDevice  = cfg.midiInDevice;
    if (!cfg.midiOutDevice.empty()) state.settings.midiOutDevice = cfg.midiOutDevice;
    if (cfg.midiOffsetMs != 0) {
        state.settings.midiOffsetMs   = cfg.midiOffsetMs;
        // ⚠️ …and AUTO off, or the override would be saved and change nothing audible.
        state.settings.midiOffsetAuto = false;
    }
    if (cfg.midiSyncOut >= 0)       state.settings.midiSyncOut   = cfg.midiSyncOut != 0;
}

/**
 * The OFFSET row's AUTO. MIDI is released when its block of sound is handed to the device, so the cable
 * leads the speakers by the output latency. The app usually sees only one buffer of it, so this lands
 * close rather than exact; an ASIO driver reports the whole latency. Derived again when SETTINGS >
 * AUDIO OUT changes the output.
 */
void derive_midi_auto_offset(AudioBackend& audio, ui::AppState& state) {
    const int rate = audio.sampleRate();
    const AudioBackend::OutputLatency lat = audio.outputLatency();
    if (rate > 0 && lat.frames > 0)
        state.midiAutoOffsetMs = (lat.frames * 1000 + rate / 2) / rate;
}

/** Opens the MIDI ports the settings name, one console line each, and reports THRU. */
void open_midi_ports(const AppConfig& cfg, SongcoreHost& host, ui::InputDispatcher& dispatch,
                     const ui::AppState& state) {
    // Opens the port the settings name and pushes the OFFSET. The MIDI screen's OUTPUT row runs the
    // same code, so boot and UI cannot disagree about which device is open.
    dispatch.boot_midi_port();
    if (cfg.midiOut) {
        // ⚠️ Prints the offset in force, which under AUTO is not the stored one.
        std::printf("midi:    OUT %s (offset %+d ms%s, sync %s)\n",
                    cfg.midiOut->is_open() ? state.midiOutOpenName.c_str() : "OFF",
                    ui::midi_offset_in_force(state.settings, state.midiAutoOffsetMs),
                    state.settings.midiOffsetAuto ? " AUTO" : "",
                    state.settings.midiSyncOut ? "ON 24 PPQN" : "off");
    }

    // The input port, by the same rule. One line whenever a backend exists, so "not plugged in" and
    // "refused to open" can be told apart from "not compiled in".
    dispatch.boot_midi_in_port();
    if (cfg.midiIn) {
        std::printf("midi:    IN  %s (%d port(s) enumerated)\n",
                    cfg.midiIn->is_open() ? state.midiInOpenName.c_str() : "OFF",
                    static_cast<int>(state.midiInDeviceNames.size()) - ui::MIDI_FIRST_PORT);
    } else {
        // Every shipping platform has a backend; a Linux box without libasound still has one, with no
        // devices.
        std::printf("midi:    IN  no input backend compiled into this build\n");
    }

    // THRU: a live key on an EXTERNAL instrument goes out on the cable — except when IN and OUT are the
    // same device, where it would feed back. Both outcomes print; the suppression is invisible otherwise.
    const bool midiThru = host.midi_in_thru();
    std::printf("midi:    THRU %s%s\n", midiThru ? "on" : "OFF",
                midiThru ? " (a live key on an EXTERNAL instrument reaches the cable)"
                         : " - IN and OUT are the SAME port, and thru on a loopback is a feedback loop");
}

/** The help banner. ASCII only: the console's encoding is not ours (a serial or ssh terminal, a legacy
 *  code page). */
void print_key_help() {
    std::printf("\nWASD/arrows move   K/Enter = A   J/Esc = B   U/I = L/R   LShift = SELECT   SPACE = START   F10 quit\n");
    std::printf("A+LEFT/RIGHT edit   A+UP/DOWN edit fast   A+B clear   A,A insert next unused\n");
    std::printf("B+LEFT/RIGHT change WHICH phrase/chain/table   B+UP/DOWN page the song\n");
    std::printf("L+B select (tap again to widen)   B copies   L+A cut/paste   L+R deselect   L+B+A clone\n");
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

// ─── The screen ──────────────────────────────────────────────────────────────────────────────────
//
// The window and everything drawn into it around the 640×480 frame.
// ⚠️ close() unloads the fonts, skin and overlay before the video: their textures belong to its renderer.
struct Screen {
    SdlVideo video;

    // The on-screen gamepad, drawn on a touchscreen with no physical controller. ⚠️ Enabled per frame
    // (`useTouch` in FrameLoop::lay_out_frame), so plugging or unplugging a controller mid-session
    // switches layouts.
    SdlTouch touch;

    // The portrait device skin's textures, decoded only on a touchscreen. Loaded when the SETTINGS >
    // LAYOUT skin column changes (`loadedSkinIdx`), never every frame.
    Skin skin;

    // Button-label font. A failed load falls back to the 5×5 font.
    Font helvFont;

    // The D-pad arrow glyphs Helvetica lacks (Linux Biolinum, SIL OFL). A failed load falls back to
    // drawn line arrows.
    Font arrowFont;

    // The CRT overlay drawn over the frame, wherever the OVERLAY row exists (caps.skinOverlay). Loaded
    // when the choice changes (`loadedOverlayIdx`).
    ScreenOverlay screenOverlay;

    // Composites `skin` around the frame when the output is portrait; owns no resources. Whether it is
    // active is derived from the output aspect each frame.
    PortraitSkin portrait;

    ui::Canvas canvas;

    /**
     * Puts the drawn `canvas` on screen with this orientation's surround — the portrait device skin or
     * the landscape touch panels — plus the CRT overlay. Returns whether a frame reached the screen (one
     * identical to the last is skipped). Shared by the loop and the render repaint hook, so a blocking
     * render keeps the skin. Expects layout.draw() to have filled `canvas`.
     */
    bool present(const ui::AppState& state, SdlInput& input) {
        // The CRT overlay: over the frame and under the on-screen buttons, at STR/255 alpha.
        const int  ovStr = state.settings.overlayStrength;
        const bool ovOn  = state.settings.overlayIndex > 0 && ovStr > 0 && screenOverlay.loaded();

        // Folded into the skip signature, so changing only the overlay still repaints. Bits 48-60 are
        // clear of the touch/portrait signature bits; zero when off.
        const uint64_t crtSig = ovOn
            ? ((1ull << 60) | (static_cast<uint64_t>(state.settings.overlayIndex & 0xF) << 56) |
               (static_cast<uint64_t>(ovStr & 0xFF) << 48))
            : 0;

        if (portrait.active()) {
            // The portrait skin: the frame sits in the bezel, chrome behind it, buttons in front.
            // touch.layout_portrait2 (in lay_out_frame) hit-tests the same button cluster.
            const uint32_t bg = state.theme.background;   // the tracker's own bg fills the bezel gap
            const auto chrome = [this, bg](SDL_Renderer* r) {
                portrait.draw_chrome(r, skin, bg);
            };
            const auto buttons = [this, &input, ovOn, ovStr](SDL_Renderer* r) {
                if (ovOn) screenOverlay.draw(r, portrait.frame_rect(), ovStr);
                portrait.draw_buttons(r, skin, helvFont, arrowFont, input);
            };
            // ⚠️ A modal dims only the bezel glass around the frame, never the casing or the buttons —
            // a dark skin around a bright cluster reads as a bug. Empty under FIT, which fills the glass.
            const uint32_t scrim = ui::modal_backdrop_active(state) ? ui::MODAL_BACKDROP : 0;
            return video.present_skinned(canvas, portrait.casing_argb(), portrait.frame_rect(),
                                         chrome, buttons, portrait.signature(input) ^ crtSig, scrim,
                                         portrait.screen_rect());
        }
        // Landscape / desktop: the centred frame, the touch panels in the bars beside it (inert
        // without a touchscreen).
        const auto overlay = [this, &input, ovOn, ovStr, fr = video.frame_rect()](SDL_Renderer* r) {
            if (ovOn) screenOverlay.draw(r, fr, ovStr);
            touch.draw(r, input);
        };
        return video.present(canvas, state.theme.background, overlay, touch.signature(input) ^ crtSig,
                             ui::modal_backdrop_active(state) ? ui::MODAL_BACKDROP : 0);
    }

    void close() {
        arrowFont.unload();
        helvFont.unload();
        skin.unload();
        screenOverlay.unload();
        video.close();
    }
};

// ─── The frame loop ──────────────────────────────────────────────────────────────────────────────
//
// Two rates. Input, MIDI and the sequencer refill run every POLL_MS (POLL_IDLE_MS while nothing is
// arriving); drawing and the per-frame derivations run once per display frame. ⚠️ The per-frame work
// must stay at the frame rate — at the poll rate it would cost more than the split saves.
//
// ⚠️ With vsync SDL_RenderPresent blocks until the vblank, and nothing is polled meanwhile. So the next
// frame is scheduled from the present's return (a vblank) plus half a period, and the display's wait
// happens in the loop's own paced wait instead. This relies on a frame drawing in under half a refresh.
// FRAME_MS is the fallback for a platform that reports no refresh rate.
constexpr Uint64 POLL_MS      = 4;
constexpr Uint64 POLL_IDLE_MS = 16;
// Off the screen (Android): a playing song needs only the lookahead pump, which the idle rate serves;
// with nothing playing the loop just waits to come back.
constexpr Uint64 BACKGROUND_IDLE_MS = 250;
constexpr Uint64 QUIET_MS     = 1000;   // input this recent still counts as something arriving
constexpr Uint64 FRAME_MS     = 16;

// An Android rotation resizes the surface over several frames and SDL does not report every step, so
// the idle skip could freeze a half-transitioned frame. The output size is polled each frame and a few
// full redraws are forced after it last changed.
constexpr int RESIZE_SETTLE_FRAMES = 4;

Uint64 env_ms(const char* name) {
    const char* v = SDL_getenv(name);
    if (!v || !v[0]) return 0;
    const long n = SDL_strtol(v, nullptr, 10);
    return n > 0 ? static_cast<Uint64>(n) : 0;
}

/**
 * What the loop keeps between ticks, and the steps of one tick. It borrows the objects boot made;
 * run() builds it, installs its render hooks, and tears down after run() here returns.
 */
class FrameLoop {
  public:
    FrameLoop(const AppConfig& config, SongcoreHost& songHost, MidiInConsole& console,
              ui::AppState& appState, ui::InputDispatcher& dispatcher, SdlInput& sdlInput,
              Screen& window, BackgroundContext& background)
        : cfg(config), engineRef(*config.engine), audio(*config.audio), host(songHost),
          midiInConsole(console), state(appState), dispatch(dispatcher), input(sdlInput),
          screen(window), bg(background), midiThru(songHost.midi_in_thru()) {}

    /**
     * Until a quit, F10, PROJECT > EXIT or the launcher's kill. ⚠️ `terminate_requested` is that kill.
     * On desktop it reads a flag set by a signal handler that does nothing else, so the work is saved
     * after this returns, on this thread. Android leaves it null: the background watcher saves there
     * instead. ⚠️ A kill during the synchronous export is not seen until the export ends.
     */
    void run();

    /** The progress readout pushed from inside a blocking render. */
    void repaint() {
        layout.draw(screen.canvas, state);
        screen.present(state, input);
    }

    bool load_pump();

  private:
    void tick();
    void lay_out_frame(Uint64 now);
    void update_skin(bool useTouch, bool padChoice);
    void update_overlay();
    void sync_audio_output(Uint64 now);
    void handle_input(Uint64 now);
    void handle_sdl_event(const SDL_Event& e, Uint64 now);
    void reopen_audio_if_lost(Uint64 now);
    void run_scripted_hooks(Uint64 now);
    void pump_midi_and_sequencer(Uint64 now);
    void play_on_in_background();
    void draw_frame(Uint64 now);
    void print_status(Uint64 now);
    void pace_tick();

    /**
     * Is there a physical pad? The platform's answer where it has one: on Android SDL counts the
     * emulator's keyboard as a controller (see app.h). Elsewhere, SDL's controller count. Cached, and
     * refreshed only when SDL reports a controller added or removed (`padDirty`) — on Android the call
     * is a JNI round trip.
     */
    bool has_pad() const {
        return cfg.physicalGamepadPresent ? cfg.physicalGamepadPresent()
                                          : (input.controller_count() > 0);
    }

    const AppConfig&     cfg;
    AudioEngine&         engineRef;
    AudioBackend&        audio;
    SongcoreHost&        host;
    MidiInConsole&       midiInConsole;
    ui::AppState&        state;
    ui::InputDispatcher& dispatch;
    SdlInput&            input;
    Screen&              screen;
    BackgroundContext&   bg;

    ui::TrackerLayout layout;
    ui::EngineFeed    feed;
    ui::MapperState   mapper;

    // Cleared by a quit; also by load_pump, so a quit during a load is not swallowed.
    bool running = true;

    // THRU as last printed. It can change mid-session (picking a loopback port on the MIDI screen).
    bool midiThru;

    Uint64 lastStatus = 0;

    // ── Dev only: scripted timed runs ────────────────────────────────────────────────────────────
    //
    //   POCKETTRACKER_AUTOPLAY_MS=<n>   press START once, n ms after the first frame
    //   POCKETTRACKER_QUIT_AFTER_MS=<n> push SDL_QUIT n ms after the first frame
    //
    // Autoplay goes through ui::handle_button, the same call real input makes. Deliberately one button
    // and no script format: a second input path would have to be kept in step with the real one.
    const Uint64 autoplayMs  = env_ms("POCKETTRACKER_AUTOPLAY_MS");
    const Uint64 quitAfterMs = env_ms("POCKETTRACKER_QUIT_AFTER_MS");
    Uint64       firstFrameMs  = 0;   // 0 until the first frame — the clock both hooks count from
    bool         autoplayFired = false;

    // The last output size the portrait log line reported, so it prints once per rotation or resize.
    int lastPortraitW = 0, lastPortraitH = 0;

    // The last layout decision reported. The start values make the first frame always print.
    bool lastUseTouch = false;
    int  lastGateW = -1, lastGateH = -1;

    // The device skin currently loaded (-1 = none yet). Reloaded only when the SETTINGS skin column
    // moves `skinIndex`.
    int loadedSkinIdx = -1;

    // The overlay currently decoded (-1 = none yet). Reloaded only when the choice changes.
    int loadedOverlayIdx = -1;

    // SETTINGS > AUDIO OUT: the row value last shown (-1 = boot, where the saved NAME is applied) and
    // the output last seen playing (-1 = not yet read).
    int shownAudioOut   = -1;
    int playingAudioOut = -1;
    // When Windows last reported a new output device, and how many retries of the saved driver that
    // arrival has had. An unplugged interface's driver says nothing when it is plugged back in; Windows
    // does, and an ASIO driver can need a few seconds after that before it will open.
    Uint64 audioArrivalMs = 0;
    int    audioRetries   = 0;

    bool physicalPad = false;   // set by run(), then by `padDirty`
    bool padDirty    = false;

    // Whether `layoutIndex` currently means the FULL/PORTRAIT choice. The list has one entry without a
    // pad and two with, so the same index means different things in the two states.
    bool padLayoutLive = false;

    // The last rotation permission sent to the platform (-1 = none yet). Pushed only on change; each
    // call is a JNI round trip.
    int lastLandscapeAllowed = -1;

    // Dev only: PT_TOP_ANCHOR=1 forces the clip-on-pad frame placement on a desktop, so it can be
    // checked without a phone. Drag the window tall and the frame moves into the top half.
    const bool forceTopAnchor = [] {
        const char* v = SDL_getenv("PT_TOP_ANCHOR");
        return v && v[0] == '1';
    }();

    // ── Idle-skip state ──────────────────────────────────────────────────────────────────────────
    // `sawInput`      — something happened this frame that may change the screen.
    // `audibleEdge`   — audio was audible last frame, so the first silent frame is still drawn (or
    //                   the scope freezes mid-wave).
    // `drewOnce`      — the first frame always draws.
    // `timedWorkEdge` — the same one-frame tail for the dispatcher's timers. See draw_frame.
    bool sawInput      = false;
    bool audibleEdge   = true;
    bool timedWorkEdge = false;
    bool drewOnce      = false;

    // Frame accounting for the status line: `drew` frames presented, `skip` frames the idle gate
    // dropped, `same` frames drawn but identical to the screen, `poll` loop ticks. A working idle skip
    // and a broken one look the same on screen; these numbers are the difference.
    long long drawn = 0, presented = 0, skipped = 0, polls = 0;

    Uint64 nextFrameMs = 0;   // when the next drawn frame is due — 0 so the first tick draws
    Uint64 lastPollMs  = 0;   // what pace_tick measures against
    Uint64 lastInputMs = 0;   // …and what tells it whether anyone is here

    Uint64   audioReopenMs = 0;  // when the next reopen attempt may run; 0 = at once
    uint64_t midiSeen      = 0;  // messages the audio thread had handled at the last tick

    // -1 so the first frame is not a resize.
    int lastOutW = -1, lastOutH = -1, resizeSettle = 0;
};

void FrameLoop::run() {
    if (autoplayMs || quitAfterMs) {
        std::printf("dev:     AUTOPLAY_MS=%llu  QUIT_AFTER_MS=%llu (scripted run)\n",
                    static_cast<unsigned long long>(autoplayMs),
                    static_cast<unsigned long long>(quitAfterMs));
    }

    physicalPad = has_pad();

    while (running && !state.shouldQuit &&
           !(cfg.terminate_requested && cfg.terminate_requested()))
        tick();
}

void FrameLoop::tick() {
    ++polls;

    // First in the body, so it times the whole iteration including the wait — the delay an input
    // actually sees. Uses last frame's transport flag. Off unless POCKETTRACKER_LATENCY=1.
    latency::poll_tick(state.isPlaying);

    // One clock reading per tick, passed to everything that needs it.
    const Uint64 now = SDL_GetTicks64();

    // Read once: the per-frame work above and below the event drain must agree about this tick.
    // ⚠️ Never off the screen — the GL context is backed up there.
    const bool frameDue = now >= nextFrameMs && !bg.backgrounded;

    // Lay the touch panels out before the event drain, so a finger hits what is on screen.
    if (frameDue) lay_out_frame(now);

    handle_input(now);
    reopen_audio_if_lost(now);
    run_scripted_hooks(now);
    pump_midi_and_sequencer(now);
    play_on_in_background();

    // The rest runs only when a frame is due (see Two rates, above FrameLoop).
    if (frameDue) draw_frame(now);

    pace_tick();
}

/**
 * What runs instead of the loop during a load. Keeps SDL pumping, so (1) the background watcher can
 * still fire, (2) presses made during the load are consumed rather than replayed afterwards, and (3) B
 * cancels.
 * ⚠️ It does NOT dispatch the presses: that would re-enter the dispatcher halfway through a load.
 * ⚠️ SDL_QUIT is honoured (and cancels), or the app would finish the load and ignore the kill.
 */
bool FrameLoop::load_pump() {
    bool cancel = false;
    const Uint64 now = SDL_GetTicks64();

    // The load refills the sequencer at this same cadence (`InputDispatcher::load_tick`), so to the
    // latency probe this is a poll: a load that keeps the song going is not a stall.
    latency::poll_tick(state.isPlaying);

    SDL_Event e;
    while (SDL_PollEvent(&e)) {
        if (e.type == SDL_QUIT) {
            running = false;
            cancel  = true;
        } else if (e.type == SDL_FINGERDOWN || e.type == SDL_FINGERUP ||
                   e.type == SDL_FINGERMOTION) {
            screen.touch.handle_finger(e, input, now);
        } else {
            input.handle_event(e, now);
        }
    }
    input.tick(now);

    // ⚠️ Drained whatever it holds, or a press would fire on the screen underneath when the load
    // ends. Only a fresh B press cancels; a B held from before the load does not.
    ui::ButtonEvent be;
    while (input.poll(be))
        if (be.button == ui::Button::B && be.action == ui::ButtonAction::PRESSED) cancel = true;

    return cancel;
}

void FrameLoop::lay_out_frame(Uint64 now) {
    SdlVideo&     video    = screen.video;
    SdlTouch&     touch    = screen.touch;
    PortraitSkin& portrait = screen.portrait;

    int outW = 0, outH = 0;
    video.output_size(outW, outH);

    // A size change arms a few forced redraws, re-armed while the size keeps moving, so the
    // countdown starts once it has settled.
    if (outW != lastOutW || outH != lastOutH) {
        resizeSettle = RESIZE_SETTLE_FRAMES;
        lastOutW = outW;
        lastOutH = outH;
    }

    // ── Layout: on-screen buttons or full screen ─────────────────────────────────────────────────
    //
    // One gate, read by both `touch` and `portrait`, so they cannot disagree. A touchscreen with no
    // controller gets on-screen buttons; with a controller, full screen — unless the user keeps the
    // buttons (the LAYOUT row offers that only where both exist). Recomputed per frame, so
    // hot-plugging a pad switches on the next frame.
    if (padDirty) { physicalPad = has_pad(); padDirty = false; }
    // ⚠️ `touchButtonsWithPad` defaults false: a pad means full screen until the user says otherwise.
    const bool padChoice = cfg.touchCapable && physicalPad;

    // ⚠️ The row's edit is read back BEFORE the index is re-derived in update_skin. Input is polled
    // after this, so a read lower down would only see this frame's derive and lose the press.
    // `padLayoutLive` stops a freshly plugged pad reading the touch-only index 0 as "FULL".
    if (padChoice && padLayoutLive)
        state.settings.touchButtonsWithPad = (state.settings.layoutIndex == 1);
    padLayoutLive = padChoice;

    const bool useTouch  = cfg.touchCapable &&
                           (!physicalPad || state.settings.touchButtonsWithPad);
    touch.set_enabled(useTouch);

    // Landscape is allowed exactly where the FULL layout is in force (a pad, no on-screen buttons);
    // any other state would rotate into a layout we do not ship. Pushed live because the boot
    // orientation hint is read only once. Null on desktop and the handhelds.
    if (cfg.allowLandscape && static_cast<int>(!useTouch) != lastLandscapeAllowed) {
        lastLandscapeAllowed = !useTouch ? 1 : 0;
        cfg.allowLandscape(!useTouch);
    }

    // The frame moves to the top half when a clip-on pad covers the bottom of a portrait phone. Each
    // term rules out a real device: a touchscreen (not a desktop or handheld), a pad attached,
    // portrait output, and no on-screen buttons (the portrait skin places the frame itself).
    const bool topAnchor = forceTopAnchor ||
                           (!useTouch && cfg.touchCapable && physicalPad && outH > outW);
    video.set_top_anchor(topAnchor);

    // The layout decision, printed on each change: from a report alone, a wrong pad answer, a wrong
    // aspect and an unset touchCapable all look the same.
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

    // The SETTINGS > LAYOUT row exists on any touchscreen device: it picks the skin while the buttons
    // are up, and FULL/PORTRAIT while a pad is attached.
    state.caps.touchLayouts = cfg.caps.touchLayouts && cfg.touchCapable;

    // BTN SOUND and BTN VIBRO exist only while on-screen buttons are drawn.
    state.caps.buttonFeedback = cfg.caps.buttonFeedback && useTouch;

    // The face-button swap row exists only with a pad attached.
    state.caps.padAttached = physicalPad;

    // The face-button swap, read live so the row applies on the next press.
    input.set_abxy(static_cast<ui::AbxyLayout>(
        std::clamp(state.settings.abxyIndex, 0, 2)));

    // The live BTN SOUND / BTN VIBRO values; a no-op with no feedback sink.
    touch.set_feedback_settings({state.settings.buttonSoundEnabled,
                                 state.settings.buttonSoundVolume,
                                 state.settings.buttonVibroEnabled,
                                 state.settings.vibroPower});

    update_skin(useTouch, padChoice);
    update_overlay();
    sync_audio_output(now);

    // Portrait first: whether it is active decides which geometry `touch` hit-tests. Hit-test and
    // drawing share the same rects, so a press always lights the button under the finger.
    // `scalingBilinear` picks INTEGER vs FIT for the frame in the bezel.
    portrait.layout(outW, outH, useTouch, state.settings.scalingBilinear);
    if (portrait.active())
        touch.layout_portrait2(portrait.cluster_rect(), portrait.button_rects(), outW, outH);
    else
        touch.layout(video.frame_rect(), outW, outH);

    // One console line when the portrait skin activates or the output rotates.
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

/**
 * The device skin, and the LAYOUT row's columns. The mode column is FULL (0) / PORTRAIT (1) on a
 * touchscreen with a pad, otherwise just PORTRAIT. The index is derived from the saved boolean, not
 * saved itself: the list's length changes with the pad, so a stored index would change its meaning.
 */
void FrameLoop::update_skin(bool useTouch, bool padChoice) {
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

        // Reload the PNGs only when the choice changes (or on the first frame).
        if (state.settings.skinIndex != loadedSkinIdx) {
            screen.skin.load(screen.video.renderer(), d.id, cfg.console, d.art);
            screen.portrait.set_skin(d.casingFillArgb, d.labelRgb, d.bezelThicknessX, d.art);
            loadedSkinIdx = state.settings.skinIndex;
        }

        // The chromeless skin draws in the live theme's colours, pushed every frame so a theme edit
        // restyles it at once. Inert for the chrome skins.
        screen.portrait.set_theme(state.theme.background, state.theme.textValue);
    } else {
        state.settings.skinCount = 0;   // no skin column on a fullscreen (controller) layout
        if (!padChoice) { state.settings.layoutCount = 1; state.settings.layoutIndex = 0; }
    }
}

/**
 * The screen overlay (CRT filter). The shell supplies the choice count and text, keeps the saved name
 * in step with the index, and decodes the PNG only when the choice changes.
 */
void FrameLoop::update_overlay() {
    if (!cfg.caps.skinOverlay) return;

    state.settings.overlayCount = screen_overlay_choice_count();
    if (state.settings.overlayIndex < 0 ||
        state.settings.overlayIndex >= state.settings.overlayCount)
        state.settings.overlayIndex = screen_overlay_index(state.settings.overlayName);
    state.settings.overlayName = screen_overlay_id(state.settings.overlayIndex);
    state.overlayText          = screen_overlay_text(state.settings.overlayIndex);

    if (state.settings.overlayIndex != loadedOverlayIdx) {
        screen.screenOverlay.load(screen.video.renderer(), state.settings.overlayIndex, cfg.console);
        loadedOverlayIdx = state.settings.overlayIndex;
    }
}

/**
 * SETTINGS > AUDIO OUT. The row edits an index; the output is switched here — at boot too, from the
 * saved NAME. The name is rewritten only when a pick PLAYS, so a driver that fails at boot (an
 * interface left unplugged) is tried again next launch. The row always shows what plays.
 */
void FrameLoop::sync_audio_output(Uint64 now) {
    if (!cfg.audioOutputs) return;

    ui::SettingsValues&             sv   = state.settings;
    const std::vector<std::string>& outs = cfg.audioOutputs->names();
    sv.audioOutCount = static_cast<int>(outs.size());

    int pick = -1;
    if (shownAudioOut < 0) {
        const auto it = std::find(outs.begin(), outs.end(), sv.audioOutput);
        if (it != outs.end()) pick = static_cast<int>(it - outs.begin());
    } else if (sv.audioOutIndex != shownAudioOut) {
        pick = sv.audioOutIndex;
    }
    // The saved driver is not playing (unplugged, at boot or mid-play): try it again after a device
    // arrives. Quietly — a failed try leaves the system output as it was.
    static constexpr Uint64 kRetryAfterMs[] = {1500, 4000};
    if (pick < 0 && audioArrivalMs != 0 && audioRetries < 2 &&
        now >= audioArrivalMs + kRetryAfterMs[audioRetries]) {
        ++audioRetries;
        const auto it = std::find(outs.begin(), outs.end(), sv.audioOutput);
        const int  saved = it != outs.end() ? static_cast<int>(it - outs.begin()) : -1;
        std::string err;
        if (saved > 0 && saved != cfg.audioOutputs->active()) {
            if (cfg.audioOutputs->select(saved, err)) {
                state.statusMessage = "AUDIO OUT: " + sv.audioOutput;
                state.statusSuccess = true;
            } else {
                std::printf("audio:   %s still will not open: %s\n", sv.audioOutput.c_str(),
                            err.c_str());
            }
        }
    }

    if (pick >= 0 && pick < sv.audioOutCount) {
        const std::string& want = outs[static_cast<size_t>(pick)];
        std::string        err;
        if (cfg.audioOutputs->select(pick, err)) {
            sv.audioOutput = want;
            if (shownAudioOut >= 0) {
                state.statusMessage = "AUDIO OUT: " + want;
                state.statusSuccess = true;
            }
        } else {
            state.statusMessage = "AUDIO OUT: " + err;
            state.statusSuccess = false;
            std::printf("audio:   %s could not open: %s\n", want.c_str(), err.c_str());
        }
    }

    // Also moves with no pick at all: a driver that did not come back after a reset.
    const int active = cfg.audioOutputs->active();
    if (active != playingAudioOut) {
        if (pick < 0 && playingAudioOut > 0) {
            state.statusMessage = "AUDIO OUT LOST: SYSTEM";
            state.statusSuccess = false;
        }
        playingAudioOut = active;
        derive_midi_auto_offset(audio, state);
        host.set_midi_offset_ms(ui::midi_offset_in_force(sv, state.midiAutoOffsetMs));
    }
    shownAudioOut = sv.audioOutIndex = active;
    state.audioOutText = outs[static_cast<size_t>(active)];
}

void FrameLoop::handle_input(Uint64 now) {
    // ⚠️ Before the event drain, so a B pressed this frame gets this frame's answer. B repeats only in
    // the on-screen keyboard, where it is backspace; everywhere else it must not fire on a timer.
    input.set_b_repeatable(state.qwerty.isOpen);

    SDL_Event e;
    while (SDL_PollEvent(&e)) handle_sdl_event(e, now);
    input.tick(now);

    // The tick's clock, given to the dispatcher (multi-tap windows, timers).
    dispatch.set_now(static_cast<long long>(now));

    ui::ButtonEvent be;
    while (input.poll(be)) {
        // ⚠️ Counts as input too: key repeat comes from input.tick(), with no SDL event behind it.
        sawInput = true;
        ui::handle_button(be, dispatch, mapper, now);
    }

    // Somebody is here, which keeps the poll rate fast (pace_tick). Derived from `sawInput`, so a new
    // input source cannot be missed. `sawInput` clears once a frame, so this re-stamps for the rest of
    // the frame — which only lengthens the hold-off.
    if (sawInput) lastInputMs = now;
}

void FrameLoop::handle_sdl_event(const SDL_Event& e, Uint64 now) {
    // Any event may change the screen (a resize, an expose, a focus change), so all of them count.
    // Over-drawing costs one frame; the pixel compare in present() catches it.
    sawInput = true;

    // A controller added or removed: re-ask whether a pad is present next frame.
    if (e.type == SDL_CONTROLLERDEVICEADDED || e.type == SDL_CONTROLLERDEVICEREMOVED ||
        e.type == SDL_JOYDEVICEADDED || e.type == SDL_JOYDEVICEREMOVED)
        padDirty = true;

    if (e.type == SDL_AUDIODEVICEADDED && !e.adevice.iscapture) {
        audioArrivalMs = now;
        audioRetries   = 0;
    }

    // ⚠️ Force the next present when the platform may have blanked or swapped the surface behind our
    // back; otherwise the identical-frame skip leaves the screen black until an input. That covers a
    // resume, a short sleep (only the focus events arrive), and a rotation (a new surface — on a
    // portrait-native phone, at boot). A device reset also loses the texture. Logged, because a forced
    // present that fired and one that did not look the same.
    const char* redrawReason = nullptr;
    if (e.type == SDL_RENDER_DEVICE_RESET) {
        screen.video.invalidate_backbuffer(/*texture_lost=*/true);
        redrawReason = "device reset - texture recreated";
    } else if (e.type == SDL_RENDER_TARGETS_RESET || e.type == SDL_APP_WILLENTERFOREGROUND ||
               e.type == SDL_APP_DIDENTERFOREGROUND ||
               (e.type == SDL_WINDOWEVENT &&
                (e.window.event == SDL_WINDOWEVENT_EXPOSED ||
                 e.window.event == SDL_WINDOWEVENT_SIZE_CHANGED ||
                 e.window.event == SDL_WINDOWEVENT_RESIZED ||
                 e.window.event == SDL_WINDOWEVENT_SHOWN ||
                 e.window.event == SDL_WINDOWEVENT_RESTORED ||
                 e.window.event == SDL_WINDOWEVENT_FOCUS_GAINED))) {
        screen.video.invalidate_backbuffer(/*texture_lost=*/false);
        redrawReason = "foreground / re-expose / resize / targets reset";
    }
    if (redrawReason && cfg.console) {
        std::printf("video:   backbuffer no longer ours (%s) - forcing a redraw\n", redrawReason);
        std::fflush(stdout);
    }

    // A redraw does not re-read the disk: the folder on screen may have changed while we were away (on
    // Android, ADD FOLDER… always returns this way). Android and iOS only.
    if (e.type == SDL_APP_DIDENTERFOREGROUND) {
        dispatch.refresh_browser_on_foreground();
        // Back on the screen: draw again. A song that played on in the background no longer needs the
        // service (`end` is idempotent).
        bg.backgrounded = false;
        if (bg.playingInBackground) {
            bg.playingInBackground = false;
            if (cfg.background.end) cfg.background.end();
        }
    }

    if (e.type == SDL_QUIT) {
        // A window close, or SDL's own SIGINT/SIGTERM translation where it has one. ⚠️ Not guaranteed
        // (Windows has none), so main.cpp installs its own handler. Either way an unclean exit: the
        // flush after the loop keeps the work.
        running = false;
    } else if (e.type == SDL_KEYDOWN && e.key.keysym.sym == SDLK_F10) {
        // Dev-only quit, outside the button model, and it does not ask. An unclean exit, so the work
        // is kept. The real exit is PROJECT > EXIT, which asks — the only exit that discards the
        // autosave.
        running = false;
    } else if (e.type == SDL_FINGERDOWN || e.type == SDL_FINGERUP || e.type == SDL_FINGERMOTION) {
        // A touch on a virtual button feeds SdlInput's own press/release, so from here on it is a key
        // like any other. A finger outside a button is ignored.
        screen.touch.handle_finger(e, input, now);
    } else {
        input.handle_event(e, now);
    }
}

/**
 * The audio device comes back. Lost two ways: released on the way into the background, or taken away
 * while open (audio-backend.h). The close first lets go of a dead stream. Retried every 500 ms with no
 * limit — the app is silent until it works. ⚠️ Not while backgrounded, or the stream released on the
 * way out would reopen on the next tick.
 */
void FrameLoop::reopen_audio_if_lost(Uint64 now) {
    if (((bg.audioClosed && !bg.backgrounded) || audio.deviceLost()) && now >= audioReopenMs) {
        audio.closeStream();
        if (audio.openStream()) {
            bg.audioClosed = false;
            std::printf("audio:   device reopened\n");
        } else {
            audioReopenMs = now + 500;
            std::printf("audio:   reopen FAILED - retrying\n");
        }
        std::fflush(stdout);
    }
}

/** Dev only; both are 0 unless an env var set them. */
void FrameLoop::run_scripted_hooks(Uint64 now) {
    if ((autoplayMs || quitAfterMs) && firstFrameMs == 0) firstFrameMs = now;
    if (autoplayMs && !autoplayFired && now - firstFrameMs >= autoplayMs) {
        autoplayFired = true;
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
}

void FrameLoop::pump_midi_and_sequencer(Uint64 now) {
    // One line per THRU change.
    if (host.midi_in_thru() != midiThru) {
        midiThru = host.midi_in_thru();
        std::printf("midi:    THRU %s (device pick)\n", midiThru ? "on" : "OFF - loopback");
        std::fflush(stdout);
    }

    // A polled MIDI input backend (Android's) fetches its bytes here; the push backends make this a
    // no-op. ⚠️ So on Android this tick is in a key's path, which is why pace_tick keeps the fast rate
    // for a polled port. ⚠️ It must stay immediately above host.poll().
    if (cfg.midiIn) cfg.midiIn->pump();

    // Which poll the CCs about to be handed back belong to (the console's per-drain count).
    ++midiInConsole.drain;

    // The lookahead refill. Work-conserving: it returns at once while the buffer is deep enough. It
    // releases the MIDI queue only when the sender thread is off. MIDI in is drained by the audio
    // thread; this publishes what that drain routes against and takes back what it handled.
    host.poll();

    // A MIDI message arrived since the last tick: keep the fast poll rate, as a button press does.
    if (const uint64_t seen = host.midi_in_messages(); seen != midiSeen) {
        midiSeen    = seen;
        lastInputMs = now;
    }
}

/**
 * Playing on in the background (Android). The notification's STOP arrives as a request and is acted on
 * here. Once the song stops in the background, the service ends and the device is released, as when
 * leaving with nothing playing.
 */
void FrameLoop::play_on_in_background() {
    if (cfg.background.takeStopRequest && cfg.background.takeStopRequest() && host.is_playing())
        host.stop();
    if (cfg.background.publishPlaying) cfg.background.publishPlaying(host.is_playing());
    if (bg.backgrounded) {
        state.isPlaying = host.is_playing();   // draw_frame, which refreshes it, is skipped
        if (bg.playingInBackground && !state.isPlaying) {
            bg.playingInBackground = false;
            if (cfg.background.end) cfg.background.end();
            audio.closeStream();
            bg.audioClosed = true;
            std::printf("lifecycle: stopped in the background - audio device released\n");
            std::fflush(stdout);
        }
    }
}

void FrameLoop::draw_frame(Uint64 now) {
    state.isPlaying = host.is_playing();
    latency::frame_tick(state.isPlaying);
    // One position per track. A track with no position returns -1 everywhere and draws no marker.
    for (int t = 0; t < 8; ++t) {
        const PlaybackPosition p = host.playheads(t);
        state.playheads[t] = {p.songRow, p.chainId, p.chainRow, p.phraseId, p.phraseStep};
        const songcore::LiveSlot q = host.live_queue(t);
        state.liveQueue[t] = {q.targetRow, q.stop, q.immediate, q.armed()};
    }
    // ⚠️ Read back from the host: stop() and a project load can both end a LIVE take.
    state.liveMode     = host.live_mode();
    // The blink phase, handed to the drawing layer.
    state.blinkPhaseMs = static_cast<int>(now % 1000);
    state.trackMask = host.track_mask();

    // Everything the UI reads back from the engine: the scope, the monitored notes, the table's
    // playing row, the SF2 preset list, the meters. After the transport fields — the waveform's decay
    // depends on isPlaying.
    feed.poll(engineRef, host, state, static_cast<long long>(now));

    // SETTINGS > METRONOME, read every frame so no edit site has to remember to push it. 0x80 lands at
    // -12 dBFS; the click is summed below the limiter (audio-engine.h).
    engineRef.setMetronome(state.settings.metronomeEnabled,
                           static_cast<float>(state.settings.metronomeVolume) / 255.0f * 0.5f);

    // SETTINGS > SCALING, polled: the row and a hand-edited settings.json both change it. Returns
    // early when unchanged; otherwise it recreates the texture, which is why it is not in present().
    screen.video.set_scaling(state.settings.scalingBilinear ? ScalingMode::FIT : ScalingMode::INTEGER);

    // ── Idle frame skip ──────────────────────────────────────────────────────────────────────────
    //
    // Draw only when something may have changed; the loop itself keeps running. A frame is drawn if:
    //   audible, audibleEdge        the scope is moving, or just stopped (one frame to settle flat)
    //   sawInput, !drewOnce         an input arrived, or this is the first frame
    //   settling                    a rotation or resize is settling (each frame forced through)
    //   metersFalling               meters and spectrum still falling after the sound stopped
    //   timedWork, timedWorkEdge    a status message that clears on a timer, and the frame it clears
    //                               on (set_now() drops the deadline in the same call)
    // Under this, present() skips a frame identical to the screen — the safety net that lets this gate
    // be conservative. ⚠️ It cannot catch a frame that is never drawn, which is why the last three
    // terms exist.
    const bool timedWork = dispatch.has_pending_timed_work();
    const bool metersFalling = layout.has_falling_meters(state);
    const bool audible = audio_is_audible(state);
    const bool settling = resizeSettle > 0;
    if (settling) {
        screen.video.invalidate_backbuffer(/*texture_lost=*/false);   // the settled frame must not be skipped
        --resizeSettle;
    }
    if (audible || audibleEdge || sawInput || !drewOnce || settling || metersFalling ||
        timedWork || timedWorkEdge) {
        layout.draw(screen.canvas, state);
        ++drawn;

        const bool didPresent = screen.present(state, input);
        if (didPresent) ++presented;
        drewOnce = true;

        // ⚠️ Anchored on the present's return — a vblank, with vsync. See Two rates. A frame drawn but
        // not presented blocked on nothing, so it takes the plain deadline below.
        if (didPresent) {
            const Uint64 hz     = static_cast<Uint64>(screen.video.refresh_hz());
            const Uint64 period = (hz >= 30 && hz <= 240) ? 1000ull / hz : FRAME_MS;
            nextFrameMs = SDL_GetTicks64() + period - period / 2;
        }
    } else {
        ++skipped;
    }

    // No vblank to measure from: the plain deadline. Re-anchored only once a whole frame behind, so a
    // stall costs one late frame and never a burst of catch-up ones.
    if (nextFrameMs <= now) nextFrameMs = SDL_GetTicks64() + FRAME_MS;

    audibleEdge   = audible;     // so the first silent frame is still drawn (the flattened scope)
    timedWorkEdge = timedWork;   // …and the frame a timer's own work vanishes on
    sawInput      = false;

    print_status(now);
}

/**
 * Once a second on the console. The frame counter shows the audio device is calling back, the playhead
 * that the sequencer advances, voices that notes reach the engine — whichever is stuck at zero names
 * the broken link on a box with no screen. Track 0, which a PHRASE or CHAIN audition plays.
 */
void FrameLoop::print_status(Uint64 now) {
    if (!cfg.console || now - lastStatus < 1000) return;

    lastStatus = now;
    const ui::TrackPlayhead& t0 = state.playheads[0];
    std::printf(
        "%s  frame %-10lld  song %3d  chain %2d  step %2d   voices %2d   %-10s cursor %X,%d"
        "   drew %lld skip %lld same %lld poll %lld\n",
        host.is_playing() ? "play" : "stop",
        static_cast<long long>(engineRef.getCurrentFrame()), t0.songRow, t0.chainRow,
        t0.step, engineRef.getActiveVoiceCount(), ui::screen_label(state.currentScreen),
        state.cursorRow, state.cursorColumn, presented, skipped, drawn - presented, polls);
    std::fflush(stdout);  // block-buffered to a pipe otherwise, and then it says nothing
}

/**
 * ⚠️ The only place the loop sleeps — without it, it burns a whole core — so every tick must reach it.
 * Measured from the last tick, so a present that blocked past the next tick is not followed by a second
 * wait.
 *
 * `busy` lists what arrives with no SDL event behind it: a running transport, a sound still ringing, a
 * MIDI port this loop must poll. A press re-arms the fast rate through `lastInputMs`.
 * ⚠️ Never sleep past a frame already due, or animation steps twice in one frame and then not at all.
 */
void FrameLoop::pace_tick() {
    const Uint64 t    = SDL_GetTicks64();
    const bool   busy = state.isPlaying || audibleEdge ||
                        (cfg.midiIn && cfg.midiIn->open_index() >= 0 && cfg.midiIn->polled()) ||
                        t - lastInputMs < QUIET_MS;
    Uint64 due = lastPollMs + (busy ? POLL_MS : POLL_IDLE_MS);
    // ⚠️ Off the screen no frame is drawn, so `nextFrameMs` stays in the past and would stop the loop
    // sleeping at all.
    if (bg.backgrounded) due = lastPollMs + (state.isPlaying ? POLL_IDLE_MS : BACKGROUND_IDLE_MS);
    else if (nextFrameMs < due) due = nextFrameMs;
    if (due > t) SDL_Delay(static_cast<Uint32>(due - t));
    lastPollMs = SDL_GetTicks64();
}

// ─── Leaving ─────────────────────────────────────────────────────────────────────────────────────

/**
 * The MIDI-in report, on every run that opened a port. Each stage counts, so a break can be located:
 * nothing at the port, queue overflow, parser orphans, or the router routing nothing.
 */
void print_midi_in_report(MidiInBase* base, SongcoreHost& host, const MidiInConsole& midiInConsole) {
    const songcore::MidiInputRouter& r = host.midi_in_router();
    std::printf("midi in: %llu bytes at the port (%llu callbacks, %llu port errors), %llu drained, "
                "%llu messages\n"
                "         routed %llu, dropped: %llu non-channel, %llu no-instrument, "
                "%llu unsupported\n"
                "         queue overflow %llu bytes, parser orphans %llu, messages with no record %llu\n"
                "         dropped during an export %llu bytes, handled messages the UI never saw %llu\n"
                // What the engine and the cable were actually handed; differs from `routed` above
                // by the thru suppression.
                "         injected %llu into the engine; thru %s: %llu to the cable, %llu suppressed\n",
                static_cast<unsigned long long>(base->bytes_received()),
                static_cast<unsigned long long>(base->callbacks()),
                static_cast<unsigned long long>(base->port_errors()),
                static_cast<unsigned long long>(host.midi_in_bytes()),
                static_cast<unsigned long long>(host.midi_in_messages()),
                static_cast<unsigned long long>(r.routed()),
                static_cast<unsigned long long>(r.nonChannel()),
                static_cast<unsigned long long>(r.noInstrument()),
                static_cast<unsigned long long>(r.unsupported()),
                static_cast<unsigned long long>(host.midi_in_sink().dropped()),
                static_cast<unsigned long long>(host.midi_in_parser().orphan_bytes()),
                static_cast<unsigned long long>(midiInConsole.silent),
                static_cast<unsigned long long>(host.midi_in_pipeline().discarded()),
                static_cast<unsigned long long>(host.midi_in_pipeline().seen_dropped()),
                static_cast<unsigned long long>(host.midi_in_injected()),
                host.midi_in_thru() ? "on" : "OFF (loopback)",
                static_cast<unsigned long long>(host.midi_in_thru_sent()),
                static_cast<unsigned long long>(host.midi_in_thru_suppressed()));

    // How fast a knob really turns — only when a CC arrived.
    if (midiInConsole.ccTotal > 0) {
        const uint64_t spanMs = midiInConsole.ccLastMs - midiInConsole.ccFirstMs;
        std::printf("         CC: %llu in %.2f s = %.0f/s mean; PEAK %llu/s (busiest second), "
                    "%llu per 100 ms, %llu per 16 ms, %llu per drain; %llu drains carried one\n",
                    static_cast<unsigned long long>(midiInConsole.ccTotal),
                    spanMs / 1000.0,
                    spanMs > 0 ? midiInConsole.ccTotal * 1000.0 / spanMs : 0.0,
                    static_cast<unsigned long long>(midiInConsole.w1000.max),
                    static_cast<unsigned long long>(midiInConsole.w100.max),
                    static_cast<unsigned long long>(midiInConsole.w16.max),
                    static_cast<unsigned long long>(midiInConsole.ccMaxPerDrain),
                    static_cast<unsigned long long>(midiInConsole.ccDrains));
    }
}

}  // namespace

int run(const AppConfig& cfg) {
    // The version in the log on every platform — a bug report usually arrives as a log file.
    // PT_VERSION_STRING comes from native/cmake/pt_version.cmake; no fallback, so a tree that forgets
    // the define fails to compile rather than print a wrong version.
    std::printf("PocketTracker %s\n", PT_VERSION_STRING);

    // Debug and release share a version string but not the developer rows (ui/platform_caps.h).
    std::printf("build:   %s\n", cfg.caps.debug ? "debug" : "release");

    AudioEngine&  engineRef  = *cfg.engine;
    AudioBackend& audio      = *cfg.audio;
    ui::FileSystem& filesystem = *cfg.filesystem;

    // Unwired at teardown, before the backend is closed.
    engineRef.onResumeRequested = [&audio] { audio.resumeStream(); };

    // ⚠️ Declared BEFORE the host: the host's MIDI consumer calls this observer from its destructor
    // (the final panic), so it must outlive the host.
    MidiJitterRecorder midiJitter;

    // ⚠️ Also before the host, which calls it from poll(). Always attached: the counters are what make
    // a silent MIDI input readable.
    MidiInConsole midiInConsole;

    SongcoreHost host(&engineRef, audio.sampleRate());
    host.set_midi_in_observer(&midiInConsole);
    if (const char* t = SDL_getenv("POCKETTRACKER_MIDI_IN_TRACE"); t && t[0] == '1')
        midiInConsole.trace = true;

    // This install's app root. Set before the first load, so a project made on another install has its
    // absolute media paths re-rooted onto ours (see set_app_root).
    host.set_app_root(cfg.appRoot);

    // The MIDI OUT port, if the platform opened one. Set before the project loads, so an EXTERNAL
    // instrument is routed from its first note.
    host.set_midi_out(cfg.midiOut);
    host.set_midi_offset_ms(cfg.midiOffsetMs);

    // ── The MIDI sender thread ───────────────────────────────────────────────────────────────────
    //
    // Started on every platform, port or not: a LEN gate and a panic's note-offs must be released
    // either way. While it runs, poll() stops releasing the queue (set_midi_pump_external).
    //
    // Dev overrides:
    //   POCKETTRACKER_MIDI_SENDER=0   don't start it; the frame loop releases the queue instead
    //   POCKETTRACKER_MIDI_JITTER=1   measure every released message and print the numbers on exit
    const bool jitterOn = [] {
        const char* j = SDL_getenv("POCKETTRACKER_MIDI_JITTER");
        return j && j[0] == '1';
    }();
    if (jitterOn) host.midi_out().set_send_observer(&midiJitter);

    // ⚠️ Declared AFTER the host: it pumps `host`, so its thread must be joined while the host lives.
    MidiSender  midiSender(engineRef, host, audio.sampleRate());
    const char* senderEnv = SDL_getenv("POCKETTRACKER_MIDI_SENDER");
    bool        senderOn  = false;
    if (senderEnv && senderEnv[0] == '0') {
        std::printf("midi:    sender thread DISABLED (POCKETTRACKER_MIDI_SENDER=0) - the 60 Hz frame "
                    "loop releases the queue, as it did before phase B3\n");
    } else {
        senderOn = midiSender.start();
    }

    if (!open_boot_project(cfg, host)) return 1;

    // ⚠️ Load the media, THEN push the params. The engine holds state no note carries — mixer, master
    // bus, reverb, delay, the EQ bank, every instrument's drive/crush/filter/loop — and without this
    // push the project would play on the engine's factory defaults.
    host.push_params();

    make_app_folders(cfg, filesystem);

    Screen screen;
    if (!screen.video.open("PocketTracker", ui::DESIGN_W, ui::DESIGN_H, /*fullscreen=*/false,
                           /*resizable=*/cfg.windowed)) {
        return 1;
    }

    SdlInput input;

    // POCKETTRACKER_INPUT_TRACE=1 prints every input event and what it mapped to — the only view of
    // the layer between the hardware and ButtonEvent. An env var because PortMaster runs the binary
    // with no arguments.
    const char* inputTrace = SDL_getenv("POCKETTRACKER_INPUT_TRACE");
    if (inputTrace && inputTrace[0] == '1') {
        input.set_trace(true);
        std::printf("input:   TRACE ON - every event prints, with what it mapped to (or did not)\n");
    }

    input.open_controllers();

    if (inputTrace && inputTrace[0] == '1') screen.touch.set_trace(true);

    // Click/haptic sink: null on desktop and handhelds, a JNI shim on Android (button_feedback.h).
    screen.touch.set_feedback(cfg.buttonFeedback);

    if (cfg.touchCapable) {
        screen.helvFont.load(screen.video.renderer(), "fonts/helvetica_regular.otf", cfg.console);
        screen.arrowFont.load(screen.video.renderer(), "fonts/LinBiolinum_Rah.ttf", cfg.console);
    }

    // The UI edits the host's live project in place. The boot screen is AppState's own default.
    ui::AppState state;
    state.project = &host.edit_project();

    // Which SETTINGS rows exist, and whether PROJECT has an EXIT — a value, not an #ifdef
    // (ui/platform_caps.h).
    state.caps = cfg.caps;

    load_settings_and_config(filesystem, state, input);
    apply_midi_overrides(cfg, state);
    derive_midi_auto_offset(audio, state);

    // The whole input layer. It edits the same Project the sequencer reads, so an edit is live at once.
    ui::InputDispatcher dispatch(state, host, filesystem);

    open_midi_ports(cfg, host, dispatch, state);
    // Latched now: by the exit report the port is closed.
    const bool midiInPortWasOpen = cfg.midiIn && cfg.midiIn->is_open();

    // What the background watcher reads. Installed below, after boot_recovery.
    BackgroundContext bg{&host, &dispatch, &filesystem, &state, &audio, cfg.console, &cfg.background};

    FrameLoop loop(cfg, host, midiInConsole, state, dispatch, input, screen, bg);

    // ── What a render needs from the shell ───────────────────────────────────────────────────────
    //
    // Export and resample are synchronous: the loop runs no frames while they work. ⚠️ The audio
    // device is PAUSED, not just the transport stopped, so the callback cannot touch the engine
    // mid-render.
    ui::InputDispatcher::RenderHooks hooks;
    hooks.suspend_audio = [&audio](bool suspend) { audio.setPaused(suspend); };
    hooks.repaint       = [&loop] { loop.repaint(); };
    hooks.load_pump     = [&loop] { return loop.load_pump(); };
    dispatch.set_render_hooks(std::move(hooks));

    // ⚠️ Installed once, here. Decoders report load progress through it; with nothing installed (tools,
    // the offline render) they run as normal. The clock is passed in because set_now() also runs due
    // work, and an autosave firing mid-load would save a half-loaded document.
    pt::set_load_tick([&dispatch](float fraction) {
        return dispatch.load_tick(static_cast<long long>(SDL_GetTicks64()), fraction);
    });

    // ── Lifecycle ────────────────────────────────────────────────────────────────────────────────
    //
    // Where relative media paths resolve (absolute ones ignore it). ⚠️ Never empty: that would resolve
    // against the process's working directory. The platform picks the project's folder, or the app root.
    dispatch.set_media_base_dir(cfg.mediaBaseDir);

    // An autosave found at launch means the last session ended badly; SETTINGS > RESUME decides
    // whether to ask or restore. ⚠️ After load_settings (RESUME is read) and push_params. Printed
    // because a handheld being brought up may have no screen yet.
    switch (dispatch.boot_recovery()) {
        using BR = ui::InputDispatcher::BootRecovery;
        case BR::NONE:     break;   // the common case, and it deserves no line of its own
        case BR::ASKED:    std::printf("autosave: FOUND - asking (SETTINGS > RESUME = ASK)\n"); break;
        case BR::RESTORED: std::printf("autosave: FOUND - restored (SETTINGS > RESUME = AUTO)\n"); break;
        case BR::DROPPED:  std::printf("autosave: FOUND but UNREADABLE - dropped\n"); break;
    }

    // The background watcher. Installed after boot_recovery, so a backgrounding during start-up cannot
    // save over the autosave being decided about. Removed first thing after the loop.
    SDL_AddEventWatch(on_app_event, &bg);

    // The help banner and the once-a-second status line are the high-volume output; a platform whose
    // stdout goes nowhere (Android) turns them off. The one-line boot diagnostics above always print.
    if (cfg.console) print_key_help();

    loop.run();

    // ── Leaving ──────────────────────────────────────────────────────────────────────────────────
    //
    // ⚠️ The watcher goes first: `bg` lives on this stack and teardown can push SDL events. On Android
    // a real destroy still arrives here as SDL_QUIT, so the saves below run.
    SDL_DelEventWatch(on_app_event, &bg);

    // ⚠️ Join the sender thread before host.stop(), so the stop's note-offs go out from the only
    // thread left — and before the host and the audio stream it reads go away.
    midiSender.stop();

    // ⚠️ The port outlives this function, but `host`, which it delivers into, does not. Unwire, then
    // close — close() waits for a callback already in flight.
    if (cfg.midiIn) {
        cfg.midiIn->set_sink(nullptr);
        cfg.midiIn->close();
    }

    if (cfg.midiIn && (host.midi_in_bytes() > 0 || midiInPortWasOpen))
        print_midi_in_report(cfg.midiIn, host, midiInConsole);

    // Settings are saved once, here, not on every edit (a held A+UP repeats every 100 ms). ⚠️ Not
    // behind a dirty flag: it compares with the bytes on disk, so an edit from any screen is saved.
    switch (ui::save_settings_if_changed(filesystem, state.settings, state.theme)) {
        using SW = ui::SettingsWrite;
        case SW::UNCHANGED: break;   // nothing moved this session; the file already says so
        case SW::SAVED:     std::printf("settings: saved\n"); break;
        // A full SD card, a read-only mount.
        case SW::FAILED:
            std::printf("settings: SAVE FAILED - %s\n", filesystem.settings_path().c_str());
            break;
    }

    // ⚠️ Every way out of the loop arrives here. Save unsaved work now: on a handheld the process is
    // often killed before the 3 s autosave fires. A no-op on a clean document, so an autosave found at
    // launch always means the last session ended badly. A confirmed PROJECT > EXIT has already deleted
    // the autosave and left the project clean; every other exit (a signal, a window close, F10) keeps
    // the work.
    dispatch.flush_autosave();

    host.stop();

    // The MIDI timing report, after the last message (the stop's panic) has left. ⚠️ `senderOn` was
    // latched at start: midiSender.stop() has cleared running() by now.
    if (jitterOn) {
        host.midi_out().set_send_observer(nullptr);
        midiJitter.report(senderOn ? "B3 sender thread" : "60 Hz frame loop (pre-B3)", audio.sampleRate(),
                          host.project().tempo);
    }

    // ⚠️ Before closeStream(), which takes away the frame count a measured output latency needs.
    latency::report(audio.sampleRate(), cfg.midiIn && cfg.midiIn->polled(), audio.outputLatency());

    engineRef.onResumeRequested = nullptr;
    audio.closeStream();
    input.close_controllers();
    // ⚠️ Fonts, skin and overlay before the video: their textures belong to its renderer.
    screen.close();
    return 0;
}

}  // namespace ptshell
