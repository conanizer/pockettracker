package com.conanizer.pockettracker.ui.modules

import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.geometry.Size
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.drawscope.DrawScope
import com.conanizer.pockettracker.BuildConfig
import com.conanizer.pockettracker.ui.theme.AppTheme
import com.conanizer.pockettracker.ui.theme.DeviceSkin
import com.conanizer.pockettracker.input.CursorCapabilities
import com.conanizer.pockettracker.input.CursorContext
import com.conanizer.pockettracker.input.CursorContextFactory
import com.conanizer.pockettracker.input.CursorValueType
import com.conanizer.pockettracker.platform.android.DeviceAdapter
import com.conanizer.pockettracker.ui.TrackerModule
import com.conanizer.pockettracker.ui.theme.VisualizerType
import com.conanizer.pockettracker.core.logic.InputAction
import com.conanizer.pockettracker.ui.drawBitmapText
import com.conanizer.pockettracker.ui.toHex2

/**
 * SETTINGS SCREEN MODULE
 *
 * Rows (0-11):
 *   0  — LAYOUT    (col1 A+dpad: FULLSCREEN / LANDSCAPE / PORTRAIT;
 *                   col2 A+dpad: theme/skin cycle — only when the layout is skinned, e.g. PORTRAIT → NORMAL/DARK)
 *   1  — SCALING   (A+dpad: INT / BILINEAR)
 *
 * All value rows change via A+dpad. Single A is reserved for actions only:
 * row 9 (THEME, opens editor) and row 10 (TEMPLATE, save/clear).
 *   2  — OVERLAY   (col1: file name / OFF; col2: STR 00-FF)
 *   3  — BTN SOUND (col1: ON/OFF; col2: VOL 00-FF)
 *   4  — BTN VIBRO (col1: ON/OFF; col2: POW 00-FF)
 *   5  — KB INSERT (single cycling value: BEFORE=1 / AFTER=0)
 *   6  — CURSOR    (REMEMBER=1 / REFRESH=0)
 *   7  — NOTE PREV (ON / OFF)
 *   8  — VISUALIZER
 *   9  — THEME     (opens editor)
 *   10 — TEMPLATE  (SAVE / CLEAR)
 *   11 — RESUME    (ASK=show RECOVER WORK? prompt / AUTO=silently restore autosave)
 */
class SettingsModule : TrackerModule {

    companion object {
        // Which device skins (themes) are available for a given layout. Only the retro PORTRAIT layout
        // is skinned today; FULLSCREEN/LANDSCAPE use plain virtual buttons with no theme.
        fun skinsForLayout(mode: DeviceAdapter.LayoutMode): List<DeviceSkin> =
            if (mode == DeviceAdapter.LayoutMode.TOUCH_PORTRAIT2) DeviceSkin.ALL else emptyList()
    }

    override val width = 510
    override val height = 392

    private val FONT_SCALE = 3
    private val CHAR_SPACING = 2
    private val ROW_HEIGHT = 21
    private val TEXT_PADDING = 3

    // Column positions
    private val NAME_X_OFFSET  = 10
    private val VAL1_X_OFFSET  = 190   // primary value
    private val SUBLABEL_OFFSET = 355   // secondary column label (STR / VOL / POW)
    private val VAL2_X_OFFSET  = 408   // secondary value (hex)

