package com.conanizer.pockettracker

import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.geometry.Size
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.drawscope.DrawScope
import com.conanizer.pockettracker.core.data.Table
import com.conanizer.pockettracker.core.data.TableRow
import com.conanizer.pockettracker.core.logic.EffectProcessor

/**
 * TABLE SCREEN MODULE
 *
 * Displays 16-row table with columns:
 * Step | Transpose | Vol | FX1 | FX2 | FX3
 *
 * Size: 510×392 pixels (same as PhraseEditorModule)
 * State type: TableState
 */
class TableModule : TrackerModule {
    override val width = 510
    override val height = 392

    // Font constants
    private val FONT_SCALE = 3      // 5×5 bitmap scaled 3× = 15×15
    private val CHAR_SPACING = 2    // 2px between characters
    private val ROW_HEIGHT = 21     // Each row is 21px tall
    private val TEXT_PADDING = 3    // 3px padding above/below text

    override fun DrawScope.draw(x: Int, y: Int, scale: Int, state: Any?) {
        val tableState = state as? TableState ?: return

        // Module background
        drawRect(
            color = Color(0xFF0a0a0a),
            topLeft = Offset((x * scale).toFloat(), (y * scale).toFloat()),
            size = Size((width * scale).toFloat(), (height * scale).toFloat())
        )

        // Calculate column X positions
        var colX = x + 10  // Start with 10px left padding
        val stepX = colX; colX += 30 + 10           // Step (1 char: 0-F)
        val transposeX = colX; colX += 45 + 15      // Transpose (3 chars: +00 to -7F)
        val volX = colX; colX += 30 + 15            // Volume (2 chars: 00-FF or --)
        val fx1NameX = colX; colX += 45 + 10        // FX1 Name (3 chars)
        val fx1ValueX = colX; colX += 30 + 15       // FX1 Value (2 chars)
        val fx2NameX = colX; colX += 45 + 10        // FX2 Name (3 chars)
        val fx2ValueX = colX; colX += 30 + 15       // FX2 Value (2 chars)
        val fx3NameX = colX; colX += 45 + 10        // FX3 Name (3 chars)
        val fx3ValueX = colX                         // FX3 Value (2 chars)

        // ROW 0: HEADER "TABLE 00" + "06 TIC"
        var rowY = y + TEXT_PADDING
        drawBitmapText(
            text = "TABLE ${tableState.table.id.toString(16).padStart(2, '0').uppercase()}",
            x = x + 10,
            y = rowY,
            scale = scale,
            color = Color.Cyan,
            spacing = CHAR_SPACING,
            fontScale = FONT_SCALE
        )

        // TIC rate on the right
        val ticText = "${tableState.ticRate.toString(16).padStart(2, '0').uppercase()} TIC"
        drawBitmapText(
            text = ticText,
            x = x + width - 120,
            y = rowY,
            scale = scale,
            color = Color.Gray,
            spacing = CHAR_SPACING,
            fontScale = FONT_SCALE
        )

        // 14px SPACER after header
        rowY = y + ROW_HEIGHT + 14 + TEXT_PADDING

        // ROW 1: COLUMN HEADERS
        drawBitmapText("N", transposeX, rowY, scale, Color.Gray, CHAR_SPACING, FONT_SCALE)
        drawBitmapText("V", volX, rowY, scale, Color.Gray, CHAR_SPACING, FONT_SCALE)
        drawBitmapText("FX1", fx1NameX, rowY, scale, Color.Gray, CHAR_SPACING, FONT_SCALE)
        drawBitmapText("FX2", fx2NameX, rowY, scale, Color.Gray, CHAR_SPACING, FONT_SCALE)
        drawBitmapText("FX3", fx3NameX, rowY, scale, Color.Gray, CHAR_SPACING, FONT_SCALE)

        // ROWS 2-17: 16 DATA ROWS
        tableState.table.rows.forEachIndexed { index, row ->
            drawTableRow(
                x = x,
                y = y,
                scale = scale,
                index = index,
                row = row,
                state = tableState,
                stepX = stepX,
                transposeX = transposeX,
                volX = volX,
                fx1NameX = fx1NameX,
                fx1ValueX = fx1ValueX,
                fx2NameX = fx2NameX,
                fx2ValueX = fx2ValueX,
                fx3NameX = fx3NameX,
                fx3ValueX = fx3ValueX
            )
        }
    }

