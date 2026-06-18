package com.conanizer.pockettracker.ui.modules

import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.geometry.Size
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.drawscope.DrawScope
import com.conanizer.pockettracker.ui.theme.AppTheme
import com.conanizer.pockettracker.input.CursorCapabilities
import com.conanizer.pockettracker.input.CursorContext
import com.conanizer.pockettracker.input.CursorContextFactory
import com.conanizer.pockettracker.input.CursorValueType
import com.conanizer.pockettracker.ui.TrackerModule
import com.conanizer.pockettracker.core.data.Project
import com.conanizer.pockettracker.core.logic.InputAction
import com.conanizer.pockettracker.ui.darken
import com.conanizer.pockettracker.ui.drawBitmapText

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
        val t = projectState.appTheme

        drawRect(
            color = Color(t.background),
            topLeft = Offset((x * scale).toFloat(), (y * scale).toFloat()),
            size = Size((width * scale).toFloat(), (height * scale).toFloat())
        )

        val nameColumnX = x + 10
        val valueColumnX = x + 210

        var rowY = y + TEXT_PADDING

        drawBitmapText(
            text = "PROJECT",
            x = nameColumnX,
            y = rowY,
            scale = scale,
            color = Color(t.textTitle),
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
            isCursorOnValue = projectState.cursorRow == currentRow && projectState.cursorColumn == 1,
            t = t
        )
        rowY += ROW_HEIGHT
        currentRow++

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
            isCursorOnValue = projectState.cursorRow == currentRow && projectState.cursorColumn == 1,
            t = t
        )
        rowY += ROW_HEIGHT * 2  // extra spacer after TRANSPOSE
        currentRow++

        // ─────────────────────────────────────
        // ROW 2: NAME (20 characters, editable per-character)
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
        // ROW 4: EXPORT (WAV MIX button)
        // ─────────────────────────────────────
        drawExportRow(
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
        // ROW 5: COMPACT (SEQ / INST buttons)
        // ─────────────────────────────────────
        drawCleanRow(
            x = x,
            y = rowY,
            scale = scale,
            nameColumnX = nameColumnX,
            valueColumnX = valueColumnX,
            projectState = projectState,
            currentRow = currentRow
        )
        rowY += ROW_HEIGHT * 2  // extra spacer after COMPACT
        currentRow++

        // ─────────────────────────────────────
        // ROW 6: SETTINGS (side menu button)
        // ─────────────────────────────────────
        drawParameterRow(
            x = x,
            y = rowY,
            scale = scale,
            nameColumnX = nameColumnX,
            valueColumnX = valueColumnX,
            parameterName = "SYSTEM",
            parameterValue = "SETTINGS",
            isCursorOnName = projectState.cursorRow == currentRow && projectState.cursorColumn == 0,
            isCursorOnValue = projectState.cursorRow == currentRow && projectState.cursorColumn == 1,
            t = t
        )

        // ─────────────────────────────────────
        // SAMPLE RAM readout — read-only info line (NOT a cursor row), REVIEW-3 5.1.
        // Value = native-heap growth since launch (≈ loaded sample + soundfont PCM), supplied by
        // MainActivity. Integer math (tenths of a MB) avoids locale decimal-separator issues.
        // ─────────────────────────────────────
        rowY += ROW_HEIGHT * 2
        val ramTenths = (projectState.sampleRamBytes * 10 + 524288) / 1048576
        drawBitmapText(
            text = "SAMPLE RAM",
            x = nameColumnX,
            y = rowY + TEXT_PADDING,
            scale = scale,
            color = Color(t.textParam),
            spacing = CHAR_SPACING,
            fontScale = FONT_SCALE
        )
        drawBitmapText(
            text = "${ramTenths / 10}.${ramTenths % 10} MB",
            x = valueColumnX,
            y = rowY + TEXT_PADDING,
            scale = scale,
            color = Color(t.textValue),
            spacing = CHAR_SPACING,
            fontScale = FONT_SCALE
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
                color = if (projectState.isSuccess) Color(t.vizWave) else Color(0xFFff0000),
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
        isCursorOnValue: Boolean,
        t: AppTheme
    ) {
        val textY = y + TEXT_PADDING

        if (isCursorOnName || isCursorOnValue) {
            drawRect(
                color = Color(t.rowCursor),
                topLeft = Offset((x * scale).toFloat(), (y * scale).toFloat()),
                size = Size((width * scale).toFloat(), (ROW_HEIGHT * scale).toFloat())
            )
        }

        drawBitmapText(
            text = parameterName,
            x = nameColumnX,
            y = textY,
            scale = scale,
            color = if (isCursorOnName || isCursorOnValue) Color(t.textCursor) else Color(t.textParam),
            spacing = CHAR_SPACING,
            fontScale = FONT_SCALE
        )

        drawBitmapText(
            text = parameterValue,
            x = valueColumnX,
            y = textY,
            scale = scale,
            color = if (isCursorOnValue) Color(t.textCursor) else Color(t.textValue),
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
        val t = projectState.appTheme
        val textY = y + TEXT_PADDING
        val isCursorOnThisRow = projectState.cursorRow == currentRow

        if (isCursorOnThisRow) {
            drawRect(
                color = Color(t.rowCursor),
                topLeft = Offset((x * scale).toFloat(), (y * scale).toFloat()),
                size = Size((width * scale).toFloat(), (ROW_HEIGHT * scale).toFloat())
            )
        }

        drawBitmapText(
            text = "NAME",
            x = nameColumnX,
            y = textY,
            scale = scale,
            color = if (isCursorOnThisRow) Color(t.textCursor) else Color(t.textParam),
            spacing = CHAR_SPACING,
            fontScale = FONT_SCALE
        )

        val projectName = projectState.project.name.take(20)

        var charX = valueColumnX
        for (charIndex in 0..19) {
            val hasChar = charIndex < projectName.length
            val isCursorOnThisChar = isCursorOnThisRow &&
                    projectState.cursorColumn == charIndex + 1

            if (isCursorOnThisChar) {
                drawRect(
                    color = Color(t.textCursor.darken(0.27f)),
                    topLeft = Offset((charX * scale).toFloat(), (y * scale).toFloat()),
                    size = Size(((5 * FONT_SCALE) * scale).toFloat(), (ROW_HEIGHT * scale).toFloat())
                )
            }

            if (hasChar) {
                drawBitmapText(
                    text = projectName[charIndex].toString(),
                    x = charX,
                    y = textY,
                    scale = scale,
                    color = if (isCursorOnThisChar) Color(t.textCursor) else Color(t.textValue),
                    spacing = CHAR_SPACING,
                    fontScale = FONT_SCALE
                )
            }

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
        val t = projectState.appTheme
        val textY = y + TEXT_PADDING
        val isCursorOnThisRow = projectState.cursorRow == currentRow

        if (isCursorOnThisRow) {
            drawRect(
                color = Color(t.rowCursor),
                topLeft = Offset((x * scale).toFloat(), (y * scale).toFloat()),
                size = Size((width * scale).toFloat(), (ROW_HEIGHT * scale).toFloat())
            )
        }

        drawBitmapText(
            text = "PROJECT",
            x = nameColumnX,
            y = textY,
            scale = scale,
            color = if (isCursorOnThisRow) Color(t.textCursor) else Color(t.textParam),
            spacing = CHAR_SPACING,
            fontScale = FONT_SCALE
        )

        val options = listOf("SAVE", "LOAD", "NEW")
        var optionX = valueColumnX

        for (optionIndex in options.indices) {
            val optionText = options[optionIndex]
            val isCursorOnThisOption = isCursorOnThisRow &&
                    projectState.cursorColumn == optionIndex + 1

            drawBitmapText(
                text = optionText,
                x = optionX,
                y = textY,
                scale = scale,
                color = if (isCursorOnThisOption) Color(t.textCursor) else Color(t.textValue),
                spacing = CHAR_SPACING,
                fontScale = FONT_SCALE
            )
            optionX += 80
        }
    }

    /**
     * Draw EXPORT row with MIX and STEMS buttons.
     * Example: EXPORT   MIX  STEMS
     */
    private fun DrawScope.drawExportRow(
        x: Int,
        y: Int,
        scale: Int,
        nameColumnX: Int,
        valueColumnX: Int,
        projectState: ProjectState,
        currentRow: Int
    ) {
        val t = projectState.appTheme
        val textY = y + TEXT_PADDING
        val isCursorOnThisRow = projectState.cursorRow == currentRow

        if (isCursorOnThisRow) {
            drawRect(
                color = Color(t.rowCursor),
                topLeft = Offset((x * scale).toFloat(), (y * scale).toFloat()),
                size = Size((width * scale).toFloat(), (ROW_HEIGHT * scale).toFloat())
            )
        }

        drawBitmapText(
            text = "EXPORT",
            x = nameColumnX,
            y = textY,
            scale = scale,
            color = if (isCursorOnThisRow) Color(t.textCursor) else Color(t.textParam),
            spacing = CHAR_SPACING,
            fontScale = FONT_SCALE
        )

        val isCursorOnMix   = isCursorOnThisRow && projectState.cursorColumn == 1
        val isCursorOnStems = isCursorOnThisRow && projectState.cursorColumn == 2

        drawBitmapText(
            text = "MIX",
            x = valueColumnX,
            y = textY,
            scale = scale,
            color = if (isCursorOnMix) Color(t.textCursor) else Color(t.textValue),
            spacing = CHAR_SPACING,
            fontScale = FONT_SCALE
        )

        drawBitmapText(
            text = "STEMS",
            x = valueColumnX + 80,
            y = textY,
            scale = scale,
            color = if (isCursorOnStems) Color(t.textCursor) else Color(t.textValue),
            spacing = CHAR_SPACING,
            fontScale = FONT_SCALE
        )

        if (projectState.isRendering) {
            drawBitmapText(
                text = "${(projectState.renderProgress * 100).toInt()}%",
                x = valueColumnX + 170,
                y = textY,
                scale = scale,
                color = Color(t.textTitle),
                spacing = CHAR_SPACING,
                fontScale = FONT_SCALE
            )
        }
    }

    /**
     * Draw COMPACT row with SEQ / INST options
     * Example: COMPACT  SEQ  INST
     */
    private fun DrawScope.drawCleanRow(
        x: Int,
        y: Int,
        scale: Int,
        nameColumnX: Int,
        valueColumnX: Int,
        projectState: ProjectState,
        currentRow: Int
    ) {
        val t = projectState.appTheme
        val textY = y + TEXT_PADDING
        val isCursorOnThisRow = projectState.cursorRow == currentRow

        if (isCursorOnThisRow) {
            drawRect(
                color = Color(t.rowCursor),
                topLeft = Offset((x * scale).toFloat(), (y * scale).toFloat()),
                size = Size((width * scale).toFloat(), (ROW_HEIGHT * scale).toFloat())
            )
        }

        drawBitmapText(
            text = "COMPACT",
            x = nameColumnX,
            y = textY,
            scale = scale,
            color = if (isCursorOnThisRow) Color(t.textCursor) else Color(t.textParam),
            spacing = CHAR_SPACING,
            fontScale = FONT_SCALE
        )

        val options = listOf("SEQ", "INST")
        var optionX = valueColumnX
        for (optionIndex in options.indices) {
            val isCursorOnThis = isCursorOnThisRow && projectState.cursorColumn == optionIndex + 1
            drawBitmapText(
                text = options[optionIndex],
                x = optionX,
                y = textY,
                scale = scale,
                color = if (isCursorOnThis) Color(t.textCursor) else Color(t.textValue),
                spacing = CHAR_SPACING,
                fontScale = FONT_SCALE
            )
            optionX += 80
        }
    }

    /**
     * Get cursor context for current cursor position
     *
     * This tells the generic input system what kind of value we're on
     * and what actions are available.
     */
    fun getCursorContext(state: ProjectState): CursorContext {
        when (state.cursorRow) {
            0 -> {
                // TEMPO row
                if (state.cursorColumn == 0) {
                    // On "TEMPO" label - read only
                    return CursorContextFactory.readOnly()
                }
                // On tempo value (000-999)
                return CursorContext(
                    valueType = CursorValueType.HEX_BYTE,  // Actually decimal, but similar behavior
                    capabilities = CursorCapabilities(
                        canIncrement = true,
                        canDecrement = true,
                        canIncrementFast = true,
                        canDecrementFast = true
                    ),
                    currentValue = state.project.tempo,
                    minValue = 20,
                    maxValue = 999,
                    smallStep = 1,
                    largeStep = 10,
                    emptyValue = -1
                )
            }
            1 -> {
                // TRANSPOSE row
                if (state.cursorColumn == 0) {
                    return CursorContextFactory.readOnly()
                }
                // Transpose value (00-FF): same signed encoding as chain transpose
                return CursorContextFactory.hexByte(
                    currentValue = state.project.transpose,
                    min = 0,
                    max = 255
                ).copy(largeStep = 12)  // A+LEFT/RIGHT = octave jump
            }
            2 -> {
                // NAME row - per-character editing
                if (state.cursorColumn == 0) {
                    return CursorContextFactory.readOnly()
                }
                // Get the character index (column 1 = char 0, column 2 = char 1, etc.)
                val charIndex = state.cursorColumn - 1
                if (charIndex >= 20) {
                    return CursorContextFactory.none()
                }

                // Get current character (or space if beyond name length)
                val currentChar = if (charIndex < state.project.name.length) {
                    state.project.name[charIndex]
                } else {
                    ' '  // Empty slot
                }

                // Use CHARACTER type for text editing
                // Cycles through: A-Z, 0-9, underscore, dash, space
                return CursorContextFactory.character(currentChar)
            }
            3 -> {
                // PROJECT row (LOAD/SAVE/NEW) - not value editing, read-only for now
                return CursorContextFactory.readOnly()
            }
            4 -> {
                // EXPORT row (WAV MIX) - actionable button
                if (state.cursorColumn == 0) {
                    return CursorContextFactory.readOnly()
                }
                // WAV MIX button - can be activated with A button
                return CursorContextFactory.readOnly()  // Action handled in MainActivity
            }
            5 -> {
                // CLEAN row - press A to clean (read-only, action handled in MainActivity)
                return CursorContextFactory.readOnly()
            }
            6 -> {
                // SETTINGS button — A key navigates to SETTINGS screen (handled in MainActivity)
                return CursorContextFactory.readOnly()
            }
            else -> return CursorContextFactory.none()
        }
    }

    /**
     * Handle input action for project screen.
     */
    fun handleInput(
        state: ProjectState,
        action: InputAction
    ): InputResult {
        when (state.cursorRow) {
            0 -> {
                // TEMPO row
                when (action) {
                    is InputAction.SET_VALUE -> {
                        state.project.tempo = action.value.coerceIn(20, 999)
                    }
                    else -> { /* Other actions not applicable */ }
                }
            }
            1 -> {
                // TRANSPOSE row
                when (action) {
                    is InputAction.SET_VALUE -> {
                        state.project.transpose = action.value.coerceIn(0, 255)
                    }
                    else -> { /* Other actions not applicable */ }
                }
            }
            2 -> {
                // NAME row - per-character editing
                val charIndex = state.cursorColumn - 1
                if (charIndex < 0 || charIndex >= 20) return InputResult(modified = false)

                when (action) {
                    is InputAction.SET_VALUE -> {
                        // Set character at position
                        val char = action.value.toChar()
                        val sb = StringBuilder(state.project.name.padEnd(20, ' '))
                        sb.setCharAt(charIndex, char)
                        state.project.name = sb.toString().trimEnd()  // Remove trailing spaces
                    }
                    is InputAction.DELETE -> {
                        // Delete character (replace with space)
                        if (charIndex < state.project.name.length) {
                            val sb = StringBuilder(state.project.name.padEnd(20, ' '))
                            sb.setCharAt(charIndex, ' ')
                            state.project.name = sb.toString().trimEnd()
                        }
                    }
                    else -> { /* Other actions not applicable */ }
                }
            }
            3 -> {
                // PROJECT row (LOAD/SAVE/NEW) - handled elsewhere
            }
        }

        return InputResult(
            modified = action !is InputAction.NONE
        )
    }

    data class InputResult(
        val modified: Boolean
    )
}

/**
 * STATE DATA FOR PROJECT SCREEN
 *
 * @param project The project being edited
 * @param cursorRow Which row (0=TEMPO, 1=TRANSPOSE, 2=NAME, 3=PROJECT, 4=EXPORT, 5=CLEAN, 6=SETTINGS)
 * @param cursorColumn Which column:
 *   - 0 = Parameter name (left column)
 *   - 1+ = Value columns (specific to each row)
 *   For NAME row: 1-20 = character position
 *   For PROJECT row: 1=SAVE, 2=LOAD, 3=NEW
 * @param statusMessage Status message to show at bottom
 * @param isSuccess True if status is success, false if error
 */
data class ProjectState(
    val project: Project,
    val cursorRow: Int = 0,
    val cursorColumn: Int = 1,  // Start on value, not name
    val statusMessage: String = "",
    val isSuccess: Boolean = true,
    val isRendering: Boolean = false,
    val isStemsRendering: Boolean = false,
    val renderProgress: Float = 0f,
    val sampleRamBytes: Long = 0L,
    val appTheme: AppTheme = AppTheme.Companion.CLASSIC
)