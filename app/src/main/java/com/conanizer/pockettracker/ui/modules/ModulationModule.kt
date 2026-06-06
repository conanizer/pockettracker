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
import com.conanizer.pockettracker.core.data.Instrument
import com.conanizer.pockettracker.core.data.ModDest
import com.conanizer.pockettracker.core.data.ModSlot
import com.conanizer.pockettracker.core.data.ModType
import com.conanizer.pockettracker.core.logic.InputAction
import com.conanizer.pockettracker.ui.drawBitmapText
import com.conanizer.pockettracker.ui.toHex2

/**
 * MODULATION SCREEN MODULE
 *
 * Displays 4 modulation slots per instrument, arranged as 2 pairs:
 * - Pair 0: MOD1 (left) + MOD2 (right)
 * - Pair 1: MOD3 (left) + MOD4 (right)
 *
 * Each slot supports: NONE, AHD, ADSR, LFO (+ future: DRUM, TRIG, TRACKING)
 * Targets: VOL, PAN, PITCH, FINE, CUT, RES, STA, MOD A/R/B
 *
 * Size: 620×392 pixels
 */

data class ModulationState(
    val instrument: Instrument,
    val cursorRow: Int,     // row within current pair (0=TYPE, 1=DEST, ...)
    val cursorPair: Int,    // 0=MOD1+MOD2, 1=MOD3+MOD4
    val cursorSide: Int,    // 0=left (MOD1/MOD3), 1=right (MOD2/MOD4)
    val appTheme: AppTheme = AppTheme.Companion.CLASSIC
) {
    val activeSlotIndex get() = cursorPair * 2 + cursorSide
    val activeSlot get() = instrument.modSlots[activeSlotIndex]

    fun pairRowCount(): Int {
        val left = instrument.modSlots[cursorPair * 2].rowCount()
        val right = instrument.modSlots[cursorPair * 2 + 1].rowCount()
        return maxOf(left, right)
    }
}

class ModulationModule : TrackerModule {
    override val width = 620
    override val height = 392

    private val FONT_SCALE = 3
    private val CHAR_SPACING = 2
    private val ROW_HEIGHT = 21
    private val TEXT_PADDING = 3

    // X positions relative to module x
    private val nameX1 = 10     // Left slot: label column
    private val valX1  = 80     // Left slot: value column
    private val nameX2 = 320    // Right slot: label column
    private val valX2  = 390    // Right slot: value column

    companion object {
        val OSC_SHAPES  = listOf("TRI", "SIN", "RMP+", "RMP-", "EXP+", "EXP-", "SQU+", "SQU-", "RND", "DRK")
        val TRIG_MODES  = listOf("FREE", "RETG", "HOLD", "ONCE")

        // SCALAR is internal-only (used for instrVol/phraseVol routes); not user-selectable.
        val USER_MOD_TYPES = ModType.values().filter { it != ModType.SCALAR }

        fun rowLabels(type: ModType): List<String> = when (type) {
            ModType.NONE     -> listOf("TYPE")
            ModType.AHD      -> listOf("TYPE", "DEST", "AMT", "ATK", "HOLD", "DEC")
            ModType.ADSR     -> listOf("TYPE", "DEST", "AMT", "ATK", "DEC",  "SUS", "REL")
            ModType.LFO      -> listOf("TYPE", "DEST", "AMT", "OSC", "TRIG", "FREQ")
            ModType.DRUM     -> listOf("TYPE", "DEST", "AMT", "ATK", "HOLD", "DEC")
            ModType.TRIG     -> listOf("TYPE", "DEST", "AMT", "ATK", "DEC",  "SUS", "REL")
            ModType.TRACKING -> listOf("TYPE", "DEST", "AMT", "ATK", "DEC")
            ModType.SCALAR   -> listOf("TYPE", "DEST", "AMT")
        }

        fun rowValue(slot: ModSlot, rowIndex: Int, slotIndex: Int = -1): String = when (rowIndex) {
            0 -> slot.type.displayName
            1 -> when {
                slotIndex >= 0 && slot.dest in listOf(ModDest.MOD_AMT, ModDest.MOD_RATE, ModDest.MOD_BOTH) -> {
                    val targetNum = ((slotIndex + 1) % 4) + 1  // 1-indexed target slot (circular)
                    when (slot.dest) {
                        ModDest.MOD_AMT  -> "→M$targetNum AMT"
                        ModDest.MOD_RATE -> "→M$targetNum RTE"
                        else             -> "→M$targetNum B"   // MOD_BOTH
                    }
                }
                else -> slot.dest.displayName
            }
            2 -> slot.amount.toHex2()
            3 -> when (slot.type) {
                ModType.LFO -> OSC_SHAPES.getOrElse(slot.oscShape) { "???" }
                else        -> slot.attack.toHex2()
            }
            4 -> when (slot.type) {
                ModType.LFO                          -> TRIG_MODES.getOrElse(slot.lfoTrigMode) { "???" }
                ModType.AHD, ModType.DRUM            -> slot.hold.toHex2()
                else                                 -> slot.decay.toHex2()
            }
            5 -> when (slot.type) {
                ModType.LFO                          -> slot.lfoFreq.toHex2()
                ModType.AHD, ModType.DRUM            -> slot.decay.toHex2()
                else                                 -> slot.sustain.toHex2()
            }
            6 -> slot.release.toHex2()
            else -> "--"
        }
    }

