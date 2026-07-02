package com.conanizer.pockettracker.ui.modules

import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.geometry.Size
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.drawscope.DrawScope
import com.conanizer.pockettracker.ui.theme.AppTheme
import com.conanizer.pockettracker.input.CursorContext
import com.conanizer.pockettracker.input.CursorContextFactory
import com.conanizer.pockettracker.ui.TrackerModule
import com.conanizer.pockettracker.core.data.Instrument
import com.conanizer.pockettracker.core.data.InstrumentType
import com.conanizer.pockettracker.core.data.Note
import com.conanizer.pockettracker.core.logic.InputAction
import com.conanizer.pockettracker.core.logic.InstrumentController
import com.conanizer.pockettracker.ui.drawBitmapText
import com.conanizer.pockettracker.ui.drawEqCell
import com.conanizer.pockettracker.ui.toHex1
import com.conanizer.pockettracker.ui.toHex2
import java.io.File

/**
 * INSTRUMENT SCREEN MODULE
 *
 * Sampler row layout (0-15):
 *   0  TYPE + LOAD + SAVE
 *   1  NAME (instrument.name, read-only)
 *   2  ROOT + DETUNE + TIC  (triple: col 1 / 3 / 5)
 *   3  VOL + SLICE + PAN  (triple: col 1 / 3 / 5)
 *   4  SPACER
 *   5  SAMPLE section: filename + LOAD WAV (col 2) + EDIT (col 3)
 *   6  SPACER
 *   7  DRIVE + FILTER
 *   8  CRUSH + FREQ
 *   9  DWNSMPL + RES
 *  10  SPACER
 *  11  REV + DEL
 *  12  EQ  (col 1; " >" opens the EQ editor)
 *  13  LOOP + START
 *  14  LOOP ST + END
 *  15  LOOP END + REVERSE
 *
 * Soundfont row layout (0-14):
 *   0  TYPE + LOAD + SAVE
 *   1  NAME
 *   2  ROOT + DETUNE + TIC
 *   3  VOL + PAN
 *   4  SPACER
 *   5  SF section: SF filename + LOAD SF2 (col 2)
 *   6  PRESET  (combined index + name)
 *   7  SPACER
 *   8  DRIVE + FILTER
 *   9  CRUSH + FREQ
 *  10  DWNSMPL + RES
 *  11  SPACER
 *  12  REV
 *  13  DEL
 *  14  EQ
 */
class InstrumentModule : TrackerModule {
    override val width  = 510
    override val height = 392

    private val FONT_SCALE   = 3
    private val CHAR_SPACING = 2
    private val ROW_HEIGHT   = 21
    private val TEXT_PADDING = 3

    // ─── Triple-row fixed column positions (relative to module left edge) ────
    private val TRIPLE_V1_OFFSET = 90    // ROOT value
    private val TRIPLE_N2_OFFSET = 185   // DETUNE label
    private val TRIPLE_V2_OFFSET = 305   // DETUNE value
    private val TRIPLE_N3_OFFSET = 368   // TIC label
    private val TRIPLE_V3_OFFSET = 438   // TIC value

    // ─── Section-source row button positions ─────────────────────────────────
    private val SRC_FILENAME_OFFSET = 105  // where filename starts
    private val SRC_LOAD_OFFSET     = 340  // LOAD button (col 2)
    private val SRC_EDIT_OFFSET     = 440  // EDIT button (col 3, sampler only)

