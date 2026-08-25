#pragma once

// ─── SETTINGS ────────────────────────────────────────────────────────────────────────────────────
//
// The C++ twin of ui/modules/SettingsModule.kt — and the first screen in the port where the two
// platforms genuinely differ. Which rows exist is a function of PlatformCaps (ui/platform_caps.h);
// where they sit is ui/settings_row_layout.h. Neither answer is re-derived here.
//
// ⚠️ THE MODULE EDITS INDICES AND FLAGS. IT DOES NOT KNOW WHAT A LAYOUT MODE IS.
//
// That is the line that keeps Android's device rows portable without dragging Android into the port.
// LAYOUT is "an enum cycle over `layoutCount` options, currently at `layoutIndex`"; OVERLAY is the
// same plus a hex byte; BTN SOUND is a toggle plus a hex byte. What an index MEANS — FULLSCREEN vs
// PORTRAIT, which .png, how loud a click is — is the platform's business, and it stays there. So
// `DeviceAdapter.LayoutMode` is NOT ported (it would be dead code in a UI that can never use it),
// and yet the LAYOUT row's cursor context and edit semantics still are — which is exactly what lets
// `ptinput` golden all thirteen rows against Kotlin under `PlatformCaps::android()`, including the
// ones the shell will never draw.
//
// The display STRINGS for those rows (the layout's name, the overlay file, the skin) are handed in
// as text the module merely paints. On the shell they are empty, and the rows are not drawn at all.
//
// ⚠️ SINGLE A IS RESERVED FOR ACTIONS. Every value row changes with A+DPAD; plain A does something
// only on THEME (opens the editor) and TEMPLATE (SAVE / CLEAR). Kotlin states this in a comment at
// the top of its class and enforces it by giving every other row an editable context; the port does
// the same, and the dispatcher's A arm therefore has exactly two cases.

#include <string>
#include <vector>

#include "ui/canvas.h"
#include "ui/cursor.h"
#include "ui/platform_caps.h"
#include "ui/settings_row_layout.h"
#include "ui/theme.h"

namespace pt::ui {

/**
 * Everything SETTINGS edits. Lives in AppState, and the shell round-trips it through settings.json.
 *
 * The counts sit beside the indices because an enum cycle's RANGE is part of its cursor context, and
 * only the platform knows it: a device with no physical buttons offers fewer layouts, and a Themes
 * folder with no .png in it offers no overlay but "OFF".
 */
struct SettingsValues {
    // ── The device rows (Android; the shell hides them) ──────────────────────────────────────────
    int  layoutIndex       = 0;
    int  layoutCount       = 1;
    int  skinIndex         = 0;
    int  skinCount         = 0;   // 0 = this layout is not skinned → no second column on LAYOUT

    // ⚠️ The PERSISTED skin selection is the STABLE ID STRING, not `skinIndex`. settings_store.h's rule:
    // an index is meaningless without the list it indexes, so store the NAME and let the platform resolve
    // it to an index against a list that now exists (Phase D). The shell resolves this to `skinIndex` at
    // boot (device_skin.h) and writes it back from whichever skin is chosen. Serialized as
    // `portrait_skin`, matching Android's SharedPreferences key so the C6 prefs import lands here.
    std::string portraitSkin = "amiga-2";

    int  overlayIndex      = 0;   // 0 = "OFF"; 1.. = a file
    int  overlayCount      = 1;   // "OFF" + however many files
    int  overlayStrength   = 128;

    // ⚠️ The PERSISTED overlay selection is the STABLE ID STRING, not `overlayIndex` — same rule and
    // same reason as `portraitSkin` above (an index is meaningless without the list it indexes). The
    // shell resolves this to `overlayIndex` at boot (shell/overlay.h) and writes it back from whichever
    // overlay is chosen. Serialized as `overlay_name`, matching Android's SharedPreferences key so the
    // C6 v2 prefs import lands here. "OFF" is the no-overlay choice.
    std::string overlayName = "OFF";

    // ⚠️ THESE DEFAULTS ARE THE FIRST LAUNCH, and only the first launch — `load_settings` overwrites
    // every one of them from settings.json, so an existing user's choices are untouched by a change
    // here. They describe the app a stranger meets, which is why button feedback is ON: a handheld
    // whose buttons answer nothing reads as a handheld that did not register the press. LO/quiet
    // rather than full: the point is a confirmation, not a noise. (`vibroPower` is a LO/HI switch,
    // 64 vs 255 — see settings_editor.cpp's POW row.)
    bool buttonSoundEnabled = true;
    int  buttonSoundVolume  = 0x0F;
    bool buttonVibroEnabled = true;
    int  vibroPower         = 64;
    bool autosaveResumeAuto = false;

