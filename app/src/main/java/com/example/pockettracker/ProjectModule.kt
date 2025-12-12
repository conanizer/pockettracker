package com.example.pockettracker

import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.geometry.Size
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.drawscope.DrawScope

/**
 * PROJECT SCREEN MODULE - FIXED STYLING
 *
 * Layout exactly like Song/Chain/Phrase screens:
 * - First column: Parameter names (TEMPO, TRANSPOSE, NAME, etc.)
 * - Second column: Parameter values
 * - Yellow text = cursor highlight (no boxes!)
 * - Grey names, white values when not selected
 *
 * Size: 510×392 pixels (same as other screens)
 */
class ProjectModule : TrackerModule {
    // ===================================
    // MODULE DIMENSIONS
    // ===================================
    override val width = 510
    override val height = 392

    // ===================================
    // FONT & LAYOUT CONSTANTS
    // ===================================
    private val FONT_SCALE = 3      // 5×5 bitmap scaled 3× = 15×15 pixels
    private val CHAR_SPACING = 2    // 2px between characters
    private val ROW_HEIGHT = 21     // Each row is 21px tall
    private val TEXT_PADDING = 3    // 3px padding above text

    /**
     * Main draw function
     */
    override fun DrawScope.draw(x: Int, y: Int, scale: Int, state: Any?) {
        val projectState = state as? ProjectState ?: return

        // ===================================
        // STEP 1: Draw background
        // ===================================
        drawRect(
            color = Color(0xFF0a0a0a),
            topLeft = Offset((x * scale).toFloat(), (y * scale).toFloat()),
            size = Size((width * scale).toFloat(), (height * scale).toFloat())
        )

        // ===================================
        // STEP 2: Calculate column positions
        // Column 1: Parameter names (left aligned at x=10)
        // Column 2: Parameter values (starts at x=140)
        // ===================================
        val nameColumnX = x + 10      // Left side: parameter names
        val valueColumnX = x + 160    // Right side: parameter values

        // ===================================
        // STEP 3: Draw header "PROJECT"
        // ===================================
        var rowY = y + TEXT_PADDING

        drawBitmapText(
            text = "PROJECT",
            x = nameColumnX,
            y = rowY,
            scale = scale,
            color = Color.Cyan,  // Header always cyan
            spacing = CHAR_SPACING,
            fontScale = FONT_SCALE
        )

        // Move down (header + spacer)
        rowY += ROW_HEIGHT + 14

        // ===================================
        // STEP 4: Draw parameter rows
        // ===================================

        // Row index for cursor matching
        var currentRow = 0

        // ─────────────────────────────────────
        // ROW 0: TEMPO (000-999, default 120)
        // ─────────────────────────────────────
        drawParameterRow(
            x = x,
            y = rowY,
            scale = scale,
            nameColumnX = nameColumnX,
            valueColumnX = valueColumnX,
            parameterName = "TEMPO",
            parameterValue = projectState.project.tempo.toString().padStart(3, '0'),
            isCursorOnName = projectState.cursorRow == currentRow && projectState.cursorColumn == 0,
            isCursorOnValue = projectState.cursorRow == currentRow && projectState.cursorColumn == 1
        )
        rowY += ROW_HEIGHT
        currentRow++

        // ─────────────────────────────────────
        // ROW 1: TRANSPOSE (00-FF, default 00)
        // ─────────────────────────────────────
        val transposeHex = projectState.project.transpose
            .toString(16)
            .padStart(2, '0')
            .uppercase()

        drawParameterRow(
            x = x,
            y = rowY,
            scale = scale,
            nameColumnX = nameColumnX,
            valueColumnX = valueColumnX,
            parameterName = "TRANSPOSE",
            parameterValue = transposeHex,
            isCursorOnName = projectState.cursorRow == currentRow && projectState.cursorColumn == 0,
            isCursorOnValue = projectState.cursorRow == currentRow && projectState.cursorColumn == 1
        )
        rowY += ROW_HEIGHT
        currentRow++

        // ─────────────────────────────────────
        // ROW 2: NAME (12 characters, editable per-character)
        // ─────────────────────────────────────
        drawNameRow(
            x = x,
            y = rowY,
            scale = scale,
            nameColumnX = nameColumnX,
            valueColumnX = valueColumnX,
            projectState = projectState,
            currentRow = currentRow
        )
        rowY += ROW_HEIGHT
        currentRow++

        // ─────────────────────────────────────
        // ROW 3: PROJECT (LOAD / SAVE / NEW)
        // ─────────────────────────────────────
        drawProjectRow(
            x = x,
            y = rowY,
            scale = scale,
            nameColumnX = nameColumnX,
            valueColumnX = valueColumnX,
            projectState = projectState,
            currentRow = currentRow
        )
        rowY += ROW_HEIGHT
        currentRow++

        // ─────────────────────────────────────
        // ROW 4: EXPORT (placeholder)
        // ─────────────────────────────────────
        drawParameterRow(
            x = x,
            y = rowY,
            scale = scale,
            nameColumnX = nameColumnX,
            valueColumnX = valueColumnX,
            parameterName = "EXPORT",
            parameterValue = "---",
            isCursorOnName = projectState.cursorRow == currentRow && projectState.cursorColumn == 0,
            isCursorOnValue = projectState.cursorRow == currentRow && projectState.cursorColumn == 1
        )
        rowY += ROW_HEIGHT
        currentRow++

        // ─────────────────────────────────────
        // ROW 5: CLEAN (placeholder)
        // ─────────────────────────────────────
        drawParameterRow(
            x = x,
            y = rowY,
            scale = scale,
            nameColumnX = nameColumnX,
            valueColumnX = valueColumnX,
            parameterName = "CLEAN",
            parameterValue = "---",
            isCursorOnName = projectState.cursorRow == currentRow && projectState.cursorColumn == 0,
            isCursorOnValue = projectState.cursorRow == currentRow && projectState.cursorColumn == 1
        )
        rowY += ROW_HEIGHT
        currentRow++

        // ─────────────────────────────────────
        // ROW 6: SYSTEM (placeholder)
        // ─────────────────────────────────────
        drawParameterRow(
            x = x,
            y = rowY,
            scale = scale,
            nameColumnX = nameColumnX,
            valueColumnX = valueColumnX,
            parameterName = "SYSTEM",
            parameterValue = "---",
            isCursorOnName = projectState.cursorRow == currentRow && projectState.cursorColumn == 0,
            isCursorOnValue = projectState.cursorRow == currentRow && projectState.cursorColumn == 1
        )

        // ===================================
        // STEP 5: Draw status message at bottom (if any)
        // ===================================
        if (projectState.statusMessage.isNotEmpty()) {
            val messageY = y + height - 40

            drawBitmapText(
                text = projectState.statusMessage,
                x = nameColumnX,
                y = messageY,
                scale = scale,
                color = if (projectState.isSuccess)
                    Color(0xFF00ff00) else Color(0xFFff0000),
                spacing = CHAR_SPACING,
                fontScale = FONT_SCALE
            )
        }
    }

