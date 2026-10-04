#pragma once

// ─── Screens ─────────────────────────────────────────────────────────────────────────────────────
//
// The labels are drawn (screen headers, the navigation map), so they are part of the UI's contract.
// Grid positions are in ui/navigation.h.

#include <string>

namespace pt::ui {

enum class ScreenType {
    // Main screens (middle row — always visible)
    SONG,
    CHAIN,
    PHRASE,
    INSTRUMENT,
    TABLE,

    // Context screens — off the main row (ui/navigation.h)
    PROJECT,
    GROOVE,
    SCALE,
    MODS,
    INST_POOL,
    MIXER,      // shared by all columns
    EFFECTS,    // shared by all columns

    // Popup screens — replace the main view temporarily
    FILE_BROWSER,
    SETTINGS,
    SAMPLE_EDITOR,

    // The order is free (screens are named, not numbered, everywhere) and new ones are appended.
    MIDI,       // reached from PROJECT > MIDI; not on the R+DPAD grid
    MIDI_MAP    // …and from MIDI > MAPPING. Not on the grid either
};

inline const char* screen_label(ScreenType s) {
    switch (s) {
        case ScreenType::SONG:          return "SONG";
        case ScreenType::CHAIN:         return "CHAIN";
        case ScreenType::PHRASE:        return "PHRASE";
        case ScreenType::INSTRUMENT:    return "INSTRUMENT";
        case ScreenType::TABLE:         return "TABLE";
        case ScreenType::PROJECT:       return "PROJECT";
        case ScreenType::GROOVE:        return "GROOVE";
        case ScreenType::SCALE:         return "SCALE";
        case ScreenType::MODS:          return "MODS";
        case ScreenType::INST_POOL:     return "INST.POOL";
        case ScreenType::MIXER:         return "MIXER";
        case ScreenType::EFFECTS:       return "EFFECTS";
        case ScreenType::FILE_BROWSER:  return "FILE BROWSER";
        case ScreenType::SETTINGS:      return "SETTINGS";
        case ScreenType::SAMPLE_EDITOR: return "SAMPLE EDITOR";
        case ScreenType::MIDI:          return "MIDI";
        case ScreenType::MIDI_MAP:      return "MIDI MAPPING";
    }
    return "";
}

/**
 * The navigation map's cell label. Deliberately NOT unique (SCALE and SONG are both "S"): the map draws
 * a screen only in its own column, which disambiguates. One char, to fit the 23 px cell.
 */
inline const char* screen_short_label(ScreenType s) {
    switch (s) {
        case ScreenType::SONG:          return "S";
        case ScreenType::CHAIN:         return "C";
        case ScreenType::PHRASE:        return "P";
        case ScreenType::INSTRUMENT:    return "I";
        case ScreenType::TABLE:         return "T";
        case ScreenType::PROJECT:       return "P";
        case ScreenType::GROOVE:        return "G";
        case ScreenType::SCALE:         return "S";
        case ScreenType::MODS:          return "M";
        case ScreenType::INST_POOL:     return "P";
        case ScreenType::MIXER:         return "V";
        case ScreenType::EFFECTS:       return "X";
        case ScreenType::FILE_BROWSER:  return "FB";
        case ScreenType::SETTINGS:      return "SE";
        case ScreenType::SAMPLE_EDITOR: return "SE";
        case ScreenType::MIDI:          return "MI";
        case ScreenType::MIDI_MAP:      return "MM";
    }
    return "";
}

/** The always-visible middle row of the navigation map. */
inline constexpr ScreenType MAIN_ROW_SCREENS[] = {ScreenType::SONG, ScreenType::CHAIN,
                                                  ScreenType::PHRASE, ScreenType::INSTRUMENT,
                                                  ScreenType::TABLE};

}  // namespace pt::ui
