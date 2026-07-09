package com.conanizer.pockettracker.ui.modules

import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.geometry.Size
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.drawscope.DrawScope
import com.conanizer.pockettracker.ui.theme.AppTheme
import com.conanizer.pockettracker.input.CursorContext
import com.conanizer.pockettracker.input.CursorContextFactory
import com.conanizer.pockettracker.ui.TrackerModule
import com.conanizer.pockettracker.core.data.Project
import com.conanizer.pockettracker.core.logic.InputAction
import com.conanizer.pockettracker.core.logic.InstrumentController
import com.conanizer.pockettracker.ui.CHAR_SPACING
import com.conanizer.pockettracker.ui.FONT_SCALE
import com.conanizer.pockettracker.ui.ROW_HEIGHT
import com.conanizer.pockettracker.ui.TEXT_PADDING
import com.conanizer.pockettracker.ui.drawBitmapText
import com.conanizer.pockettracker.ui.drawEqCell
import com.conanizer.pockettracker.ui.toHex2

/**
 * INSTRUMENT POOL SCREEN MODULE (620×392)
 *
 * M8-style overview of all 128 instrument slots with a short list of mixer-related params.
 * Reached above the INSTRUMENT screen (R+DPAD up). The selected row IS the project's
 * `currentInstrument`, so navigating here and jumping back to the INSTRUMENT view stays in sync.
 *
 * Columns (cursorColumn):
 *   0 NAME  — slot id + name; A on an empty slot loads a sample/SF2 (handled in the dispatcher)
 *   1 V(vol)  2 RV(reverb)  3 DE(delay)  4 EQ   — per-instrument mixer values (A+dpad edits)
 *
 * Reorder (M8's EDIT+UP/DOWN on the name column) is intentionally NOT in v1 — deferred to v2.
 */
class InstrumentPoolModule : TrackerModule {
    override val width  = 620
    override val height = 392

    private val VISIBLE_ROWS = 16
    private val NAME_MAX_CHARS = 12  // name column width before the value columns

    // Column x offsets (from module left edge). The module is drawn at screen-x 10 and clipped at
    // screen-x 509, so the visible table spans ~10..509. The four value columns (V/RV/DE/EQ) are
    // packed tightly (50px apart). The block sits 6px left of centre-balanced so the EQ cell on the
    // selected row has room for a trailing ">" (opens the EQ editor) before the right clip.
    private val ID_X   = 14
    private val NAME_X = 56
    private val VOL_X  = 297
    private val REV_X  = 347
    private val DEL_X  = 397
    private val EQ_X   = 447

    override fun DrawScope.draw(x: Int, y: Int, scale: Int, state: Any?) {
        val s = state as? InstrumentPoolState ?: return
        val t = s.appTheme
        val instruments = s.project.instruments

        drawRect(
            color   = Color(t.background),
            topLeft = Offset((x * scale).toFloat(), (y * scale).toFloat()),
            size    = Size((width * scale).toFloat(), (height * scale).toFloat())
        )

        drawBitmapText("INST.POOL", x + ID_X, y + TEXT_PADDING, scale,
            Color(t.textTitle), CHAR_SPACING, FONT_SCALE)

        // Column header row
        val headerY = y + ROW_HEIGHT + 14
        drawBitmapText("# ",  x + ID_X,   headerY, scale, Color(t.textParam), CHAR_SPACING, FONT_SCALE)
        drawBitmapText("NAME", x + NAME_X, headerY, scale, Color(t.textParam), CHAR_SPACING, FONT_SCALE)
        drawBitmapText("V",  x + VOL_X, headerY, scale, Color(t.textParam), CHAR_SPACING, FONT_SCALE)
        drawBitmapText("RV", x + REV_X, headerY, scale, Color(t.textParam), CHAR_SPACING, FONT_SCALE)
        drawBitmapText("DE", x + DEL_X, headerY, scale, Color(t.textParam), CHAR_SPACING, FONT_SCALE)
        drawBitmapText("EQ", x + EQ_X,  headerY, scale, Color(t.textParam), CHAR_SPACING, FONT_SCALE)

        // Scroll so the selected row stays centred (stateless — derived from selection).
        val maxScroll = (instruments.size - VISIBLE_ROWS).coerceAtLeast(0)
        val scroll = (s.selectedInstrument - VISIBLE_ROWS / 2).coerceIn(0, maxScroll)

        val dataTop = y + ROW_HEIGHT + 14 + ROW_HEIGHT
        for (rowIndex in 0 until VISIBLE_ROWS) {
            val slot = scroll + rowIndex
            if (slot >= instruments.size) break
            drawRow(x, dataTop + rowIndex * ROW_HEIGHT, scale, slot, instruments[slot], s, t)
        }
    }