    /**
     * Draw a single table row
     */
    private fun DrawScope.drawTableRow(
        x: Int,
        y: Int,
        scale: Int,
        index: Int,
        row: TableRow,
        state: TableState,
        stepX: Int,
        transposeX: Int,
        volX: Int,
        fx1NameX: Int,
        fx1ValueX: Int,
        fx2NameX: Int,
        fx2ValueX: Int,
        fx3NameX: Int,
        fx3ValueX: Int
    ) {
        // Calculate row Y position
        val dataRowY = y + ROW_HEIGHT + 14 + ROW_HEIGHT + (index * ROW_HEIGHT)

        // Check if any cell in this row is selected
        val isRowSelected = state.selectionMode && (1..8).any { col -> state.isCellSelected(index, col) }

        // Row background color logic
        val bgColor = when {
            state.playbackRow == index -> Color(0xFF004400)  // Green playing
            isRowSelected -> Color(0xFF1a3a1a)               // Selection green
            index == state.cursorRow -> Color(0xFF333333)    // Gray cursor
            index % 4 == 0 -> Color(0xFF151515)              // Lighter every 4
            else -> Color(0xFF0a0a0a)                        // Default dark
        }

        // Draw row background
        drawRect(
            color = bgColor,
            topLeft = Offset((x * scale).toFloat(), (dataRowY * scale).toFloat()),
            size = Size((width * scale).toFloat(), (ROW_HEIGHT * scale).toFloat())
        )

        val textY = dataRowY + TEXT_PADDING

        // COLUMN 0: STEP NUMBER (hex 0-F)
        drawBitmapText(
            text = index.toString(16).uppercase(),
            x = stepX,
            y = textY,
            scale = scale,
            color = if (index == state.cursorRow && state.cursorColumn == 0)
                Color.Yellow else Color(0xFF666666),
            spacing = CHAR_SPACING,
            fontScale = FONT_SCALE
        )

        // COLUMN 1: TRANSPOSE (00-FF hex, like chain transpose)
        val transposeStr = row.transpose.toString(16).padStart(2, '0').uppercase()
        drawBitmapText(
            text = transposeStr,
            x = transposeX,
            y = textY,
            scale = scale,
            color = when {
                index == state.cursorRow && state.cursorColumn == 1 -> Color.Yellow
                state.selectionMode && state.isCellSelected(index, 1) -> Color(0xFF00DD00)  // Selection green
                row.transpose == 0x00 -> Color(0xFF444444)  // Dim if no transpose
                else -> Color.White
            },
            spacing = CHAR_SPACING,
            fontScale = FONT_SCALE
        )

        // COLUMN 2: VOLUME (00-FF or -- if empty)
        val volStr = if (row.volume == -1) "--" else row.volume.toString(16).padStart(2, '0').uppercase()
        drawBitmapText(
            text = volStr,
            x = volX,
            y = textY,
            scale = scale,
            color = when {
                index == state.cursorRow && state.cursorColumn == 2 -> Color.Yellow
                state.selectionMode && state.isCellSelected(index, 2) -> Color(0xFF00DD00)  // Selection green
                row.volume == -1 -> Color(0xFF444444)  // Dim if no volume change
                else -> Color(0xFFaaaaaa)
            },
            spacing = CHAR_SPACING,
            fontScale = FONT_SCALE
        )

        // COLUMNS 3-4: FX1
        drawFxColumn(
            fx1NameX, fx1ValueX, textY, scale,
            row.fx1Type, row.fx1Value,
            index == state.cursorRow && state.cursorColumn == 3,
            index == state.cursorRow && state.cursorColumn == 4,
            state.selectionMode && state.isCellSelected(index, 3),
            state.selectionMode && state.isCellSelected(index, 4)
        )

        // COLUMNS 5-6: FX2
        drawFxColumn(
            fx2NameX, fx2ValueX, textY, scale,
            row.fx2Type, row.fx2Value,
            index == state.cursorRow && state.cursorColumn == 5,
            index == state.cursorRow && state.cursorColumn == 6,
            state.selectionMode && state.isCellSelected(index, 5),
            state.selectionMode && state.isCellSelected(index, 6)
        )

        // COLUMNS 7-8: FX3
        drawFxColumn(
            fx3NameX, fx3ValueX, textY, scale,
            row.fx3Type, row.fx3Value,
            index == state.cursorRow && state.cursorColumn == 7,
            index == state.cursorRow && state.cursorColumn == 8,
            state.selectionMode && state.isCellSelected(index, 7),
            state.selectionMode && state.isCellSelected(index, 8)
        )
    }

