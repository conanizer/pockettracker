package com.example.pockettracker.core.data

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
    FILE_BROWSER("FILE BROWSER", "FB")  // File selection popup
}

// Navigation grid: 5 columns × 5 rows
// Returns which screen appears at [row, col] for current screen
data class NavigationGrid(
    val grid: Array<Array<ScreenType?>>  // [row][col]
) {
    override fun equals(other: Any?): Boolean {
        if (this === other) return true
        if (javaClass != other?.javaClass) return false
        other as NavigationGrid
        return grid.contentDeepEquals(other.grid)
    }

    override fun hashCode(): Int {
        return grid.contentDeepHashCode()
    }
}

// Main row screens (always visible at row 3)
val MAIN_ROW_SCREENS = listOf(
    ScreenType.SONG,
    ScreenType.CHAIN,
    ScreenType.PHRASE,
    ScreenType.INSTRUMENT,
    ScreenType.TABLE
)

// Get column index for a main screen
fun getColumnIndex(screen: ScreenType): Int {
    return when (screen) {
        ScreenType.SONG -> 0
        ScreenType.CHAIN -> 1
        ScreenType.PHRASE -> 2
        ScreenType.INSTRUMENT -> 3
        ScreenType.TABLE -> 4
        else -> -1  // Context screen
    }
}

// Get navigation grid based on current screen
fun getNavigationGrid(currentScreen: ScreenType): NavigationGrid {
    // Initialize 5×5 grid with nulls
    val grid = Array(5) { Array<ScreenType?>(5) { null } }

    // Row 3 (middle): Always show S C P I T
    grid[2] = arrayOf(
        ScreenType.SONG,
        ScreenType.CHAIN,
        ScreenType.PHRASE,
        ScreenType.INSTRUMENT,
        ScreenType.TABLE
    )

    // Get current column (which main screen we're on or related to)
    val currentCol = when (currentScreen) {
        ScreenType.SONG -> 0
        ScreenType.CHAIN -> 1
        ScreenType.PHRASE, ScreenType.GROOVE, ScreenType.SCALE -> 2
        ScreenType.INSTRUMENT, ScreenType.MODS, ScreenType.INST_POOL -> 3
        ScreenType.TABLE -> 4
        else -> 2  // Default to middle
    }

    // Row 1 (top): Show only when on that specific screen
    when (currentScreen) {
        ScreenType.PHRASE -> grid[0][2] = ScreenType.SCALE
        ScreenType.INSTRUMENT -> grid[0][3] = ScreenType.INST_POOL
        else -> { /* No top screens for other main screens */ }
    }

    // Row 2: Show screens above current column
    grid[1][currentCol] = ScreenType.PROJECT  // Project always on row 2
    when (currentScreen) {
        ScreenType.PHRASE, ScreenType.GROOVE, ScreenType.SCALE -> {
            grid[1][2] = ScreenType.GROOVE
        }
        ScreenType.INSTRUMENT, ScreenType.MODS, ScreenType.INST_POOL -> {
            grid[1][3] = ScreenType.MODS
        }
        else -> {
            grid[1][currentCol] = ScreenType.PROJECT
        }
    }

    // Row 4: Mixer shows in current column (and maybe adjacent)
    grid[3][currentCol] = ScreenType.MIXER

    // Row 5: Effects shows in current column (and maybe adjacent)
    grid[4][currentCol] = ScreenType.EFFECTS

    return NavigationGrid(grid)
}