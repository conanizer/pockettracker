package com.conanizer.pockettracker

import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.geometry.Size
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.drawscope.DrawScope
import com.conanizer.pockettracker.core.data.Groove

/**
 * GROOVE SCREEN MODULE
 *
 * Displays a 16-step groove pattern where each step defines
 * how many ticks that phrase step takes.
 *
 * Layout:
 *   Header: "GROOVE XX  LEN:NN"
 *   Column headers: "# | TIC"
 *   16 data rows: step number + tick value (hex or "--" for end-marker)
 *
 * Tick value -1 = end of pattern ("--"), loops from row 0.
 * Tick value 0x00 = skip step (phrase row is not triggered, takes no time).
 * Tick value 0x01-0xFF = duration in ticks for that phrase step.
 *
 * Size: 510×392 pixels
 * State type: GrooveState
 */

data class GrooveState(
    val groove: Groove,
    val cursorRow: Int,      // 0-15
    val cursorColumn: Int = 1  // 0=step(RO), 1=tick value
)

class GrooveModule : TrackerModule {
    override val width = 510
    override val height = 392

    private val FONT_SCALE = 3
    private val CHAR_SPACING = 2
    private val ROW_HEIGHT = 21
    private val TEXT_PADDING = 3

    override fun DrawScope.draw(x: Int, y: Int, scale: Int, state: Any?) {
        val grooveState = state as? GrooveState ?: return
        val groove = grooveState.groove

        // Module background
        drawRect(
            color = Color(0xFF0a0a0a),
            topLeft = Offset((x * scale).toFloat(), (y * scale).toFloat()),
            size = Size((width * scale).toFloat(), (height * scale).toFloat())
        )

        // Column X positions (relative to module x)
        val stepX = x + 10        // Step number column (1 hex char)
        val tickX = x + 10 + 40  // Tick value column (2 hex chars)

        // ─── ROW 0: HEADER ───
        val headerY = y + TEXT_PADDING
        val grooveIdStr = groove.id.toString(16).padStart(2, '0').uppercase()
        val activeLen = groove.activeLength()

        drawBitmapText(
            text = "GROOVE $grooveIdStr",
            x = x + 10,
            y = headerY,
            scale = scale,
            color = Color.Cyan,
            spacing = CHAR_SPACING,
            fontScale = FONT_SCALE
        )
        drawBitmapText(
            text = "LEN:${activeLen.toString().padStart(2, ' ')}",
            x = x + width - 100,
            y = headerY,
            scale = scale,
            color = Color.Gray,
            spacing = CHAR_SPACING,
            fontScale = FONT_SCALE
        )

        // ─── ROW 1: COLUMN HEADERS ───
        val colHeaderY = y + ROW_HEIGHT + 14 + TEXT_PADDING
        drawBitmapText("TIC", tickX, colHeaderY, scale, Color.Gray, CHAR_SPACING, FONT_SCALE)

        // ─── ROWS 2-17: 16 DATA ROWS ───
        val dataStartY = y + ROW_HEIGHT + 14 + ROW_HEIGHT + TEXT_PADDING

        for (index in 0..15) {
            val tickValue = groove.steps[index]
            val rowY = dataStartY + (index * ROW_HEIGHT)
            val isCursor = (index == grooveState.cursorRow)
            val isEndMarker = (tickValue == -1)
            // Rows at or after the first -1 are "inactive" (past end of pattern)
            val isPastEnd = (index >= activeLen)

            // Cursor row background highlight
            if (isCursor) {
                drawRect(
                    color = Color(0xFF1a1a1a),
                    topLeft = Offset(
                        (x * scale).toFloat(),
                        ((rowY - TEXT_PADDING) * scale).toFloat()
                    ),
                    size = Size((width * scale).toFloat(), (ROW_HEIGHT * scale).toFloat())
                )
            }

            // Step number (read-only)
            val stepColor = when {
                isCursor -> Color.Yellow
                isPastEnd -> Color(0xFF333333)
                else -> Color(0xFF666666)
            }
            drawBitmapText(
                text = index.toString(16).uppercase(),
                x = stepX,
                y = rowY,
                scale = scale,
                color = stepColor,
                spacing = CHAR_SPACING,
                fontScale = FONT_SCALE
            )

            // Tick value (-1 = "--", otherwise show hex)
            val tickText = if (isEndMarker) "--" else tickValue.toString(16).padStart(2, '0').uppercase()
            val tickColor = when {
                isCursor && grooveState.cursorColumn == 1 -> Color.Yellow
                isEndMarker || isPastEnd -> Color(0xFF333333)
                else -> Color.White
            }
            drawBitmapText(
                text = tickText,
                x = tickX,
                y = rowY,
                scale = scale,
                color = tickColor,
                spacing = CHAR_SPACING,
                fontScale = FONT_SCALE
            )
        }
    }

    /**
     * Get cursor context for the current cursor position.
     */
    fun getCursorContext(state: GrooveState): CursorContext {
        return when (state.cursorColumn) {
            0 -> CursorContextFactory.readOnly()  // Step number
            1 -> {
                val tickValue = state.groove.steps[state.cursorRow]
                CursorContextFactory.hexByte(
                    currentValue = tickValue,
                    min = 0,                // 00 = skip step
                    max = 255,
                    emptyValue = -1,        // -1 = end-of-pattern marker
                    canDelete = tickValue != -1,
                    canInsert = tickValue == -1
                )
            }
            else -> CursorContextFactory.none()
        }
    }

    /**
     * Handle input for groove editing.
     */
    fun handleInput(
        state: GrooveState,
        action: com.conanizer.pockettracker.core.logic.InputAction
    ): InputResult {
        when (state.cursorColumn) {
            1 -> {
                when (action) {
                    is com.conanizer.pockettracker.core.logic.InputAction.SET_VALUE -> {
                        state.groove.steps[state.cursorRow] = action.value.coerceIn(0, 255)
                    }
                    is com.conanizer.pockettracker.core.logic.InputAction.DELETE -> {
                        // A+B: set to -1 (end-of-pattern marker)
                        state.groove.steps[state.cursorRow] = -1
                    }
                    is com.conanizer.pockettracker.core.logic.InputAction.INSERT_DEFAULT -> {
                        // A on empty: insert default tick value (12 = standard TICS_PER_STEP)
                        state.groove.steps[state.cursorRow] = 0x0C
                    }
                    else -> {}
                }
            }
        }
        return InputResult(
            modified = action !is com.conanizer.pockettracker.core.logic.InputAction.NONE
        )
    }

    data class InputResult(val modified: Boolean)
}