    override fun DrawScope.draw(x: Int, y: Int, scale: Int, state: Any?) {
        val instrumentState = state as? InstrumentState ?: return
        val instrument      = instrumentState.instrument
        val isSoundFont     = instrument.instrumentType == InstrumentType.SOUNDFONT
        val sfOffset        = if (isSoundFont) 1 else 0

        val t = instrumentState.appTheme
        drawRect(
            color   = Color(t.background),
            topLeft = Offset((x * scale).toFloat(), (y * scale).toFloat()),
            size    = Size((width * scale).toFloat(), (height * scale).toFloat())
        )

        val nameColumnX  = x + 10
        val valueColumnX = x + 150

        var rowY       = y + TEXT_PADDING
        var currentRow = 0

        drawBitmapText(
            "INSTRUMENT ${instrument.id.toHex2()}",
            nameColumnX, rowY, scale, Color(t.textTitle), CHAR_SPACING, FONT_SCALE
        )
        rowY += ROW_HEIGHT + 14

        // ── ROW 0: TYPE + LOAD + SAVE ───────────────────────────────────────
        drawTypeLoadRow(
            x, rowY, scale, nameColumnX, valueColumnX,
            instrument,
            instrumentState.cursorRow, instrumentState.cursorColumn, currentRow, t
        )
        rowY += ROW_HEIGHT; currentRow++

        // ── ROW 1: INSTRUMENT NAME ───────────────────────────────────────────
        drawNameRow(x, rowY, scale, nameColumnX, valueColumnX, instrumentState, currentRow, t)
        rowY += ROW_HEIGHT; currentRow++

        // ── ROW 2: ROOT + DETUNE + TIC ───────────────────────────────────────
        drawTripleParameterRow(
            x, rowY, scale, nameColumnX,
            "ROOT",   instrument.root.toString(),
            "DETUNE", instrument.detune.toHex2(),
            "TIC",    instrument.tableTicRate.toHex2(),
            instrumentState.cursorRow, instrumentState.cursorColumn, currentRow, t
        )
        rowY += ROW_HEIGHT; currentRow++

        // ── ROW 3: VOL + PAN (soundfont) / VOL + SLICE + PAN (sampler) ───────
        if (isSoundFont) {
            drawDualParameterRow(
                x, rowY, scale, nameColumnX, valueColumnX,
                "VOL", instrument.volume.toHex2(),
                "PAN", instrument.pan.toHex2(),
                instrumentState.cursorRow, instrumentState.cursorColumn, currentRow, t
            )
        } else {
            val sliceModeStr = listOf("OFF", "CUT", "TRU")[instrument.slicingMode.coerceIn(0, 2)]
            drawTripleParameterRow(
                x, rowY, scale, nameColumnX,
                "VOL",   instrument.volume.toHex2(),
                "SLICE", sliceModeStr,
                "PAN",   instrument.pan.toHex2(),
                instrumentState.cursorRow, instrumentState.cursorColumn, currentRow, t
            )
        }
        rowY += ROW_HEIGHT; currentRow++

        // ── ROW 4: SPACER ─────────────────────────────────────────────────────
        rowY += ROW_HEIGHT; currentRow++

        // ── ROW 5: SAMPLE / SF section row ───────────────────────────────────
        drawSectionSourceRow(
            x, rowY, scale, nameColumnX,
            instrument,
            instrumentState.cursorRow, instrumentState.cursorColumn, currentRow,
            isSoundFont, t
        )
        rowY += ROW_HEIGHT; currentRow++

        // ── ROW 6 (SF only): PRESET combined ─────────────────────────────────
        if (isSoundFont) {
            val presetCount  = instrumentState.soundfontPresetCount
            val presetIdx    = instrumentState.soundfontPresetIndex
            val presetNumStr = if (presetCount > 0) "${presetIdx + 1}/$presetCount" else "--"
            val presetName   = instrumentState.soundfontPresetName.ifEmpty { "" }
            val presetValStr = if (presetName.isEmpty()) presetNumStr else "$presetNumStr $presetName"
            val isCursor     = instrumentState.cursorRow == currentRow
            if (isCursor) drawRowBg(x, rowY, scale, t)
            drawBitmapText("PRESET", nameColumnX, rowY + TEXT_PADDING, scale,
                if (isCursor) Color(t.textCursor) else Color(t.textParam), CHAR_SPACING, FONT_SCALE)
            drawBitmapText(presetValStr, valueColumnX, rowY + TEXT_PADDING, scale,
                if (isCursor) Color(t.textCursor) else Color(t.textValue), CHAR_SPACING, FONT_SCALE)
            rowY += ROW_HEIGHT; currentRow++
        }

        // ── ROW 6/7: SPACER ───────────────────────────────────────────────────
        rowY += ROW_HEIGHT; currentRow++

        // ── ROW 7/8: DRIVE + FILTER ────────────────────────────────────────────
        drawDualParameterRow(
            x, rowY, scale, nameColumnX, valueColumnX,
            "DRIVE", instrument.drive.toHex2(),
            "FILTER", instrument.filterType,
            instrumentState.cursorRow, instrumentState.cursorColumn, currentRow, t
        )
        rowY += ROW_HEIGHT; currentRow++

        // ── ROW 8/9: CRUSH + FREQ ──────────────────────────
        drawDualParameterRow(
            x, rowY, scale, nameColumnX, valueColumnX,
            "CRUSH", instrument.crush.toHex1(),
            "FREQ", instrument.filterCut.toHex2(),
            instrumentState.cursorRow, instrumentState.cursorColumn, currentRow, t
        )
        rowY += ROW_HEIGHT; currentRow++

        // ── ROW 9/10: DWNSMPL + RES ────────────────────────────────────────────
        drawDualParameterRow(
            x, rowY, scale, nameColumnX, valueColumnX,
            "DWNSMPL", instrument.downsample.toHex1(),
            "RES", instrument.filterRes.toHex2(),
            instrumentState.cursorRow, instrumentState.cursorColumn, currentRow, t
        )
        rowY += ROW_HEIGHT; currentRow++

        // ── ROW 10/11: SPACER ─────────────────────────────────────────────────
        rowY += ROW_HEIGHT; currentRow++

        // ── Instrument-type-specific tail rows ────────────────────────────────
        if (isSoundFont) {
            // ROW 12: REV
            drawParameterRow(x, rowY, scale, nameColumnX, valueColumnX,
                "REV", instrument.reverbSend.toHex2(),
                isCursorOnName  = instrumentState.cursorRow == currentRow && instrumentState.cursorColumn == 0,
                isCursorOnValue = instrumentState.cursorRow == currentRow && instrumentState.cursorColumn == 1,
                t = t)
            rowY += ROW_HEIGHT; currentRow++

            // ROW 13: DEL
            drawParameterRow(x, rowY, scale, nameColumnX, valueColumnX,
                "DEL", instrument.delaySend.toHex2(),
                isCursorOnName  = instrumentState.cursorRow == currentRow && instrumentState.cursorColumn == 0,
                isCursorOnValue = instrumentState.cursorRow == currentRow && instrumentState.cursorColumn == 1,
                t = t)
            rowY += ROW_HEIGHT; currentRow++

            // ROW 14: EQ — value dims to textParam when unassigned; " >" opens the EQ editor.
            run {
                val textY = rowY + TEXT_PADDING
                val onRow = instrumentState.cursorRow == currentRow
                if (onRow) drawRowBg(x, rowY, scale, t)
                val eqCursor = onRow && instrumentState.cursorColumn == 1
                drawBitmapText("EQ", nameColumnX, textY, scale,
                    if (onRow) Color(t.textCursor) else Color(t.textParam), CHAR_SPACING, FONT_SCALE)
                drawEqCell(valueColumnX, textY, scale, instrument.eqSlot, eqCursor, t)
            }
        } else {
            // ROW 11: REV + DEL (send levels)
            drawDualParameterRow(
                x, rowY, scale, nameColumnX, valueColumnX,
                "REV", instrument.reverbSend.toHex2(),
                "DEL", instrument.delaySend.toHex2(),
                instrumentState.cursorRow, instrumentState.cursorColumn, currentRow, t
            )
            rowY += ROW_HEIGHT; currentRow++

            // ROW 12: EQ (alone, col 1) — value dims to "--" when unassigned; " >" opens the EQ editor.
            run {
                val textY = rowY + TEXT_PADDING
                val onRow = instrumentState.cursorRow == currentRow
                if (onRow) drawRowBg(x, rowY, scale, t)
                val eqCursor = onRow && instrumentState.cursorColumn == 1
                drawBitmapText("EQ", nameColumnX, textY, scale,
                    if (onRow) Color(t.textCursor) else Color(t.textParam), CHAR_SPACING, FONT_SCALE)
                drawEqCell(valueColumnX, textY, scale, instrument.eqSlot, eqCursor, t)
            }
            rowY += ROW_HEIGHT; currentRow++

            // ROW 13: LOOP + START
            drawDualParameterRow(
                x, rowY, scale, nameColumnX, valueColumnX,
                "LOOP",  instrument.loopMode,
                "START", instrument.sampleStart.toHex2(),
                instrumentState.cursorRow, instrumentState.cursorColumn, currentRow, t
            )
            rowY += ROW_HEIGHT; currentRow++

            // ROW 14: LOOP ST + END
            drawDualParameterRow(
                x, rowY, scale, nameColumnX, valueColumnX,
                "LOOP ST", instrument.loopStart.toHex2(),
                "END",     instrument.sampleEnd.toHex2(),
                instrumentState.cursorRow, instrumentState.cursorColumn, currentRow, t
            )
            rowY += ROW_HEIGHT; currentRow++

            // ROW 15: LOOP END + REVERSE
            drawDualParameterRow(
                x, rowY, scale, nameColumnX, valueColumnX,
                "LOOP END", instrument.loopEnd.toHex2(),
                "REVERSE",  if (instrument.reverse) "on" else "off",
                instrumentState.cursorRow, instrumentState.cursorColumn, currentRow, t
            )
        }
        // Status messages ("SF LOADED", "SRC MISSING", ...) are drawn by the global overlay
        // on the visualizer header (PixelPerfectRenderer), not inside this module.
    }

