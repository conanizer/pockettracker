#include "app.h"

// <cmath> before <SDL.h> — see sdl-audio-engine.h.
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
// Counts what MIDI input delivered, printed at exit — inside the app a working input and a dead one
// look the same. Runs on the frame loop's thread, so it may print.
struct MidiInConsole : songcore::IMidiInObserver {
    bool     trace    = false;   // POCKETTRACKER_MIDI_IN_TRACE=1
    uint64_t messages = 0;
    uint64_t records  = 0;
    uint64_t silent   = 0;       // messages that produced no bus record at all

    // CC arrival rates while a knob turns: per poll, per 16 ms frame, peak per second. ⚠️ Every
    // controller number counts, so two knobs read as one stream; one poll shares one timestamp.
    uint64_t ccTotal   = 0;
    uint64_t ccFirstMs = 0;      // wall clock of the first and last CC — the span the rate is over
    uint64_t ccLastMs  = 0;
    uint64_t ccDrains  = 0;      // drains that carried at least one CC
    uint64_t ccMaxPerDrain = 0;
    uint64_t drain     = 0;      // the poll that took the messages back: bumped above host.poll()

    // Most CCs inside one fixed `width`-ms window; a burst across a boundary counts twice, so a floor.
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

        // Rebuilt from the parsed message, so a line that disagrees with the cable is a parser bug.
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
    uint64_t ccDrain_ = ~0ull, ccInDrain_ = 0;
};

// ─── The background watcher ──────────────────────────────────────────────────────────────────────
//
// Borrowed from run()'s stack. ⚠️ The watcher is removed first thing in run()'s teardown.
struct BackgroundContext {
    SongcoreHost*        host       = nullptr;
    ui::InputDispatcher* dispatch   = nullptr;
    ui::FileSystem*      filesystem = nullptr;
    ui::AppState*        state      = nullptr;
    AudioBackend*        audio      = nullptr;
    bool                 console    = false;
    const AppConfig::BackgroundPlayback* background = nullptr;

    // No device is open and the loop owes a reopen. No lock: the watcher runs on the loop's thread.
    bool audioClosed = false;
    // Off the screen: nothing may be drawn (the GL context is backed up).
    bool backgrounded = false;
    // …and a song was left playing there, under the playback service.
    bool playingInBackground = false;
};

// SDL_APP_WILLENTERBACKGROUND (Android's Home press): save what would be lost and hand the audio
// device back. Runs on the native thread inside the loop's own SDL_PollEvent, so it needs no lock; a
// watcher because SDL flushes the queue after a quit. Each step is idempotent — it fires on every
// trip to the background.
int SDLCALL on_app_event(void* userdata, SDL_Event* e) {
    if (e->type != SDL_APP_WILLENTERBACKGROUND) return 0;

    auto* c = static_cast<BackgroundContext*>(userdata);

    // Always printed: the steps are usually no-ops, so this tells a run watcher from a silent one.
    if (c->console) std::printf("lifecycle: entering background\n");

    c->backgrounded = true;

    // 1. A playing song keeps playing if the playback service started; without it Android freezes the
    // process and the stream dies silently — so stop.
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

    // 2. A backgrounded app can be killed before the 3 s autosave debounce fires.
    c->dispatch->flush_autosave();

    // 3. Settings: on Android the save after the loop runs only on a real destroy.
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

    // 4. ⚠️ A stream left open in the background comes back dead. Last, because the close waits for
    // the callback in flight; the loop reopens it on return.
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
// The loop runs at full rate; only the draw is skipped when nothing changes. Audible = playing, the
// preview lane active, or the waveform above the silence floor (a preview ringing after STOP).
constexpr float SCOPE_SILENCE_THRESHOLD = 0.002f;

bool audio_is_audible(const ui::AppState& s) {
    if (s.isPlaying || s.previewLaneActive) return true;

    // ⚠️ Null means silence: engine_feed leaves it null when there is nothing to show.
    if (s.waveform) {
        for (int i = 0; i < ui::WAVEFORM_SIZE; ++i) {
            if (std::fabs(s.waveform[i]) > SCOPE_SILENCE_THRESHOLD) return true;
        }
    }
    return false;
}

// ─── Boot steps ──────────────────────────────────────────────────────────────────────────────────

// Opens the requested project or the blank document. False = it did not parse.
bool open_boot_project(const AppConfig& cfg, SongcoreHost& host) {
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
        host.new_project();
        std::printf("project: (none given) - starting on a blank document\n");
    }
    return true;
}

// Creates the app folders at boot: a handheld user's first move is copying samples onto the card.
void make_app_folders(const AppConfig& cfg, ui::FileSystem& filesystem) {
    filesystem.projects_directory();
    filesystem.samples_directory();
    filesystem.renders_directory();
    filesystem.instruments_directory();
    filesystem.soundfonts_directory();
    filesystem.themes_directory();
    filesystem.scales_directory();
    filesystem.grooves_directory();
    filesystem.reverbs_directory();
    filesystem.delays_directory();
    std::printf("files:   %s\n", cfg.appRoot.c_str());

    // The app's own files may live elsewhere (on Android they do).
    if (filesystem.settings_path() != cfg.appRoot + "/settings.json")
        std::printf("files:   app files: %s\n", filesystem.settings_path().c_str());

    // The count too: a storage backend that lists nothing looks like an empty folder.
    const std::string projects = filesystem.projects_directory();
    std::printf("files:   projects: %s (%zu entries)\n", projects.c_str(),
                filesystem.list_files(projects).size());
}

