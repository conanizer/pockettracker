package com.conanizer.pockettracker.ui.modules

import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.geometry.Size
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.drawscope.DrawScope
import com.conanizer.pockettracker.ui.theme.AppTheme
import com.conanizer.pockettracker.input.CursorContext
import com.conanizer.pockettracker.input.CursorContextFactory
import com.conanizer.pockettracker.ui.TrackerModule
import com.conanizer.pockettracker.core.data.Groove
import com.conanizer.pockettracker.core.logic.InputAction
import com.conanizer.pockettracker.ui.CHAR_SPACING
import com.conanizer.pockettracker.ui.FONT_SCALE
import com.conanizer.pockettracker.ui.ROW_HEIGHT
import com.conanizer.pockettracker.ui.TEXT_PADDING
import com.conanizer.pockettracker.ui.drawBitmapText
import com.conanizer.pockettracker.ui.toHex1
import com.conanizer.pockettracker.ui.toHex2

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
    val cursorColumn: Int = 1,  // 0=step(RO), 1=tick value
    val appTheme: AppTheme = AppTheme.Companion.CLASSIC
)

class GrooveModule : TrackerModule {
    override val width = 510
    override val height = 392


    override fun DrawScope.draw(x: Int, y: Int, scale: Int, state: Any?) {
        val grooveState = state as? GrooveState ?: return
        val groove = grooveState.groove
        val t = grooveState.appTheme

        // Module background
        drawRect(
            color = Color(t.background),
            topLeft = Offset((x * scale).toFloat(), (y * scale).toFloat()),
            size = Size((width * scale).toFloat(), (height * scale).toFloat())
        )

        // Column X positions (relative to module x)
        val stepX = x + 10        // Step number column (1 hex char)
        val tickX = x + 10 + 40  // Tick value column (2 hex chars)

        // ─── ROW 0: HEADER ───
        val headerY = y + TEXT_PADDING
        val grooveIdStr = groove.id.toHex2()
        val activeLen = groove.activeLength()

        drawBitmapText(
            text = "GROOVE $grooveIdStr",
            x = x + 10,
            y = headerY,
            scale = scale,
            color = Color(t.textTitle),
            spacing = CHAR_SPACING,
            fontScale = FONT_SCALE
        )
        drawBitmapText(
            text = "LEN:${activeLen.toString().padStart(2, ' ')}",
            x = x + width - 130,
            y = headerY,
            scale = scale,
            color = Color(t.textParam),
            spacing = CHAR_SPACING,
            fontScale = FONT_SCALE
        )

        // ─── ROW 1: COLUMN HEADERS ───
        val colHeaderY = y + ROW_HEIGHT + 14 + TEXT_PADDING
        drawBitmapText("TIC", tickX, colHeaderY, scale, Color(t.textParam), CHAR_SPACING, FONT_SCALE)

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
                    color = Color(t.rowCursor),
                    topLeft = Offset(
                        (x * scale).toFloat(),
                        ((rowY - TEXT_PADDING) * scale).toFloat()
                    ),
                    size = Size((width * scale).toFloat(), (ROW_HEIGHT * scale).toFloat())
                )
            }

            // Step number (read-only)
            val stepColor = if (isCursor) Color(t.textCursor) else Color(t.textEmpty)
            drawBitmapText(
                text = index.toHex1(),
                x = stepX,
                y = rowY,
                scale = scale,
                color = stepColor,
                spacing = CHAR_SPACING,
                fontScale = FONT_SCALE
            )

            // Tick value (-1 = "--", otherwise show hex)
            val tickText = if (isEndMarker) "--" else tickValue.toHex2()
            val tickColor = when {
                isCursor && grooveState.cursorColumn == 1 -> Color(t.textCursor)
                isEndMarker || isPastEnd                  -> Color(t.textEmpty)
                else                                      -> Color(t.textValue)
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
        action: InputAction
    ): InputResult {
        when (state.cursorColumn) {
            1 -> {
                when (action) {
                    is InputAction.SET_VALUE -> {
                        state.groove.steps[state.cursorRow] = action.value.coerceIn(0, 255)
                    }
                    is InputAction.DELETE -> {
                        // A+B: set to -1 (end-of-pattern marker)
                        state.groove.steps[state.cursorRow] = -1
                    }
                    is InputAction.INSERT_DEFAULT -> {
                        // A on empty: insert default tick value (12 = standard TICS_PER_STEP)
                        state.groove.steps[state.cursorRow] = 0x0C
                    }
                    else -> {}
                }
            }
        }
        return InputResult(
            modified = action !is InputAction.NONE
        )
    }

    data class InputResult(val modified: Boolean)
}