    override fun DrawScope.draw(x: Int, y: Int, scale: Int, state: Any?) {
        val s = state as? SettingsState ?: return
        val t = s.appTheme

        drawRect(
            color = Color(t.background),
            topLeft = Offset((x * scale).toFloat(), (y * scale).toFloat()),
            size = Size((width * scale).toFloat(), (height * scale).toFloat())
        )

        val nameColumnX  = x + NAME_X_OFFSET
        val val1ColumnX  = x + VAL1_X_OFFSET
        val subLabelX    = x + SUBLABEL_OFFSET
        val val2ColumnX  = x + VAL2_X_OFFSET

        var rowY = y + TEXT_PADDING
        drawBitmapText("SETTINGS", nameColumnX, rowY, scale,
            Color(t.textTitle), CHAR_SPACING, FONT_SCALE)
        rowY += ROW_HEIGHT + 14

        var row = 0

        // ── ROW 0: LAYOUT (+ theme switch when the layout is skinned) ──
        val layoutText = when (s.layoutMode) {
            DeviceAdapter.LayoutMode.FULL            -> "FULLSCREEN"
            DeviceAdapter.LayoutMode.TOUCH_PORTRAIT  -> "T.PORT"
            DeviceAdapter.LayoutMode.TOUCH_LANDSCAPE -> "LANDSCAPE"
            DeviceAdapter.LayoutMode.TOUCH_PORTRAIT2 -> "PORTRAIT"
        }
        if (s.availableSkins.isNotEmpty()) {
            // Second value = theme name (NORM
            // / DARK), drawn with no sublabel header.
            val skinText = s.availableSkins.firstOrNull { it.id == s.currentSkinId }?.displayName ?: ""
            drawDualParamRow(x, rowY, scale, nameColumnX, val1ColumnX, subLabelX, val2ColumnX, t,
                "LAYOUT", layoutText, "", skinText,
                s.cursorRow == row, s.cursorColumn)
        } else {
            drawParameterRow(x, rowY, scale, nameColumnX, val1ColumnX, t,
                "LAYOUT", layoutText,
                s.cursorRow == row && s.cursorColumn == 0,
                s.cursorRow == row && s.cursorColumn == 1)
        }
        rowY += ROW_HEIGHT; row++

        // ── ROW 1: SCALING ─────────────────────────────────────────────
        val scalingText = when (s.scalingMode) {
            DeviceAdapter.ScalingMode.INTEGER  -> "INT"
            DeviceAdapter.ScalingMode.BILINEAR -> "BILINEAR"
        }
        drawParameterRow(x, rowY, scale, nameColumnX, val1ColumnX, t,
            "SCALING", scalingText,
            s.cursorRow == row && s.cursorColumn == 0,
            s.cursorRow == row && s.cursorColumn == 1)
        rowY += ROW_HEIGHT; row++

        // ── ROW 2: OVERLAY (debug-only — still raw, hidden in release) ──
        if (BuildConfig.DEBUG) {
            val overlayDisplayName = if (s.overlayName == "OFF") "OFF"
                                     else s.overlayName.uppercase().take(8)
            drawDualParamRow(x, rowY, scale, nameColumnX, val1ColumnX, subLabelX, val2ColumnX, t,
                "OVERLAY", overlayDisplayName, "STR", s.overlayStrength.toHex2(),
                s.cursorRow == row, s.cursorColumn)
            rowY += ROW_HEIGHT * 2
        } else {
            rowY += ROW_HEIGHT   // keep the group gap OVERLAY's spacer used to provide
        }
        row++

        // ── ROW 3: BTN SOUND ───────────────────────────────────────────
        drawDualParamRow(x, rowY, scale, nameColumnX, val1ColumnX, subLabelX, val2ColumnX, t,
            "BTN SOUND", if (s.buttonSoundEnabled) "ON" else "OFF",
            "VOL", s.buttonSoundVolume.toHex2(),
            s.cursorRow == row, s.cursorColumn)
        rowY += ROW_HEIGHT; row++

        // ── ROW 4: BTN VIBRO ───────────────────────────────────────────
        drawDualParamRow(x, rowY, scale, nameColumnX, val1ColumnX, subLabelX, val2ColumnX, t,
            "BTN VIBRO", if (s.buttonVibroEnabled) "ON" else "OFF",
            "POW", s.vibroPower.toHex2(),
            s.cursorRow == row, s.cursorColumn)
        rowY += ROW_HEIGHT * 2; row++

        // ── ROW 5: KB INSERT ───────────────────────────────────────────
        drawParameterRow(x, rowY, scale, nameColumnX, val1ColumnX, t,
            "KB INSERT", if (s.insertBefore) "BEFORE" else "AFTER",
            s.cursorRow == row && s.cursorColumn == 0,
            s.cursorRow == row && s.cursorColumn == 1)
        rowY += ROW_HEIGHT; row++

        // ── ROW 6: CURSOR ──────────────────────────────────────────────
        drawParameterRow(x, rowY, scale, nameColumnX, val1ColumnX, t,
            "CURSOR", if (s.cursorRemember) "REMEMBER" else "REFRESH",
            s.cursorRow == row && s.cursorColumn == 0,
            s.cursorRow == row && s.cursorColumn == 1)
        rowY += ROW_HEIGHT; row++

        // ── ROW 7: NOTE PREVIEW ────────────────────────────────────────
        drawParameterRow(x, rowY, scale, nameColumnX, val1ColumnX, t,
            "NOTE PREV", if (s.notePreviewEnabled) "ON" else "OFF",
            s.cursorRow == row && s.cursorColumn == 0,
            s.cursorRow == row && s.cursorColumn == 1)
        rowY += ROW_HEIGHT * 2; row++

        // ── ROW 8: VISUALIZER ─────────────────────────────────────────
        val vizText = when (s.visualizerType) {
            VisualizerType.SCOPE          -> "SCOPE"
            VisualizerType.FLAT           -> "FLAT"
            VisualizerType.OCTA           -> "OCTA"
            VisualizerType.OCTA_FULL      -> "OCTA.F"
            VisualizerType.SPECTRUM       -> "SPECT"
            VisualizerType.SPECTRUM_PEAKS -> "SPCT.P"
        }
        drawParameterRow(x, rowY, scale, nameColumnX, val1ColumnX, t,
            "VISUALIZER", vizText,
            s.cursorRow == row && s.cursorColumn == 0,
            s.cursorRow == row && s.cursorColumn == 1)
        rowY += ROW_HEIGHT; row++

        // ── ROW 9: THEME ───────────────────────────────────────────────
        drawParameterRow(x, rowY, scale, nameColumnX, val1ColumnX, t,
            "THEME", "${s.currentThemeName} >",
            s.cursorRow == row && s.cursorColumn == 0,
            s.cursorRow == row && s.cursorColumn == 1)
        rowY += ROW_HEIGHT * 2; row++

        // ── ROW 10: SONG TEMPLATE ─────────────────────────────────────
        drawTemplateRow(x, rowY, scale, nameColumnX, val1ColumnX, s, row)
        rowY += ROW_HEIGHT; row++

        // ── ROW 11: RESUME (autosave recovery behaviour) ──────────────
        drawParameterRow(x, rowY, scale, nameColumnX, val1ColumnX, t,
            "RESUME", if (s.autosaveResumeAuto) "AUTO" else "ASK",
            s.cursorRow == row && s.cursorColumn == 0,
            s.cursorRow == row && s.cursorColumn == 1)
    }