// settings.json, then config.json and the factory banks.
void load_settings_and_config(ui::FileSystem& filesystem, ui::AppState& state, SdlInput& input) {
    // ⚠️ False also means a file that would not parse — and the save at quit overwrites it, so say so.
    if (ui::load_settings(filesystem, state.settings, state.theme)) {
        std::printf("settings: %s\n", filesystem.settings_path().c_str());
    } else if (filesystem.file_exists(filesystem.settings_path())) {
        std::fprintf(stderr,
                     "warning: %s exists but could not be read or parsed - starting on the factory "
                     "settings, and they will be saved over it at quit\n",
                     filesystem.settings_path().c_str());
    }

    // config.json: seeded only when absent (the template states every current value). The binding
    // defaults come from here — pt-ui cannot name an SDL key or pad button.
    if (ui::seed_config_template(filesystem, SdlInput::default_keyboard_bindings(),
                                 SdlInput::default_gamepad_bindings()))
        std::printf("config:   seeded template %s\n", filesystem.config_path().c_str());

    // The same text as config.example.json, to show settings newer than the user's file. Not on the
    // Miyoo Mini: its launcher puts its own template there.
    if (!SdlInput::on_miyoo_mini() &&
        ui::write_config_example(filesystem, SdlInput::default_keyboard_bindings(),
                                 SdlInput::default_gamepad_bindings()))
        std::printf("config:   wrote %s\n", ui::config_example_path(filesystem).c_str());

    // The factory scales and grooves, only into a folder with none, so a user's pruning is kept.
    if (const int seeded = ui::seed_scale_bank(filesystem); seeded > 0)
        std::printf("files:   seeded %d factory scales in %s\n", seeded,
                    filesystem.scales_directory().c_str());

    if (const int seeded = ui::seed_groove_bank(filesystem); seeded > 0)
        std::printf("files:   seeded %d factory grooves in %s\n", seeded,
                    filesystem.grooves_directory().c_str());

    if (ui::load_folder_config(filesystem, state.folderConfig))
        std::printf("config:   %s\n", filesystem.config_path().c_str());

    // Every rejected entry is printed: a hand-edited file that looks applied and is not is the worst failure.
    {
        ui::InputConfig                     inputCfg;
        std::vector<ui::InputConfigWarning> warnings;
        ui::load_input_config(filesystem, inputCfg, warnings);
        for (const ui::InputConfigWarning& w : warnings) std::printf("config:   %s\n", w.text.c_str());
        input.apply_input_config(inputCfg);

        // config.json only seeds the ABXY row: once the row says anything but AUTO, it wins.
        if (state.settings.abxyIndex == 0 && inputCfg.abxy != ui::AbxyLayout::AUTO)
            state.settings.abxyIndex = static_cast<int>(inputCfg.abxy);
    }

    // Saved skin id and overlay name → the indices SETTINGS edits (unknown → DARK / OFF).
    state.settings.skinIndex = device_skin_index(state.settings.portraitSkin);

    state.settings.overlayIndex = screen_overlay_index(state.settings.overlayName);
}

// The backends go into AppState for the MIDI screen (either may be null). Env-var overrides land as
// settings before anything opens.
void apply_midi_overrides(const AppConfig& cfg, ui::AppState& state) {
    state.midiOut = cfg.midiOut;
    state.midiIn  = cfg.midiIn;
    if (!cfg.midiInDevice.empty())  state.settings.midiInDevice  = cfg.midiInDevice;
    if (!cfg.midiOutDevice.empty()) state.settings.midiOutDevice = cfg.midiOutDevice;
    if (cfg.midiOffsetMs != 0) {
        state.settings.midiOffsetMs   = cfg.midiOffsetMs;
        // ⚠️ …and AUTO off, or the saved override would change nothing audible.
        state.settings.midiOffsetAuto = false;
    }
    if (cfg.midiSyncOut >= 0)       state.settings.midiSyncOut   = cfg.midiSyncOut != 0;
}

// The OFFSET row's AUTO: the cable leads the speakers by the output latency. Usually only one buffer
// of it is visible, so this is close rather than exact; ASIO reports it all.
void derive_midi_auto_offset(AudioBackend& audio, ui::AppState& state) {
    const int rate = audio.sampleRate();
    const AudioBackend::OutputLatency lat = audio.outputLatency();
    if (rate > 0 && lat.frames > 0)
        state.midiAutoOffsetMs = (lat.frames * 1000 + rate / 2) / rate;
}

// Opens the ports the settings name — the same code as the MIDI screen's rows — one console line each.
void open_midi_ports(const AppConfig& cfg, SongcoreHost& host, ui::InputDispatcher& dispatch,
                     const ui::AppState& state) {
    dispatch.boot_midi_port();
    if (cfg.midiOut) {
        // ⚠️ The offset in force, which under AUTO is not the stored one.
        std::printf("midi:    OUT %s (offset %+d ms%s, sync %s)\n",
                    cfg.midiOut->is_open() ? state.midiOutOpenName.c_str() : "OFF",
                    ui::midi_offset_in_force(state.settings, state.midiAutoOffsetMs),
                    state.settings.midiOffsetAuto ? " AUTO" : "",
                    state.settings.midiSyncOut ? "ON 24 PPQN" : "off");
    }

    // One line whenever a backend exists: "not plugged in" vs "refused" vs "not compiled in".
    dispatch.boot_midi_in_port();
    if (cfg.midiIn) {
        std::printf("midi:    IN  %s (%d port(s) enumerated)\n",
                    cfg.midiIn->is_open() ? state.midiInOpenName.c_str() : "OFF",
                    static_cast<int>(state.midiInDeviceNames.size()) - ui::MIDI_FIRST_PORT);
    } else {
        std::printf("midi:    IN  no input backend compiled into this build\n");
    }

    // THRU, except when IN and OUT are the same device (it would feed back). Both outcomes print.
    const bool midiThru = host.midi_in_thru();
    std::printf("midi:    THRU %s%s\n", midiThru ? "on" : "OFF",
                midiThru ? " (a live key on an EXTERNAL instrument reaches the cable)"
                         : " - IN and OUT are the SAME port, and thru on a loopback is a feedback loop");
}