    // ─────────────────────────────────────────────────────────────────────────
    // DRAW
    // ─────────────────────────────────────────────────────────────────────────

    override fun DrawScope.draw(x: Int, y: Int, scale: Int, state: Any?) {
        val modState = state as? ModulationState ?: return
        val inst = modState.instrument
        val t    = modState.appTheme

        // Module background
        drawRect(
            color = Color(t.background),
            topLeft = Offset((x * scale).toFloat(), (y * scale).toFloat()),
            size = Size((width * scale).toFloat(), (height * scale).toFloat())
        )

        // ─── HEADER ───────────────────────────────────────────────────────────
        val instIdStr = inst.id.toHex2()
        drawBitmapText(
            text = "MOD $instIdStr",
            x = x + nameX1, y = y + TEXT_PADDING,
            scale = scale, color = Color(t.textTitle),
            spacing = CHAR_SPACING, fontScale = FONT_SCALE
        )

        // ─── TWO PAIRS ────────────────────────────────────────────────────────
        // Pair 0 header starts one full row below the module header
        var pairTopY = y + TEXT_PADDING + ROW_HEIGHT + 6

        for (pair in 0..1) {
            val leftSlot  = inst.modSlots[pair * 2]
            val rightSlot = inst.modSlots[pair * 2 + 1]
            val pairRows  = maxOf(leftSlot.rowCount(), rightSlot.rowCount())

            val leftSlotNum  = pair * 2 + 1
            val rightSlotNum = pair * 2 + 2

            // Pair header (MOD1/MOD3 | MOD2/MOD4)
            val headerColor = if (pair == modState.cursorPair) Color(t.textParam) else Color(t.textEmpty)
            drawBitmapText(
                text = "MOD$leftSlotNum",
                x = x + nameX1, y = pairTopY,
                scale = scale, color = headerColor,
                spacing = CHAR_SPACING, fontScale = FONT_SCALE
            )
            drawBitmapText(
                text = "MOD$rightSlotNum",
                x = x + nameX2, y = pairTopY,
                scale = scale, color = headerColor,
                spacing = CHAR_SPACING, fontScale = FONT_SCALE
            )

            val dataStartY = pairTopY + ROW_HEIGHT

            // Data rows
            for (rowIdx in 0 until pairRows) {
                val isCursorRow = (pair == modState.cursorPair && rowIdx == modState.cursorRow)

                if (isCursorRow) {
                    drawRect(
                        color = Color(t.rowCursor),
                        topLeft = Offset(
                            (x * scale).toFloat(),
                            ((dataStartY + rowIdx * ROW_HEIGHT - TEXT_PADDING) * scale).toFloat()
                        ),
                        size = Size((width * scale).toFloat(), (ROW_HEIGHT * scale).toFloat())
                    )
                }

                val isActivePair = (pair == modState.cursorPair)

                // Left slot row
                if (rowIdx < leftSlot.rowCount()) {
                    val labels      = rowLabels(leftSlot.type)
                    val label       = labels.getOrElse(rowIdx) { "" }
                    val value       = rowValue(leftSlot, rowIdx, pair * 2)
                    val isActive    = isActivePair && modState.cursorSide == 0
                    val labelColor  = if (isCursorRow && isActive) Color(t.textCursor) else Color(t.textParam)
                    val valueColor  = if (isCursorRow && isActive) Color(t.textCursor) else Color(t.textValue)

                    drawBitmapText(label, x + nameX1, dataStartY + rowIdx * ROW_HEIGHT, scale, labelColor, CHAR_SPACING, FONT_SCALE)
                    drawBitmapText(value, x + valX1,  dataStartY + rowIdx * ROW_HEIGHT, scale, valueColor, CHAR_SPACING, FONT_SCALE)
                }

                // Right slot row
                if (rowIdx < rightSlot.rowCount()) {
                    val labels      = rowLabels(rightSlot.type)
                    val label       = labels.getOrElse(rowIdx) { "" }
                    val value       = rowValue(rightSlot, rowIdx, pair * 2 + 1)
                    val isActive    = isActivePair && modState.cursorSide == 1
                    val labelColor  = if (isCursorRow && isActive) Color(t.textCursor) else Color(t.textParam)
                    val valueColor  = if (isCursorRow && isActive) Color(t.textCursor) else Color(t.textValue)

                    drawBitmapText(label, x + nameX2, dataStartY + rowIdx * ROW_HEIGHT, scale, labelColor, CHAR_SPACING, FONT_SCALE)
                    drawBitmapText(value, x + valX2,  dataStartY + rowIdx * ROW_HEIGHT, scale, valueColor, CHAR_SPACING, FONT_SCALE)
                }
            }

            // Move pairTopY past this pair's header + data rows + gap
            pairTopY += ROW_HEIGHT + pairRows * ROW_HEIGHT + ROW_HEIGHT
        }
    }