    /**
     * Draw a simple parameter row (single value)
     * Example: TEMPO    120
     */
    private fun DrawScope.drawParameterRow(
        x: Int,
        y: Int,
        scale: Int,
        nameColumnX: Int,
        valueColumnX: Int,
        parameterName: String,
        parameterValue: String,
        isCursorOnName: Boolean,
        isCursorOnValue: Boolean
    ) {
        // Calculate Y position for text
        val textY = y + TEXT_PADDING

        // Draw row background if cursor is on this row
        if (isCursorOnName || isCursorOnValue) {
            drawRect(
                color = Color(0xFF333333),  // Dark gray background
                topLeft = Offset((x * scale).toFloat(), (y * scale).toFloat()),
                size = Size((width * scale).toFloat(), (ROW_HEIGHT * scale).toFloat())
            )
        }

        // COLUMN 1: Parameter name
        // Yellow if cursor on name, gray otherwise
        drawBitmapText(
            text = parameterName,
            x = nameColumnX,
            y = textY,
            scale = scale,
            color = if (isCursorOnName || isCursorOnValue)
                Color.Yellow else Color.Gray,
            spacing = CHAR_SPACING,
            fontScale = FONT_SCALE
        )

        // COLUMN 2: Parameter value
        // Yellow if cursor on value, white otherwise
        drawBitmapText(
            text = parameterValue,
            x = valueColumnX,
            y = textY,
            scale = scale,
            color = if (isCursorOnValue)
                Color.Yellow else Color.White,
            spacing = CHAR_SPACING,
            fontScale = FONT_SCALE
        )
    }