    private fun DrawScope.drawDualParamRow(
        x: Int, y: Int, scale: Int,
        nameColumnX: Int, val1ColumnX: Int, subLabelX: Int, val2ColumnX: Int,
        t: AppTheme,
        paramName: String, val1: String, subLabel: String, val2: String,
        isOnRow: Boolean, cursorColumn: Int
    ) {
        val textY = y + TEXT_PADDING
        if (isOnRow) {
            drawRect(
                color = Color(t.rowCursor),
                topLeft = Offset((x * scale).toFloat(), (y * scale).toFloat()),
                size = Size((width * scale).toFloat(), (ROW_HEIGHT * scale).toFloat())
            )
        }
        drawBitmapText(paramName, nameColumnX, textY, scale,
            if (isOnRow) Color(t.textCursor) else Color(t.textParam), CHAR_SPACING, FONT_SCALE)
        drawBitmapText(val1, val1ColumnX, textY, scale,
            if (isOnRow && cursorColumn == 1) Color(t.textCursor) else Color(t.textValue), CHAR_SPACING, FONT_SCALE)
        drawBitmapText(subLabel, subLabelX, textY, scale,
            if (isOnRow) Color(t.textParam) else Color(t.textParam), CHAR_SPACING, FONT_SCALE)
        drawBitmapText(val2, val2ColumnX, textY, scale,
            if (isOnRow && cursorColumn == 2) Color(t.textCursor) else Color(t.textValue), CHAR_SPACING, FONT_SCALE)
    }