    // ═══════════════════════════════════════════════════════════════════════════
    // DRAW HELPERS
    // ═══════════════════════════════════════════════════════════════════════════

    private fun DrawScope.drawRowBg(x: Int, y: Int, scale: Int, t: AppTheme) {
        drawRect(Color(t.rowCursor),
            topLeft = Offset((x * scale).toFloat(), (y * scale).toFloat()),
            size    = Size((width * scale).toFloat(), (ROW_HEIGHT * scale).toFloat()))
    }

    private fun DrawScope.drawParameterRow(
        x: Int, y: Int, scale: Int,
        nameColumnX: Int, valueColumnX: Int,
        parameterName: String, parameterValue: String,
        isCursorOnName: Boolean, isCursorOnValue: Boolean,
        t: AppTheme
    ) {
        val textY = y + TEXT_PADDING
        if (isCursorOnName || isCursorOnValue) drawRowBg(x, y, scale, t)
        drawBitmapText(parameterName, nameColumnX, textY, scale,
            if (isCursorOnName || isCursorOnValue) Color(t.textCursor) else Color(t.textParam),
            CHAR_SPACING, FONT_SCALE)
        drawBitmapText(parameterValue, valueColumnX, textY, scale,
            if (isCursorOnValue) Color(t.textCursor) else Color(t.textValue),
            CHAR_SPACING, FONT_SCALE)
    }

