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
import com.conanizer.pockettracker.ui.CHAR_SPACING
import com.conanizer.pockettracker.ui.FONT_SCALE
import com.conanizer.pockettracker.ui.ROW_HEIGHT
import com.conanizer.pockettracker.ui.TEXT_PADDING
import com.conanizer.pockettracker.ui.drawBitmapText
import com.conanizer.pockettracker.ui.drawEqCell
import com.conanizer.pockettracker.ui.toHex2

data class EffectState(
    val project: Project,
    val cursorRow: Int,  // 0-9
    val appTheme: AppTheme = AppTheme.Companion.CLASSIC
)

/**
 * EFFECTS SCREEN MODULE
 *
 * Displays master bus FX selector, reverb send controls,
 * delay send controls, and master EQ slot.
 *
 * Cursor rows (effectsCursorRow):
 *   0  MASTER FX TYPE  (OTT / DUST)
 *   1  REVERB SIZE     (feedback 00-FF)
 *   2  REVERB DAMP     (damping  00-FF)
 *   3  REVERB INP EQ   (-1=off, 00-7F slot)
 *   4  DELAY TIME      (00-FF free / 00-0B sync subdivision)
 *   5  DELAY FDBK      (00-FF)
 *   6  DELAY REV       (00-FF delay→reverb send level)
 *   7  DELAY INP EQ    (-1=off, 00-7F slot)
 *
 * Size: 620×392 pixels
 */
class EffectModule : TrackerModule {
    override val width  = 620
    override val height = 392

    private val LABEL_X      = 10
    private val VALUE_X      = 120

    companion object {
        const val ROW_MASTER_TYPE = 0
        const val ROW_REV_SIZE    = 1
        const val ROW_REV_DAMP    = 2
        const val ROW_REV_EQ      = 3
        const val ROW_DLY_TIME    = 4
        const val ROW_DLY_FDBK    = 5
        const val ROW_DLY_REV     = 6
        const val ROW_DLY_EQ      = 7
        const val MAX_CURSOR_ROW  = 7

        // Subdivision names matching kDelaySyncBeats[] in delay-module.h
        val DELAY_SYNC_NAMES = listOf(
            "1/1",  "1/2",  "1/4",   "1/8",
            "1/16", "1/32",
            "1/4T", "1/8T", "1/16T",
            "1/4.", "1/8.", "1/16."
        )

        // Maps effectsCursorRow → visual row index used for background highlight
        // Visual layout (each step = ROW_HEIGHT px):
        //  0  "EFFECTS" header
        //  1  [spacer]
        //  2  "MASTER FX" section label
        //  3  TYPE                   ← cursorRow 0
        //  4  [spacer]
        //  5  "REVERB" section label
        //  6  SIZE                   ← cursorRow 1
        //  7  DAMP                   ← cursorRow 2
        //  8  INP EQ                 ← cursorRow 3
        //  9  [spacer]
        // 10  "DELAY" section label
        // 11  TIME                   ← cursorRow 4
        // 12  FDBK                   ← cursorRow 5
        // 13  REV                    ← cursorRow 6
        // 14  INP EQ                 ← cursorRow 7
        private val CURSOR_TO_VIS = intArrayOf(3, 6, 7, 8, 11, 12, 13, 14)
    }

    data class InputResult(
        val modified: Boolean,
        val masterFxChanged:        Boolean = false,
        val reverbParamsChanged:    Boolean = false,
        val delayParamsChanged:     Boolean = false,
        val delayReverbSendChanged: Boolean = false,
        val masterEqChanged:        Boolean = false
    )