    private fun DrawScope.drawTemplateRow(
        x: Int, y: Int, scale: Int,
        nameColumnX: Int, valueColumnX: Int,
        s: SettingsState, currentRow: Int
    ) {
        val t = s.appTheme
        val textY = y + TEXT_PADDING
        val isCursorOnThisRow = s.cursorRow == currentRow

        if (isCursorOnThisRow) {
            drawRect(
                color = Color(t.rowCursor),
                topLeft = Offset((x * scale).toFloat(), (y * scale).toFloat()),
                size = Size((width * scale).toFloat(), (ROW_HEIGHT * scale).toFloat())
            )
        }
        drawBitmapText("TEMPLATE", nameColumnX, textY, scale,
            if (isCursorOnThisRow) Color(t.textCursor) else Color(t.textParam), CHAR_SPACING, FONT_SCALE)

        val options = listOf("SAVE", "CLEAR")
        var optionX = valueColumnX
        for (i in options.indices) {
            drawBitmapText(options[i], optionX, textY, scale,
                if (isCursorOnThisRow && s.cursorColumn == i + 1) Color(t.textCursor) else Color(t.textValue),
                CHAR_SPACING, FONT_SCALE)
            optionX += 80
        }
    }

    private fun DrawScope.drawParameterRow(
        x: Int, y: Int, scale: Int,
        nameColumnX: Int, valueColumnX: Int,
        t: AppTheme,
        parameterName: String, parameterValue: String,
        isCursorOnName: Boolean, isCursorOnValue: Boolean
    ) {
        val textY = y + TEXT_PADDING
        if (isCursorOnName || isCursorOnValue) {
            drawRect(
                color = Color(t.rowCursor),
                topLeft = Offset((x * scale).toFloat(), (y * scale).toFloat()),
                size = Size((width * scale).toFloat(), (ROW_HEIGHT * scale).toFloat())
            )
        }
        drawBitmapText(parameterName, nameColumnX, textY, scale,
            if (isCursorOnName || isCursorOnValue) Color(t.textCursor) else Color(t.textParam), CHAR_SPACING, FONT_SCALE)
        drawBitmapText(parameterValue, valueColumnX, textY, scale,
            if (isCursorOnValue) Color(t.textCursor) else Color(t.textValue), CHAR_SPACING, FONT_SCALE)
    }

    // ═══════════════════════════════════════════════════════════════════
    // CURSOR CONTEXT
    // ═══════════════════════════════════════════════════════════════════

    // Layout modes selectable via A+dpad on the LAYOUT row. On touch-only devices FULL is
    // excluded — it hides the virtual controls, which would leave the device with no input.
    // (TOUCH_PORTRAIT is intentionally omitted: it is a legacy state that was never reachable.)
    private fun layoutModeList(hasPhysical: Boolean): List<DeviceAdapter.LayoutMode> {
        val modes = if (hasPhysical)
            listOf(DeviceAdapter.LayoutMode.FULL,
                   DeviceAdapter.LayoutMode.TOUCH_LANDSCAPE,
                   DeviceAdapter.LayoutMode.TOUCH_PORTRAIT2)
        else
            listOf(DeviceAdapter.LayoutMode.TOUCH_LANDSCAPE,
                   DeviceAdapter.LayoutMode.TOUCH_PORTRAIT2)
        // Landscape is hidden in release (no themed asset yet); keep it in debug for testing.
        return if (BuildConfig.LANDSCAPE_LAYOUT) modes
               else modes.filter { it != DeviceAdapter.LayoutMode.TOUCH_LANDSCAPE }
    }

