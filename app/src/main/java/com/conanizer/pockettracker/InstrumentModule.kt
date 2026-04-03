package com.conanizer.pockettracker

import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.geometry.Size
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.drawscope.DrawScope
import com.conanizer.pockettracker.core.data.Instrument
import com.conanizer.pockettracker.core.data.InstrumentType
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
            instrument = instrument,
            cursorRow = instrumentState.cursorRow,
            cursorColumn = instrumentState.cursorColumn,
            currentRow = currentRow
        )
        rowY += ROW_HEIGHT
        currentRow++

        // ─────────────────────────────────────
        // ROW 1: NAME (shows loaded source filename)
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
        // ROW 2: LOAD SOURCE button (new row)
        // ─────────────────────────────────────
        drawLoadSourceRow(
            x = x,
            y = rowY,
            scale = scale,
            nameColumnX = nameColumnX,
            valueColumnX = valueColumnX,
            instrument = instrument,
            cursorRow = instrumentState.cursorRow,
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
        // ROW 5: TBL TIC (table tick rate)
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
        // ROW 6: SPACER (empty row)
        // ─────────────────────────────────────
        rowY += ROW_HEIGHT
        currentRow++

        // ─────────────────────────────────────
        // ROWS 7+: type-specific layout
        // ─────────────────────────────────────
        if (instrument.instrumentType == InstrumentType.SOUNDFONT) {
            // SOUNDFONT: BANK + PRESET, then PRESET NAME
            val bankHex = instrument.sfBank.toString(16).padStart(2, '0').uppercase()
            val presetHex = instrument.sfPreset.toString(16).padStart(2, '0').uppercase()
            drawDualParameterRow(
                x = x,
                y = rowY,
                scale = scale,
                nameColumnX = nameColumnX,
                valueColumnX = valueColumnX,
                param1Name = "BANK",
                param1Value = bankHex,
                param2Name = "PRESET",
                param2Value = presetHex,
                cursorRow = instrumentState.cursorRow,
                cursorColumn = instrumentState.cursorColumn,
                currentRow = currentRow
            )
            rowY += ROW_HEIGHT
            currentRow++

            // PRESET NAME row (read-only, from tsf)
            val isCursorOnNameRow = instrumentState.cursorRow == currentRow
            if (isCursorOnNameRow) {
                drawRect(
                    color = Color(0xFF333333),
                    topLeft = Offset((x * scale).toFloat(), (rowY * scale).toFloat()),
                    size = Size((width * scale).toFloat(), (ROW_HEIGHT * scale).toFloat())
                )
            }
            drawBitmapText(
                text = "NAME",
                x = nameColumnX,
                y = rowY + TEXT_PADDING,
                scale = scale,
                color = if (isCursorOnNameRow) Color.Yellow else Color.Gray,
                spacing = CHAR_SPACING,
                fontScale = FONT_SCALE
            )
            drawBitmapText(
                text = instrumentState.soundfontPresetName.ifEmpty { "---" },
                x = valueColumnX,
                y = rowY + TEXT_PADDING,
                scale = scale,
                color = if (isCursorOnNameRow) Color.Yellow else Color.White,
                spacing = CHAR_SPACING,
                fontScale = FONT_SCALE
            )
        } else {
            // SAMPLER: DRIVE+FILTER (row 7), CRUSH+CUT (row 8), DWNSMPL+RES (row 9), SPACER (row 10), then sample params
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

            // ROW 8: CRUSH + CUT
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

            // ROW 9: DWNSMPL + RES
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

            // ROW 10: SPACER (empty row)
        // ─────────────────────────────────────
        rowY += ROW_HEIGHT
        currentRow++

            // ROW 11: START (sample start point)
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

            // ROW 12: END (sample end point)
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

            // ROW 13: REV (reverse: off/on)
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

            // ROW 14: LOOP (loop mode: off/fwd/png)
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

            // ROW 15: LOOP ST (loop start point)
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
        } // end SAMPLER-specific rows

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
     * Draw TYPE + LOAD + SAVE row
     * TYPE at columns 0-1, LOAD .pti at column 2, SAVE .pti at column 3
     * Columns: 0=TYPE name, 1=TYPE value (editable), 2=LOAD button, 3=SAVE button
     */
    private fun DrawScope.drawTypeLoadRow(
        x: Int,
        y: Int,
        scale: Int,
        nameColumnX: Int,
        valueColumnX: Int,
        instrument: Instrument,
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

        // COLUMN 1: type value (sampler/soundfont) — editable, handled in MainActivity
        val typeText = if (instrument.instrumentType == InstrumentType.SOUNDFONT) "soundfont" else "sampler"
        drawBitmapText(
            text = typeText,
            x = valueColumnX,
            y = textY,
            scale = scale,
            color = if (isCursorOnThisRow && cursorColumn == 1) Color.Yellow else Color.White,
            spacing = CHAR_SPACING,
            fontScale = FONT_SCALE
        )

        // COLUMN 2: LOAD .pti button
        val name2ColumnX = nameColumnX + 320
        drawBitmapText(
            text = "LOAD",
            x = name2ColumnX,
            y = textY,
            scale = scale,
            color = if (isCursorOnThisRow && cursorColumn == 2) Color.Yellow else Color.White,
            spacing = CHAR_SPACING,
            fontScale = FONT_SCALE
        )

        // COLUMN 3: SAVE .pti button
        val value2ColumnX = valueColumnX + 270
        drawBitmapText(
            text = "SAVE",
            x = value2ColumnX,
            y = textY,
            scale = scale,
            color = if (isCursorOnThisRow && cursorColumn == 3) Color.Yellow else Color.White,
            spacing = CHAR_SPACING,
            fontScale = FONT_SCALE
        )
    }

    /**
     * Draw LOAD SOURCE row (row 2)
     * Button label adapts: "LOAD WAV" for SAMPLER, "LOAD SF2" for SOUNDFONT
     * Button is at column 1 (valueColumnX)
     */
    private fun DrawScope.drawLoadSourceRow(
        x: Int,
        y: Int,
        scale: Int,
        nameColumnX: Int,
        valueColumnX: Int,
        instrument: Instrument,
        cursorRow: Int,
        currentRow: Int
    ) {
        val textY = y + TEXT_PADDING
        val isCursorOnThisRow = cursorRow == currentRow

        if (isCursorOnThisRow) {
            drawRect(
                color = Color(0xFF333333),
                topLeft = Offset((x * scale).toFloat(), (y * scale).toFloat()),
                size = Size((width * scale).toFloat(), (ROW_HEIGHT * scale).toFloat())
            )
        }

        val buttonText = if (instrument.instrumentType == InstrumentType.SOUNDFONT) "LOAD SF2" else "LOAD WAV"
        drawBitmapText(
            text = buttonText,
            x = valueColumnX,
            y = textY,
            scale = scale,
            color = if (isCursorOnThisRow) Color.Yellow else Color.White,
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

        // COLUMN 2: Show loaded source filename, or "empty" if nothing loaded
        val sourcePath = if (instrumentState.instrument.instrumentType == InstrumentType.SOUNDFONT)
            instrumentState.instrument.soundfontPath
        else
            instrumentState.instrument.sampleFilePath
        val sampleName = if (sourcePath != null) {
            java.io.File(sourcePath).name
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
     * Get cursor context for current cursor position.
     *
     * New row layout:
     * - Row 0:  TYPE (col 1 editable) | LOAD .pti (col 2) | SAVE .pti (col 3)
     * - Row 1:  NAME (read-only)
     * - Row 2:  LOAD SOURCE button (col 1)
     * - Row 3:  ROOT + VOL (dual: 1=ROOT, 3=VOL)
     * - Row 4:  DETUNE + PAN (dual: 1=DETUNE, 3=PAN)
     * - Row 5:  TBL TIC (col 1)
     * - Row 6:  SPACER
     * SAMPLER only (rows 7-15):
     * - Row 7:  DRIVE + FILTER (dual)
     * - Row 8:  CRUSH + CUT (dual)
     * - Row 9:  DWNSMPL + RES (dual)
     * - Row 10: SPACER
     * - Row 11: START
     * - Row 12: END
     * - Row 13: REV
     * - Row 14: LOOP
     * - Row 15: LOOP ST
     * SOUNDFONT only (rows 7-8):
     * - Row 7:  BANK + PRESET (dual: 1=BANK, 3=PRESET)
     * - Row 8:  PRESET NAME (read-only)
     */
    fun getCursorContext(state: InstrumentState): CursorContext {
        val isSoundFont = state.instrument.instrumentType == InstrumentType.SOUNDFONT
        when (state.cursorRow) {
            0 -> {
                // TYPE | type value | LOAD | SAVE — cols 1,2,3 all handled in MainActivity
                return CursorContextFactory.readOnly()
            }
            1 -> return CursorContextFactory.readOnly()  // NAME
            2 -> return CursorContextFactory.readOnly()  // LOAD SOURCE button
            3 -> {
                // ROOT + VOL
                when (state.cursorColumn) {
                    0, 2 -> return CursorContextFactory.readOnly()
                    1 -> {
                        val isEmpty = state.instrument.root == Note.EMPTY
                        val currentValue = if (isEmpty) 0 else state.instrument.root.toMidi()
                        return CursorContextFactory.note(currentValue, isEmpty)
                    }
                    3 -> return CursorContextFactory.hexByte(state.instrument.volume, 0, 255)
                    else -> return CursorContextFactory.none()
                }
            }
            4 -> {
                // DETUNE + PAN
                when (state.cursorColumn) {
                    0, 2 -> return CursorContextFactory.readOnly()
                    1 -> return CursorContextFactory.hexByte(state.instrument.detune, 0, 255)
                    3 -> return CursorContextFactory.hexByte(state.instrument.pan, 0, 255)
                    else -> return CursorContextFactory.none()
                }
            }
            5 -> {
                if (state.cursorColumn == 0) return CursorContextFactory.readOnly()
                return CursorContextFactory.hexByte(state.instrument.tableTicRate, 0, 255)
            }
            6 -> return CursorContextFactory.none()  // SPACER
            7 -> {
                if (isSoundFont) {
                    // BANK + PRESET
                    when (state.cursorColumn) {
                        0, 2 -> return CursorContextFactory.readOnly()
                        1 -> return CursorContextFactory.hexByte(state.instrument.sfBank, 0, 127)
                        3 -> return CursorContextFactory.hexByte(state.instrument.sfPreset, 0, 127)
                        else -> return CursorContextFactory.none()
                    }
                } else {
                    // DRIVE + FILTER
                    when (state.cursorColumn) {
                        0, 2 -> return CursorContextFactory.readOnly()
                        1 -> return CursorContextFactory.hexByte(state.instrument.drive, 0, 255)
                        3 -> {
                            val filterTypes = listOf("off", "lp", "hp", "bp")
                            return CursorContextFactory.toggleTernary(state.instrument.filterType, filterTypes)
                        }
                        else -> return CursorContextFactory.none()
                    }
                }
            }
            8 -> {
                if (isSoundFont) return CursorContextFactory.readOnly()  // PRESET NAME
                // CRUSH + CUT
                when (state.cursorColumn) {
                    0, 2 -> return CursorContextFactory.readOnly()
                    1 -> return CursorContextFactory.hexNibble(state.instrument.crush)
                    3 -> return CursorContextFactory.hexByte(state.instrument.filterCut, 0, 255)
                    else -> return CursorContextFactory.none()
                }
            }
            9 -> {
                if (isSoundFont) return CursorContextFactory.none()  // no row 9 for soundfont
                // DWNSMPL + RES
                when (state.cursorColumn) {
                    0, 2 -> return CursorContextFactory.readOnly()
                    1 -> return CursorContextFactory.hexNibble(state.instrument.downsample)
                    3 -> return CursorContextFactory.hexByte(state.instrument.filterRes, 0, 255)
                    else -> return CursorContextFactory.none()
                }
            }
            10 -> return CursorContextFactory.none()  // SPACER (SAMPLER) or unused
            11 -> {
                if (isSoundFont) return CursorContextFactory.none()
                if (state.cursorColumn == 0) return CursorContextFactory.readOnly()
                return CursorContextFactory.hexByte(state.instrument.sampleStart, 0, 255)
            }
            12 -> {
                if (isSoundFont) return CursorContextFactory.none()
                if (state.cursorColumn == 0) return CursorContextFactory.readOnly()
                return CursorContextFactory.hexByte(state.instrument.sampleEnd, 0, 255)
            }
            13 -> {
                if (isSoundFont) return CursorContextFactory.none()
                if (state.cursorColumn == 0) return CursorContextFactory.readOnly()
                return CursorContextFactory.toggleBinary(state.instrument.reverse)
            }
            14 -> {
                if (isSoundFont) return CursorContextFactory.none()
                if (state.cursorColumn == 0) return CursorContextFactory.readOnly()
                val loopModes = listOf("off", "fwd", "png")
                return CursorContextFactory.toggleTernary(state.instrument.loopMode, loopModes)
            }
            15 -> {
                if (isSoundFont) return CursorContextFactory.none()
                if (state.cursorColumn == 0) return CursorContextFactory.readOnly()
                return CursorContextFactory.hexByte(state.instrument.loopStart, 0, 255)
            }
            else -> return CursorContextFactory.none()
        }
    }

    /**
     * Handle input action for instrument screen.
     *
     * New row layout (rows 0-2 handled in MainActivity, rows 3+ handled here):
     * - Row 0:  TYPE/LOAD/SAVE — handled in MainActivity
     * - Row 1:  NAME — read-only
     * - Row 2:  LOAD SOURCE — handled in MainActivity
     * - Row 3:  ROOT + VOL
     * - Row 4:  DETUNE + PAN
     * - Row 5:  TBL TIC
     * - Row 6:  SPACER
     * SAMPLER (rows 7-15): DRIVE+FILTER, CRUSH+CUT, DWNSMPL+RES, SPACER, START, END, REV, LOOP, LOOP ST
     * SOUNDFONT (rows 7-8): BANK+PRESET, PRESET NAME (read-only)
     */
    fun handleInput(
        state: InstrumentState,
        action: com.conanizer.pockettracker.core.logic.InputAction,
        instrumentController: com.conanizer.pockettracker.core.logic.InstrumentController
    ): InputResult {
        val isSoundFont = state.instrument.instrumentType == InstrumentType.SOUNDFONT
        when (state.cursorRow) {
            0, 1, 2 -> { /* handled in MainActivity */ }
            3 -> {
                // ROOT + VOL
                when (state.cursorColumn) {
                    1 -> when (action) {
                        is com.conanizer.pockettracker.core.logic.InputAction.SET_VALUE ->
                            instrumentController.updateRoot(state.instrument, Note.fromMidi(action.value))
                        is com.conanizer.pockettracker.core.logic.InputAction.DELETE ->
                            instrumentController.updateRoot(state.instrument, Note.fromString("C-4"))
                        else -> {}
                    }
                    3 -> when (action) {
                        is com.conanizer.pockettracker.core.logic.InputAction.SET_VALUE ->
                            instrumentController.updateVolume(state.instrument, action.value)
                        else -> {}
                    }
                }
            }
            4 -> {
                // DETUNE + PAN
                when (state.cursorColumn) {
                    1 -> when (action) {
                        is com.conanizer.pockettracker.core.logic.InputAction.SET_VALUE ->
                            instrumentController.updateDetune(state.instrument, action.value)
                        else -> {}
                    }
                    3 -> when (action) {
                        is com.conanizer.pockettracker.core.logic.InputAction.SET_VALUE ->
                            instrumentController.updatePan(state.instrument, action.value)
                        else -> {}
                    }
                }
            }
            5 -> when (action) {
                is com.conanizer.pockettracker.core.logic.InputAction.SET_VALUE ->
                    instrumentController.updateTableTicRate(state.instrument, action.value)
                else -> {}
            }
            6 -> { /* SPACER */ }
            7 -> {
                if (isSoundFont) {
                    // BANK + PRESET
                    when (state.cursorColumn) {
                        1 -> when (action) {
                            is com.conanizer.pockettracker.core.logic.InputAction.SET_VALUE ->
                                instrumentController.updateSfBank(state.instrument, action.value)
                            else -> {}
                        }
                        3 -> when (action) {
                            is com.conanizer.pockettracker.core.logic.InputAction.SET_VALUE ->
                                instrumentController.updateSfPreset(state.instrument, action.value)
                            else -> {}
                        }
                    }
                } else {
                    // DRIVE + FILTER
                    when (state.cursorColumn) {
                        1 -> when (action) {
                            is com.conanizer.pockettracker.core.logic.InputAction.SET_VALUE ->
                                instrumentController.updateDrive(state.instrument, action.value)
                            else -> {}
                        }
                        3 -> when (action) {
                            is com.conanizer.pockettracker.core.logic.InputAction.SET_VALUE -> {
                                val filterTypes = listOf("off", "lp", "hp", "bp")
                                if (action.value in 0..3) instrumentController.updateFilterType(state.instrument, filterTypes[action.value])
                            }
                            else -> {}
                        }
                    }
                }
            }
            8 -> {
                if (!isSoundFont) {
                    // CRUSH + CUT
                    when (state.cursorColumn) {
                        1 -> when (action) {
                            is com.conanizer.pockettracker.core.logic.InputAction.SET_VALUE ->
                                instrumentController.updateCrush(state.instrument, action.value)
                            else -> {}
                        }
                        3 -> when (action) {
                            is com.conanizer.pockettracker.core.logic.InputAction.SET_VALUE ->
                                instrumentController.updateFilterCut(state.instrument, action.value)
                            else -> {}
                        }
                    }
                }
            }
            9 -> {
                if (!isSoundFont) {
                    // DWNSMPL + RES
                    when (state.cursorColumn) {
                        1 -> when (action) {
                            is com.conanizer.pockettracker.core.logic.InputAction.SET_VALUE ->
                                instrumentController.updateDownsample(state.instrument, action.value)
                            else -> {}
                        }
                        3 -> when (action) {
                            is com.conanizer.pockettracker.core.logic.InputAction.SET_VALUE ->
                                instrumentController.updateFilterRes(state.instrument, action.value)
                            else -> {}
                        }
                    }
                }
            }
            10 -> { /* SPACER */ }
            11 -> when (action) {
                is com.conanizer.pockettracker.core.logic.InputAction.SET_VALUE ->
                    instrumentController.updateSampleStart(state.instrument, action.value)
                else -> {}
            }
            12 -> when (action) {
                is com.conanizer.pockettracker.core.logic.InputAction.SET_VALUE ->
                    instrumentController.updateSampleEnd(state.instrument, action.value)
                else -> {}
            }
            13 -> when (action) {
                is com.conanizer.pockettracker.core.logic.InputAction.SET_VALUE ->
                    instrumentController.updateReverse(state.instrument, action.value == 1)
                else -> {}
            }
            14 -> when (action) {
                is com.conanizer.pockettracker.core.logic.InputAction.SET_VALUE -> {
                    val loopModes = listOf("off", "fwd", "png")
                    if (action.value in 0..2) instrumentController.updateLoopMode(state.instrument, loopModes[action.value])
                }
                else -> {}
            }
            15 -> when (action) {
                is com.conanizer.pockettracker.core.logic.InputAction.SET_VALUE ->
                    instrumentController.updateLoopStart(state.instrument, action.value)
                else -> {}
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
 * @param cursorRow Which row (new layout):
 *   - 0=TYPE+LOAD+SAVE, 1=NAME, 2=LOAD SOURCE
 *   - 3=ROOT+VOL, 4=DETUNE+PAN, 5=TBL TIC, 6=SPACER
 *   SAMPLER: 7=DRIVE+FILTER, 8=CRUSH+CUT, 9=DWNSMPL+RES, 10=SPACER,
 *            11=START, 12=END, 13=REV, 14=LOOP, 15=LOOP ST
 *   SOUNDFONT: 7=BANK+PRESET, 8=PRESET NAME
 * @param cursorColumn Which column (0=name label, 1=value1, 2=button/name2, 3=value2/button)
 * @param statusMessage Status message to show at bottom
 * @param isSuccess True if status is success, false if error
 * @param soundfontPresetName Display name for the current soundfont preset (from tsf)
 */
data class InstrumentState(
    val instrument: Instrument,
    val cursorRow: Int = 0,
    val cursorColumn: Int = 1,  // Start on value, not name
    val statusMessage: String = "",
    val isSuccess: Boolean = true,
    val soundfontPresetName: String = ""
)