// The help banner. ASCII only: the console's encoding is not ours.
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
// The window and everything drawn around the 640×480 frame. ⚠️ close() unloads the fonts, skin and
// overlay before the video: their textures belong to its renderer.
struct Screen {
    SdlVideo video;

    // The on-screen gamepad; enabled per frame, so plugging a controller switches layouts live.
    SdlTouch touch;

    // The portrait device skin, reloaded only when the SETTINGS skin column changes.
    Skin skin;

    // Button labels; a failed load falls back to the 5×5 font.
    Font helvFont;

    // The D-pad arrows Helvetica lacks (Linux Biolinum, SIL OFL); a failed load draws line arrows.
    Font arrowFont;

    // The CRT overlay, reloaded when the OVERLAY choice changes.
    ScreenOverlay screenOverlay;

    // Composites `skin` around the frame when the output is portrait; owns no resources.
    PortraitSkin portrait;

    ui::Canvas canvas;

    // Puts `canvas` on screen with this orientation's surround and the overlay; false when the frame
    // was identical and skipped. Shared with the render repaint hook, so a blocking render keeps the skin.
    bool present(const ui::AppState& state, SdlInput& input) {
        const int  ovStr = state.settings.overlayStrength;
        const bool ovOn  = state.settings.overlayIndex > 0 && ovStr > 0 && screenOverlay.loaded();

        // Folded into the skip signature (bits 48–60, clear of the others), so an overlay change repaints.
        const uint64_t crtSig = ovOn
            ? ((1ull << 60) | (static_cast<uint64_t>(state.settings.overlayIndex & 0xF) << 56) |
               (static_cast<uint64_t>(ovStr & 0xFF) << 48))
            : 0;
        const uint32_t scrim = ui::modal_backdrop_active(state) ? ui::MODAL_BACKDROP : 0;

        if (portrait.active()) {
            // The frame in the bezel, chrome behind it, buttons in front; the theme bg fills the gap.
            const auto chrome = [this, bg = state.theme.background](SDL_Renderer* r) {
                portrait.draw_chrome(r, skin, bg);
            };
            const auto buttons = [this, &input, ovOn, ovStr](SDL_Renderer* r) {
                if (ovOn) screenOverlay.draw(r, portrait.frame_rect(), ovStr);
                portrait.draw_buttons(r, skin, helvFont, arrowFont, input);
            };
            // ⚠️ A modal dims only the bezel glass, never the casing or buttons — a dark skin around a
            // bright cluster reads as a bug.
            return video.present_skinned(canvas, portrait.casing_argb(), portrait.frame_rect(),
                                         chrome, buttons, portrait.signature(input) ^ crtSig, scrim,
                                         portrait.screen_rect());
        }
        // Landscape / desktop: the centred frame, the touch panels in the bars beside it.
        const auto overlay = [this, &input, ovOn, ovStr, fr = video.frame_rect()](SDL_Renderer* r) {
            if (ovOn) screenOverlay.draw(r, fr, ovStr);
            touch.draw(r, input);
        };
        return video.present(canvas, state.theme.background, overlay, touch.signature(input) ^ crtSig,
                             scrim);
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
// Two rates: input, MIDI and the sequencer refill every POLL_MS (POLL_IDLE_MS while idle); drawing
// once per display frame. ⚠️ Under vsync SDL_RenderPresent blocks until the vblank and nothing is
// polled meanwhile, so the next frame is scheduled half a period after the present returns and the
// wait happens in the loop's own pacing — this assumes a frame draws in under half a refresh.
// FRAME_MS is the fallback where no refresh rate is reported.
constexpr Uint64 POLL_MS      = 4;
constexpr Uint64 POLL_IDLE_MS = 16;
// Off the screen (Android): the idle rate serves a playing song's lookahead pump.
constexpr Uint64 BACKGROUND_IDLE_MS = 250;
constexpr Uint64 QUIET_MS     = 1000;   // input this recent still counts as something arriving
constexpr Uint64 FRAME_MS     = 16;

// An Android rotation resizes the surface over several frames, not all reported, so a few full redraws
// are forced after the size last changed.
constexpr int RESIZE_SETTLE_FRAMES = 4;

Uint64 env_ms(const char* name) {
    const char* v = SDL_getenv(name);
    if (!v || !v[0]) return 0;
    const long n = SDL_strtol(v, nullptr, 10);
    return n > 0 ? static_cast<Uint64>(n) : 0;
}

bool env_is(const char* name, char c) {
    const char* v = SDL_getenv(name);
    return v && v[0] == c;
}

// What the loop keeps between ticks, and the steps of one tick; borrows what boot made.
class FrameLoop {
  public:
    FrameLoop(const AppConfig& config, SongcoreHost& songHost, MidiInConsole& console,
              ui::AppState& appState, ui::InputDispatcher& dispatcher, SdlInput& sdlInput,
              Screen& window, BackgroundContext& background)
        : cfg(config), engineRef(*config.engine), audio(*config.audio), host(songHost),
          midiInConsole(console), state(appState), dispatch(dispatcher), input(sdlInput),
          screen(window), bg(background), midiThru(songHost.midi_in_thru()) {}

    // Until a quit, F10, PROJECT > EXIT or `terminate_requested` (desktop's signal flag; the work is
    // saved after this returns). ⚠️ A kill during the synchronous export is seen only when it ends.
    void run();

    // The progress readout pushed from inside a blocking render.
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

    // A physical pad? The platform's answer where it has one (see app.h), else SDL's count. Cached and
    // refreshed on controller add/remove — on Android the call is a JNI round trip.
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

    // THRU as last printed; picking a loopback port mid-session changes it.
    bool midiThru;

    Uint64 lastStatus = 0;

    // Dev only: POCKETTRACKER_AUTOPLAY_MS presses START once, POCKETTRACKER_QUIT_AFTER_MS pushes
    // SDL_QUIT, each n ms after the first frame. One button, no script format: a second input path
    // would have to be kept in step with the real one.
    const Uint64 autoplayMs  = env_ms("POCKETTRACKER_AUTOPLAY_MS");
    const Uint64 quitAfterMs = env_ms("POCKETTRACKER_QUIT_AFTER_MS");
    Uint64       firstFrameMs  = 0;   // 0 until the first frame — the clock both hooks count from
    bool         autoplayFired = false;

    // Last values logged, so each prints once per change; the start values make the first frame print.
    int lastPortraitW = 0, lastPortraitH = 0;

    bool lastUseTouch = false;
    int  lastGateW = -1, lastGateH = -1;

    // Loaded skin / overlay (-1 = none yet); reloaded only when the choice changes.
    int loadedSkinIdx = -1;
    int loadedOverlayIdx = -1;

    // SETTINGS > AUDIO OUT: the row value last shown (-1 = boot, which applies the saved NAME) and the
    // output last seen playing (-1 = not read yet).
    int shownAudioOut   = -1;
    int playingAudioOut = -1;
    // A new Windows output device and the retries of the saved driver since. A replugged interface's
    // driver says nothing, and can need a few seconds after Windows' notice before it opens.
    Uint64 audioArrivalMs = 0;
    int    audioRetries   = 0;

    bool physicalPad = false;   // set by run(), then by `padDirty`
    bool padDirty    = false;

    // Whether `layoutIndex` means the FULL/PORTRAIT choice: the list has one entry without a pad, two with.
    bool padLayoutLive = false;

    // The last rotation permission sent (-1 = none); pushed on change, a JNI round trip.
    int lastLandscapeAllowed = -1;

    // Dev only: PT_TOP_ANCHOR=1 forces the clip-on-pad frame placement on a desktop.
    const bool forceTopAnchor = env_is("PT_TOP_ANCHOR", '1');

    // Idle skip: `sawInput` — this frame may change the screen; `audibleEdge` / `timedWorkEdge` — the
    // first quiet frame still draws (or the scope freezes mid-wave); `drewOnce` — the first frame draws.
    bool sawInput      = false;
    bool audibleEdge   = true;
    bool timedWorkEdge = false;
    bool drewOnce      = false;

    // The status line's frame counts — a working idle skip and a broken one look the same on screen.
    long long drawn = 0, presented = 0, skipped = 0, polls = 0;

    Uint64 nextFrameMs = 0;   // 0 so the first tick draws
    Uint64 lastPollMs  = 0;   // what pace_tick measures against
    Uint64 lastInputMs = 0;

    Uint64   audioReopenMs = 0;  // when the next reopen attempt may run; 0 = at once
    uint64_t midiSeen      = 0;  // messages the audio thread had handled at the last tick

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

    // First, so it times the whole iteration including the wait. Off unless POCKETTRACKER_LATENCY=1.
    latency::poll_tick(state.isPlaying);

    const Uint64 now = SDL_GetTicks64();

    // Read once so the work around the event drain agrees. ⚠️ Never off the screen (GL context backed up).
    const bool frameDue = now >= nextFrameMs && !bg.backgrounded;

    // Before the event drain, so a finger hits what is on screen.
    if (frameDue) lay_out_frame(now);

    handle_input(now);
    reopen_audio_if_lost(now);
    run_scripted_hooks(now);
    pump_midi_and_sequencer(now);
    play_on_in_background();

    if (frameDue) draw_frame(now);

    pace_tick();
}

// Runs instead of the loop during a load: keeps SDL pumping so the background watcher can fire, presses
// are consumed rather than replayed afterwards, and B cancels. ⚠️ It does not dispatch them (that would
// re-enter the dispatcher mid-load); SDL_QUIT cancels too.
bool FrameLoop::load_pump() {
    bool cancel = false;
    const Uint64 now = SDL_GetTicks64();

    // The load refills the sequencer at this cadence, so to the latency probe this is a poll.
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

    // ⚠️ Drained, or a press would fire on the screen underneath afterwards. Only a fresh B cancels.
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

    // A size change arms a few forced redraws, re-armed while the size keeps moving.
    if (outW != lastOutW || outH != lastOutH) {
        resizeSettle = RESIZE_SETTLE_FRAMES;
        lastOutW = outW;
        lastOutH = outH;
    }

    // ── Layout: on-screen buttons or full screen ─────────────────────────────────────────────────
    //
    // One gate read by both `touch` and `portrait`. A touchscreen with no controller gets on-screen
    // buttons; with one, full screen unless the user keeps the buttons (`touchButtonsWithPad`, false by
    // default). Per frame, so hot-plugging switches on the next one.
    if (padDirty) { physicalPad = has_pad(); padDirty = false; }
    const bool padChoice = cfg.touchCapable && physicalPad;

    // ⚠️ The row's edit is read back before update_skin re-derives the index — input is polled after
    // this. `padLayoutLive` stops a freshly plugged pad reading the touch-only index 0 as FULL.
    if (padChoice && padLayoutLive)
        state.settings.touchButtonsWithPad = (state.settings.layoutIndex == 1);
    padLayoutLive = padChoice;

    const bool useTouch  = cfg.touchCapable &&
                           (!physicalPad || state.settings.touchButtonsWithPad);
    touch.set_enabled(useTouch);

    // Landscape only while FULL is in force; pushed live because the boot hint is read once.
    if (cfg.allowLandscape && static_cast<int>(!useTouch) != lastLandscapeAllowed) {
        lastLandscapeAllowed = !useTouch ? 1 : 0;
        cfg.allowLandscape(!useTouch);
    }

    // The frame moves to the top half when a clip-on pad covers a portrait phone's bottom: a
    // touchscreen, a pad, portrait output, no on-screen buttons.
    const bool topAnchor = forceTopAnchor ||
                           (!useTouch && cfg.touchCapable && physicalPad && outH > outW);
    video.set_top_anchor(topAnchor);

    // Printed on change: a wrong pad answer, a wrong aspect and an unset touchCapable look alike otherwise.
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

    // LAYOUT picks the skin while buttons are up, FULL/PORTRAIT while a pad is attached.
    state.caps.touchLayouts = cfg.caps.touchLayouts && cfg.touchCapable;

    // BTN SOUND/VIBRO only while on-screen buttons are drawn; the face-button swap only with a pad.
    state.caps.buttonFeedback = cfg.caps.buttonFeedback && useTouch;

    state.caps.padAttached = physicalPad;

    // Read live, so the row applies on the next press.
    input.set_abxy(static_cast<ui::AbxyLayout>(
        std::clamp(state.settings.abxyIndex, 0, 2)));

    touch.set_feedback_settings({state.settings.buttonSoundEnabled,
                                 state.settings.buttonSoundVolume,
                                 state.settings.buttonVibroEnabled,
                                 state.settings.vibroPower});

    update_skin(useTouch, padChoice);
    update_overlay();
    sync_audio_output(now);

    // Portrait first: it decides which geometry `touch` hit-tests (the same rects it draws).
    portrait.layout(outW, outH, useTouch, state.settings.scalingBilinear);
    if (portrait.active())
        touch.layout_portrait2(portrait.cluster_rect(), portrait.button_rects(), outW, outH);
    else
        touch.layout(video.frame_rect(), outW, outH);

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

// The device skin and the LAYOUT row: FULL (0) / PORTRAIT (1) with a pad, else just PORTRAIT. The
// index is derived from the saved boolean — the list's length changes with the pad.
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
        state.settings.portraitSkin = d.id;

        if (state.settings.skinIndex != loadedSkinIdx) {
            screen.skin.load(screen.video.renderer(), d.id, cfg.console, d.art);
            screen.portrait.set_skin(d.casingFillArgb, d.labelRgb, d.bezelThicknessX, d.art);
            loadedSkinIdx = state.settings.skinIndex;
        }

        // The chromeless skin follows the live theme, so a theme edit restyles it at once.
        screen.portrait.set_theme(state.theme.background, state.theme.textValue);
    } else {
        state.settings.skinCount = 0;
        if (!padChoice) { state.settings.layoutCount = 1; state.settings.layoutIndex = 0; }
    }
}

// The CRT overlay: choice count and text, the saved name kept in step, the PNG decoded on change.
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

// SETTINGS > AUDIO OUT: the row edits an index, the switch happens here (at boot from the saved NAME).
// The name is rewritten only when a pick plays, so a driver absent at boot is tried again next launch.
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
    // The saved driver is not playing: try it again after a device arrives. A failed try changes nothing.
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