    override fun DrawScope.draw(x: Int, y: Int, scale: Int, state: Any?) {
        val s    = state as? EffectState ?: return
        val proj = s.project
        val t    = s.appTheme

        drawRect(
            color    = Color(t.background),
            topLeft  = Offset((x * scale).toFloat(), (y * scale).toFloat()),
            size     = Size((width * scale).toFloat(), (height * scale).toFloat())
        )

        val selVis = CURSOR_TO_VIS.getOrElse(s.cursorRow) { -1 }
        if (selVis >= 0) {
            drawRect(
                color    = Color(t.rowCursor),
                topLeft  = Offset((x * scale).toFloat(), ((y + selVis * ROW_HEIGHT) * scale).toFloat()),
                size     = Size((width * scale).toFloat(), (ROW_HEIGHT * scale).toFloat())
            )
        }

        fun rowY(vis: Int) = y + TEXT_PADDING + vis * ROW_HEIGHT

        fun valueColor(isSel: Boolean, isEmpty: Boolean = false): Color = when {
            isEmpty -> Color(t.textEmpty)
            isSel   -> Color(t.textCursor)
            else    -> Color(t.textValue)
        }

        drawBitmapText("EFFECTS", x + LABEL_X, rowY(0), scale, Color(t.textTitle), CHAR_SPACING, FONT_SCALE)

        drawBitmapText("MASTER FX", x + LABEL_X, rowY(2), scale, Color(t.textTitle), CHAR_SPACING, FONT_SCALE)

        val typeSel = s.cursorRow == ROW_MASTER_TYPE
        drawBitmapText("TYPE", x + LABEL_X, rowY(3), scale, Color(t.textParam), CHAR_SPACING, FONT_SCALE)
        drawBitmapText(
            if (proj.masterBusFx == 0) "OTT" else "DUST",
            x + VALUE_X, rowY(3), scale, valueColor(typeSel), CHAR_SPACING, FONT_SCALE
        )

        drawBitmapText("REVERB", x + LABEL_X, rowY(5), scale, Color(t.textTitle), CHAR_SPACING, FONT_SCALE)

        val revSizeSel = s.cursorRow == ROW_REV_SIZE
        drawBitmapText("SIZE",   x + LABEL_X, rowY(6), scale, Color(t.textParam), CHAR_SPACING, FONT_SCALE)
        drawBitmapText(proj.reverbFeedback.toHex2(), x + VALUE_X, rowY(6), scale, valueColor(revSizeSel), CHAR_SPACING, FONT_SCALE)

        val revDampSel = s.cursorRow == ROW_REV_DAMP
        drawBitmapText("DAMP",   x + LABEL_X, rowY(7), scale, Color(t.textParam), CHAR_SPACING, FONT_SCALE)
        drawBitmapText(proj.reverbDamp.toHex2(),     x + VALUE_X, rowY(7), scale, valueColor(revDampSel), CHAR_SPACING, FONT_SCALE)

        val revEqSel = s.cursorRow == ROW_REV_EQ
        drawBitmapText("INP EQ", x + LABEL_X, rowY(8), scale, Color(t.textParam), CHAR_SPACING, FONT_SCALE)
        drawEqCell(x + VALUE_X, rowY(8), scale, proj.reverbInputEq, revEqSel, t)

        drawBitmapText("DELAY",  x + LABEL_X, rowY(10), scale, Color(t.textTitle), CHAR_SPACING, FONT_SCALE)

        val dlyTimeSel = s.cursorRow == ROW_DLY_TIME
        drawBitmapText("TIME",   x + LABEL_X, rowY(11), scale, Color(t.textParam), CHAR_SPACING, FONT_SCALE)
        val timeText = if (proj.delaySync) {
            DELAY_SYNC_NAMES.getOrElse(proj.delayTime.coerceIn(0, 11)) { "??" }
        } else {
            proj.delayTime.toHex2()
        }
        drawBitmapText(timeText, x + VALUE_X, rowY(11), scale, valueColor(dlyTimeSel), CHAR_SPACING, FONT_SCALE)

        val dlyFdbkSel = s.cursorRow == ROW_DLY_FDBK
        drawBitmapText("FDBK",   x + LABEL_X, rowY(12), scale, Color(t.textParam), CHAR_SPACING, FONT_SCALE)
        drawBitmapText(proj.delayFeedback.toHex2(),  x + VALUE_X, rowY(12), scale, valueColor(dlyFdbkSel), CHAR_SPACING, FONT_SCALE)

        val dlyRevSel = s.cursorRow == ROW_DLY_REV
        drawBitmapText("REV",    x + LABEL_X, rowY(13), scale, Color(t.textParam), CHAR_SPACING, FONT_SCALE)
        drawBitmapText(proj.delayReverbSend.toHex2(), x + VALUE_X, rowY(13), scale, valueColor(dlyRevSel), CHAR_SPACING, FONT_SCALE)

        val dlyEqSel = s.cursorRow == ROW_DLY_EQ
        drawBitmapText("INP EQ", x + LABEL_X, rowY(14), scale, Color(t.textParam), CHAR_SPACING, FONT_SCALE)
        drawEqCell(x + VALUE_X, rowY(14), scale, proj.delayInputEq, dlyEqSel, t)
    }