    fun getCursorContext(state: SettingsState): CursorContext {
        if (state.cursorColumn == 0) return CursorContextFactory.readOnly()

        return when (state.cursorRow) {
            0  -> when (state.cursorColumn) {
                2 -> if (state.availableSkins.isEmpty()) CursorContextFactory.readOnly()
                     else CursorContext(
                        valueType = CursorValueType.HEX_BYTE,   // THEME — A+dpad cycles available skins
                        capabilities = CursorCapabilities(
                            canIncrement = true, canDecrement = true,
                            canIncrementFast = false, canDecrementFast = false
                        ),
                        currentValue = state.availableSkins
                            .indexOfFirst { it.id == state.currentSkinId }.coerceAtLeast(0),
                        minValue = 0, maxValue = (state.availableSkins.size - 1).coerceAtLeast(0),
                        smallStep = 1, largeStep = 1, emptyValue = -1
                     )
                else -> CursorContext(
                    valueType = CursorValueType.HEX_BYTE,   // LAYOUT — A+dpad cycles selectable modes
                    capabilities = CursorCapabilities(
                        canIncrement = true, canDecrement = true,
                        canIncrementFast = false, canDecrementFast = false
                    ),
                    currentValue = layoutModeList(state.hasPhysicalButtons)
                        .indexOf(state.layoutMode).coerceAtLeast(0),
                    minValue = 0,
                    maxValue = (layoutModeList(state.hasPhysicalButtons).size - 1).coerceAtLeast(0),
                    smallStep = 1, largeStep = 1, emptyValue = -1
                )
            }
            1  -> CursorContext(
                valueType = CursorValueType.HEX_BYTE,   // SCALING — A+dpad toggles INT / BILINEAR
                capabilities = CursorCapabilities(
                    canIncrement = true, canDecrement = true,
                    canIncrementFast = false, canDecrementFast = false
                ),
                currentValue = if (state.scalingMode == DeviceAdapter.ScalingMode.BILINEAR) 1 else 0,
                minValue = 0, maxValue = 1, smallStep = 1, largeStep = 1, emptyValue = -1
            )
            2  -> when (state.cursorColumn) {
                1 -> {
                    val options = listOf("OFF") + state.overlayFiles
                    CursorContext(
                        valueType = CursorValueType.HEX_BYTE,
                        capabilities = CursorCapabilities(
                            canIncrement = true, canDecrement = true,
                            canIncrementFast = false, canDecrementFast = false
                        ),
                        currentValue = options.indexOf(state.overlayName).coerceAtLeast(0),
                        minValue = 0, maxValue = (options.size - 1).coerceAtLeast(0),
                        smallStep = 1, largeStep = 1, emptyValue = -1
                    )
                }
                else -> CursorContextFactory.hexByte(currentValue = state.overlayStrength, min = 0, max = 255)
            }
            3  -> when (state.cursorColumn) {   // BTN SOUND
                1 -> CursorContext(
                    valueType = CursorValueType.HEX_BYTE,
                    capabilities = CursorCapabilities(
                        canIncrement = true, canDecrement = true,
                        canIncrementFast = false, canDecrementFast = false
                    ),
                    currentValue = if (state.buttonSoundEnabled) 1 else 0,
                    minValue = 0, maxValue = 1, smallStep = 1, largeStep = 1, emptyValue = -1
                )
                else -> CursorContextFactory.hexByte(currentValue = state.buttonSoundVolume, min = 0, max = 255)
            }
            4  -> when (state.cursorColumn) {   // BTN VIBRO
                1 -> CursorContext(
                    valueType = CursorValueType.HEX_BYTE,
                    capabilities = CursorCapabilities(
                        canIncrement = true, canDecrement = true,
                        canIncrementFast = false, canDecrementFast = false
                    ),
                    currentValue = if (state.buttonVibroEnabled) 1 else 0,
                    minValue = 0, maxValue = 1, smallStep = 1, largeStep = 1, emptyValue = -1
                )
                else -> CursorContextFactory.hexByte(currentValue = state.vibroPower, min = 0, max = 255)
            }
            5  -> CursorContext(
                valueType = CursorValueType.HEX_BYTE,   // KB INSERT
                capabilities = CursorCapabilities(
                    canIncrement = true, canDecrement = true,
                    canIncrementFast = false, canDecrementFast = false
                ),
                currentValue = if (state.insertBefore) 1 else 0,
                minValue = 0, maxValue = 1, smallStep = 1, largeStep = 1, emptyValue = -1
            )
            6  -> CursorContext(
                valueType = CursorValueType.HEX_BYTE,   // CURSOR
                capabilities = CursorCapabilities(
                    canIncrement = true, canDecrement = true,
                    canIncrementFast = false, canDecrementFast = false
                ),
                currentValue = if (state.cursorRemember) 1 else 0,
                minValue = 0, maxValue = 1, smallStep = 1, largeStep = 1, emptyValue = -1
            )
            7  -> CursorContext(
                valueType = CursorValueType.HEX_BYTE,   // NOTE PREV
                capabilities = CursorCapabilities(
                    canIncrement = true, canDecrement = true,
                    canIncrementFast = false, canDecrementFast = false
                ),
                currentValue = if (state.notePreviewEnabled) 1 else 0,
                minValue = 0, maxValue = 1, smallStep = 1, largeStep = 1, emptyValue = -1
            )
            8  -> CursorContext(
                valueType = CursorValueType.HEX_BYTE,   // VISUALIZER
                capabilities = CursorCapabilities(
                    canIncrement = true, canDecrement = true,
                    canIncrementFast = false, canDecrementFast = false
                ),
                currentValue = VisualizerType.values().indexOf(state.visualizerType)
                    .coerceAtLeast(0),
                minValue = 0, maxValue = VisualizerType.values().size - 1,
                smallStep = 1, largeStep = 1, emptyValue = -1
            )
            9  -> CursorContextFactory.readOnly()   // THEME: A opens editor
            10 -> CursorContextFactory.readOnly()   // TEMPLATE: A triggers save/clear
            11 -> CursorContext(
                valueType = CursorValueType.HEX_BYTE,   // RESUME
                capabilities = CursorCapabilities(
                    canIncrement = true, canDecrement = true,
                    canIncrementFast = false, canDecrementFast = false
                ),
                currentValue = if (state.autosaveResumeAuto) 1 else 0,
                minValue = 0, maxValue = 1, smallStep = 1, largeStep = 1, emptyValue = -1
            )
            else -> CursorContextFactory.none()
        }
    }