    // Also moves with no pick: a driver that did not come back after a reset.
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
    // ⚠️ Before the drain, so this frame's B gets this frame's answer. B repeats only as the keyboard's
    // backspace.
    input.set_b_repeatable(state.qwerty.isOpen);

    SDL_Event e;
    while (SDL_PollEvent(&e)) handle_sdl_event(e, now);
    input.tick(now);

    dispatch.set_now(static_cast<long long>(now));

    ui::ButtonEvent be;
    while (input.poll(be)) {
        // ⚠️ Key repeat comes from input.tick(), with no SDL event behind it.
        sawInput = true;
        ui::handle_button(be, dispatch, mapper, now);
    }

    // Somebody is here: keeps the poll rate fast (pace_tick).
    if (sawInput) lastInputMs = now;
}

void FrameLoop::handle_sdl_event(const SDL_Event& e, Uint64 now) {
    // Any event may change the screen; over-drawing costs one frame, and present() compares pixels.
    sawInput = true;

    if (e.type == SDL_CONTROLLERDEVICEADDED || e.type == SDL_CONTROLLERDEVICEREMOVED ||
        e.type == SDL_JOYDEVICEADDED || e.type == SDL_JOYDEVICEREMOVED)
        padDirty = true;

    if (e.type == SDL_AUDIODEVICEADDED && !e.adevice.iscapture) {
        audioArrivalMs = now;
        audioRetries   = 0;
    }

    // ⚠️ Force the next present when the platform may have blanked or swapped the surface (resume,
    // short sleep, rotation, device reset), or the identical-frame skip leaves it black. Logged.
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

    // The folder on screen may have changed while away (Android's ADD FOLDER… returns this way).
    if (e.type == SDL_APP_DIDENTERFOREGROUND) {
        dispatch.refresh_browser_on_foreground();
        // Back on screen; a song that played on no longer needs the service.
        bg.backgrounded = false;
        if (bg.playingInBackground) {
            bg.playingInBackground = false;
            if (cfg.background.end) cfg.background.end();
        }
    }

    if (e.type == SDL_QUIT) {
        // A window close, or SDL's signal translation where it has one (Windows has none, so main.cpp
        // installs its own). An unclean exit: the flush keeps the work.
        running = false;
    } else if (e.type == SDL_KEYDOWN && e.key.keysym.sym == SDLK_F10) {
        // Dev-only quit; it does not ask, and keeps the autosave. Only PROJECT > EXIT discards it.
        running = false;
    } else if (e.type == SDL_FINGERDOWN || e.type == SDL_FINGERUP || e.type == SDL_FINGERMOTION) {
        // A virtual button feeds SdlInput's own press/release; a finger outside one is ignored.
        screen.touch.handle_finger(e, input, now);
    } else {
        input.handle_event(e, now);
    }
}

