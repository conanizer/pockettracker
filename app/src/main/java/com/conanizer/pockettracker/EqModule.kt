package com.conanizer.pockettracker

import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.geometry.Size
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.drawscope.DrawScope
import com.conanizer.pockettracker.core.data.EqBand
import com.conanizer.pockettracker.core.data.Project
import com.conanizer.pockettracker.core.logic.InputAction

data class EqState(
    val project:       Project,
    val slotIndex:     Int,
    val cursorRow:     Int,
    val callerContext: EqCallerContext
)

/**
 * EQ EDITOR SCREEN MODULE
 *
 * Full-screen editor for a single EQ preset (3 bands × TYPE/FREQ/GAIN/Q).
 * Replaces the normal screen content while open.
 *
 * Cursor rows (0-11):
 *   0   Band 1 TYPE   (0-6)
 *   1   Band 1 FREQ   (00-FF)
 *   2   Band 1 GAIN   (00-FF)
 *   3   Band 1 Q      (00-FF)
 *   4   Band 2 TYPE
 *   5   Band 2 FREQ
 *   6   Band 2 GAIN
 *   7   Band 2 Q
 *   8   Band 3 TYPE
 *   9   Band 3 FREQ
 *   10  Band 3 GAIN
 *   11  Band 3 Q
 *
 * Controls:
 *   DPAD UP/DOWN    — navigate rows
 *   A + DPAD UP/DN  — +1 / -1  (TYPE cycles through band type names)
 *   A + DPAD LT/RT  — −16 / +16 (TYPE: ±1 same as small step)
 *   B + DPAD LT/RT  — prev / next EQ preset slot; updates caller's slot reference
 *   SELECT          — close
 *
 * Size: 620×392 pixels
 */
class EqModule : TrackerModule {
    override val width  = 620
    override val height = 392

    private val FONT_SCALE   = 3
    private val CHAR_SPACING = 2
    private val ROW_HEIGHT   = 21
    private val TEXT_PADDING = 3
    private val LABEL_X      = 10
    private val VALUE_X      = 120

    companion object {
        const val ROW_B1_TYPE  = 0
        const val ROW_B1_FREQ  = 1
        const val ROW_B1_GAIN  = 2
        const val ROW_B1_Q     = 3
        const val ROW_B2_TYPE  = 4
        const val ROW_B2_FREQ  = 5
        const val ROW_B2_GAIN  = 6
        const val ROW_B2_Q     = 7
        const val ROW_B3_TYPE  = 8
        const val ROW_B3_FREQ  = 9
        const val ROW_B3_GAIN  = 10
        const val ROW_B3_Q     = 11
        const val MAX_CURSOR_ROW = 11

        val PARAM_LABELS = listOf("TYPE", "FREQ", "GAIN", "Q")

        // Visual layout (each step = ROW_HEIGHT px):
        //  0  "EQ xx" header
        //  1  [spacer]
        //  2  "BAND 1" section
        //  3  TYPE   ← cursorRow 0
        //  4  FREQ   ← cursorRow 1
        //  5  GAIN   ← cursorRow 2
        //  6  Q      ← cursorRow 3
        //  7  "BAND 2" section
        //  8  TYPE   ← cursorRow 4
        //  9  FREQ   ← cursorRow 5
        // 10  GAIN   ← cursorRow 6
        // 11  Q      ← cursorRow 7
        // 12  "BAND 3" section
        // 13  TYPE   ← cursorRow 8
        // 14  FREQ   ← cursorRow 9
        // 15  GAIN   ← cursorRow 10
        // 16  Q      ← cursorRow 11
        // 17  hint
        private val CURSOR_TO_VIS = intArrayOf(3, 4, 5, 6, 8, 9, 10, 11, 13, 14, 15, 16)
    }

    data class InputResult(
        val modified: Boolean,
        val eqBandChanged: Boolean = false
    )