    private fun DrawScope.drawDualParameterRow(
        x: Int, y: Int, scale: Int,
        nameColumnX: Int, valueColumnX: Int,
        param1Name: String, param1Value: String,
        param2Name: String, param2Value: String,
        cursorRow: Int, cursorColumn: Int, currentRow: Int,
        t: AppTheme
    ) {
        val textY          = y + TEXT_PADDING
        val isCursorOnRow  = cursorRow == currentRow
        if (isCursorOnRow) drawRowBg(x, y, scale, t)

        val name2X  = nameColumnX + 230
        val value2X = valueColumnX + 220

        drawBitmapText(param1Name, nameColumnX, textY, scale,
            if (isCursorOnRow && cursorColumn == 1) Color(t.textCursor) else Color(t.textParam),
            CHAR_SPACING, FONT_SCALE)
        drawBitmapText(param1Value, valueColumnX, textY, scale,
            if (isCursorOnRow && cursorColumn == 1) Color(t.textCursor) else Color(t.textValue),
            CHAR_SPACING, FONT_SCALE)
        drawBitmapText(param2Name, name2X, textY, scale,
            if (isCursorOnRow && cursorColumn == 3) Color(t.textCursor) else Color(t.textParam),
            CHAR_SPACING, FONT_SCALE)
        drawBitmapText(param2Value, value2X, textY, scale,
            if (isCursorOnRow && cursorColumn == 3) Color(t.textCursor) else Color(t.textValue),
            CHAR_SPACING, FONT_SCALE)
    }

    /** Three parameter pairs in a single row. Cursor cols: 1=param1, 3=param2, 5=param3. */
    private fun DrawScope.drawTripleParameterRow(
        x: Int, y: Int, scale: Int,
        nameColumnX: Int,
        param1Name: String, param1Value: String,
        param2Name: String, param2Value: String,
        param3Name: String, param3Value: String,
        cursorRow: Int, cursorColumn: Int, currentRow: Int,
        t: AppTheme
    ) {
        val textY         = y + TEXT_PADDING
        val isCursorOnRow = cursorRow == currentRow
        if (isCursorOnRow) drawRowBg(x, y, scale, t)

        val v1X = x + TRIPLE_V1_OFFSET
        val n2X = x + TRIPLE_N2_OFFSET
        val v2X = x + TRIPLE_V2_OFFSET
        val n3X = x + TRIPLE_N3_OFFSET
        val v3X = x + TRIPLE_V3_OFFSET

        drawBitmapText(param1Name, nameColumnX, textY, scale,
            if (isCursorOnRow && cursorColumn == 1) Color(t.textCursor) else Color(t.textParam),
            CHAR_SPACING, FONT_SCALE)
        drawBitmapText(param1Value, v1X, textY, scale,
            if (isCursorOnRow && cursorColumn == 1) Color(t.textCursor) else Color(t.textValue),
            CHAR_SPACING, FONT_SCALE)

        drawBitmapText(param2Name, n2X, textY, scale,
            if (isCursorOnRow && cursorColumn == 3) Color(t.textCursor) else Color(t.textParam),
            CHAR_SPACING, FONT_SCALE)
        drawBitmapText(param2Value, v2X, textY, scale,
            if (isCursorOnRow && cursorColumn == 3) Color(t.textCursor) else Color(t.textValue),
            CHAR_SPACING, FONT_SCALE)

        drawBitmapText(param3Name, n3X, textY, scale,
            if (isCursorOnRow && cursorColumn == 5) Color(t.textCursor) else Color(t.textParam),
            CHAR_SPACING, FONT_SCALE)
        drawBitmapText(param3Value, v3X, textY, scale,
            if (isCursorOnRow && cursorColumn == 5) Color(t.textCursor) else Color(t.textValue),
            CHAR_SPACING, FONT_SCALE)
    }