// Reopens a device released on the way into the background or lost while open; every 500 ms until it
// works. ⚠️ Not while backgrounded — unless a song is playing there.
void FrameLoop::reopen_audio_if_lost(Uint64 now) {
    const bool owed = bg.audioClosed && (!bg.backgrounded || bg.playingInBackground);
    if ((owed || audio.deviceLost()) && now >= audioReopenMs) {
        audio.closeStream();
        // ⚠️ Owed until an open succeeds: a backend's lost-device report is spent by the attempt.
        bg.audioClosed = true;
        if (audio.openStream()) {
            bg.audioClosed = false;
            derive_midi_auto_offset(audio, state);
            host.set_midi_offset_ms(ui::midi_offset_in_force(state.settings, state.midiAutoOffsetMs));
            std::printf("audio:   device reopened\n");
        } else {
            audioReopenMs = now + 500;
            std::printf("audio:   reopen FAILED - retrying\n");
        }
        std::fflush(stdout);
    }
}

// Dev only; both are 0 unless an env var set them.
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
    if (host.midi_in_thru() != midiThru) {
        midiThru = host.midi_in_thru();
        std::printf("midi:    THRU %s (device pick)\n", midiThru ? "on" : "OFF - loopback");
        std::fflush(stdout);
    }

    // A polled backend (Android's) fetches its bytes here, so pace_tick keeps the fast rate for it.
    // ⚠️ Must stay immediately above host.poll().
    if (cfg.midiIn) cfg.midiIn->pump();

    ++midiInConsole.drain;

    // The lookahead refill; returns at once while the buffer is deep enough. MIDI in is drained by
    // the audio thread — this publishes what it routes against and takes back what it handled.
    host.poll();

    // A MIDI message keeps the fast poll rate, as a button press does.
    if (const uint64_t seen = host.midi_in_messages(); seen != midiSeen) {
        midiSeen    = seen;
        lastInputMs = now;
    }
}

