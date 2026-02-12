package com.example.pockettracker

import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.geometry.Size
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.drawscope.DrawScope
import com.example.pockettracker.core.data.ScreenType

/**
 * SIMPLIFIED NAVIGATION MAP MODULE
 *
 * Instead of big when() blocks, we use a LOOKUP TABLE
 * This is a "data-driven" approach - the grid structure is DATA, not CODE
 */
class NavigationMapModule : TrackerModule {
    override val width = 115
    override val height = 105

    // ===================================
    // CONSTANTS
    // ===================================
    private val CELL_WIDTH = 23
    private val CELL_HEIGHT = 21
    private val FONT_SCALE = 3
    private val CHAR_SPACING = 2

    // ===================================
    // THE MAGIC: 5x5 GRID TEMPLATE
    // This defines what's in each column
    // null = empty cell (dark)
    // ===================================
    // Each column is defined as Array of 5 rows (indexes 0-4)
    private val COLUMN_LAYOUTS = mapOf(
        // COLUMN 0: Song
        0 to arrayOf(null, ScreenType.PROJECT, ScreenType.SONG, ScreenType.MIXER, ScreenType.EFFECTS),

        // COLUMN 1: Chain
        1 to arrayOf(null, ScreenType.PROJECT, ScreenType.CHAIN, ScreenType.MIXER, ScreenType.EFFECTS),

        // COLUMN 2: Phrase (has 5 screens!)
        2 to arrayOf(ScreenType.SCALE, ScreenType.GROOVE, ScreenType.PHRASE, ScreenType.MIXER, ScreenType.EFFECTS),

        // COLUMN 3: Instrument (has 5 screens!)
        3 to arrayOf(ScreenType.INST_POOL, ScreenType.MODS, ScreenType.INSTRUMENT, ScreenType.MIXER, ScreenType.EFFECTS),

        // COLUMN 4: Table
        4 to arrayOf(null, ScreenType.PROJECT, ScreenType.TABLE, ScreenType.MIXER, ScreenType.EFFECTS)
    )

    override fun DrawScope.draw(x: Int, y: Int, scale: Int, state: Any?) {
        val navState = state as? NavigationMapState ?: return

        // ===================================
        // STEP 1: Draw background
        // ===================================
        drawRect(
            color = Color(0xFF0f0f0f),
            topLeft = Offset((x * scale).toFloat(), (y * scale).toFloat()),
            size = Size((width * scale).toFloat(), (height * scale).toFloat())
        )

        // ===================================
        // STEP 2: Determine current column
        // For shared screens (Project/Mixer/Effects),
        // use sourceColumn to know which column we're in
        // ===================================
        val currentCol = when (navState.currentScreen) {
            ScreenType.SONG -> 0
            ScreenType.CHAIN -> 1
            ScreenType.PHRASE, ScreenType.GROOVE, ScreenType.SCALE -> 2
            ScreenType.INSTRUMENT, ScreenType.MODS, ScreenType.INST_POOL -> 3
            ScreenType.TABLE -> 4
            // Shared screens - use the column we came from
            ScreenType.PROJECT, ScreenType.MIXER, ScreenType.EFFECTS -> navState.sourceColumn
            // Popup screens - use the column we came from
            ScreenType.FILE_BROWSER -> navState.sourceColumn
        }

        // ===================================
        // STEP 3: Build the grid
        // This is the SIMPLE part now!
        // ===================================
        val grid = buildGrid(currentCol)

        // ===================================
        // STEP 4: Draw all cells
        // ===================================
        for (row in 0..4) {
            for (col in 0..4) {
                val screenAtCell = grid[row][col]
                val cellX = x + (col * CELL_WIDTH)
                val cellY = y + (row * CELL_HEIGHT)

                drawNavigationCell(
                    cellX = cellX,
                    cellY = cellY,
                    scale = scale,
                    screen = screenAtCell,
                    isCurrentScreen = (screenAtCell == navState.currentScreen),
                    row = row,
                    col = col
                )
            }
        }
    }