    /** Row 0: TYPE + LOAD .pti + SAVE .pti */
    private fun DrawScope.drawTypeLoadRow(
        x: Int, y: Int, scale: Int,
        nameColumnX: Int, valueColumnX: Int,
        instrument: Instrument,
        cursorRow: Int, cursorColumn: Int, currentRow: Int,
        t: AppTheme
    ) {
        val textY          = y + TEXT_PADDING
        val isCursorOnRow  = cursorRow == currentRow
        if (isCursorOnRow) drawRowBg(x, y, scale, t)

        val loadX = nameColumnX + 325
        val saveX = valueColumnX + 270

        val typeText = if (instrument.instrumentType == InstrumentType.SOUNDFONT) "soundfont" else "sampler"
        drawBitmapText("TYPE", nameColumnX, textY, scale,
            if (isCursorOnRow && cursorColumn == 1) Color(t.textCursor) else Color(t.textParam),
            CHAR_SPACING, FONT_SCALE)
        drawBitmapText(typeText, valueColumnX, textY, scale,
            if (isCursorOnRow && cursorColumn == 1) Color(t.textCursor) else Color(t.textValue),
            CHAR_SPACING, FONT_SCALE)
        drawBitmapText("LOAD", loadX, textY, scale,
            if (isCursorOnRow && cursorColumn == 2) Color(t.textCursor) else Color(t.textValue),
            CHAR_SPACING, FONT_SCALE)
        drawBitmapText("SAVE", saveX, textY, scale,
            if (isCursorOnRow && cursorColumn == 3) Color(t.textCursor) else Color(t.textValue),
            CHAR_SPACING, FONT_SCALE)
    }

    /** Row 1: instrument name (read-only display). */
    private fun DrawScope.drawNameRow(
        x: Int, y: Int, scale: Int,
        nameColumnX: Int, valueColumnX: Int,
        instrumentState: InstrumentState,
        currentRow: Int,
        t: AppTheme
    ) {
        val textY          = y + TEXT_PADDING
        val isCursorOnRow  = instrumentState.cursorRow == currentRow
        if (isCursorOnRow) drawRowBg(x, y, scale, t)
        drawBitmapText("NAME", nameColumnX, textY, scale,
            if (isCursorOnRow) Color(t.textCursor) else Color(t.textParam), CHAR_SPACING, FONT_SCALE)
        // "______" until a sample/SF2 is loaded (or the user names it via SELECT); otherwise the name.
        val displayName = if (instrumentState.instrument.hasDefaultName()) "______" else instrumentState.instrument.name
        drawBitmapText(displayName, valueColumnX, textY, scale,
            if (isCursorOnRow) Color(t.textCursor) else Color(t.textValue), CHAR_SPACING, FONT_SCALE)
    }

    /**
     * Row 5: section header + action buttons. The loaded file name lives on the NAME row, not here.
     * Sampler: SMPL | EDIT (col 3, aligned with the FX value column) | LOAD (col 2)
     * Soundfont: SF | LOAD (col 2)
     */
    private fun DrawScope.drawSectionSourceRow(
        x: Int, y: Int, scale: Int,
        nameColumnX: Int,
        instrument: Instrument,
        cursorRow: Int, cursorColumn: Int, currentRow: Int,
        isSoundFont: Boolean,
        t: AppTheme
    ) {
        val textY          = y + TEXT_PADDING
        val isCursorOnRow  = cursorRow == currentRow
        if (isCursorOnRow) drawRowBg(x, y, scale, t)

        val header = if (isSoundFont) "SF" else "SMPL"
        val loadX  = x + 150  // LOAD aligned with the DRIVE/CRUSH/DWNSMPL value column (col 2, left)
        val editX  = x + 335  // EDIT to the right of LOAD (col 3)

        drawBitmapText(header, nameColumnX, textY, scale, Color(t.textParam), CHAR_SPACING, FONT_SCALE)
        drawBitmapText("LOAD", loadX, textY, scale,
            if (isCursorOnRow && cursorColumn == 2) Color(t.textCursor) else Color(t.textValue),
            CHAR_SPACING, FONT_SCALE)
        if (!isSoundFont) {
            drawBitmapText("EDIT >", editX, textY, scale,
                if (isCursorOnRow && cursorColumn == 3) Color(t.textCursor) else Color(t.textValue),
                CHAR_SPACING, FONT_SCALE)
        }
    }

    // ═══════════════════════════════════════════════════════════════════════════
    // CURSOR CONTEXT
    // ═══════════════════════════════════════════════════════════════════════════