    // Which way round the pad's face buttons are PRINTED: 0 AUTO, 1 XBOX, 2 NINTENDO - the order of
    // ui::AbxyLayout, which is what the shell hands to its input layer.
    //
    // AUTO is a no-op, which is what makes this safe to add to an existing install: a settings.json
    // written before the row existed has no key, falls through to this default, and nothing about
    // that install changes.
    int  abxyIndex          = 0;

    // Show the on-screen buttons even though a physical pad is attached - a phone with a clip-on
    // controller has both, and only its owner knows which they want.
    //
    // FALSE is today's behaviour (a pad turns the buttons off), so an existing install is unchanged.
    // It is a value of its OWN and not `layoutIndex`, because the layout list has one entry when no
    // pad is attached and two when one is: the same index would mean different things in the two
    // states, and unplugging the pad would forget the choice.
    bool touchButtonsWithPad = false;

    // ── The rows every platform has ──────────────────────────────────────────────────────────────
    // BILINEAR, not INTEGER: integer scaling only fills the screen on a display that is an exact
    // multiple of 640×480, and on everything else — the portrait overlay above all — it leaves the
    // editor small and ringed with black. A first run should show the app filling the screen.
    bool scalingBilinear    = true;
    bool insertBefore       = true;
    bool cursorRemember     = false;
    bool notePreviewEnabled = true;

    // FOLDER = REMEMBER / REFRESH (v0.9.4 D2a). When REMEMBER, the file browser opens a SAMPLE load at
    // the folder the last sample was loaded from, carried in `lastSampleFolder`.
    // ⚠️ Only `rememberFolder` PERSISTS. `lastSampleFolder` is SESSION-ONLY (never serialized) — it
    // resets to the default folder on every launch, like CURSOR remembering: the choice survives a
    // restart, the remembered path does not. `rememberFolder` is a new settings.json key; absent in an
    // older file it defaults OFF, so no import-version bump is needed (it was never a prefs key).
    bool        rememberFolder   = false;
    std::string lastSampleFolder;   // runtime only — see settings_store.cpp (not saved)

    // NAV = POOL / SONG. POOL is what B+LEFT/RIGHT has always done — scroll the 00..FF chain and phrase
    // pools. SONG makes B+D-pad walk the ARRANGEMENT instead, so the cursor is a SONG CELL and the chain
    // and phrase on screen are whatever that cell names (the LSDJ/LGPT ruleset).
    //
    // DEFAULT SONG: the arrangement-relative ruleset is the one this app is modelled on, and meeting it
    // on first launch is the point rather than a hazard to opt into.
    //
    // ⚠️ THIS DEFAULT REACHES EXISTING INSTALLS, not just fresh ones. The key is absent from every
    // settings.json written before the row existed, so `get_bool` falls through to this value for
    // everyone who upgrades — unlike the five first-launch defaults, which `load_settings` overwrites
    // from a file that already has their keys. Under SONG a phrase nobody has placed in the song is
    // unreachable, so an upgrading user with unplaced sketches meets that on the first launch after
    // the update; the NAV row is the way back and the manual says so.
    bool navSongRelative = true;

    // ── MIDI (plan §8.1, phase B4.3) ─────────────────────────────────────────────────────────────
    //
    // ⚠️ THESE TWO ARE NOT ON THE SETTINGS SCREEN — they belong to the MIDI screen and merely LIVE in
    // this struct, because this struct is what settings.json round-trips. The plan puts them here on
    // purpose (§7): the device pick and the latency alignment describe THIS MACHINE'S CABLE, not the
    // song, so a project carried to another device keeps its routing intent and re-picks its port.
    // Everything the SONG means by MIDI — channel, bank, program, LEN, CC slots, PROG CHG — is in
    // `songcore::Project`/`Instrument` and travels with the .ptp.
    //
    // ⚠️ THE DEVICE IS THE NAME STRING, NEVER AN INDEX — the same rule `portraitSkin` and `overlayName`
    // above already follow, and MIDI is the case that rule was WRITTEN for: a port list is rebuilt from
    // the OS on every enumeration and reorders itself whenever anything is plugged or unplugged, so an
    // index saved on Tuesday names a different synth on Wednesday. "OFF" is the no-device choice.
    std::string midiOutDevice = "OFF";

