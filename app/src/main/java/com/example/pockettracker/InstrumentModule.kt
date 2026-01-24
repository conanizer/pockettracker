package com.example.pockettracker

import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.geometry.Size
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.drawscope.DrawScope
import com.example.pockettracker.core.data.Instrument
import com.example.pockettracker.core.data.Note

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

        // Debug logs removed - they were spamming logcat on every frame (60+ fps)
        // Use breakpoints or event-specific logging instead

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
        val valueColumnX = x + 150    // Right side: parameter values

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
        // ROW 3: ROOT + VOL (dual parameter row)
        // ─────────────────────────────────────
        val volHex = instrument.volume.toString(16).padStart(2, '0').uppercase()
        drawDualParameterRow(
            x = x,
            y = rowY,
            scale = scale,
            nameColumnX = nameColumnX,
            valueColumnX = valueColumnX,
            param1Name = "ROOT",
            param1Value = instrument.root.toString(),
            param2Name = "VOL",
            param2Value = volHex,
            cursorRow = instrumentState.cursorRow,
            cursorColumn = instrumentState.cursorColumn,
            currentRow = currentRow
        )
        rowY += ROW_HEIGHT
        currentRow++

        // ─────────────────────────────────────
        // ROW 4: DETUNE + PAN (dual parameter row)
        // ─────────────────────────────────────
        val detuneHex = instrument.detune.toString(16).padStart(2, '0').uppercase()
        val panHex = instrument.pan.toString(16).padStart(2, '0').uppercase()
        drawDualParameterRow(
            x = x,
            y = rowY,
            scale = scale,
            nameColumnX = nameColumnX,
            valueColumnX = valueColumnX,
            param1Name = "DETUNE",
            param1Value = detuneHex,
            param2Name = "PAN",
            param2Value = panHex,
            cursorRow = instrumentState.cursorRow,
            cursorColumn = instrumentState.cursorColumn,
            currentRow = currentRow
        )
        rowY += ROW_HEIGHT
        currentRow++

        // ─────────────────────────────────────
        // ROW 5: SPACER (empty row)
        // ─────────────────────────────────────
        rowY += ROW_HEIGHT
        currentRow++

        // ─────────────────────────────────────
        // ROW 6: DRIVE + FILTER (dual parameter row)
        // ─────────────────────────────────────
        val driveHex = instrument.drive.toString(16).padStart(2, '0').uppercase()
        drawDualParameterRow(
            x = x,
            y = rowY,
            scale = scale,
            nameColumnX = nameColumnX,
            valueColumnX = valueColumnX,
            param1Name = "DRIVE",
            param1Value = driveHex,
            param2Name = "FILTER",
            param2Value = instrument.filterType,
            cursorRow = instrumentState.cursorRow,
            cursorColumn = instrumentState.cursorColumn,
            currentRow = currentRow
        )
        rowY += ROW_HEIGHT
        currentRow++

        // ─────────────────────────────────────
        // ROW 7: CRUSH + CUT (dual parameter row)
        // ─────────────────────────────────────
        val crushHex = instrument.crush.toString(16).uppercase()
        val cutHex = instrument.filterCut.toString(16).padStart(2, '0').uppercase()
        drawDualParameterRow(
            x = x,
            y = rowY,
            scale = scale,
            nameColumnX = nameColumnX,
            valueColumnX = valueColumnX,
            param1Name = "CRUSH",
            param1Value = crushHex,
            param2Name = "CUT",
            param2Value = cutHex,
            cursorRow = instrumentState.cursorRow,
            cursorColumn = instrumentState.cursorColumn,
            currentRow = currentRow
        )
        rowY += ROW_HEIGHT
        currentRow++

        // ─────────────────────────────────────
        // ROW 8: DWNSMPL + RES (dual parameter row)
        // ─────────────────────────────────────
        val downsampleHex = instrument.downsample.toString(16).uppercase()
        val resHex = instrument.filterRes.toString(16).padStart(2, '0').uppercase()
        drawDualParameterRow(
            x = x,
            y = rowY,
            scale = scale,
            nameColumnX = nameColumnX,
            valueColumnX = valueColumnX,
            param1Name = "DWNSMPL",
            param1Value = downsampleHex,
            param2Name = "RES",
            param2Value = resHex,
            cursorRow = instrumentState.cursorRow,
            cursorColumn = instrumentState.cursorColumn,
            currentRow = currentRow
        )
        rowY += ROW_HEIGHT
        currentRow++

        // ─────────────────────────────────────
        // ROW 9: SPACER (empty row)
        // ─────────────────────────────────────
        rowY += ROW_HEIGHT
        currentRow++

        // ─────────────────────────────────────
        // ROW 10: START (sample start point)
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
        // ROW 11: END (sample end point)
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
        // ROW 12: REV (reverse: off/on)
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
        // ROW 13: LOOP (loop mode: off/fwd/png)
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
        // ROW 14: LOOP ST (loop start point)
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
     * Draw a dual-parameter row (two parameters side-by-side)
     * Example: DRIVE    80    FILTER   lp
     * Columns: 0=name1, 1=value1, 2=name2, 3=value2
     */
    private fun DrawScope.drawDualParameterRow(
        x: Int,
        y: Int,
        scale: Int,
        nameColumnX: Int,
        valueColumnX: Int,
        param1Name: String,
        param1Value: String,
        param2Name: String,
        param2Value: String,
        cursorRow: Int,
        cursorColumn: Int,
        currentRow: Int
    ) {
        val textY = y + TEXT_PADDING
        val isCursorOnThisRow = cursorRow == currentRow

        // Draw row background if cursor is on this row
        if (isCursorOnThisRow) {
            drawRect(
                color = Color(0xFF333333),  // Dark gray background
                topLeft = Offset((x * scale).toFloat(), (y * scale).toFloat()),
                size = Size((width * scale).toFloat(), (ROW_HEIGHT * scale).toFloat())
            )
        }

        // COLUMN 0: First parameter name (left side)
        // Yellow only if cursor is on its value (column 1)
        drawBitmapText(
            text = param1Name,
            x = nameColumnX,
            y = textY,
            scale = scale,
            color = if (cursorRow == currentRow && cursorColumn == 1)
                Color.Yellow else Color.Gray,
            spacing = CHAR_SPACING,
            fontScale = FONT_SCALE
        )

        // COLUMN 1: First parameter value
        drawBitmapText(
            text = param1Value,
            x = valueColumnX,
            y = textY,
            scale = scale,
            color = if (cursorRow == currentRow && cursorColumn == 1)
                Color.Yellow else Color.White,
            spacing = CHAR_SPACING,
            fontScale = FONT_SCALE
        )

        // COLUMN 2: Second parameter name (right side, offset by 230px)
        // Yellow only if cursor is on its value (column 3)
        val name2ColumnX = nameColumnX + 230
        drawBitmapText(
            text = param2Name,
            x = name2ColumnX,
            y = textY,
            scale = scale,
            color = if (cursorRow == currentRow && cursorColumn == 3)
                Color.Yellow else Color.Gray,
            spacing = CHAR_SPACING,
            fontScale = FONT_SCALE
        )

        // COLUMN 3: Second parameter value (offset by 220px from first value)
        val value2ColumnX = valueColumnX + 220
        drawBitmapText(
            text = param2Value,
            x = value2ColumnX,
            y = textY,
            scale = scale,
            color = if (cursorRow == currentRow && cursorColumn == 3)
                Color.Yellow else Color.White,
            spacing = CHAR_SPACING,
            fontScale = FONT_SCALE
        )
    }

    /**
     * Draw NAME row showing loaded sample filename (read-only)
     * Example: NAME
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
     *
     * Row layout:
     * - Row 0: TYPE (read-only)
     * - Row 1: LOAD (button)
     * - Row 2: NAME (read-only)
     * - Row 3: ROOT + VOL (dual: cols 0=name, 1=ROOT, 2=name, 3=VOL)
     * - Row 4: DETUNE + PAN (dual: cols 0=name, 1=DETUNE, 2=name, 3=PAN)
     * - Row 5: SPACER
     * - Row 6: DRIVE + FILTER (dual)
     * - Row 7: CRUSH + CUT (dual)
     * - Row 8: DWNSMPL + RES (dual)
     * - Row 9: SPACER
     * - Row 10: START
     * - Row 11: END
     * - Row 12: REV
     * - Row 13: LOOP
     * - Row 14: LOOP ST
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
                // ROOT + VOL row (columns: 0=name, 1=ROOT, 2=name, 3=VOL)
                when (state.cursorColumn) {
                    0, 2 -> return CursorContextFactory.readOnly()  // Parameter names
                    1 -> {  // ROOT value
                        val isEmpty = state.instrument.root == Note.EMPTY
                        val currentValue = if (isEmpty) 0 else state.instrument.root.toMidi()
                        return CursorContextFactory.note(currentValue, isEmpty)
                    }
                    3 -> return CursorContextFactory.hexByte(  // VOL value
                        currentValue = state.instrument.volume,
                        min = 0,
                        max = 255
                    )
                    else -> return CursorContextFactory.none()
                }
            }
            4 -> {
                // DETUNE + PAN row (columns: 0=name, 1=DETUNE, 2=name, 3=PAN)
                when (state.cursorColumn) {
                    0, 2 -> return CursorContextFactory.readOnly()  // Parameter names
                    1 -> return CursorContextFactory.hexByte(  // DETUNE value
                        currentValue = state.instrument.detune,
                        min = 0,
                        max = 255
                    )
                    3 -> return CursorContextFactory.hexByte(  // PAN value
                        currentValue = state.instrument.pan,
                        min = 0,
                        max = 255
                    )
                    else -> return CursorContextFactory.none()
                }
            }
            5 -> {
                // SPACER row - no editing
                return CursorContextFactory.none()
            }
            6 -> {
                // DRIVE + FILTER row (columns: 0=name, 1=drive, 2=name, 3=filter)
                when (state.cursorColumn) {
                    0, 2 -> return CursorContextFactory.readOnly()  // Parameter names
                    1 -> return CursorContextFactory.hexByte(  // DRIVE value
                        currentValue = state.instrument.drive,
                        min = 0,
                        max = 255
                    )
                    3 -> {  // FILTER type
                        val filterTypes = listOf("off", "lp", "hp", "bp")
                        return CursorContextFactory.toggleTernary(state.instrument.filterType, filterTypes)
                    }
                    else -> return CursorContextFactory.none()
                }
            }
            7 -> {
                // CRUSH + CUT row (columns: 0=name, 1=crush, 2=name, 3=cut)
                when (state.cursorColumn) {
                    0, 2 -> return CursorContextFactory.readOnly()  // Parameter names
                    1 -> return CursorContextFactory.hexNibble(  // CRUSH value
                        currentValue = state.instrument.crush,

                    )
                    3 -> return CursorContextFactory.hexByte(  // CUT value
                        currentValue = state.instrument.filterCut,
                        min = 0,
                        max = 255
                    )
                    else -> return CursorContextFactory.none()
                }
            }
            8 -> {
                // DWNSMPL + RES row (columns: 0=name, 1=downsample, 2=name, 3=res)
                when (state.cursorColumn) {
                    0, 2 -> return CursorContextFactory.readOnly()  // Parameter names
                    1 -> return CursorContextFactory.hexNibble(  // DWNSMPL value
                        currentValue = state.instrument.downsample,
                    )
                    3 -> return CursorContextFactory.hexByte(  // RES value
                        currentValue = state.instrument.filterRes,
                        min = 0,
                        max = 255
                    )
                    else -> return CursorContextFactory.none()
                }
            }
            9 -> {
                // SPACER row - no editing
                return CursorContextFactory.none()
            }
            10 -> {
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
            11 -> {
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
            12 -> {
                // REV row - binary toggle (off/on)
                if (state.cursorColumn == 0) {
                    return CursorContextFactory.readOnly()
                }
                return CursorContextFactory.toggleBinary(state.instrument.reverse)
            }
            13 -> {
                // LOOP row - ternary toggle (off/fwd/png)
                if (state.cursorColumn == 0) {
                    return CursorContextFactory.readOnly()
                }
                val loopModes = listOf("off", "fwd", "png")
                return CursorContextFactory.toggleTernary(state.instrument.loopMode, loopModes)
            }
            14 -> {
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

    /**
     * Handle input action for instrument screen.
     *
     * Uses InstrumentController for all business logic (proper UI/logic separation).
     */
    fun handleInput(
        state: InstrumentState,
        action: com.example.pockettracker.core.logic.InputAction,
        instrumentController: com.example.pockettracker.core.logic.InstrumentController
    ): InputResult {
        when (state.cursorRow) {
            0 -> {
                // TYPE row - read-only for now (TODO: A+DPAD to change type)
            }
            1 -> {
                // LOAD row - handled as button action, not value editing
            }
            2 -> {
                // NAME row - read-only (shows loaded sample filename)
            }
            3 -> {
                // ROOT + VOL row (columns: 0=name, 1=ROOT, 2=name, 3=VOL)
                when (state.cursorColumn) {
                    1 -> {  // ROOT value
                        when (action) {
                            is com.example.pockettracker.core.logic.InputAction.SET_VALUE -> {
                                instrumentController.updateRoot(state.instrument, Note.fromMidi(action.value))
                            }
                            is com.example.pockettracker.core.logic.InputAction.DELETE -> {
                                instrumentController.updateRoot(state.instrument, Note.fromString("C-4"))
                            }
                            else -> {}
                        }
                    }
                    3 -> {  // VOL value
                        when (action) {
                            is com.example.pockettracker.core.logic.InputAction.SET_VALUE -> {
                                instrumentController.updateVolume(state.instrument, action.value)
                            }
                            else -> {}
                        }
                    }
                }
            }
            4 -> {
                // DETUNE + PAN row (columns: 0=name, 1=DETUNE, 2=name, 3=PAN)
                when (state.cursorColumn) {
                    1 -> {  // DETUNE value
                        when (action) {
                            is com.example.pockettracker.core.logic.InputAction.SET_VALUE -> {
                                instrumentController.updateDetune(state.instrument, action.value)
                            }
                            else -> {}
                        }
                    }
                    3 -> {  // PAN value
                        when (action) {
                            is com.example.pockettracker.core.logic.InputAction.SET_VALUE -> {
                                instrumentController.updatePan(state.instrument, action.value)
                            }
                            else -> {}
                        }
                    }
                }
            }
            5 -> {
                // SPACER row - no editing
            }
            6 -> {
                // DRIVE + FILTER row (columns: 0=name, 1=drive, 2=name, 3=filter)
                when (state.cursorColumn) {
                    1 -> {  // DRIVE value
                        when (action) {
                            is com.example.pockettracker.core.logic.InputAction.SET_VALUE -> {
                                instrumentController.updateDrive(state.instrument, action.value)
                            }
                            else -> {}
                        }
                    }
                    3 -> {  // FILTER type
                        when (action) {
                            is com.example.pockettracker.core.logic.InputAction.SET_VALUE -> {
                                val filterTypes = listOf("off", "lp", "hp", "bp")
                                if (action.value in 0..3) {
                                    instrumentController.updateFilterType(state.instrument, filterTypes[action.value])
                                }
                            }
                            else -> {}
                        }
                    }
                }
            }
            7 -> {
                // CRUSH + CUT row (columns: 0=name, 1=crush, 2=name, 3=cut)
                when (state.cursorColumn) {
                    1 -> {  // CRUSH value
                        when (action) {
                            is com.example.pockettracker.core.logic.InputAction.SET_VALUE -> {
                                instrumentController.updateCrush(state.instrument, action.value)
                            }
                            else -> {}
                        }
                    }
                    3 -> {  // CUT value
                        when (action) {
                            is com.example.pockettracker.core.logic.InputAction.SET_VALUE -> {
                                instrumentController.updateFilterCut(state.instrument, action.value)
                            }
                            else -> {}
                        }
                    }
                }
            }
            8 -> {
                // DWNSMPL + RES row (columns: 0=name, 1=downsample, 2=name, 3=res)
                when (state.cursorColumn) {
                    1 -> {  // DWNSMPL value
                        when (action) {
                            is com.example.pockettracker.core.logic.InputAction.SET_VALUE -> {
                                instrumentController.updateDownsample(state.instrument, action.value)
                            }
                            else -> {}
                        }
                    }
                    3 -> {  // RES value
                        when (action) {
                            is com.example.pockettracker.core.logic.InputAction.SET_VALUE -> {
                                instrumentController.updateFilterRes(state.instrument, action.value)
                            }
                            else -> {}
                        }
                    }
                }
            }
            9 -> {
                // SPACER row - no editing
            }
            10 -> {
                // START (sample start point)
                when (action) {
                    is com.example.pockettracker.core.logic.InputAction.SET_VALUE -> {
                        instrumentController.updateSampleStart(state.instrument, action.value)
                    }
                    else -> {}
                }
            }
            11 -> {
                // END (sample end point)
                when (action) {
                    is com.example.pockettracker.core.logic.InputAction.SET_VALUE -> {
                        instrumentController.updateSampleEnd(state.instrument, action.value)
                    }
                    else -> {}
                }
            }
            12 -> {
                // REV (reverse: off/on)
                when (action) {
                    is com.example.pockettracker.core.logic.InputAction.SET_VALUE -> {
                        instrumentController.updateReverse(state.instrument, action.value == 1)
                    }
                    else -> {}
                }
            }
            13 -> {
                // LOOP mode (off/fwd/png)
                when (action) {
                    is com.example.pockettracker.core.logic.InputAction.SET_VALUE -> {
                        val loopModes = listOf("off", "fwd", "png")
                        if (action.value in 0..2) {
                            instrumentController.updateLoopMode(state.instrument, loopModes[action.value])
                        }
                    }
                    else -> {}
                }
            }
            14 -> {
                // LOOP ST (loop start point)
                when (action) {
                    is com.example.pockettracker.core.logic.InputAction.SET_VALUE -> {
                        instrumentController.updateLoopStart(state.instrument, action.value)
                    }
                    else -> {}
                }
            }
        }

        return InputResult(
            modified = action !is com.example.pockettracker.core.logic.InputAction.NONE
        )
    }

    data class InputResult(
        val modified: Boolean
    )
}

/**
 * STATE DATA FOR INSTRUMENT SCREEN
 *
 * @param instrument The instrument being edited
 * @param cursorRow Which row:
 *   - 0=TYPE, 1=LOAD, 2=NAME
 *   - 3=ROOT+VOL, 4=DETUNE+PAN, 5=SPACER
 *   - 6=DRIVE+FILTER, 7=CRUSH+CUT, 8=DWNSMPL+RES, 9=SPACER
 *   - 10=START, 11=END, 12=REV, 13=LOOP, 14=LOOP ST
 * @param cursorColumn Which column:
 *   - 0 = Parameter name (left column)
 *   - 1 = Value 1 (for dual-param rows)
 *   - 2 = Name 2 (for dual-param rows)
 *   - 3 = Value 2 (for dual-param rows)
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