// Android: the notification's STOP is acted on here; a song that stops in the background ends the
// service and releases the device.
void FrameLoop::play_on_in_background() {
    if (cfg.background.takeStopRequest && cfg.background.takeStopRequest() && host.is_playing())
        host.stop();
    if (cfg.background.publishPlaying) cfg.background.publishPlaying(host.is_playing());
    if (bg.backgrounded) {
        state.isPlaying = host.is_playing();   // draw_frame is skipped
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
    // A track with no position returns -1 everywhere and draws no marker.
    for (int t = 0; t < 8; ++t) {
        const PlaybackPosition p = host.playheads(t);
        state.playheads[t] = {p.songRow, p.chainId, p.chainRow, p.phraseId, p.phraseStep};
        const songcore::LiveSlot q = host.live_queue(t);
        state.liveQueue[t] = {q.targetRow, q.stop, q.immediate, q.armed()};
    }
    // ⚠️ Read back from the host: stop() and a project load can both end a LIVE take.
    state.liveMode     = host.live_mode();
    state.blinkPhaseMs = static_cast<int>(now % 1000);
    state.trackMask = host.track_mask();

    // Everything the UI reads back from the engine; after isPlaying, which the waveform's decay uses.
    feed.poll(engineRef, host, state, static_cast<long long>(now));

    // Read every frame so no edit site has to push it. 0x80 lands at -12 dBFS, below the limiter.
    engineRef.setMetronome(state.settings.metronomeEnabled,
                           static_cast<float>(state.settings.metronomeVolume) / 255.0f * 0.5f);

    // Polled (the row or a hand-edited settings.json); recreates the texture on change, so not in present().
    screen.video.set_scaling(state.settings.scalingBilinear ? ScalingMode::FIT : ScalingMode::INTEGER);

    // ── Idle frame skip ──────────────────────────────────────────────────────────────────────────
    //
    // Draw only when something may have changed: sound (or the frame after it stops), input, the
    // first frame, a settling resize, falling meters, a timed status message (and the frame it clears
    // on). present() then skips a frame identical to the screen. ⚠️ That net cannot catch a frame that
    // is never drawn — hence the last terms.
    const bool timedWork = dispatch.has_pending_timed_work();
    const bool metersFalling = layout.has_falling_meters(state);
    const bool audible = audio_is_audible(state);
    const bool settling = resizeSettle > 0;
    if (settling) {
        screen.video.invalidate_backbuffer(/*texture_lost=*/false);
        --resizeSettle;
    }
    if (audible || audibleEdge || sawInput || !drewOnce || settling || metersFalling ||
        timedWork || timedWorkEdge) {
        layout.draw(screen.canvas, state);
        ++drawn;

        const bool didPresent = screen.present(state, input);
        if (didPresent) ++presented;
        drewOnce = true;

        // ⚠️ Anchored on the present's return (a vblank, with vsync). A frame not presented blocked on
        // nothing, so it takes the plain deadline below.
        if (didPresent) {
            const Uint64 hz     = static_cast<Uint64>(screen.video.refresh_hz());
            const Uint64 period = (hz >= 30 && hz <= 240) ? 1000ull / hz : FRAME_MS;
            nextFrameMs = SDL_GetTicks64() + period - period / 2;
        }
    } else {
        ++skipped;
    }

    // The plain deadline, re-anchored only once a whole frame behind: a stall costs one late frame,
    // never a burst.
    if (nextFrameMs <= now) nextFrameMs = SDL_GetTicks64() + FRAME_MS;

    audibleEdge   = audible;
    timedWorkEdge = timedWork;
    sawInput      = false;

    print_status(now);
}

// Once a second on the console: frame counter (the device calls back), playhead (the sequencer
// advances), voices (notes reach the engine) — whichever is stuck names the broken link on a box with
// no screen. `cpu`/`max` are the last second's mean and worst block; `late` counts every overrun
// block since launch, each one an audible dropout.
void FrameLoop::print_status(Uint64 now) {
    if (!cfg.console || now - lastStatus < 1000) return;

    lastStatus = now;
    const ui::TrackPlayhead& t0 = state.playheads[0];
    const AudioEngine::BlockTiming bt = engineRef.getBlockTiming();
    std::printf(
        "%s  frame %-10lld  song %3d  chain %2d  step %2d   voices %2d   %-10s cursor %X,%d"
        "   drew %lld skip %lld same %lld poll %lld   cpu %d.%d%% max %d.%d%% late %lld\n",
        host.is_playing() ? "play" : "stop",
        static_cast<long long>(engineRef.getCurrentFrame()), t0.songRow, t0.chainRow,
        t0.step, engineRef.getActiveVoiceCount(), ui::screen_label(state.currentScreen),
        state.cursorRow, state.cursorColumn, presented, skipped, drawn - presented, polls,
        bt.meanLoad / 10, bt.meanLoad % 10, bt.worstLoad / 10, bt.worstLoad % 10,
        static_cast<long long>(bt.lateBlocks));
    std::fflush(stdout);  // block-buffered to a pipe otherwise
}

// ⚠️ The only place the loop sleeps, so every tick must reach it. Measured from the last tick, so a
// present that blocked past it is not followed by a second wait. `busy` is what arrives with no SDL
// event: a running transport, a ringing sound, a polled MIDI port. ⚠️ Never sleep past a frame already
// due, or animation steps twice in one frame and then not at all.
void FrameLoop::pace_tick() {
    const Uint64 t    = SDL_GetTicks64();
    const bool   busy = state.isPlaying || audibleEdge ||
                        (cfg.midiIn && cfg.midiIn->open_index() >= 0 && cfg.midiIn->polled()) ||
                        t - lastInputMs < QUIET_MS;
    Uint64 due = lastPollMs + (busy ? POLL_MS : POLL_IDLE_MS);
    // ⚠️ Off the screen `nextFrameMs` stays in the past and would stop the loop sleeping.
    if (bg.backgrounded) due = lastPollMs + (state.isPlaying ? POLL_IDLE_MS : BACKGROUND_IDLE_MS);
    else if (nextFrameMs < due) due = nextFrameMs;
    if (due > t) SDL_Delay(static_cast<Uint32>(due - t));
    lastPollMs = SDL_GetTicks64();
}

// ─── Leaving ─────────────────────────────────────────────────────────────────────────────────────

// The MIDI-in report: a count per stage, so a break can be located.
void print_midi_in_report(MidiInBase* base, SongcoreHost& host, const MidiInConsole& midiInConsole) {
    const songcore::MidiInputRouter& r = host.midi_in_router();
    std::printf("midi in: %llu bytes at the port (%llu callbacks, %llu port errors), %llu drained, "
                "%llu messages\n"
                "         routed %llu, dropped: %llu non-channel, %llu no-instrument, "
                "%llu unsupported\n"
                "         queue overflow %llu bytes, parser orphans %llu, messages with no record %llu\n"
                "         dropped during an export %llu bytes, handled messages the UI never saw %llu\n"
                // What the engine and the cable were handed; differs from `routed` by the thru suppression.
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
    std::printf("PocketTracker %s\n", PT_VERSION_STRING);

    // Debug and release share a version string but not the developer rows.
    std::printf("build:   %s\n", cfg.caps.debug ? "debug" : "release");

    AudioEngine&  engineRef  = *cfg.engine;
    AudioBackend& audio      = *cfg.audio;
    ui::FileSystem& filesystem = *cfg.filesystem;

    // Unwired at teardown, before the backend closes.
    engineRef.onResumeRequested = [&audio] { audio.resumeStream(); };

    // ⚠️ Before the host: its MIDI consumer calls this from its destructor (the final panic).
    MidiJitterRecorder midiJitter;

    // ⚠️ Also before the host, which calls it from poll(). Always attached — the counters make a
    // silent input readable.
    MidiInConsole midiInConsole;

    SongcoreHost host(&engineRef, audio.sampleRate());
    host.set_midi_in_observer(&midiInConsole);
    midiInConsole.trace = env_is("POCKETTRACKER_MIDI_IN_TRACE", '1');

    // Before the first load, so another install's absolute media paths are re-rooted onto ours.
    host.set_app_root(cfg.appRoot);

    // Before the project loads, so an EXTERNAL instrument is routed from its first note.
    host.set_midi_out(cfg.midiOut);
    host.set_midi_offset_ms(cfg.midiOffsetMs);

    // ── The MIDI sender thread ───────────────────────────────────────────────────────────────────
    //
    // Started with or without a port: a LEN gate and a panic's note-offs must be released either way.
    // Dev: POCKETTRACKER_MIDI_SENDER=0 leaves the release to the frame loop; POCKETTRACKER_MIDI_JITTER=1
    // measures every released message and reports at exit.
    const bool jitterOn = env_is("POCKETTRACKER_MIDI_JITTER", '1');
    if (jitterOn) host.midi_out().set_send_observer(&midiJitter);

    // ⚠️ After the host: its thread pumps `host`, so it is joined while the host lives.
    MidiSender midiSender(engineRef, host, audio.sampleRate());
    bool       senderOn = false;
    if (env_is("POCKETTRACKER_MIDI_SENDER", '0')) {
        std::printf("midi:    sender thread DISABLED (POCKETTRACKER_MIDI_SENDER=0) - the 60 Hz frame "
                    "loop releases the queue\n");
    } else {
        senderOn = midiSender.start();
    }

    if (!open_boot_project(cfg, host)) return 1;

    // ⚠️ Media, then params: the engine holds state no note carries (mixer, buses, EQ, every
    // instrument's drive/filter/loop); without this the project plays on factory defaults.
    host.push_params();

    make_app_folders(cfg, filesystem);

    Screen screen;
    if (!screen.video.open("PocketTracker", ui::DESIGN_W, ui::DESIGN_H, /*fullscreen=*/false,
                           /*resizable=*/cfg.windowed)) {
        return 1;
    }

    SdlInput input;

    // POCKETTRACKER_INPUT_TRACE=1 prints every input event and what it mapped to. An env var because
    // PortMaster passes no arguments.
    const bool inputTrace = env_is("POCKETTRACKER_INPUT_TRACE", '1');
    if (inputTrace) {
        input.set_trace(true);
        std::printf("input:   TRACE ON - every event prints, with what it mapped to (or did not)\n");
    }

    input.open_controllers();

    if (inputTrace) screen.touch.set_trace(true);

    screen.touch.set_feedback(cfg.buttonFeedback);

    if (cfg.touchCapable) {
        screen.helvFont.load(screen.video.renderer(), "fonts/helvetica_regular.otf", cfg.console);
        screen.arrowFont.load(screen.video.renderer(), "fonts/LinBiolinum_Rah.ttf", cfg.console);
    }

    // The UI edits the host's live project in place.
    ui::AppState state;
    state.appVersion = PT_VERSION_STRING;
    state.project = &host.edit_project();

    state.caps = cfg.caps;

    load_settings_and_config(filesystem, state, input);
    apply_midi_overrides(cfg, state);
    derive_midi_auto_offset(audio, state);

    // It edits the Project the sequencer reads, so an edit is live at once.
    ui::InputDispatcher dispatch(state, host, filesystem);

    open_midi_ports(cfg, host, dispatch, state);
    // Latched now: the port is closed by the exit report.
    const bool midiInPortWasOpen = cfg.midiIn && cfg.midiIn->is_open();

    BackgroundContext bg{&host, &dispatch, &filesystem, &state, &audio, cfg.console, &cfg.background};

    FrameLoop loop(cfg, host, midiInConsole, state, dispatch, input, screen, bg);

    // ── What a render needs from the shell ───────────────────────────────────────────────────────
    //
    // Export and resample are synchronous. ⚠️ The audio device is paused, not just the transport, so
    // the callback cannot touch the engine mid-render.
    ui::InputDispatcher::RenderHooks hooks;
    hooks.suspend_audio = [&audio](bool suspend) { audio.setPaused(suspend); };
    hooks.repaint       = [&loop] { loop.repaint(); };
    hooks.load_pump     = [&loop] { return loop.load_pump(); };
    dispatch.set_render_hooks(std::move(hooks));

    // ⚠️ Installed once. Decoders report load progress through it (nothing installed = tools, offline
    // render). The clock is passed in: set_now() also runs due work, and an autosave mid-load would
    // save a half-loaded document.
    pt::set_load_tick([&dispatch](float fraction) {
        return dispatch.load_tick(static_cast<long long>(SDL_GetTicks64()), fraction);
    });

    // ⚠️ Never empty — it would resolve against the working directory. Absolute paths ignore it.
    dispatch.set_media_base_dir(cfg.mediaBaseDir);

    // An autosave at launch means the last session ended badly; SETTINGS > RESUME asks or restores.
    // ⚠️ After load_settings and push_params. Printed — a handheld in bring-up may have no screen.
    switch (dispatch.boot_recovery()) {
        using BR = ui::InputDispatcher::BootRecovery;
        case BR::NONE:     break;
        case BR::ASKED:    std::printf("autosave: FOUND - asking (SETTINGS > RESUME = ASK)\n"); break;
        case BR::RESTORED: std::printf("autosave: FOUND - restored (SETTINGS > RESUME = AUTO)\n"); break;
        case BR::DROPPED:  std::printf("autosave: FOUND but UNREADABLE - dropped\n"); break;
    }

    // After boot_recovery, so a backgrounding during start-up cannot save over the autosave in question.
    SDL_AddEventWatch(on_app_event, &bg);

    if (cfg.console) print_key_help();

    loop.run();

    // ── Leaving ──────────────────────────────────────────────────────────────────────────────────
    //
    // ⚠️ The watcher first: `bg` lives on this stack and teardown can push events. On Android a real
    // destroy also arrives as SDL_QUIT.
    SDL_DelEventWatch(on_app_event, &bg);

    // ⚠️ Joined before host.stop(), so the stop's note-offs go out from the only thread left.
    midiSender.stop();

    // ⚠️ The port outlives `host`, which it delivers into: unwire, then close (it waits for a callback
    // in flight).
    if (cfg.midiIn) {
        cfg.midiIn->set_sink(nullptr);
        cfg.midiIn->close();
    }

    if (cfg.midiIn && (host.midi_in_bytes() > 0 || midiInPortWasOpen))
        print_midi_in_report(cfg.midiIn, host, midiInConsole);

    // Saved once, here (a held A+UP repeats every 100 ms). Compared with the bytes on disk, not a dirty
    // flag, so an edit from any screen is saved.
    switch (ui::save_settings_if_changed(filesystem, state.settings, state.theme)) {
        using SW = ui::SettingsWrite;
        case SW::UNCHANGED: break;
        case SW::SAVED:     std::printf("settings: saved\n"); break;
        case SW::FAILED:
            std::printf("settings: SAVE FAILED - %s\n", filesystem.settings_path().c_str());
            break;
    }

    // ⚠️ Every way out arrives here; a handheld is often killed before the 3 s autosave fires. A no-op
    // on a clean document — a confirmed PROJECT > EXIT has already left it clean and deleted the
    // autosave; every other exit keeps the work.
    dispatch.flush_autosave();

    host.stop();

    // After the stop's panic has left. ⚠️ `senderOn` was latched: stop() has cleared running().
    if (jitterOn) {
        host.midi_out().set_send_observer(nullptr);
        midiJitter.report(senderOn ? "sender thread" : "60 Hz frame loop", audio.sampleRate(),
                          host.project().tempo);
    }

    // ⚠️ Before closeStream(), which takes away the frame count the measured latency needs.
    latency::report(audio.sampleRate(), cfg.midiIn && cfg.midiIn->polled(), audio.outputLatency());

    engineRef.onResumeRequested = nullptr;
    audio.closeStream();
    input.close_controllers();
    screen.close();
    return 0;
}

}  // namespace ptshell