    fun getCursorContext(state: InstrumentState): CursorContext {
        val isSoundFont = state.instrument.instrumentType == InstrumentType.SOUNDFONT
        val sfOffset    = if (isSoundFont) 1 else 0
        val row         = state.cursorRow
        val col         = state.cursorColumn

        return when {
            row == 0 -> CursorContextFactory.readOnly()
            row == 1 -> CursorContextFactory.readOnly()
            row == 2 -> when (col) {  // ROOT + DETUNE + TIC
                1 -> {
                    val isEmpty = state.instrument.root == Note.EMPTY
                    CursorContextFactory.note(if (isEmpty) 0 else state.instrument.root.toMidi(), isEmpty)
                }
                3 -> CursorContextFactory.hexByte(state.instrument.detune, 0, 255, default = 0x80)
                5 -> CursorContextFactory.hexByte(state.instrument.tableTicRate, 0, 255, default = 0x06)
                else -> CursorContextFactory.none()
            }

            row == 3 -> if (isSoundFont) when (col) {  // VOL + PAN
                1 -> CursorContextFactory.hexByte(state.instrument.volume, 0, 255, default = 0xFF)
                3 -> CursorContextFactory.hexByte(state.instrument.pan, 0, 255, default = 0x80)
                else -> CursorContextFactory.none()
            } else when (col) {  // VOL + SLICE + PAN (triple)
                1 -> CursorContextFactory.hexByte(state.instrument.volume, 0, 255, default = 0xFF)
                3 -> {
                    val sliceModes = listOf("OFF", "CUT", "TRU")
                    CursorContextFactory.toggleTernary(sliceModes[state.instrument.slicingMode.coerceIn(0, 2)], sliceModes)
                }
                5 -> CursorContextFactory.hexByte(state.instrument.pan, 0, 255, default = 0x80)
                else -> CursorContextFactory.none()
            }

            row == 4 -> CursorContextFactory.none()       // SPACER
            row == 5 -> CursorContextFactory.readOnly()   // SAMPLE/SF source buttons (handled in MainActivity)

            isSoundFont && row == 6 -> {  // PRESET
                val maxIdx = (state.soundfontPresetCount - 1).coerceAtLeast(0)
                if (col == 1) CursorContextFactory.hexByte(state.soundfontPresetIndex, 0, maxIdx)
                else CursorContextFactory.none()
            }

            row == 6 + sfOffset -> CursorContextFactory.none()  // SPACER

            row == 7 + sfOffset -> when (col) {  // DRIVE + FILTER
                1 -> CursorContextFactory.hexByte(state.instrument.drive, 0, 255, default = 0x00)
                3 -> CursorContextFactory.toggleTernary(
                    state.instrument.filterType, listOf("off", "lp", "hp", "bp"))
                else -> CursorContextFactory.none()
            }

            row == 8 + sfOffset -> when (col) {  // CRUSH + FREQ
                1 -> CursorContextFactory.hexNibble(state.instrument.crush, default = 0)
                3 -> CursorContextFactory.hexByte(state.instrument.filterCut, 0, 255, default = 0x00)
                else -> CursorContextFactory.none()
            }

            row == 9 + sfOffset -> when (col) {  // DWNSMPL + RES
                1 -> CursorContextFactory.hexNibble(state.instrument.downsample, default = 0)
                3 -> CursorContextFactory.hexByte(state.instrument.filterRes, 0, 255, default = 0x00)
                else -> CursorContextFactory.none()
            }

            row == 10 + sfOffset -> CursorContextFactory.none()  // SPACER

            // Soundfont-specific tail
            isSoundFont && row == 12 -> if (col == 0) CursorContextFactory.readOnly()
                else CursorContextFactory.hexByte(state.instrument.reverbSend, 0, 255, default = 0x00)
            isSoundFont && row == 13 -> if (col == 0) CursorContextFactory.readOnly()
                else CursorContextFactory.hexByte(state.instrument.delaySend, 0, 255, default = 0x00)
            isSoundFont && row == 14 -> if (col == 0) CursorContextFactory.readOnly()
                else CursorContextFactory.hexByte(
                    if (state.instrument.eqSlot < 0) 0 else state.instrument.eqSlot,
                    min = 0, max = 127, emptyValue = -1,
                    canDelete = state.instrument.eqSlot >= 0,
                    canInsert = state.instrument.eqSlot < 0)

            // Sampler-specific tail
            !isSoundFont && row == 11 -> when (col) {  // REV + DEL
                1 -> CursorContextFactory.hexByte(state.instrument.reverbSend, 0, 255, default = 0x00)
                3 -> CursorContextFactory.hexByte(state.instrument.delaySend, 0, 255, default = 0x00)
                else -> CursorContextFactory.none()
            }
            !isSoundFont && row == 12 -> if (col == 1) CursorContextFactory.hexByte(  // EQ (alone)
                    if (state.instrument.eqSlot < 0) 0 else state.instrument.eqSlot,
                    min = 0, max = 127, emptyValue = -1,
                    canDelete = state.instrument.eqSlot >= 0,
                    canInsert = state.instrument.eqSlot < 0)
                else CursorContextFactory.none()
            !isSoundFont && row == 13 -> when (col) {  // LOOP + START
                1 -> CursorContextFactory.toggleTernary(state.instrument.loopMode, listOf("off", "fwd", "png"))
                3 -> CursorContextFactory.hexByte(state.instrument.sampleStart, 0, 255, default = 0x00)
                else -> CursorContextFactory.none()
            }
            !isSoundFont && row == 14 -> when (col) {  // LOOP ST + END
                1 -> CursorContextFactory.hexByte(state.instrument.loopStart, 0, 255, default = 0x00)
                3 -> CursorContextFactory.hexByte(state.instrument.sampleEnd, 0, 255, default = 0xFF)
                else -> CursorContextFactory.none()
            }
            !isSoundFont && row == 15 -> when (col) {  // LOOP END + REVERSE
                1 -> CursorContextFactory.hexByte(state.instrument.loopEnd, 0, 255, default = 0xFF)
                3 -> CursorContextFactory.toggleBinary(state.instrument.reverse)
                else -> CursorContextFactory.none()
            }

            else -> CursorContextFactory.none()
        }
    }

