#pragma once

// ─── Screens ─────────────────────────────────────────────────────────────────────────────────────
//
// A 1:1 port of core/data/ScreenType.kt. The labels are drawn (screen headers, the navigation map),
// so they are part of the UI's contract, not decoration.

#include <string>

namespace pt::ui {

enum class ScreenType {
    // Main screens (middle row — always visible)
    SONG,
    CHAIN,
    PHRASE,
    INSTRUMENT,
    TABLE,

    // Context screens — appear in specific columns
    PROJECT,    // top of all columns
    GROOVE,     // row 2, phrase column
    SCALE,      // row 1, phrase column
    MODS,       // row 2, instrument column
    INST_POOL,  // row 1, instrument column
    MIXER,      // row 4, all columns
    EFFECTS,    // row 5, all columns

    // Popup screens — replace the main view temporarily
    FILE_BROWSER,
    SETTINGS,
    SAMPLE_EDITOR
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
    }
    return "";
}

/**
 * The navigation map's cell label — NavigationMapModule.getScreenLabel().
 *
 * Deliberately NOT unique: SCALE and SONG are both "S", and PROJECT / PHRASE / INST_POOL are all "P".
 * The cell's COLUMN disambiguates it (the map only ever draws a screen in its own column), so the
 * label is a mnemonic, not an identifier — and it must stay one char wide to fit the 23px cell.
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
    }
    return "";
}

/** The always-visible middle row of the navigation map. */
inline constexpr ScreenType MAIN_ROW_SCREENS[] = {ScreenType::SONG, ScreenType::CHAIN,
                                                  ScreenType::PHRASE, ScreenType::INSTRUMENT,
                                                  ScreenType::TABLE};

}  // namespace pt::ui