    /**
     * Build the 5x5 grid showing what's visible
     *
     * SIMPLE VERSION: Just copy the current column from our template!
     *
     * @param currentCol Which column (0-4) we're in
     * @return 5x5 grid with nulls for empty cells
     */
    private fun buildGrid(currentCol: Int): Array<Array<ScreenType?>> {
        // ===================================
        // Create empty 5x5 grid (all nulls)
        // ===================================
        val grid = Array(5) { Array<ScreenType?>(5) { null } }

        // ===================================
        // Fill in the MAIN ROW (row 2)
        // This row is ALWAYS visible (S C P I T)
        // ===================================
        grid[2][0] = ScreenType.SONG
        grid[2][1] = ScreenType.CHAIN
        grid[2][2] = ScreenType.PHRASE
        grid[2][3] = ScreenType.INSTRUMENT
        grid[2][4] = ScreenType.TABLE

        // ===================================
        // Fill in the CURRENT COLUMN
        // Get the layout for this column from our lookup table
        // ===================================
        val columnLayout = COLUMN_LAYOUTS[currentCol] ?: COLUMN_LAYOUTS[2]!!  // Default to Phrase column

        // Copy this column's layout into the grid
        for (row in 0..4) {
            grid[row][currentCol] = columnLayout[row]
        }

        return grid
    }

    /**
     * Draw a single cell in the navigation map
     * (This part stays the same as your original)
     */
    private fun DrawScope.drawNavigationCell(
        cellX: Int,
        cellY: Int,
        scale: Int,
        screen: ScreenType?,
        isCurrentScreen: Boolean,
        row: Int,
        col: Int
    ) {
        // ===================================
        // Empty cell - draw very dark background
        // ===================================
        if (screen == null) {
            drawRect(
                color = Color(0xFF0a0a0a),
                topLeft = Offset((cellX * scale).toFloat(), (cellY * scale).toFloat()),
                size = Size((CELL_WIDTH * scale).toFloat(), (CELL_HEIGHT * scale).toFloat())
            )
            return
        }

        // ===================================
        // Cell has a screen - draw it
        // ===================================

        // Background color (darker if current position)
        val bgColor = if (isCurrentScreen) {
            Color(0xFF1a1a1a)  // Darker = you are here
        } else {
            Color(0xFF0f0f0f)  // Normal dark
        }

        drawRect(
            color = bgColor,
            topLeft = Offset((cellX * scale).toFloat(), (cellY * scale).toFloat()),
            size = Size((CELL_WIDTH * scale).toFloat(), (CELL_HEIGHT * scale).toFloat())
        )

        // ===================================
        // Text color - SIMPLE RULE:
        // Yellow = current position
        // White = other visible screens
        // ===================================
        val textColor = if (isCurrentScreen) Color.Yellow else Color.White

        // Get label and draw it
        val label = getScreenLabel(screen)

        // Center the text in the cell
        val labelWidth = (label.length * 5 * FONT_SCALE) + ((label.length - 1) * CHAR_SPACING)
        val textX = cellX + (CELL_WIDTH - labelWidth) / 2
        val textY = cellY + 3

        drawBitmapText(
            text = label,
            x = textX,
            y = textY,
            scale = scale,
            color = textColor,
            spacing = CHAR_SPACING,
            fontScale = FONT_SCALE
        )
    }

    /**
     * Get short label for each screen type
     * (Same as before)
     */
    private fun getScreenLabel(screen: ScreenType): String {
        return when (screen) {
            ScreenType.SONG -> "S"
            ScreenType.CHAIN -> "C"
            ScreenType.PHRASE -> "P"
            ScreenType.INSTRUMENT -> "I"
            ScreenType.TABLE -> "T"
            ScreenType.PROJECT -> "P"
            ScreenType.GROOVE -> "G"
            ScreenType.SCALE -> "S"
            ScreenType.MODS -> "M"
            ScreenType.INST_POOL -> "PI"
            ScreenType.MIXER -> "V"
            ScreenType.EFFECTS -> "X"
            ScreenType.FILE_BROWSER -> "FB"
        }
    }
}

/**
 * State for navigation map
 * (No changes needed here)
 */
data class NavigationMapState(
    val currentScreen: ScreenType,
    val sourceColumn: Int  // Which column we came from (0-4)
)