    /**
     * Draw an FX column (name + value)
     */
    private fun DrawScope.drawFxColumn(
        nameX: Int,
        valueX: Int,
        textY: Int,
        scale: Int,
        fxType: Int,
        fxValue: Int,
        cursorOnName: Boolean,
        cursorOnValue: Boolean,
        nameSelected: Boolean = false,
        valueSelected: Boolean = false
    ) {
        val fxName = getEffectTypeName(fxType)
        val fxValueStr = fxValue.toString(16).padStart(2, '0').uppercase()

        // FX Name
        drawBitmapText(
            text = fxName,
            x = nameX,
            y = textY,
            scale = scale,
            color = when {
                cursorOnName -> Color.Yellow
                nameSelected -> Color(0xFF00DD00)  // Selection green
                fxType == 0 -> Color(0xFF444444)
                else -> Color(0xFFaaaaaa)
            },
            spacing = CHAR_SPACING,
            fontScale = FONT_SCALE
        )

        // FX Value
        drawBitmapText(
            text = fxValueStr,
            x = valueX,
            y = textY,
            scale = scale,
            color = when {
                cursorOnValue -> Color.Yellow
                valueSelected -> Color(0xFF00DD00)  // Selection green
                fxType == 0 -> Color(0xFF444444)
                else -> Color(0xFFaaaaaa)
            },
            spacing = CHAR_SPACING,
            fontScale = FONT_SCALE
        )
    }

    /**
     * Get cursor context for current cursor position
     */
    fun getCursorContext(state: TableState): CursorContext {
        return when (state.cursorColumn) {
            0 -> CursorContextFactory.readOnly()  // Step number
            1 -> CursorContextFactory.hexByte(    // Transpose
                currentValue = state.table.rows[state.cursorRow].transpose,
                min = 0,
                max = 255,
                canDelete = state.table.rows[state.cursorRow].transpose != 0x00  // Can delete if not already 00
            )
            2 -> CursorContextFactory.hexByte(    // Volume
                currentValue = if (state.table.rows[state.cursorRow].volume == -1) 0 else state.table.rows[state.cursorRow].volume,
                emptyValue = -1,
                canDelete = state.table.rows[state.cursorRow].volume != -1,  // Can delete if not already empty
                canInsert = true  // A on empty cell inserts a value
            )
            3 -> {  // FX1 Type
                val row = state.table.rows[state.cursorRow]
                CursorContextFactory.effectType(row.fx1Type, 1)
            }
            4 -> CursorContextFactory.hexByte(    // FX1 Value
                currentValue = state.table.rows[state.cursorRow].fx1Value,
                min = 0,
                max = 255
            )
            5 -> {  // FX2 Type
                val row = state.table.rows[state.cursorRow]
                CursorContextFactory.effectType(row.fx2Type, 2)
            }
            6 -> CursorContextFactory.hexByte(    // FX2 Value
                currentValue = state.table.rows[state.cursorRow].fx2Value,
                min = 0,
                max = 255
            )
            7 -> {  // FX3 Type
                val row = state.table.rows[state.cursorRow]
                CursorContextFactory.effectType(row.fx3Type, 3)
            }
            8 -> CursorContextFactory.hexByte(    // FX3 Value
                currentValue = state.table.rows[state.cursorRow].fx3Value,
                min = 0,
                max = 255
            )
            else -> CursorContextFactory.none()
        }
    }