    // ─────────────────────────────────────────────────────────────────────────
    // CURSOR CONTEXT
    // ─────────────────────────────────────────────────────────────────────────

    fun getCursorContext(state: ModulationState): CursorContext {
        val slot = state.activeSlot

        return when (state.cursorRow) {
            0 -> CursorContext(                          // TYPE cycling (SCALAR excluded — internal only)
                valueType = CursorValueType.EFFECT_TYPE,
                capabilities = CursorCapabilities(canIncrement = true, canDecrement = true),
                currentValue = USER_MOD_TYPES.indexOf(slot.type).coerceAtLeast(0),
                minValue = 0,
                maxValue = USER_MOD_TYPES.size - 1,
                smallStep = 1,
                largeStep = 1
            )
            1 -> if (slot.type == ModType.NONE) CursorContextFactory.readOnly()
                 else CursorContext(                     // DEST cycling
                valueType = CursorValueType.EFFECT_TYPE,
                capabilities = CursorCapabilities(canIncrement = true, canDecrement = true),
                currentValue = slot.dest.ordinal,
                minValue = 0,
                maxValue = ModDest.values().size - 1,
                smallStep = 1,
                largeStep = 1
            )
            2 -> if (slot.type == ModType.NONE) CursorContextFactory.readOnly()
                 else CursorContextFactory.hexByte(slot.amount, 0, 255)   // AMT
            3 -> when {
                slot.rowCount() < 4  -> CursorContextFactory.readOnly()
                slot.type == ModType.LFO -> CursorContext(                // OSC shape
                    valueType = CursorValueType.EFFECT_TYPE,
                    capabilities = CursorCapabilities(canIncrement = true, canDecrement = true),
                    currentValue = slot.oscShape,
                    minValue = 0,
                    maxValue = OSC_SHAPES.size - 1,
                    smallStep = 1,
                    largeStep = 1
                )
                else -> CursorContextFactory.hexByte(slot.attack, 0, 255) // ATK
            }
            4 -> when {
                slot.rowCount() < 5  -> CursorContextFactory.readOnly()
                slot.type == ModType.LFO -> CursorContext(                // TRIG mode
                    valueType = CursorValueType.EFFECT_TYPE,
                    capabilities = CursorCapabilities(canIncrement = true, canDecrement = true),
                    currentValue = slot.lfoTrigMode,
                    minValue = 0,
                    maxValue = TRIG_MODES.size - 1,
                    smallStep = 1,
                    largeStep = 1
                )
                slot.type == ModType.AHD || slot.type == ModType.DRUM ->
                    CursorContextFactory.hexByte(slot.hold, 0, 255)       // HOLD
                else -> CursorContextFactory.hexByte(slot.decay, 0, 255)  // DEC (ADSR)
            }
            5 -> when {
                slot.rowCount() < 6  -> CursorContextFactory.readOnly()
                slot.type == ModType.LFO ->
                    CursorContextFactory.hexByte(slot.lfoFreq, 0, 255)    // FREQ
                slot.type == ModType.AHD || slot.type == ModType.DRUM ->
                    CursorContextFactory.hexByte(slot.decay, 0, 255)      // DEC (AHD)
                else -> CursorContextFactory.hexByte(slot.sustain, 0, 255)// SUS (ADSR)
            }
            6 -> when {
                slot.rowCount() < 7  -> CursorContextFactory.readOnly()
                else -> CursorContextFactory.hexByte(slot.release, 0, 255)// REL (ADSR)
            }
            else -> CursorContextFactory.readOnly()
        }
    }

