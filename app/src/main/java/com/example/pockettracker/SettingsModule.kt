package com.example.pockettracker

import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.geometry.Size
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.drawscope.DrawScope

/**
 * SETTINGS SCREEN MODULE
 *
 * A side menu opened from the PROJECT screen (row 6, SETTINGS button).
 * Press A on SETTINGS row in PROJECT screen to open.
 * Press B to return to PROJECT screen.
 *
 * Rows:
 *   0 — LAYOUT    (cycle: FULLSCREEN / TOUCH LANDSCAPE / AMIGA PORTRAIT)
 *   1 — SCALING   (cycle: INT / BILINEAR / NEAREST)
 *   2 — BTN SOUND (ON / OFF, cycling via A+DPAD up/down)
 *   3 — BTN VOL   (00-FF, hex byte via A+DPAD up/down)
 *   4 — BTN VIBRO (ON / OFF, cycling via A+DPAD up/down)
 *   5 — VIBRO POW (00-FF, hex byte via A+DPAD up/down)
 *   6 — KB INSERT (single cycling value: BEFORE=1 / AFTER=0)
 *
 * Size: 510×392 pixels (same as other screens)
 */
class SettingsModule : TrackerModule {
    override val width = 510
    override val height = 392

    private val FONT_SCALE = 3
    private val CHAR_SPACING = 2
    private val ROW_HEIGHT = 21
    private val TEXT_PADDING = 3

    override fun DrawScope.draw(x: Int, y: Int, scale: Int, state: Any?) {
        val s = state as? SettingsState ?: return

        // Background
        drawRect(
            color = Color(0xFF0a0a0a),
            topLeft = Offset((x * scale).toFloat(), (y * scale).toFloat()),
            size = Size((width * scale).toFloat(), (height * scale).toFloat())
        )

        val nameColumnX = x + 10
        val valueColumnX = x + 170

        // Header
        var rowY = y + TEXT_PADDING
        drawBitmapText(
            text = "SETTINGS",
            x = nameColumnX,
            y = rowY,
            scale = scale,
            color = Color.Cyan,
            spacing = CHAR_SPACING,
            fontScale = FONT_SCALE
        )
        rowY += ROW_HEIGHT + 14

        var currentRow = 0

        // ── ROW 0: LAYOUT ──────────────────────────────────────────────
        val layoutText = when (s.layoutMode) {
            DeviceAdapter.LayoutMode.FULL            -> "FULLSCREEN"
            DeviceAdapter.LayoutMode.TOUCH_PORTRAIT  -> "T.PORT"
            DeviceAdapter.LayoutMode.TOUCH_LANDSCAPE -> "TOUCH LANDSCAPE"
            DeviceAdapter.LayoutMode.TOUCH_PORTRAIT2 -> "AMIGA PORTRAIT"
        }
        drawParameterRow(x, rowY, scale, nameColumnX, valueColumnX,
            "LAYOUT", layoutText,
            isCursorOnName = s.cursorRow == currentRow && s.cursorColumn == 0,
            isCursorOnValue = s.cursorRow == currentRow && s.cursorColumn == 1)
        rowY += ROW_HEIGHT; currentRow++

        // ── ROW 1: SCALING ─────────────────────────────────────────────
        val scalingText = when (s.scalingMode) {
            DeviceAdapter.ScalingMode.INTEGER  -> "INT"
            DeviceAdapter.ScalingMode.BILINEAR -> "BILINEAR"
            DeviceAdapter.ScalingMode.NEAREST  -> "NEAREST"
        }
        drawParameterRow(x, rowY, scale, nameColumnX, valueColumnX,
            "SCALING", scalingText,
            isCursorOnName = s.cursorRow == currentRow && s.cursorColumn == 0,
            isCursorOnValue = s.cursorRow == currentRow && s.cursorColumn == 1)
        rowY += ROW_HEIGHT; currentRow++

        // ── ROW 2: BTN SOUND ───────────────────────────────────────────
        drawParameterRow(x, rowY, scale, nameColumnX, valueColumnX,
            "BTN SOUND", if (s.buttonSoundEnabled) "ON" else "OFF",
            isCursorOnName = s.cursorRow == currentRow && s.cursorColumn == 0,
            isCursorOnValue = s.cursorRow == currentRow && s.cursorColumn == 1)
        rowY += ROW_HEIGHT; currentRow++

        // ── ROW 3: BTN VOL ─────────────────────────────────────────────
        val btnVolHex = s.buttonSoundVolume.toString(16).padStart(2, '0').uppercase()
        drawParameterRow(x, rowY, scale, nameColumnX, valueColumnX,
            "BTN VOL", btnVolHex,
            isCursorOnName = s.cursorRow == currentRow && s.cursorColumn == 0,
            isCursorOnValue = s.cursorRow == currentRow && s.cursorColumn == 1)
        rowY += ROW_HEIGHT; currentRow++

        // ── ROW 4: BTN VIBRO ───────────────────────────────────────────
        drawParameterRow(x, rowY, scale, nameColumnX, valueColumnX,
            "BTN VIBRO", if (s.buttonVibroEnabled) "ON" else "OFF",
            isCursorOnName = s.cursorRow == currentRow && s.cursorColumn == 0,
            isCursorOnValue = s.cursorRow == currentRow && s.cursorColumn == 1)
        rowY += ROW_HEIGHT; currentRow++

        // ── ROW 5: VIBRO POW ───────────────────────────────────────────
        val vibroPowHex = s.vibroPower.toString(16).padStart(2, '0').uppercase()
        drawParameterRow(x, rowY, scale, nameColumnX, valueColumnX,
            "VIBRO POW", vibroPowHex,
            isCursorOnName = s.cursorRow == currentRow && s.cursorColumn == 0,
            isCursorOnValue = s.cursorRow == currentRow && s.cursorColumn == 1)
        rowY += ROW_HEIGHT; currentRow++

        // ── ROW 6: KB INSERT ───────────────────────────────────────────
        val kbInsertText = if (s.insertBefore) "BEFORE" else "AFTER"
        drawParameterRow(x, rowY, scale, nameColumnX, valueColumnX,
            "KB INSERT", kbInsertText,
            isCursorOnName = s.cursorRow == currentRow && s.cursorColumn == 0,
            isCursorOnValue = s.cursorRow == currentRow && s.cursorColumn == 1)
    }