    override fun DrawScope.draw(x: Int, y: Int, scale: Int, state: Any?) {
        val s    = state as? EqState ?: return
        val proj = s.project
        val preset = proj.eqPresets.getOrNull(s.slotIndex)

        // Background
        drawRect(
            color   = Color(0xFF0a0a0a),
            topLeft = Offset((x * scale).toFloat(), (y * scale).toFloat()),
            size    = Size((width * scale).toFloat(), (height * scale).toFloat())
        )

        // Row highlight for selected cursor row
        val selVis = CURSOR_TO_VIS.getOrElse(s.cursorRow) { -1 }
        if (selVis >= 0) {
            drawRect(
                color   = Color(0xFF333333),
                topLeft = Offset((x * scale).toFloat(), ((y + selVis * ROW_HEIGHT) * scale).toFloat()),
                size    = Size((width * scale).toFloat(), (ROW_HEIGHT * scale).toFloat())
            )
        }

        fun rowY(vis: Int) = y + TEXT_PADDING + vis * ROW_HEIGHT

        fun valueColor(isSel: Boolean): Color = if (isSel) Color.Yellow else Color.White

        // ── Header ────────────────────────────────────────────────────────────
        val slotText = "EQ ${s.slotIndex.toHex2()}"
        drawBitmapText(slotText, x + LABEL_X, rowY(0), scale, Color.Cyan, CHAR_SPACING, FONT_SCALE)

        val callerLabel = when (val ctx = s.callerContext) {
            is EqCallerContext.MasterEq       -> "MASTER"
            is EqCallerContext.ReverbInputEq  -> "REV IN"
            is EqCallerContext.DelayInputEq   -> "DLY IN"
            is EqCallerContext.InstrumentEq   -> "INST ${ctx.instrId.toHex2()}"
            is EqCallerContext.SampleEditorFx -> "SAMPLE"
        }
        val charW = 5 * FONT_SCALE + CHAR_SPACING   // 17px
        val callerX = x + width - LABEL_X - callerLabel.length * charW
        drawBitmapText(callerLabel, callerX, rowY(0), scale, Color(0xFF888888), CHAR_SPACING, FONT_SCALE)
        // vis 1 = spacer

        // ── Band sections ─────────────────────────────────────────────────────
        for (bandIdx in 0..2) {
            val bandBase = bandIdx * 4          // cursor row base for this band
            val visBand  = 2 + bandIdx * 5      // vis row for section label (2, 7, 12)

            drawBitmapText("BAND ${bandIdx + 1}", x + LABEL_X, rowY(visBand), scale, Color.Cyan, CHAR_SPACING, FONT_SCALE)

            val band: EqBand? = preset?.bands?.getOrNull(bandIdx)

            for (paramIdx in 0..3) {
                val curRow = bandBase + paramIdx
                val visRow = visBand + 1 + paramIdx
                val isSel  = s.cursorRow == curRow

                drawBitmapText(PARAM_LABELS[paramIdx], x + LABEL_X, rowY(visRow), scale, Color.Gray, CHAR_SPACING, FONT_SCALE)

                val valueText = when {
                    band == null -> "--"
                    paramIdx == 0 -> EQ_BAND_TYPE_NAMES.getOrElse(band.type) { "???" }
                    paramIdx == 1 -> band.freq.toHex2()
                    paramIdx == 2 -> band.gain.toHex2()
                    else          -> band.q.toHex2()
                }
                drawBitmapText(valueText, x + VALUE_X, rowY(visRow), scale, valueColor(isSel), CHAR_SPACING, FONT_SCALE)
            }
        }

        // ── Hint ──────────────────────────────────────────────────────────────
        drawBitmapText("B+←→ PRESET  SELECT=CLOSE", x + LABEL_X, rowY(17), scale, Color(0xFF555555), CHAR_SPACING, FONT_SCALE)
    }

    fun getCursorContext(state: EqState): CursorContext {
        val preset = state.project.eqPresets.getOrNull(state.slotIndex)
            ?: return CursorContextFactory.none()
        val band = preset.bands.getOrNull(state.cursorBand)
            ?: return CursorContextFactory.none()
        return when (state.cursorParam) {
            0 -> CursorContext(
                valueType    = CursorValueType.HEX_BYTE,
                capabilities = CursorCapabilities(
                    canIncrement     = true,
                    canDecrement     = true,
                    canIncrementFast = true,
                    canDecrementFast = true
                ),
                currentValue = band.type,
                minValue = 0, maxValue = EQ_BAND_TYPE_NAMES.size - 1,
                smallStep = 1, largeStep = 1
            )
            1 -> CursorContextFactory.hexByte(band.freq, min = 0, max = 255)
            2 -> CursorContextFactory.hexByte(band.gain, min = 0, max = 255)
            3 -> CursorContextFactory.hexByte(band.q,    min = 0, max = 255)
            else -> CursorContextFactory.none()
        }
    }

    fun handleInput(
        state: EqState,
        action: InputAction,
        onProjectModified: () -> Unit
    ): InputResult {
        if (action !is InputAction.SET_VALUE) return InputResult(modified = false)
        val preset = state.project.eqPresets.getOrNull(state.slotIndex)
            ?: return InputResult(modified = false)
        val band = preset.bands.getOrNull(state.cursorBand)
            ?: return InputResult(modified = false)
        when (state.cursorParam) {
            0 -> band.type = action.value.coerceIn(0, EQ_BAND_TYPE_NAMES.size - 1)
            1 -> band.freq = action.value.coerceIn(0, 255)
            2 -> band.gain = action.value.coerceIn(0, 255)
            3 -> band.q    = action.value.coerceIn(0, 255)
        }
        onProjectModified()
        return InputResult(modified = true, eqBandChanged = true)
    }
}

private val EqState.cursorBand:  Int get() = cursorRow / 4
private val EqState.cursorParam: Int get() = cursorRow % 4