    // ─────────────────────────────────────────────────────────────────────────
    // INPUT HANDLING
    // ─────────────────────────────────────────────────────────────────────────

    fun handleInput(state: ModulationState, action: InputAction): InputResult {
        val slot = state.activeSlot

        when (action) {
            is InputAction.SET_VALUE -> {
                when (state.cursorRow) {
                    0 -> slot.type = USER_MOD_TYPES.getOrElse(
                            action.value.coerceIn(0, USER_MOD_TYPES.size - 1)
                        ) { ModType.NONE }
                    1 -> if (slot.type != ModType.NONE) slot.dest = ModDest.values().getOrElse(
                            action.value.coerceIn(0, ModDest.values().size - 1)
                        ) { ModDest.NONE }
                    2 -> if (slot.type != ModType.NONE) slot.amount = action.value.coerceIn(0, 255)
                    3 -> when {
                        slot.rowCount() < 4  -> { }
                        slot.type == ModType.LFO ->
                            slot.oscShape = action.value.coerceIn(0, OSC_SHAPES.size - 1)
                        else -> slot.attack = action.value.coerceIn(0, 255)
                    }
                    4 -> when {
                        slot.rowCount() < 5  -> { }
                        slot.type == ModType.LFO ->
                            slot.lfoTrigMode = action.value.coerceIn(0, TRIG_MODES.size - 1)
                        slot.type == ModType.AHD || slot.type == ModType.DRUM ->
                            slot.hold = action.value.coerceIn(0, 255)
                        else -> slot.decay = action.value.coerceIn(0, 255)
                    }
                    5 -> when {
                        slot.rowCount() < 6  -> { }
                        slot.type == ModType.LFO ->
                            slot.lfoFreq = action.value.coerceIn(0, 255)
                        slot.type == ModType.AHD || slot.type == ModType.DRUM ->
                            slot.decay = action.value.coerceIn(0, 255)
                        else -> slot.sustain = action.value.coerceIn(0, 255)
                    }
                    6 -> if (slot.rowCount() >= 7) slot.release = action.value.coerceIn(0, 255)
                }
            }
            is InputAction.DELETE -> {
                // A+B: reset entire active slot to defaults
                state.instrument.modSlots[state.activeSlotIndex] = ModSlot()
            }
            else -> return InputResult(modified = false)
        }
        return InputResult(modified = true)
    }

    data class InputResult(val modified: Boolean)
}