    fun getCursorContext(state: EffectState): CursorContext {
        val proj = state.project
        return when (state.cursorRow) {
            ROW_MASTER_TYPE -> CursorContext(
                valueType = CursorValueType.HEX_BYTE,
                capabilities = CursorCapabilities(canIncrement = true, canDecrement = true),
                currentValue = proj.masterBusFx,
                minValue = 0, maxValue = 1, smallStep = 1, largeStep = 1
            )
            ROW_REV_SIZE -> CursorContextFactory.hexByte(proj.reverbFeedback,   min = 0, max = 255, default = 0x60)
            ROW_REV_DAMP -> CursorContextFactory.hexByte(proj.reverbDamp,       min = 0, max = 255, default = 0x80)
            ROW_REV_EQ   -> CursorContextFactory.hexByte(
                currentValue = if (proj.reverbInputEq < 0) -1 else proj.reverbInputEq,
                min = 0, max = 127, emptyValue = -1, canDelete = true, canInsert = true
            )
            ROW_DLY_TIME -> if (proj.delaySync)
                CursorContextFactory.hexByte(proj.delayTime.coerceIn(0, 11), min = 0, max = 11)
            else
                CursorContextFactory.hexByte(proj.delayTime, min = 0, max = 255, default = 0x40)
            ROW_DLY_FDBK -> CursorContextFactory.hexByte(proj.delayFeedback,   min = 0, max = 255, default = 0x60)
            ROW_DLY_REV  -> CursorContextFactory.hexByte(proj.delayReverbSend, min = 0, max = 255, default = 0x00)
            ROW_DLY_EQ   -> CursorContextFactory.hexByte(
                currentValue = if (proj.delayInputEq < 0) -1 else proj.delayInputEq,
                min = 0, max = 127, emptyValue = -1, canDelete = true, canInsert = true
            )
            else -> CursorContextFactory.none()
        }
    }

    fun handleInput(
        state: EffectState,
        action: InputAction,
        onProjectModified: () -> Unit
    ): InputResult {
        val proj = state.project

        when (state.cursorRow) {
            ROW_MASTER_TYPE -> if (action is InputAction.SET_VALUE) {
                proj.masterBusFx = action.value.coerceIn(0, 1)
                onProjectModified()
                return InputResult(modified = true, masterFxChanged = true)
            }
            ROW_REV_SIZE -> if (action is InputAction.SET_VALUE) {
                proj.reverbFeedback = action.value.coerceIn(0, 255)
                onProjectModified()
                return InputResult(modified = true, reverbParamsChanged = true)
            }
            ROW_REV_DAMP -> if (action is InputAction.SET_VALUE) {
                proj.reverbDamp = action.value.coerceIn(0, 255)
                onProjectModified()
                return InputResult(modified = true, reverbParamsChanged = true)
            }
            ROW_REV_EQ -> {
                when (action) {
                    is InputAction.SET_VALUE     -> proj.reverbInputEq = action.value.coerceIn(0, 127)
                    InputAction.DELETE           -> proj.reverbInputEq = -1
                    InputAction.INSERT_DEFAULT   -> proj.reverbInputEq = 0
                    else                         -> return InputResult(modified = false)
                }
                onProjectModified()
                return InputResult(modified = true, reverbParamsChanged = true)
            }
            ROW_DLY_TIME -> if (action is InputAction.SET_VALUE) {
                proj.delayTime = if (proj.delaySync) action.value.coerceIn(0, 11)
                                 else                action.value.coerceIn(0, 255)
                onProjectModified()
                return InputResult(modified = true, delayParamsChanged = true)
            }
            ROW_DLY_FDBK -> if (action is InputAction.SET_VALUE) {
                proj.delayFeedback = action.value.coerceIn(0, 255)
                onProjectModified()
                return InputResult(modified = true, delayParamsChanged = true)
            }
            ROW_DLY_REV -> if (action is InputAction.SET_VALUE) {
                proj.delayReverbSend = action.value.coerceIn(0, 255)
                onProjectModified()
                return InputResult(modified = true, delayReverbSendChanged = true)
            }
            ROW_DLY_EQ -> {
                when (action) {
                    is InputAction.SET_VALUE     -> proj.delayInputEq = action.value.coerceIn(0, 127)
                    InputAction.DELETE           -> proj.delayInputEq = -1
                    InputAction.INSERT_DEFAULT   -> proj.delayInputEq = 0
                    else                         -> return InputResult(modified = false)
                }
                onProjectModified()
                return InputResult(modified = true, delayParamsChanged = true)
            }
        }
        return InputResult(modified = false)
    }
}