    // ═══════════════════════════════════════════════════════════════════════════
    // INPUT HANDLER
    // ═══════════════════════════════════════════════════════════════════════════

    fun handleInput(
        state: InstrumentState,
        action: InputAction,
        instrumentController: InstrumentController
    ): InputResult {
        val isSoundFont = state.instrument.instrumentType == InstrumentType.SOUNDFONT
        val sfOffset    = if (isSoundFont) 1 else 0
        val row         = state.cursorRow
        val col         = state.cursorColumn

        when {
            row == 0 || row == 1 -> { /* TYPE / NAME — handled in MainActivity */ }
            row == 4 || row == 5 -> { /* SPACER / source buttons — handled in MainActivity */ }

            row == 2 -> when (col) {  // ROOT + DETUNE + TIC
                1 -> when (action) {
                    is InputAction.SET_VALUE ->
                        instrumentController.updateRoot(state.instrument, Note.fromMidi(action.value))
                    is InputAction.DELETE ->
                        instrumentController.updateRoot(state.instrument, Note.fromString("C-4"))
                    else -> {}
                }
                3 -> when (action) {
                    is InputAction.SET_VALUE ->
                        instrumentController.updateDetune(state.instrument, action.value)
                    else -> {}
                }
                5 -> when (action) {
                    is InputAction.SET_VALUE ->
                        instrumentController.updateTableTicRate(state.instrument, action.value)
                    else -> {}
                }
            }

            row == 3 -> if (isSoundFont) when (col) {  // VOL + PAN
                1 -> when (action) {
                    is InputAction.SET_VALUE ->
                        instrumentController.updateVolume(state.instrument, action.value)
                    else -> {}
                }
                3 -> when (action) {
                    is InputAction.SET_VALUE ->
                        instrumentController.updatePan(state.instrument, action.value)
                    else -> {}
                }
            } else when (col) {  // VOL + SLICE + PAN (triple)
                1 -> when (action) {
                    is InputAction.SET_VALUE ->
                        instrumentController.updateVolume(state.instrument, action.value)
                    else -> {}
                }
                3 -> when (action) {
                    is InputAction.SET_VALUE ->
                        instrumentController.updateSlicingMode(state.instrument, action.value)
                    else -> {}
                }
                5 -> when (action) {
                    is InputAction.SET_VALUE ->
                        instrumentController.updatePan(state.instrument, action.value)
                    else -> {}
                }
            }

            isSoundFont && row == 6 -> when (col) {  // PRESET
                1 -> when (action) {
                    is InputAction.SET_VALUE ->
                        instrumentController.setSoundfontPresetByIndex(state.instrument, action.value)
                    else -> {}
                }
            }

            row == 7 + sfOffset -> when (col) {  // DRIVE + FILTER
                1 -> when (action) {
                    is InputAction.SET_VALUE ->
                        instrumentController.updateDrive(state.instrument, action.value)
                    else -> {}
                }
                3 -> when (action) {
                    is InputAction.SET_VALUE -> {
                        val filterTypes = listOf("off", "lp", "hp", "bp")
                        if (action.value in 0..3)
                            instrumentController.updateFilterType(state.instrument, filterTypes[action.value])
                    }
                    else -> {}
                }
            }

            row == 8 + sfOffset -> when (col) {  // CRUSH + FREQ
                1 -> when (action) {
                    is InputAction.SET_VALUE ->
                        instrumentController.updateCrush(state.instrument, action.value)
                    else -> {}
                }
                3 -> when (action) {
                    is InputAction.SET_VALUE ->
                        instrumentController.updateFilterCut(state.instrument, action.value)
                    else -> {}
                }
            }

            row == 9 + sfOffset -> when (col) {  // DWNSMPL + RES
                1 -> when (action) {
                    is InputAction.SET_VALUE ->
                        instrumentController.updateDownsample(state.instrument, action.value)
                    else -> {}
                }
                3 -> when (action) {
                    is InputAction.SET_VALUE ->
                        instrumentController.updateFilterRes(state.instrument, action.value)
                    else -> {}
                }
            }

            // Soundfont tail rows
            isSoundFont && row == 12 -> when (action) {
                is InputAction.SET_VALUE ->
                    instrumentController.updateReverbSend(state.instrument, action.value)
                else -> {}
            }
            isSoundFont && row == 13 -> when (action) {
                is InputAction.SET_VALUE ->
                    instrumentController.updateDelaySend(state.instrument, action.value)
                else -> {}
            }
            isSoundFont && row == 14 -> when (action) {
                is InputAction.SET_VALUE ->
                    instrumentController.updateEqSlot(state.instrument, action.value)
                is InputAction.DELETE ->
                    instrumentController.updateEqSlot(state.instrument, -1)
                is InputAction.INSERT_DEFAULT ->
                    instrumentController.updateEqSlot(state.instrument, 0)
                else -> {}
            }

            // Sampler tail rows
            !isSoundFont && row == 11 -> when (col) {  // REV + DEL
                1 -> when (action) {
                    is InputAction.SET_VALUE ->
                        instrumentController.updateReverbSend(state.instrument, action.value)
                    else -> {}
                }
                3 -> when (action) {
                    is InputAction.SET_VALUE ->
                        instrumentController.updateDelaySend(state.instrument, action.value)
                    else -> {}
                }
            }
            !isSoundFont && row == 12 -> when (action) {  // EQ (alone)
                is InputAction.SET_VALUE ->
                    instrumentController.updateEqSlot(state.instrument, action.value)
                is InputAction.DELETE ->
                    instrumentController.updateEqSlot(state.instrument, -1)
                is InputAction.INSERT_DEFAULT ->
                    instrumentController.updateEqSlot(state.instrument, 0)
                else -> {}
            }
            !isSoundFont && row == 13 -> when (col) {  // LOOP + START
                1 -> when (action) {
                    is InputAction.SET_VALUE -> {
                        val loopModes = listOf("off", "fwd", "png")
                        if (action.value in 0..2)
                            instrumentController.updateLoopMode(state.instrument, loopModes[action.value])
                    }
                    else -> {}
                }
                3 -> when (action) {
                    is InputAction.SET_VALUE ->
                        instrumentController.updateSampleStart(state.instrument, action.value)
                    else -> {}
                }
            }
            !isSoundFont && row == 14 -> when (col) {  // LOOP ST + END
                1 -> when (action) {
                    is InputAction.SET_VALUE ->
                        instrumentController.updateLoopStart(state.instrument, action.value)
                    else -> {}
                }
                3 -> when (action) {
                    is InputAction.SET_VALUE ->
                        instrumentController.updateSampleEnd(state.instrument, action.value)
                    else -> {}
                }
            }
            !isSoundFont && row == 15 -> when (col) {  // LOOP END + REVERSE
                1 -> when (action) {
                    is InputAction.SET_VALUE ->
                        instrumentController.updateLoopEnd(state.instrument, action.value)
                    else -> {}
                }
                3 -> when (action) {
                    is InputAction.SET_VALUE ->
                        instrumentController.updateReverse(state.instrument, action.value == 1)
                    else -> {}
                }
            }
        }

        return InputResult(modified = action !is InputAction.NONE)
    }