    // The INPUT port (phase E2) — same kind, same rule, same "OFF" for no device. ⚠️ It is a NAME here
    // too, and the input list is the one MORE likely to reorder: a USB keyboard is unplugged between
    // sessions where a desk synth is not.
    //
    // ⚠️ **THE ROW THAT EDITS THIS ARRIVES IN E3, AND THIS IS NOT A SETTING WITHOUT A CONSUMER.** It is
    // read at boot and it OPENS THE PORT (`InputDispatcher::boot_midi_in_port`), which is what makes the
    // desk loopback survive a restart; what E3 adds is a way to change it without editing settings.json.
    // The alternative — the shell opening an input port privately until the row exists — is the "two
    // owners of which port is open" bug B4.3 already paid for once.
    std::string midiInDevice = "OFF";

    // Signed milliseconds; positive = MIDI leaves LATER than the audio. Our own output has tens of ms
    // of latency that the cable does not, so the user nudges the two into line by ear (plan §4.3).
    int         midiOffsetMs  = 0;

    // SYNC OUT — the 24 PPQN clock, Start/Stop/Continue and the song position (plan phase C).
    //
    // ⚠️ SETTINGS AND NOT THE PROJECT, where PROG CHG next to it on the same screen is the project's.
    // The distinction is "what does the SONG mean" versus "what is plugged into THIS machine": whether
    // an instrument states its bank and program is a musical decision that travels with the .ptp;
    // whether there is a drum machine on the other end of the cable waiting to be told the tempo is a
    // fact about the desk it is sitting on.
    //
    // Default OFF, deliberately. Clock is ~51 messages a second on a 31 250 baud wire, and a synth left
    // switched to external sync sits silent until it gets one — so a user who has not asked for sync
    // must not be given either surprise.
    bool        midiSyncOut   = false;

    // ⚠️ VISUALIZER is NOT here. It lives on the THEME (`Theme::visualizerType`), which is where
    // Kotlin keeps it too — and not by accident: the oscilloscope reads it off the theme it is already
    // being handed, so it needs no second channel. Note that Android deliberately CARRIES IT ACROSS a
    // theme change (`BUILTINS[next].copy(visualizerType = appTheme.visualizerType)`): the palette is
    // the theme's, the visualizer is the user's. `handle_input` therefore takes a `Theme&`.

    // ── Debug ────────────────────────────────────────────────────────────────────────────────────
    bool traceEnabled = false;
    bool engineCpp    = false;   // the ENG column — Android only; there is no Kotlin here to switch to
};

struct SettingsState {
    const SettingsValues& values;

    int cursorRow    = 0;   // a SettingsRow — the row's NUMBER, not its position on this platform
    int cursorColumn = 1;

    // Text the module paints but does not own: what the current index NAMES on this platform.
    // (Braced defaults on all four, not just the two with a value — a member with no brace-or-equal
    // initializer makes every aggregate `SettingsState{values}` a -Wmissing-field-initializers warning
    // under gcc, and the port compiles clean under -Wall -Wextra.)
    std::string layoutText{};
    std::string skinText{};
    std::string overlayText = "OFF";
    std::string themeName   = "CLASSIC";

    PlatformCaps caps{};
    Theme        theme = theme_classic();
};

struct SettingsInputResult {
    bool modified = false;
};

class SettingsModule {
public:
    static constexpr int WIDTH  = 510;
    static constexpr int HEIGHT = 392;

    /** SCOPE / FLAT / OCTA / OCTA.F / SPECT / SPCT.P — the six, in VisualizerType order. */
    static const std::vector<std::string>& visualizer_names();

    void draw(Canvas& c, int x, int y, const SettingsState& s) const;

    CursorContext cursor_context(const SettingsState& s) const;

    /**
     * Writes straight into `values` (and, for VISUALIZER, into `theme`) — where Kotlin returns a
     * 16-field nullable diff for MainActivity to apply. The difference is Compose, not behaviour:
     * Kotlin's settings live in ~16 separate `mutableStateOf` refs and SharedPreferences, so its
     * module cannot hold a reference to them. Here they are one struct in AppState, and a module that
     * mutates its subject is what every other screen in this port already does.
     */
    SettingsInputResult handle_input(SettingsValues& values, Theme& theme, const PlatformCaps& caps,
                                     int cursor_row, int cursor_column,
                                     const InputAction& action) const;
};

}  // namespace pt::ui
