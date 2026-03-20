package com.conanizer.pockettracker

import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.geometry.Size
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.drawscope.DrawScope
import com.conanizer.pockettracker.core.data.Instrument
import com.conanizer.pockettracker.core.data.Note

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
        // ROW 0: TYPE + LOAD button
        // TYPE at columns 0-1, LOAD button at column 3 (value2ColumnX)
        // ─────────────────────────────────────
        drawTypeLoadRow(
            x = x,
            y = rowY,
            scale = scale,
            nameColumnX = nameColumnX,
            valueColumnX = valueColumnX,
            cursorRow = instrumentState.cursorRow,
            cursorColumn = instrumentState.cursorColumn,
            currentRow = currentRow
        )
        rowY += ROW_HEIGHT
        currentRow++

        // ─────────────────────────────────────
        // ROW 1: NAME (shows loaded sample filename)
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
        // ROW 2: ROOT + VOL (dual parameter row)
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
        // ROW 3: DETUNE + PAN (dual parameter row)
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
        // ROW 4: TBL TIC (table tick rate)
        // ─────────────────────────────────────
        val ticHex = instrument.tableTicRate.toString(16).padStart(2, '0').uppercase()
        drawParameterRow(
            x = x,
            y = rowY,
            scale = scale,
            nameColumnX = nameColumnX,
            valueColumnX = valueColumnX,
            parameterName = "TBL TIC",
            parameterValue = ticHex,
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
     * Draw TYPE + LOAD row
     * TYPE at columns 0-1, LOAD button at column 3 (no parameter name in column 2)
     * Columns: 0=TYPE name, 1=TYPE value, 2=empty, 3=LOAD button
     */
    private fun DrawScope.drawTypeLoadRow(
        x: Int,
        y: Int,
        scale: Int,
        nameColumnX: Int,
        valueColumnX: Int,
        cursorRow: Int,
        cursorColumn: Int,
        currentRow: Int
    ) {
        val textY = y + TEXT_PADDING
        val isCursorOnThisRow = cursorRow == currentRow

        // Draw row background if cursor is on this row
        if (isCursorOnThisRow) {
            drawRect(
                color = Color(0xFF333333),
                topLeft = Offset((x * scale).toFloat(), (y * scale).toFloat()),
                size = Size((width * scale).toFloat(), (ROW_HEIGHT * scale).toFloat())
            )
        }

        // COLUMN 0: "TYPE" label
        drawBitmapText(
            text = "TYPE",
            x = nameColumnX,
            y = textY,
            scale = scale,
            color = if (isCursorOnThisRow && cursorColumn == 1) Color.Yellow else Color.Gray,
            spacing = CHAR_SPACING,
            fontScale = FONT_SCALE
        )

        // COLUMN 1: "sample" value (read-only for now)
        drawBitmapText(
            text = "sample",
            x = valueColumnX,
            y = textY,
            scale = scale,
            color = if (isCursorOnThisRow && cursorColumn == 1) Color.Yellow else Color.White,
            spacing = CHAR_SPACING,
            fontScale = FONT_SCALE
        )

        // COLUMN 2: empty (no parameter name)

        // COLUMN 3: LOAD button at value2ColumnX
        val value2ColumnX = valueColumnX + 220
        drawBitmapText(
            text = "LOAD",
            x = value2ColumnX,
            y = textY,
            scale = scale,
            color = if (isCursorOnThisRow && cursorColumn == 3) Color.Yellow else Color.White,
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

        // COLUMN 2: Show loaded sample filename, or "empty" if nothing is loaded
        val sampleName = if (instrumentState.instrument.sampleFilePath != null) {
            val file = java.io.File(instrumentState.instrument.sampleFilePath!!)
            file.name
        } else {
            "empty"
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
     * - Row 0: TYPE (col 1) + LOAD (col 3)
     * - Row 1: NAME (read-only)
     * - Row 2: ROOT + VOL (dual: cols 0=name, 1=ROOT, 2=name, 3=VOL)
     * - Row 3: DETUNE + PAN (dual: cols 0=name, 1=DETUNE, 2=name, 3=PAN)
     * - Row 4: TBL TIC (col 1)
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
                // TYPE + LOAD row (columns: 0=name, 1=TYPE, 2=empty, 3=LOAD)
                when (state.cursorColumn) {
                    0, 2 -> return CursorContextFactory.readOnly()  // Parameter names / empty
                    1 -> return CursorContextFactory.readOnly()  // TYPE value (read-only for now)
                    3 -> return CursorContextFactory.readOnly()  // LOAD button (handled in MainActivity)
                    else -> return CursorContextFactory.none()
                }
            }
            1 -> {
                // NAME row - read-only (shows loaded sample filename)
                return CursorContextFactory.readOnly()
            }
            2 -> {
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
            3 -> {
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
            4 -> {
                // TBL TIC row - hex byte (00-FF)
                if (state.cursorColumn == 0) {
                    return CursorContextFactory.readOnly()
                }
                return CursorContextFactory.hexByte(
                    currentValue = state.instrument.tableTicRate,
                    min = 0,
                    max = 255
                )
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
     *
     * Row layout:
     * - Row 0: TYPE (col 1) + LOAD (col 3)
     * - Row 1: NAME (read-only)
     * - Row 2: ROOT + VOL
     * - Row 3: DETUNE + PAN
     * - Row 4: TBL TIC
     * - Row 5: SPACER
     * - Row 6: DRIVE + FILTER
     * - Row 7: CRUSH + CUT
     * - Row 8: DWNSMPL + RES
     * - Row 9: SPACER
     * - Row 10: START
     * - Row 11: END
     * - Row 12: REV
     * - Row 13: LOOP
     * - Row 14: LOOP ST
     */
    fun handleInput(
        state: InstrumentState,
        action: com.conanizer.pockettracker.core.logic.InputAction,
        instrumentController: com.conanizer.pockettracker.core.logic.InstrumentController
    ): InputResult {
        when (state.cursorRow) {
            0 -> {
                // TYPE + LOAD row
                // Column 1: TYPE (read-only for now)
                // Column 3: LOAD button (handled in MainActivity as button action)
            }
            1 -> {
                // NAME row - read-only (shows loaded sample filename)
            }
            2 -> {
                // ROOT + VOL row (columns: 0=name, 1=ROOT, 2=name, 3=VOL)
                when (state.cursorColumn) {
                    1 -> {  // ROOT value
                        when (action) {
                            is com.conanizer.pockettracker.core.logic.InputAction.SET_VALUE -> {
                                instrumentController.updateRoot(state.instrument, Note.fromMidi(action.value))
                            }
                            is com.conanizer.pockettracker.core.logic.InputAction.DELETE -> {
                                instrumentController.updateRoot(state.instrument, Note.fromString("C-4"))
                            }
                            else -> {}
                        }
                    }
                    3 -> {  // VOL value
                        when (action) {
                            is com.conanizer.pockettracker.core.logic.InputAction.SET_VALUE -> {
                                instrumentController.updateVolume(state.instrument, action.value)
                            }
                            else -> {}
                        }
                    }
                }
            }
            3 -> {
                // DETUNE + PAN row (columns: 0=name, 1=DETUNE, 2=name, 3=PAN)
                when (state.cursorColumn) {
                    1 -> {  // DETUNE value
                        when (action) {
                            is com.conanizer.pockettracker.core.logic.InputAction.SET_VALUE -> {
                                instrumentController.updateDetune(state.instrument, action.value)
                            }
                            else -> {}
                        }
                    }
                    3 -> {  // PAN value
                        when (action) {
                            is com.conanizer.pockettracker.core.logic.InputAction.SET_VALUE -> {
                                instrumentController.updatePan(state.instrument, action.value)
                            }
                            else -> {}
                        }
                    }
                }
            }
            4 -> {
                // TBL TIC row - table tick rate
                when (action) {
                    is com.conanizer.pockettracker.core.logic.InputAction.SET_VALUE -> {
                        instrumentController.updateTableTicRate(state.instrument, action.value)
                    }
                    else -> {}
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
                            is com.conanizer.pockettracker.core.logic.InputAction.SET_VALUE -> {
                                instrumentController.updateDrive(state.instrument, action.value)
                            }
                            else -> {}
                        }
                    }
                    3 -> {  // FILTER type
                        when (action) {
                            is com.conanizer.pockettracker.core.logic.InputAction.SET_VALUE -> {
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
                            is com.conanizer.pockettracker.core.logic.InputAction.SET_VALUE -> {
                                instrumentController.updateCrush(state.instrument, action.value)
                            }
                            else -> {}
                        }
                    }
                    3 -> {  // CUT value
                        when (action) {
                            is com.conanizer.pockettracker.core.logic.InputAction.SET_VALUE -> {
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
                            is com.conanizer.pockettracker.core.logic.InputAction.SET_VALUE -> {
                                instrumentController.updateDownsample(state.instrument, action.value)
                            }
                            else -> {}
                        }
                    }
                    3 -> {  // RES value
                        when (action) {
                            is com.conanizer.pockettracker.core.logic.InputAction.SET_VALUE -> {
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
                    is com.conanizer.pockettracker.core.logic.InputAction.SET_VALUE -> {
                        instrumentController.updateSampleStart(state.instrument, action.value)
                    }
                    else -> {}
                }
            }
            11 -> {
                // END (sample end point)
                when (action) {
                    is com.conanizer.pockettracker.core.logic.InputAction.SET_VALUE -> {
                        instrumentController.updateSampleEnd(state.instrument, action.value)
                    }
                    else -> {}
                }
            }
            12 -> {
                // REV (reverse: off/on)
                when (action) {
                    is com.conanizer.pockettracker.core.logic.InputAction.SET_VALUE -> {
                        instrumentController.updateReverse(state.instrument, action.value == 1)
                    }
                    else -> {}
                }
            }
            13 -> {
                // LOOP mode (off/fwd/png)
                when (action) {
                    is com.conanizer.pockettracker.core.logic.InputAction.SET_VALUE -> {
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
                    is com.conanizer.pockettracker.core.logic.InputAction.SET_VALUE -> {
                        instrumentController.updateLoopStart(state.instrument, action.value)
                    }
                    else -> {}
                }
            }
        }

        return InputResult(
            modified = action !is com.conanizer.pockettracker.core.logic.InputAction.NONE
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
 *   - 0=TYPE+LOAD, 1=NAME
 *   - 2=ROOT+VOL, 3=DETUNE+PAN, 4=TBL TIC, 5=SPACER
 *   - 6=DRIVE+FILTER, 7=CRUSH+CUT, 8=DWNSMPL+RES, 9=SPACER
 *   - 10=START, 11=END, 12=REV, 13=LOOP, 14=LOOP ST
 * @param cursorColumn Which column:
 *   - 0 = Parameter name (left column)
 *   - 1 = Value 1 (for dual-param rows)
 *   - 2 = Name 2 (for dual-param rows, empty for TYPE+LOAD row)
 *   - 3 = Value 2 / LOAD button (for dual-param rows)
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