    /**
     * Handle input for table editing
     */
    fun handleInput(
        state: TableState,
        action: com.conanizer.pockettracker.core.logic.InputAction
    ): InputResult {
        val row = state.table.rows[state.cursorRow]

        when (state.cursorColumn) {
            0 -> { /* Step number - read only */ }
            1 -> {  // Transpose
                when (action) {
                    is com.conanizer.pockettracker.core.logic.InputAction.SET_VALUE -> {
                        row.transpose = action.value.coerceIn(0, 255)
                    }
                    is com.conanizer.pockettracker.core.logic.InputAction.DELETE -> {
                        row.transpose = 0x00  // Reset to no transpose
                    }
                    else -> {}
                }
            }
            2 -> {  // Volume
                when (action) {
                    is com.conanizer.pockettracker.core.logic.InputAction.SET_VALUE -> {
                        row.volume = action.value.coerceIn(0, 255)
                    }
                    is com.conanizer.pockettracker.core.logic.InputAction.DELETE -> {
                        row.volume = -1  // Reset to no volume change (empty)
                    }
                    else -> {}
                }
            }
            3 -> {  // FX1 Type
                when (action) {
                    is com.conanizer.pockettracker.core.logic.InputAction.SET_VALUE -> {
                        // Convert index to effect type code
                        row.fx1Type = EffectProcessor.EFFECT_TYPES.getOrElse(action.value) { EffectProcessor.FX_NONE }
                    }
                    is com.conanizer.pockettracker.core.logic.InputAction.DELETE -> {
                        row.fx1Type = 0
                        row.fx1Value = 0
                    }
                    else -> {}
                }
            }
            4 -> {  // FX1 Value
                when (action) {
                    is com.conanizer.pockettracker.core.logic.InputAction.SET_VALUE -> {
                        row.fx1Value = action.value.coerceIn(0, 255)
                    }
                    else -> {}
                }
            }
            5 -> {  // FX2 Type
                when (action) {
                    is com.conanizer.pockettracker.core.logic.InputAction.SET_VALUE -> {
                        // Convert index to effect type code
                        row.fx2Type = EffectProcessor.EFFECT_TYPES.getOrElse(action.value) { EffectProcessor.FX_NONE }
                    }
                    is com.conanizer.pockettracker.core.logic.InputAction.DELETE -> {
                        row.fx2Type = 0
                        row.fx2Value = 0
                    }
                    else -> {}
                }
            }
            6 -> {  // FX2 Value
                when (action) {
                    is com.conanizer.pockettracker.core.logic.InputAction.SET_VALUE -> {
                        row.fx2Value = action.value.coerceIn(0, 255)
                    }
                    else -> {}
                }
            }
            7 -> {  // FX3 Type
                when (action) {
                    is com.conanizer.pockettracker.core.logic.InputAction.SET_VALUE -> {
                        // Convert index to effect type code
                        row.fx3Type = EffectProcessor.EFFECT_TYPES.getOrElse(action.value) { EffectProcessor.FX_NONE }
                    }
                    is com.conanizer.pockettracker.core.logic.InputAction.DELETE -> {
                        row.fx3Type = 0
                        row.fx3Value = 0
                    }
                    else -> {}
                }
            }
            8 -> {  // FX3 Value
                when (action) {
                    is com.conanizer.pockettracker.core.logic.InputAction.SET_VALUE -> {
                        row.fx3Value = action.value.coerceIn(0, 255)
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
 * STATE DATA FOR TABLE SCREEN
 *
 * @param table The table being edited
 * @param cursorRow Current row (0-15)
 * @param cursorColumn Current column:
 *   0 = Step number (read-only)
 *   1 = Transpose
 *   2 = Volume
 *   3 = FX1 Type
 *   4 = FX1 Value
 *   5 = FX2 Type
 *   6 = FX2 Value
 *   7 = FX3 Type
 *   8 = FX3 Value
 * @param playbackRow Currently playing row (null if not playing)
 * @param ticRate Current TIC rate for this table
 * @param selectionMode Whether selection mode is active
 * @param isCellSelected Function to check if a cell is selected
 */
data class TableState(
    val table: Table,
    val cursorRow: Int = 0,
    val cursorColumn: Int = 1,  // Start on transpose
    val playbackRow: Int? = null,
    val ticRate: Int = 0x06,  // Default: 6 tics per row
    val selectionMode: Boolean = false,
    val isCellSelected: (Int, Int) -> Boolean = { _, _ -> false }
)