    private fun DrawScope.drawParameterRow(
        x: Int, y: Int, scale: Int,
        nameColumnX: Int, valueColumnX: Int,
        parameterName: String, parameterValue: String,
        isCursorOnName: Boolean, isCursorOnValue: Boolean
    ) {
        val textY = y + TEXT_PADDING

        if (isCursorOnName || isCursorOnValue) {
            drawRect(
                color = Color(0xFF333333),
                topLeft = Offset((x * scale).toFloat(), (y * scale).toFloat()),
                size = Size((width * scale).toFloat(), (ROW_HEIGHT * scale).toFloat())
            )
        }

        drawBitmapText(
            text = parameterName,
            x = nameColumnX,
            y = textY,
            scale = scale,
            color = if (isCursorOnName || isCursorOnValue) Color.Yellow else Color.Gray,
            spacing = CHAR_SPACING,
            fontScale = FONT_SCALE
        )

        drawBitmapText(
            text = parameterValue,
            x = valueColumnX,
            y = textY,
            scale = scale,
            color = if (isCursorOnValue) Color.Yellow else Color.White,
            spacing = CHAR_SPACING,
            fontScale = FONT_SCALE
        )
    }

    // ═══════════════════════════════════════════════════════════════════
    // CURSOR CONTEXT
    // ═══════════════════════════════════════════════════════════════════