    /**
     * Draw NAME row with per-character editing
     * Example: NAME     M Y S O N G _ _ _ _ _ _
     */
    private fun DrawScope.drawNameRow(
        x: Int,
        y: Int,
        scale: Int,
        nameColumnX: Int,
        valueColumnX: Int,
        projectState: ProjectState,
        currentRow: Int
    ) {
        val textY = y + TEXT_PADDING
        val isCursorOnThisRow = projectState.cursorRow == currentRow

        // Draw row background if cursor is here
        if (isCursorOnThisRow) {
            drawRect(
                color = Color(0xFF333333),
                topLeft = Offset((x * scale).toFloat(), (y * scale).toFloat()),
                size = Size((width * scale).toFloat(), (ROW_HEIGHT * scale).toFloat())
            )
        }

        // COLUMN 1: "NAME" label
        drawBitmapText(
            text = "NAME",
            x = nameColumnX,
            y = textY,
            scale = scale,
            // Yellow if cursor is anywhere on this row
            color = if (isCursorOnThisRow) Color.Yellow else Color.Gray,
            spacing = CHAR_SPACING,
            fontScale = FONT_SCALE
        )

        // COLUMN 2: Name characters (12 characters max)
        // Pad name to 12 characters with underscores
        val displayName = projectState.project.name
            .take(12)  // Max 12 chars
            .padEnd(12, '_')  // Fill with underscores

        // Draw each character separately
        var charX = valueColumnX
        for (charIndex in 0..11) {
            val char = displayName[charIndex]

            // Is cursor on THIS specific character?
            // cursorColumn: 0=name label, 1=first char, 2=second char, etc.
            val isCursorOnThisChar = isCursorOnThisRow &&
                    projectState.cursorColumn == charIndex + 1

            drawBitmapText(
                text = char.toString(),
                x = charX,
                y = textY,
                scale = scale,
                // Yellow if cursor is on this character, white otherwise
                color = if (isCursorOnThisChar) Color.Yellow else Color.White,
                spacing = CHAR_SPACING,
                fontScale = FONT_SCALE
            )

            // Move to next character position
            // Each char is 5px * 3 (scale) + 2px spacing = 17px
            charX += (5 * FONT_SCALE) + CHAR_SPACING
        }
    }

    /**
     * Draw PROJECT row with LOAD / SAVE / NEW
     * Example: PROJECT  LOAD  SAVE  NEW
     */
    private fun DrawScope.drawProjectRow(
        x: Int,
        y: Int,
        scale: Int,
        nameColumnX: Int,
        valueColumnX: Int,
        projectState: ProjectState,
        currentRow: Int
    ) {
        val textY = y + TEXT_PADDING
        val isCursorOnThisRow = projectState.cursorRow == currentRow

        // Draw row background if cursor is here
        if (isCursorOnThisRow) {
            drawRect(
                color = Color(0xFF333333),
                topLeft = Offset((x * scale).toFloat(), (y * scale).toFloat()),
                size = Size((width * scale).toFloat(), (ROW_HEIGHT * scale).toFloat())
            )
        }

        // COLUMN 1: "PROJECT" label
        drawBitmapText(
            text = "PROJECT",
            x = nameColumnX,
            y = textY,
            scale = scale,
            color = if (isCursorOnThisRow) Color.Yellow else Color.Gray,
            spacing = CHAR_SPACING,
            fontScale = FONT_SCALE
        )

        // COLUMN 2: Options (LOAD, SAVE, NEW)
        val options = listOf("LOAD", "SAVE", "NEW")
        var optionX = valueColumnX

        for (optionIndex in options.indices) {
            val optionText = options[optionIndex]

            // Is cursor on THIS option?
            // cursorColumn: 0=label, 1=LOAD, 2=SAVE, 3=NEW
            val isCursorOnThisOption = isCursorOnThisRow &&
                    projectState.cursorColumn == optionIndex + 1

            drawBitmapText(
                text = optionText,
                x = optionX,
                y = textY,
                scale = scale,
                color = if (isCursorOnThisOption) Color.Yellow else Color.White,
                spacing = CHAR_SPACING,
                fontScale = FONT_SCALE
            )

            // Move to next option position
            // Leave ~40px between options
            optionX += 80
        }
    }
}

/**
 * STATE DATA FOR PROJECT SCREEN
 *
 * @param project The project being edited
 * @param cursorRow Which row (0=TEMPO, 1=TRANSPOSE, 2=NAME, 3=PROJECT, etc.)
 * @param cursorColumn Which column:
 *   - 0 = Parameter name (left column)
 *   - 1+ = Value columns (specific to each row)
 *   For NAME row: 1-12 = character position
 *   For PROJECT row: 1=LOAD, 2=SAVE, 3=NEW
 * @param statusMessage Status message to show at bottom
 * @param isSuccess True if status is success, false if error
 */
data class ProjectState(
    val project: Project,
    val cursorRow: Int = 0,
    val cursorColumn: Int = 1,  // Start on value, not name
    val statusMessage: String = "",
    val isSuccess: Boolean = true
)