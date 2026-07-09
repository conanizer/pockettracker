package com.conanizer.pockettracker.core.data

// Define all available screens in the tracker
enum class ScreenType(val label: String, val shortLabel: String) {
    // Main screens (middle row - always visible)
    SONG("SONG", "S"),
    CHAIN("CHAIN", "C"),
    PHRASE("PHRASE", "P"),
    INSTRUMENT("INSTRUMENT", "I"),
    TABLE("TABLE", "T"),

    // Context screens - appear in specific columns
    PROJECT("PROJECT", "P"),      // Top of all columns
    GROOVE("GROOVE", "G"),        // Row 2, Phrase column
    SCALE("SCALE", "SC"),         // Row 1, Phrase column (when on Phrase)
    MODS("MODS", "M"),           // Row 2, Instrument column
    INST_POOL("INST.POOL", "IP"), // Row 1, Instrument column (when on Instrument)
    MIXER("MIXER", "V"),         // Row 4, all columns
    EFFECTS("EFFECTS", "X"),     // Row 5, all columns

    // Popup screens - replace main view temporarily
    FILE_BROWSER("FILE BROWSER", "FB"),  // File selection popup
    SETTINGS("SETTINGS", "SE"),          // Settings side menu (opened from PROJECT screen)
    SAMPLE_EDITOR("SAMPLE EDITOR", "SE") // Full-screen waveform editor (opened from INSTRUMENT)
}

// Main row screens (always visible at row 3)
val MAIN_ROW_SCREENS = listOf(
    ScreenType.SONG,
    ScreenType.CHAIN,
    ScreenType.PHRASE,
    ScreenType.INSTRUMENT,
    ScreenType.TABLE
)