    fun getCursorContext(state: SettingsState): CursorContext {
        if (state.cursorColumn == 0) return CursorContextFactory.readOnly()

        return when (state.cursorRow) {
            0 -> CursorContextFactory.readOnly()  // LAYOUT: A key cycles (handled in MainActivity)
            1 -> CursorContextFactory.readOnly()  // SCALING: A key cycles (handled in MainActivity)
            2 -> CursorContext(                   // BTN SOUND: ON(1) / OFF(0)
                valueType = CursorValueType.HEX_BYTE,
                capabilities = CursorCapabilities(
                    canIncrement = true, canDecrement = true,
                    canIncrementFast = false, canDecrementFast = false
                ),
                currentValue = if (state.buttonSoundEnabled) 1 else 0,
                minValue = 0, maxValue = 1, smallStep = 1, largeStep = 1, emptyValue = -1
            )
            3 -> CursorContextFactory.hexByte(    // BTN VOL: 00-FF
                currentValue = state.buttonSoundVolume, min = 0, max = 255
            )
            4 -> CursorContext(                   // BTN VIBRO: ON(1) / OFF(0)
                valueType = CursorValueType.HEX_BYTE,
                capabilities = CursorCapabilities(
                    canIncrement = true, canDecrement = true,
                    canIncrementFast = false, canDecrementFast = false
                ),
                currentValue = if (state.buttonVibroEnabled) 1 else 0,
                minValue = 0, maxValue = 1, smallStep = 1, largeStep = 1, emptyValue = -1
            )
            5 -> CursorContextFactory.hexByte(    // VIBRO POW: 00-FF
                currentValue = state.vibroPower, min = 0, max = 255
            )
            6 -> CursorContext(                   // KB INSERT: BEFORE(1) / AFTER(0)
                valueType = CursorValueType.HEX_BYTE,
                capabilities = CursorCapabilities(
                    canIncrement = true, canDecrement = true,
                    canIncrementFast = false, canDecrementFast = false
                ),
                currentValue = if (state.insertBefore) 1 else 0,
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
        action: com.example.pockettracker.core.logic.InputAction
    ): InputResult {
        when (state.cursorRow) {
            2 -> {  // BTN SOUND
                if (action is com.example.pockettracker.core.logic.InputAction.SET_VALUE) {
                    return InputResult(modified = true, buttonSoundEnabled = action.value > 0)
                }
            }
            3 -> {  // BTN VOL
                if (action is com.example.pockettracker.core.logic.InputAction.SET_VALUE) {
                    return InputResult(modified = true, buttonSoundVolume = action.value.coerceIn(0, 255))
                }
            }
            4 -> {  // BTN VIBRO
                if (action is com.example.pockettracker.core.logic.InputAction.SET_VALUE) {
                    return InputResult(modified = true, buttonVibroEnabled = action.value > 0)
                }
            }
            5 -> {  // VIBRO POW
                if (action is com.example.pockettracker.core.logic.InputAction.SET_VALUE) {
                    return InputResult(modified = true, vibroPower = action.value.coerceIn(0, 255))
                }
            }
            6 -> {  // KB INSERT
                if (action is com.example.pockettracker.core.logic.InputAction.SET_VALUE) {
                    return InputResult(modified = true, insertBefore = action.value > 0)
                }
            }
        }
        return InputResult(modified = action !is com.example.pockettracker.core.logic.InputAction.NONE)
    }

    data class InputResult(
        val modified: Boolean,
        val buttonSoundEnabled: Boolean? = null,
        val buttonSoundVolume: Int? = null,
        val buttonVibroEnabled: Boolean? = null,
        val vibroPower: Int? = null,
        val insertBefore: Boolean? = null
    )
}

// ═══════════════════════════════════════════════════════════════════════
// STATE
// ═══════════════════════════════════════════════════════════════════════

data class SettingsState(
    val cursorRow: Int = 0,
    val cursorColumn: Int = 1,
    val layoutMode: DeviceAdapter.LayoutMode = DeviceAdapter.LayoutMode.FULL,
    val scalingMode: DeviceAdapter.ScalingMode = DeviceAdapter.ScalingMode.INTEGER,
    val buttonSoundEnabled: Boolean = false,
    val buttonSoundVolume: Int = 255,
    val buttonVibroEnabled: Boolean = false,
    val vibroPower: Int = 255,
    val insertBefore: Boolean = true
)