    // ═══════════════════════════════════════════════════════════════════
    // INPUT HANDLING
    // ═══════════════════════════════════════════════════════════════════

    fun handleInput(
        state: SettingsState,
        action: InputAction
    ): InputResult {
        when (state.cursorRow) {
            0 -> when (state.cursorColumn) {
                2 -> if (action is InputAction.SET_VALUE) {   // THEME (skin)
                    return InputResult(modified = true,
                        skinId = state.availableSkins.getOrNull(action.value)?.id ?: state.currentSkinId)
                }
                else -> if (action is InputAction.SET_VALUE) {   // LAYOUT
                    val modes = layoutModeList(state.hasPhysicalButtons)
                    return InputResult(modified = true,
                        layoutMode = modes.getOrElse(action.value) { modes.first() })
                }
            }
            1 -> if (action is InputAction.SET_VALUE) {   // SCALING
                return InputResult(modified = true,
                    scalingMode = if (action.value > 0) DeviceAdapter.ScalingMode.BILINEAR
                                  else DeviceAdapter.ScalingMode.INTEGER)
            }
            2 -> when (state.cursorColumn) {   // OVERLAY
                1 -> if (action is InputAction.SET_VALUE) {
                    val options = listOf("OFF") + state.overlayFiles
                    return InputResult(modified = true, overlayName = options.getOrElse(action.value) { "OFF" })
                }
                2 -> if (action is InputAction.SET_VALUE) {
                    return InputResult(modified = true, overlayStrength = action.value.coerceIn(0, 255))
                }
            }
            3 -> when (state.cursorColumn) {   // BTN SOUND
                1 -> if (action is InputAction.SET_VALUE) {
                    return InputResult(modified = true, buttonSoundEnabled = action.value > 0)
                }
                2 -> if (action is InputAction.SET_VALUE) {
                    return InputResult(modified = true, buttonSoundVolume = action.value.coerceIn(0, 255))
                }
            }
            4 -> when (state.cursorColumn) {   // BTN VIBRO
                1 -> if (action is InputAction.SET_VALUE) {
                    return InputResult(modified = true, buttonVibroEnabled = action.value > 0)
                }
                2 -> if (action is InputAction.SET_VALUE) {
                    return InputResult(modified = true, vibroPower = action.value.coerceIn(0, 255))
                }
            }
            5 -> if (action is InputAction.SET_VALUE) {   // KB INSERT
                return InputResult(modified = true, insertBefore = action.value > 0)
            }
            6 -> if (action is InputAction.SET_VALUE) {   // CURSOR
                return InputResult(modified = true, cursorRemember = action.value > 0)
            }
            7 -> if (action is InputAction.SET_VALUE) {   // NOTE PREV
                return InputResult(modified = true, notePreviewEnabled = action.value > 0)
            }
            8 -> if (action is InputAction.SET_VALUE) {   // VISUALIZER
                val types = VisualizerType.values()
                return InputResult(modified = true, visualizerType = types.getOrNull(action.value) ?: types[0])
            }
            11 -> if (action is InputAction.SET_VALUE) {   // RESUME
                return InputResult(modified = true, autosaveResumeAuto = action.value > 0)
            }
        }
        return InputResult(modified = action !is InputAction.NONE)
    }