    data class InputResult(val modified: Boolean)
}

/**
 * STATE DATA FOR INSTRUMENT SCREEN
 *
 * cursorRow:
 *   SAMPLER:   0=TYPE, 1=NAME, 2=ROOT+DET+TIC, 3=VOL+SLICE+PAN, 4=SPACER, 5=SAMPLE section,
 *              6=SPACER, 7=DRIVE+FILTER, 8=CRUSH+FREQ, 9=DWNSMPL+RES, 10=SPACER, 11=REV+DEL,
 *              12=EQ, 13=LOOP+START, 14=LOOP ST+END, 15=LOOP END+REVERSE
 *   SOUNDFONT: 0=TYPE, 1=NAME, 2=ROOT+DET+TIC, 3=VOL+PAN, 4=SPACER, 5=SF section, 6=PRESET,
 *              7=SPACER, 8=DRIVE+FILTER, 9=CRUSH+FREQ, 10=DWNSMPL+RES, 11=SPACER, 12=REV,
 *              13=DEL, 14=EQ
 *
 * cursorColumn:
 *   0=name label, 1=value1, 2=button/name2, 3=value2/button
 *   Triple rows (2 and, on the sampler, 3) additionally use: 5=value3
 *   Row 5 source section: cursor lives at col 2 (LOAD) or col 3 (EDIT, sampler only)
 */
data class InstrumentState(
    val instrument: Instrument,
    val cursorRow: Int = 0,
    val cursorColumn: Int = 1,
    val soundfontPresetName: String = "",
    val soundfontPresetCount: Int = 0,
    val soundfontPresetIndex: Int = 0,
    val appTheme: AppTheme = AppTheme.Companion.CLASSIC
)