    private fun DrawScope.drawRow(
        x: Int, rowY: Int, scale: Int,
        slot: Int, instrument: com.conanizer.pockettracker.core.data.Instrument,
        s: InstrumentPoolState, t: AppTheme
    ) {
        val isSelectedRow = slot == s.selectedInstrument
        if (isSelectedRow) {
            drawRect(Color(t.rowCursor),
                topLeft = Offset((x * scale).toFloat(), (rowY * scale).toFloat()),
                size    = Size((width * scale).toFloat(), (ROW_HEIGHT * scale).toFloat()))
        }
        val textY = rowY + TEXT_PADDING

        // Per-column color: highlight the value the cursor sits on.
        fun colColor(col: Int) =
            if (isSelectedRow && s.cursorColumn == col) Color(t.textCursor) else Color(t.textValue)

        drawBitmapText(slot.toHex2(), x + ID_X, textY, scale,
            if (isSelectedRow) Color(t.textCursor) else Color(t.textParam), CHAR_SPACING, FONT_SCALE)

        // Empty slots (still auto-named "INSTxx") show a dim placeholder spanning the full name width.
        val nameStr = if (instrument.hasDefaultName()) "_".repeat(NAME_MAX_CHARS) else instrument.name.take(NAME_MAX_CHARS)
        val nameColor = when {
            isSelectedRow && s.cursorColumn == 0 -> Color(t.textCursor)
            instrument.hasDefaultName()          -> Color(t.textEmpty)
            else                                  -> Color(t.textValue)
        }
        drawBitmapText(nameStr, x + NAME_X, textY, scale, nameColor, CHAR_SPACING, FONT_SCALE)

        drawBitmapText(instrument.volume.toHex2(),     x + VOL_X, textY, scale, colColor(1), CHAR_SPACING, FONT_SCALE)
        drawBitmapText(instrument.reverbSend.toHex2(), x + REV_X, textY, scale, colColor(2), CHAR_SPACING, FONT_SCALE)
        drawBitmapText(instrument.delaySend.toHex2(),  x + DEL_X, textY, scale, colColor(3), CHAR_SPACING, FONT_SCALE)
        // EQ value + ">" (opens the EQ editor); the arrow shows only on the selected row, where there's
        // room before the right clip. Shared renderer keeps it identical to every other EQ cell.
        val eqCursor = isSelectedRow && s.cursorColumn == 4
        drawEqCell(x + EQ_X, textY, scale, instrument.eqSlot, eqCursor, t, showArrow = isSelectedRow)
    }

    // ═══════════════════════════════════════════════════════════════════
    // CURSOR CONTEXT  (column 0 = NAME is selection-only; 1-5 are mixer values)
    // ═══════════════════════════════════════════════════════════════════

    fun getCursorContext(state: InstrumentPoolState): CursorContext {
        val inst = state.project.instruments[state.selectedInstrument]
        return when (state.cursorColumn) {
            1 -> CursorContextFactory.hexByte(inst.volume, 0, 255, default = 0xFF)
            2 -> CursorContextFactory.hexByte(inst.reverbSend, 0, 255, default = 0x00)
            3 -> CursorContextFactory.hexByte(inst.delaySend, 0, 255, default = 0x00)
            4 -> CursorContextFactory.hexByte(
                if (inst.eqSlot < 0) 0 else inst.eqSlot,
                min = 0, max = 127, emptyValue = -1,
                canDelete = inst.eqSlot >= 0, canInsert = inst.eqSlot < 0)
            else -> CursorContextFactory.readOnly()  // NAME: A loads on empty, no value edit
        }
    }

    fun handleInput(
        state: InstrumentPoolState,
        action: InputAction,
        instrumentController: InstrumentController
    ): Boolean {
        val inst = state.project.instruments[state.selectedInstrument]
        when (state.cursorColumn) {
            1 -> if (action is InputAction.SET_VALUE) { instrumentController.updateVolume(inst, action.value); return true }
            2 -> if (action is InputAction.SET_VALUE) { instrumentController.updateReverbSend(inst, action.value); return true }
            3 -> if (action is InputAction.SET_VALUE) { instrumentController.updateDelaySend(inst, action.value); return true }
            4 -> when (action) {
                is InputAction.SET_VALUE     -> { instrumentController.updateEqSlot(inst, action.value.coerceIn(0, 127)); return true }
                InputAction.DELETE           -> { instrumentController.updateEqSlot(inst, -1); return true }
                InputAction.INSERT_DEFAULT   -> { instrumentController.updateEqSlot(inst, 0); return true }
                else -> {}
            }
        }
        return false
    }
}

data class InstrumentPoolState(
    val project: Project,
    val selectedInstrument: Int = 0,
    val cursorColumn: Int = 0,
    val appTheme: AppTheme = AppTheme.Companion.CLASSIC
)
