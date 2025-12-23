package com.example.pockettracker

import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.geometry.Size
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.drawscope.DrawScope

/**
 * INSTRUMENT SCREEN MODULE
 *
 * Layout pattern follows ProjectModule:
 * - First column: Parameter names (TYPE, LOAD, NAME, etc.)
 * - Second+ columns: Parameter values
 * - Yellow text = cursor highlight (no boxes!)
 * - Grey names, white values when not selected
 *
 * Size: 510×392 pixels (same as other screens)
 */
class InstrumentModule : TrackerModule {
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
        val instrumentState = state as? InstrumentState ?: return
        val instrument = instrumentState.instrument

        android.util.Log.d("InstrumentModule", "Drawing INSTRUMENT screen")
        android.util.Log.d("InstrumentModule", "  Cursor: row=${instrumentState.cursorRow}, col=${instrumentState.cursorColumn}")
        android.util.Log.d("InstrumentModule", "  Instrument: id=${instrument.id}, name=${instrument.name}")

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
        // ===================================
        val nameColumnX = x + 10      // Left side: parameter names
        val valueColumnX = x + 170    // Right side: parameter values

        // ===================================
        // STEP 3: Draw header "INSTRUMENT XX"
        // ===================================
        var rowY = y + TEXT_PADDING

        drawBitmapText(
            text = "INSTRUMENT ${instrument.id.toString(16).padStart(2, '0').uppercase()}",
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
        var currentRow = 0

        // ─────────────────────────────────────
        // ROW 0: TYPE (read-only for now)
        // ─────────────────────────────────────
        drawParameterRow(
            x = x,
            y = rowY,
            scale = scale,
            nameColumnX = nameColumnX,
            valueColumnX = valueColumnX,
            parameterName = "TYPE",
            parameterValue = "sample",  // TODO: Will be changeable with A+DPAD
            isCursorOnName = instrumentState.cursorRow == currentRow && instrumentState.cursorColumn == 0,
            isCursorOnValue = instrumentState.cursorRow == currentRow && instrumentState.cursorColumn == 1
        )
        rowY += ROW_HEIGHT
        currentRow++

        // ─────────────────────────────────────
        // ROW 1: LOAD (button to open file browser)
        // ─────────────────────────────────────
        drawParameterRow(
            x = x,
            y = rowY,
            scale = scale,
            nameColumnX = nameColumnX,
            valueColumnX = valueColumnX,
            parameterName = "LOAD",
            parameterValue = "[LOAD]",
            isCursorOnName = instrumentState.cursorRow == currentRow && instrumentState.cursorColumn == 0,
            isCursorOnValue = instrumentState.cursorRow == currentRow && instrumentState.cursorColumn == 1
        )
        rowY += ROW_HEIGHT
        currentRow++

        // ─────────────────────────────────────
        // ROW 2: NAME (character editing, 12 chars)
        // ─────────────────────────────────────
        drawNameRow(
            x = x,
            y = rowY,
            scale = scale,
            nameColumnX = nameColumnX,
            valueColumnX = valueColumnX,
            instrumentState = instrumentState,
            currentRow = currentRow
        )
        rowY += ROW_HEIGHT
        currentRow++

        // ─────────────────────────────────────
        // ROW 3: ROOT (note value)
        // ─────────────────────────────────────
        drawParameterRow(
            x = x,
            y = rowY,
            scale = scale,
            nameColumnX = nameColumnX,
            valueColumnX = valueColumnX,
            parameterName = "ROOT",
            parameterValue = instrument.root.toString(),  // Shows as "C-4", etc.
            isCursorOnName = instrumentState.cursorRow == currentRow && instrumentState.cursorColumn == 0,
            isCursorOnValue = instrumentState.cursorRow == currentRow && instrumentState.cursorColumn == 1
        )
        rowY += ROW_HEIGHT
        currentRow++

        // ─────────────────────────────────────
        // ROW 4: DETUNE (00-FF hex)
        // ─────────────────────────────────────
        val detuneHex = instrument.detune.toString(16).padStart(2, '0').uppercase()
        drawParameterRow(
            x = x,
            y = rowY,
            scale = scale,
            nameColumnX = nameColumnX,
            valueColumnX = valueColumnX,
            parameterName = "DETUNE",
            parameterValue = detuneHex,
            isCursorOnName = instrumentState.cursorRow == currentRow && instrumentState.cursorColumn == 0,
            isCursorOnValue = instrumentState.cursorRow == currentRow && instrumentState.cursorColumn == 1
        )
        rowY += ROW_HEIGHT
        currentRow++

        // ─────────────────────────────────────
        // ROW 5: SPACER (empty row)
        // ─────────────────────────────────────
        rowY += ROW_HEIGHT
        currentRow++

        // ─────────────────────────────────────
        // ROW 6: START (sample start point)
        // ─────────────────────────────────────
        val startHex = instrument.sampleStart.toString(16).padStart(2, '0').uppercase()
        drawParameterRow(
            x = x,
            y = rowY,
            scale = scale,
            nameColumnX = nameColumnX,
            valueColumnX = valueColumnX,
            parameterName = "START",
            parameterValue = startHex,
            isCursorOnName = instrumentState.cursorRow == currentRow && instrumentState.cursorColumn == 0,
            isCursorOnValue = instrumentState.cursorRow == currentRow && instrumentState.cursorColumn == 1
        )
        rowY += ROW_HEIGHT
        currentRow++

        // ─────────────────────────────────────
        // ROW 7: END (sample end point)
        // ─────────────────────────────────────
        val endHex = instrument.sampleEnd.toString(16).padStart(2, '0').uppercase()
        drawParameterRow(
            x = x,
            y = rowY,
            scale = scale,
            nameColumnX = nameColumnX,
            valueColumnX = valueColumnX,
            parameterName = "END",
            parameterValue = endHex,
            isCursorOnName = instrumentState.cursorRow == currentRow && instrumentState.cursorColumn == 0,
            isCursorOnValue = instrumentState.cursorRow == currentRow && instrumentState.cursorColumn == 1
        )
        rowY += ROW_HEIGHT
        currentRow++

        // ─────────────────────────────────────
        // ROW 8: REV (reverse: off/on)
        // ─────────────────────────────────────
        drawParameterRow(
            x = x,
            y = rowY,
            scale = scale,
            nameColumnX = nameColumnX,
            valueColumnX = valueColumnX,
            parameterName = "REV",
            parameterValue = if (instrument.reverse) "on" else "off",
            isCursorOnName = instrumentState.cursorRow == currentRow && instrumentState.cursorColumn == 0,
            isCursorOnValue = instrumentState.cursorRow == currentRow && instrumentState.cursorColumn == 1
        )
        rowY += ROW_HEIGHT
        currentRow++

        // ─────────────────────────────────────
        // ROW 9: LOOP (loop mode: off/fwd/png)
        // ─────────────────────────────────────
        drawParameterRow(
            x = x,
            y = rowY,
            scale = scale,
            nameColumnX = nameColumnX,
            valueColumnX = valueColumnX,
            parameterName = "LOOP",
            parameterValue = instrument.loopMode,
            isCursorOnName = instrumentState.cursorRow == currentRow && instrumentState.cursorColumn == 0,
            isCursorOnValue = instrumentState.cursorRow == currentRow && instrumentState.cursorColumn == 1
        )
        rowY += ROW_HEIGHT
        currentRow++

        // ─────────────────────────────────────
        // ROW 10: LOOP ST (loop start point)
        // ─────────────────────────────────────
        val loopStartHex = instrument.loopStart.toString(16).padStart(2, '0').uppercase()
        drawParameterRow(
            x = x,
            y = rowY,
            scale = scale,
            nameColumnX = nameColumnX,
            valueColumnX = valueColumnX,
            parameterName = "LOOP ST",
            parameterValue = loopStartHex,
            isCursorOnName = instrumentState.cursorRow == currentRow && instrumentState.cursorColumn == 0,
            isCursorOnValue = instrumentState.cursorRow == currentRow && instrumentState.cursorColumn == 1
        )

        // ===================================
        // STEP 5: Draw status message at bottom (if any)
        // ===================================
        if (instrumentState.statusMessage.isNotEmpty()) {
            val messageY = y + height - 40

            drawBitmapText(
                text = instrumentState.statusMessage,
                x = nameColumnX,
                y = messageY,
                scale = scale,
                color = if (instrumentState.isSuccess)
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
        // Yellow if cursor on row, gray otherwise
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
     * Draw NAME row showing loaded sample filename (read-only)
     * Example: NAME     kick.wav
     */
    private fun DrawScope.drawNameRow(
        x: Int,
        y: Int,
        scale: Int,
        nameColumnX: Int,
        valueColumnX: Int,
        instrumentState: InstrumentState,
        currentRow: Int
    ) {
        val textY = y + TEXT_PADDING
        val isCursorOnThisRow = instrumentState.cursorRow == currentRow

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
            color = if (isCursorOnThisRow) Color.Yellow else Color.Gray,
            spacing = CHAR_SPACING,
            fontScale = FONT_SCALE
        )

        // COLUMN 2: Show loaded sample filename
        val sampleName = if (instrumentState.instrument.sampleFilePath != null) {
            // Extract filename from full path
            val file = java.io.File(instrumentState.instrument.sampleFilePath!!)
            file.name
        } else if (instrumentState.instrument.name.isEmpty()) {
            // Empty/uninitialized instrument
            "empty"
        } else {
            // Show default sample name based on sampleId (12 hardcoded samples)
            when (instrumentState.instrument.sampleId) {
                0 -> "kick.wav"
                1 -> "snare.wav"
                2 -> "hihat.wav"
                3 -> "bass.wav"
                4 -> "shimmer.wav"
                5 -> "tambo.wav"
                6 -> "lofi.wav"
                7 -> "choirstring.wav"
                8 -> "apache162.wav"
                9 -> "copta162.wav"
                10 -> "funky162.wav"
                11 -> "eightoeight.wav"
                else -> "[no sample]"
            }
        }

        drawBitmapText(
            text = sampleName,
            x = valueColumnX,
            y = textY,
            scale = scale,
            color = if (isCursorOnThisRow) Color.Yellow else Color.White,
            spacing = CHAR_SPACING,
            fontScale = FONT_SCALE
        )
    }

    /**
     * Get cursor context for current cursor position
     *
     * This tells the generic input system what kind of value we're on
     * and what actions are available.
     */
    fun getCursorContext(state: InstrumentState): CursorContext {
        when (state.cursorRow) {
            0 -> {
                // TYPE row - read-only for now
                return CursorContextFactory.readOnly()
            }
            1 -> {
                // LOAD row - read-only (handled as button action in MainActivity)
                return CursorContextFactory.readOnly()
            }
            2 -> {
                // NAME row - read-only (shows loaded sample filename)
                return CursorContextFactory.readOnly()
            }
            3 -> {
                // ROOT row - note value
                if (state.cursorColumn == 0) {
                    return CursorContextFactory.readOnly()
                }
                val isEmpty = state.instrument.root == Note.EMPTY
                val currentValue = if (isEmpty) 0 else state.instrument.root.toMidi()
                return CursorContextFactory.note(currentValue, isEmpty)
            }
            4 -> {
                // DETUNE row - hex byte (00-FF)
                if (state.cursorColumn == 0) {
                    return CursorContextFactory.readOnly()
                }
                return CursorContextFactory.hexByte(
                    currentValue = state.instrument.detune,
                    min = 0,
                    max = 255
                )
            }
            5 -> {
                // SPACER row - no editing
                return CursorContextFactory.none()
            }
            6 -> {
                // START row - hex byte (00-FF)
                if (state.cursorColumn == 0) {
                    return CursorContextFactory.readOnly()
                }
                return CursorContextFactory.hexByte(
                    currentValue = state.instrument.sampleStart,
                    min = 0,
                    max = 255
                )
            }
            7 -> {
                // END row - hex byte (00-FF)
                if (state.cursorColumn == 0) {
                    return CursorContextFactory.readOnly()
                }
                return CursorContextFactory.hexByte(
                    currentValue = state.instrument.sampleEnd,
                    min = 0,
                    max = 255
                )
            }
            8 -> {
                // REV row - binary toggle (off/on)
                if (state.cursorColumn == 0) {
                    return CursorContextFactory.readOnly()
                }
                return CursorContextFactory.toggleBinary(state.instrument.reverse)
            }
            9 -> {
                // LOOP row - ternary toggle (off/fwd/png)
                if (state.cursorColumn == 0) {
                    return CursorContextFactory.readOnly()
                }
                val loopModes = listOf("off", "fwd", "png")
                return CursorContextFactory.toggleTernary(state.instrument.loopMode, loopModes)
            }
            10 -> {
                // LOOP ST row - hex byte (00-FF)
                if (state.cursorColumn == 0) {
                    return CursorContextFactory.readOnly()
                }
                return CursorContextFactory.hexByte(
                    currentValue = state.instrument.loopStart,
                    min = 0,
                    max = 255
                )
            }
            else -> return CursorContextFactory.none()
        }
    }
}

/**
 * STATE DATA FOR INSTRUMENT SCREEN
 *
 * @param instrument The instrument being edited
 * @param cursorRow Which row (0=TYPE, 1=LOAD, 2=NAME, 3=ROOT, etc.)
 * @param cursorColumn Which column:
 *   - 0 = Parameter name (left column)
 *   - 1+ = Value columns (specific to each row)
 *   For NAME row: 1-12 = character position
 * @param statusMessage Status message to show at bottom
 * @param isSuccess True if status is success, false if error
 */
data class InstrumentState(
    val instrument: Instrument,
    val cursorRow: Int = 0,
    val cursorColumn: Int = 1,  // Start on value, not name
    val statusMessage: String = "",
    val isSuccess: Boolean = true
)