    data class InputResult(
        val modified: Boolean,
        val layoutMode: DeviceAdapter.LayoutMode? = null,
        val skinId: String? = null,
        val scalingMode: DeviceAdapter.ScalingMode? = null,
        val overlayName: String? = null,
        val overlayStrength: Int? = null,
        val buttonSoundEnabled: Boolean? = null,
        val buttonSoundVolume: Int? = null,
        val buttonVibroEnabled: Boolean? = null,
        val vibroPower: Int? = null,
        val insertBefore: Boolean? = null,
        val cursorRemember: Boolean? = null,
        val notePreviewEnabled: Boolean? = null,
        val autosaveResumeAuto: Boolean? = null,
        val visualizerType: VisualizerType? = null
    )
}

// ═══════════════════════════════════════════════════════════════════════
// STATE
// ═══════════════════════════════════════════════════════════════════════

data class SettingsState(
    val cursorRow: Int = 0,
    val cursorColumn: Int = 1,
    val hasPhysicalButtons: Boolean = true,
    val layoutMode: DeviceAdapter.LayoutMode = DeviceAdapter.LayoutMode.FULL,
    val currentSkinId: String = DeviceSkin.AMIGA_NORMAL.id,
    val availableSkins: List<DeviceSkin> = emptyList(),
    val scalingMode: DeviceAdapter.ScalingMode = DeviceAdapter.ScalingMode.INTEGER,
    val overlayFiles: List<String> = emptyList(),
    val overlayName: String = "OFF",
    val overlayStrength: Int = 128,
    val buttonSoundEnabled: Boolean = false,
    val buttonSoundVolume: Int = 255,
    val buttonVibroEnabled: Boolean = false,
    val vibroPower: Int = 255,
    val insertBefore: Boolean = true,
    val cursorRemember: Boolean = false,
    val notePreviewEnabled: Boolean = true,
    val autosaveResumeAuto: Boolean = false,
    val visualizerType: VisualizerType = VisualizerType.SCOPE,
    val currentThemeName: String = "CLASSIC",
    val appTheme: AppTheme = AppTheme.Companion.CLASSIC
)
