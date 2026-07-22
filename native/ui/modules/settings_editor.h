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
    bool buttonSoundEnabled = false;
    int  buttonSoundVolume  = 255;
    bool buttonVibroEnabled = false;
    int  vibroPower         = 255;
    bool autosaveResumeAuto = false;

    // ── The rows every platform has ──────────────────────────────────────────────────────────────
    bool scalingBilinear    = false;
    bool insertBefore       = true;
    bool cursorRemember     = false;
    bool notePreviewEnabled = true;

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
