package com.example.pockettracker

import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.geometry.Size
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.drawscope.DrawScope
import com.example.pockettracker.core.data.Project

/**
 * PROJECT SCREEN MODULE - FIXED STYLING
 *
 * Layout exactly like Song/Chain/Phrase screens:
 * - First column: Parameter names (TEMPO, TRANSPOSE, NAME, etc.)
 * - Second column: Parameter values
 * - Yellow text = cursor highlight (no boxes!)
 * - Grey names, white values when not selected
 *
 * Size: 510×392 pixels (same as other screens)
 */
class ProjectModule : TrackerModule {
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
        val projectState = state as? ProjectState ?: return

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
        // Column 1: Parameter names (left aligned at x=10)
        // Column 2: Parameter values (starts at x=140)
        // ===================================
        val nameColumnX = x + 10      // Left side: parameter names
        val valueColumnX = x + 170    // Right side: parameter values

        // ===================================
        // STEP 3: Draw header "PROJECT"
        // ===================================
        var rowY = y + TEXT_PADDING

        drawBitmapText(
            text = "PROJECT",
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

        // Row index for cursor matching
        var currentRow = 0

        // ─────────────────────────────────────
        // ROW 0: TEMPO (000-999, default 120)
        // ─────────────────────────────────────
        drawParameterRow(
            x = x,
            y = rowY,
            scale = scale,
            nameColumnX = nameColumnX,
            valueColumnX = valueColumnX,
            parameterName = "TEMPO",
            parameterValue = projectState.project.tempo.toString().padStart(3, '0'),
            isCursorOnName = projectState.cursorRow == currentRow && projectState.cursorColumn == 0,
            isCursorOnValue = projectState.cursorRow == currentRow && projectState.cursorColumn == 1
        )
        rowY += ROW_HEIGHT
        currentRow++

        // ─────────────────────────────────────
        // ROW 1: TRANSPOSE (00-FF, default 00)
        // ─────────────────────────────────────
        val transposeHex = projectState.project.transpose
            .toString(16)
            .padStart(2, '0')
            .uppercase()

        drawParameterRow(
            x = x,
            y = rowY,
            scale = scale,
            nameColumnX = nameColumnX,
            valueColumnX = valueColumnX,
            parameterName = "TRANSPOSE",
            parameterValue = transposeHex,
            isCursorOnName = projectState.cursorRow == currentRow && projectState.cursorColumn == 0,
            isCursorOnValue = projectState.cursorRow == currentRow && projectState.cursorColumn == 1
        )
        rowY += ROW_HEIGHT
        currentRow++

        // ─────────────────────────────────────
        // ROW 2: NAME (12 characters, editable per-character)
        // ─────────────────────────────────────
        drawNameRow(
            x = x,
            y = rowY,
            scale = scale,
            nameColumnX = nameColumnX,
            valueColumnX = valueColumnX,
            projectState = projectState,
            currentRow = currentRow
        )
        rowY += ROW_HEIGHT
        currentRow++

        // ─────────────────────────────────────
        // ROW 3: PROJECT (LOAD / SAVE / NEW)
        // ─────────────────────────────────────
        drawProjectRow(
            x = x,
            y = rowY,
            scale = scale,
            nameColumnX = nameColumnX,
            valueColumnX = valueColumnX,
            projectState = projectState,
            currentRow = currentRow
        )
        rowY += ROW_HEIGHT
        currentRow++

        // ─────────────────────────────────────
        // ROW 4: EXPORT (WAV MIX button)
        // ─────────────────────────────────────
        drawExportRow(
            x = x,
            y = rowY,
            scale = scale,
            nameColumnX = nameColumnX,
            valueColumnX = valueColumnX,
            projectState = projectState,
            currentRow = currentRow
        )
        rowY += ROW_HEIGHT
        currentRow++

        // ─────────────────────────────────────
        // ROW 5: CLEAN (SEQ / INST buttons)
        // ─────────────────────────────────────
        drawCleanRow(
            x = x,
            y = rowY,
            scale = scale,
            nameColumnX = nameColumnX,
            valueColumnX = valueColumnX,
            projectState = projectState,
            currentRow = currentRow
        )
        rowY += ROW_HEIGHT
        currentRow++

        // ─────────────────────────────────────
        // ROW 6: SYSTEM (placeholder)
        // ─────────────────────────────────────
        drawParameterRow(
            x = x,
            y = rowY,
            scale = scale,
            nameColumnX = nameColumnX,
            valueColumnX = valueColumnX,
            parameterName = "SYSTEM",
            parameterValue = "---",
            isCursorOnName = projectState.cursorRow == currentRow && projectState.cursorColumn == 0,
            isCursorOnValue = projectState.cursorRow == currentRow && projectState.cursorColumn == 1
        )
        rowY += ROW_HEIGHT
        currentRow++

        // ─────────────────────────────────────
        // ROW 7: LAYOUT (cycle: FULLSCREEN / TOUCH LANDSCAPE / AMIGA PORTRAIT)
        // ─────────────────────────────────────
        val layoutText = when (projectState.layoutMode) {
            DeviceAdapter.LayoutMode.FULL            -> "FULLSCREEN"
            DeviceAdapter.LayoutMode.TOUCH_PORTRAIT  -> "T.PORT"       // legacy, not shown in cycle
            DeviceAdapter.LayoutMode.TOUCH_LANDSCAPE -> "TOUCH LANDSCAPE"
            DeviceAdapter.LayoutMode.TOUCH_PORTRAIT2 -> "AMIGA PORTRAIT"
        }
        drawParameterRow(
            x = x,
            y = rowY,
            scale = scale,
            nameColumnX = nameColumnX,
            valueColumnX = valueColumnX,
            parameterName = "LAYOUT",
            parameterValue = layoutText,
            isCursorOnName = projectState.cursorRow == currentRow && projectState.cursorColumn == 0,
            isCursorOnValue = projectState.cursorRow == currentRow && projectState.cursorColumn == 1
        )
        rowY += ROW_HEIGHT
        currentRow++

        // ─────────────────────────────────────
        // ROW 8: SCALING (INT / BILINEAR / NEAREST)
        // ─────────────────────────────────────
        val scalingText = when (projectState.scalingMode) {
            DeviceAdapter.ScalingMode.INTEGER  -> "INT"
            DeviceAdapter.ScalingMode.BILINEAR -> "BILINEAR"
            DeviceAdapter.ScalingMode.NEAREST  -> "NEAREST"
        }
        drawParameterRow(
            x = x,
            y = rowY,
            scale = scale,
            nameColumnX = nameColumnX,
            valueColumnX = valueColumnX,
            parameterName = "SCALING",
            parameterValue = scalingText,
            isCursorOnName = projectState.cursorRow == currentRow && projectState.cursorColumn == 0,
            isCursorOnValue = projectState.cursorRow == currentRow && projectState.cursorColumn == 1
        )
        rowY += ROW_HEIGHT
        currentRow++

        // ─────────────────────────────────────
        // ROW 9: BUTTON SOUND (ON / OFF)
        // ─────────────────────────────────────
        drawParameterRow(
            x = x,
            y = rowY,
            scale = scale,
            nameColumnX = nameColumnX,
            valueColumnX = valueColumnX,
            parameterName = "BTN SOUND",
            parameterValue = if (projectState.buttonSoundEnabled) "ON" else "OFF",
            isCursorOnName = projectState.cursorRow == currentRow && projectState.cursorColumn == 0,
            isCursorOnValue = projectState.cursorRow == currentRow && projectState.cursorColumn == 1
        )
        rowY += ROW_HEIGHT
        currentRow++

        // ─────────────────────────────────────
        // ROW 10: BUTTON VOLUME (00-FF)
        // ─────────────────────────────────────
        val btnVolHex = projectState.buttonSoundVolume
            .toString(16)
            .padStart(2, '0')
            .uppercase()
        drawParameterRow(
            x = x,
            y = rowY,
            scale = scale,
            nameColumnX = nameColumnX,
            valueColumnX = valueColumnX,
            parameterName = "BTN VOL",
            parameterValue = btnVolHex,
            isCursorOnName = projectState.cursorRow == currentRow && projectState.cursorColumn == 0,
            isCursorOnValue = projectState.cursorRow == currentRow && projectState.cursorColumn == 1
        )
        rowY += ROW_HEIGHT
        currentRow++

        // ─────────────────────────────────────
        // ROW 11: BUTTON VIBRO (ON / OFF)
        // ─────────────────────────────────────
        drawParameterRow(
            x = x,
            y = rowY,
            scale = scale,
            nameColumnX = nameColumnX,
            valueColumnX = valueColumnX,
            parameterName = "BTN VIBRO",
            parameterValue = if (projectState.buttonVibroEnabled) "ON" else "OFF",
            isCursorOnName = projectState.cursorRow == currentRow && projectState.cursorColumn == 0,
            isCursorOnValue = projectState.cursorRow == currentRow && projectState.cursorColumn == 1
        )
        rowY += ROW_HEIGHT
        currentRow++

        // ─────────────────────────────────────
        // ROW 12: VIBRO POWER (00-FF)
        // ─────────────────────────────────────
        val vibroPowHex = projectState.vibroPower
            .toString(16)
            .padStart(2, '0')
            .uppercase()
        drawParameterRow(
            x = x,
            y = rowY,
            scale = scale,
            nameColumnX = nameColumnX,
            valueColumnX = valueColumnX,
            parameterName = "VIBRO POW",
            parameterValue = vibroPowHex,
            isCursorOnName = projectState.cursorRow == currentRow && projectState.cursorColumn == 0,
            isCursorOnValue = projectState.cursorRow == currentRow && projectState.cursorColumn == 1
        )
        rowY += ROW_HEIGHT
        currentRow++

        // ─────────────────────────────────────
        // ROW 13: KEYBOARD INSERT MODE (BEFORE / AFTER)
        // Controls whether A inserts before or after the text cursor
        // ─────────────────────────────────────
        val isCursorOnInsertRow = projectState.cursorRow == currentRow

        if (isCursorOnInsertRow) {
            drawRect(
                color = Color(0xFF333333),
                topLeft = Offset((x * scale).toFloat(), (rowY * scale).toFloat()),
                size = Size((width * scale).toFloat(), (ROW_HEIGHT * scale).toFloat())
            )
        }
        drawBitmapText(
            text = "KB INSERT",
            x = nameColumnX,
            y = rowY + TEXT_PADDING,
            scale = scale,
            color = if (isCursorOnInsertRow) Color.Yellow else Color.Gray,
            spacing = CHAR_SPACING,
            fontScale = FONT_SCALE
        )
        val insertOptions = listOf("BEFORE", "AFTER")
        var insertOptX = valueColumnX
        for (optIdx in insertOptions.indices) {
            val isSelected = if (optIdx == 0) projectState.insertBefore else !projectState.insertBefore
            val isCursorHere = isCursorOnInsertRow && projectState.cursorColumn == optIdx + 1
            drawBitmapText(
                text = insertOptions[optIdx],
                x = insertOptX,
                y = rowY + TEXT_PADDING,
                scale = scale,
                color = when {
                    isCursorHere -> Color.Yellow
                    isSelected   -> Color.Cyan
                    else         -> Color.White
                },
                spacing = CHAR_SPACING,
                fontScale = FONT_SCALE
            )
            insertOptX += 100
        }

        // ===================================
        // STEP 5: Draw status message at bottom (if any)
        // ===================================
        if (projectState.statusMessage.isNotEmpty()) {
            val messageY = y + height - 40

            drawBitmapText(
                text = projectState.statusMessage,
                x = nameColumnX,
                y = messageY,
                scale = scale,
                color = if (projectState.isSuccess)
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
        // Calculate Y position for text
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
        // Yellow if cursor on name, gray otherwise
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
     * Draw NAME row with per-character editing
     * Example: NAME     M Y S O N G _ _ _ _ _ _
     */
    private fun DrawScope.drawNameRow(
        x: Int,
        y: Int,
        scale: Int,
        nameColumnX: Int,
        valueColumnX: Int,
        projectState: ProjectState,
        currentRow: Int
    ) {
        val textY = y + TEXT_PADDING
        val isCursorOnThisRow = projectState.cursorRow == currentRow

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
            // Yellow if cursor is anywhere on this row
            color = if (isCursorOnThisRow) Color.Yellow else Color.Gray,
            spacing = CHAR_SPACING,
            fontScale = FONT_SCALE
        )

        // COLUMN 2: Name characters (12 characters max)
        // Show actual name characters, then "-" for empty slots
        val projectName = projectState.project.name.take(12)

        // Draw each character separately
        var charX = valueColumnX
        for (charIndex in 0..11) {
            // Get character to display:
            // - If within name length: show actual character
            // - If beyond name length: show "-" for empty slot
            val char = if (charIndex < projectName.length) {
                projectName[charIndex]
            } else {
                '-'  // Empty slot indicator
            }

            // Is cursor on THIS specific character?
            // cursorColumn: 0=name label, 1=first char, 2=second char, etc.
            val isCursorOnThisChar = isCursorOnThisRow &&
                    projectState.cursorColumn == charIndex + 1

            // Choose color: yellow for cursor, gray for empty slots, white for content
            val charColor = when {
                isCursorOnThisChar -> Color.Yellow
                charIndex >= projectName.length -> Color.Gray  // Gray for empty slots
                else -> Color.White  // White for name content
            }

            drawBitmapText(
                text = char.toString(),
                x = charX,
                y = textY,
                scale = scale,
                color = charColor,
                spacing = CHAR_SPACING,
                fontScale = FONT_SCALE
            )

            // Move to next character position
            // Each char is 5px * 3 (scale) + 2px spacing = 17px
            charX += (5 * FONT_SCALE) + CHAR_SPACING
        }
    }

    /**
     * Draw PROJECT row with LOAD / SAVE / NEW
     * Example: PROJECT  LOAD  SAVE  NEW
     */
    private fun DrawScope.drawProjectRow(
        x: Int,
        y: Int,
        scale: Int,
        nameColumnX: Int,
        valueColumnX: Int,
        projectState: ProjectState,
        currentRow: Int
    ) {
        val textY = y + TEXT_PADDING
        val isCursorOnThisRow = projectState.cursorRow == currentRow

        // Draw row background if cursor is here
        if (isCursorOnThisRow) {
            drawRect(
                color = Color(0xFF333333),
                topLeft = Offset((x * scale).toFloat(), (y * scale).toFloat()),
                size = Size((width * scale).toFloat(), (ROW_HEIGHT * scale).toFloat())
            )
        }

        // COLUMN 1: "PROJECT" label
        drawBitmapText(
            text = "PROJECT",
            x = nameColumnX,
            y = textY,
            scale = scale,
            color = if (isCursorOnThisRow) Color.Yellow else Color.Gray,
            spacing = CHAR_SPACING,
            fontScale = FONT_SCALE
        )

        // COLUMN 2: Options (LOAD, SAVE, NEW)
        val options = listOf("LOAD", "SAVE", "NEW")
        var optionX = valueColumnX

        for (optionIndex in options.indices) {
            val optionText = options[optionIndex]

            // Is cursor on THIS option?
            // cursorColumn: 0=label, 1=LOAD, 2=SAVE, 3=NEW
            val isCursorOnThisOption = isCursorOnThisRow &&
                    projectState.cursorColumn == optionIndex + 1

            drawBitmapText(
                text = optionText,
                x = optionX,
                y = textY,
                scale = scale,
                color = if (isCursorOnThisOption) Color.Yellow else Color.White,
                spacing = CHAR_SPACING,
                fontScale = FONT_SCALE
            )

            // Move to next option position
            // Leave ~40px between options
            optionX += 80
        }
    }

    /**
     * Draw EXPORT row with WAV MIX option
     * Example: EXPORT   WAV MIX
     */
    private fun DrawScope.drawExportRow(
        x: Int,
        y: Int,
        scale: Int,
        nameColumnX: Int,
        valueColumnX: Int,
        projectState: ProjectState,
        currentRow: Int
    ) {
        val textY = y + TEXT_PADDING
        val isCursorOnThisRow = projectState.cursorRow == currentRow

        // Draw row background if cursor is here
        if (isCursorOnThisRow) {
            drawRect(
                color = Color(0xFF333333),
                topLeft = Offset((x * scale).toFloat(), (y * scale).toFloat()),
                size = Size((width * scale).toFloat(), (ROW_HEIGHT * scale).toFloat())
            )
        }

        // COLUMN 1: "EXPORT" label
        drawBitmapText(
            text = "EXPORT",
            x = nameColumnX,
            y = textY,
            scale = scale,
            color = if (isCursorOnThisRow) Color.Yellow else Color.Gray,
            spacing = CHAR_SPACING,
            fontScale = FONT_SCALE
        )

        // COLUMN 2: WAV MIX option
        val isCursorOnWavMix = isCursorOnThisRow && projectState.cursorColumn == 1

        // Show render status or "WAV MIX" button
        val buttonText = if (projectState.isRendering) {
            "RENDERING ${(projectState.renderProgress * 100).toInt()}%"
        } else {
            "WAV MIX"
        }

        drawBitmapText(
            text = buttonText,
            x = valueColumnX,
            y = textY,
            scale = scale,
            color = when {
                projectState.isRendering -> Color.Cyan
                isCursorOnWavMix -> Color.Yellow
                else -> Color.White
            },
            spacing = CHAR_SPACING,
            fontScale = FONT_SCALE
        )
    }

    /**
     * Draw CLEAN row with SEQ / INST options
     * Example: CLEAN    SEQ  INST
     */
    private fun DrawScope.drawCleanRow(
        x: Int,
        y: Int,
        scale: Int,
        nameColumnX: Int,
        valueColumnX: Int,
        projectState: ProjectState,
        currentRow: Int
    ) {
        val textY = y + TEXT_PADDING
        val isCursorOnThisRow = projectState.cursorRow == currentRow

        if (isCursorOnThisRow) {
            drawRect(
                color = Color(0xFF333333),
                topLeft = Offset((x * scale).toFloat(), (y * scale).toFloat()),
                size = Size((width * scale).toFloat(), (ROW_HEIGHT * scale).toFloat())
            )
        }

        drawBitmapText(
            text = "CLEAN",
            x = nameColumnX,
            y = textY,
            scale = scale,
            color = if (isCursorOnThisRow) Color.Yellow else Color.Gray,
            spacing = CHAR_SPACING,
            fontScale = FONT_SCALE
        )

        val options = listOf("SEQ", "INST")
        var optionX = valueColumnX
        for (optionIndex in options.indices) {
            val isCursorOnThis = isCursorOnThisRow && projectState.cursorColumn == optionIndex + 1
            drawBitmapText(
                text = options[optionIndex],
                x = optionX,
                y = textY,
                scale = scale,
                color = if (isCursorOnThis) Color.Yellow else Color.White,
                spacing = CHAR_SPACING,
                fontScale = FONT_SCALE
            )
            optionX += 80
        }
    }

    /**
     * Get cursor context for current cursor position
     *
     * This tells the generic input system what kind of value we're on
     * and what actions are available.
     */
    fun getCursorContext(state: ProjectState): CursorContext {
        when (state.cursorRow) {
            0 -> {
                // TEMPO row
                if (state.cursorColumn == 0) {
                    // On "TEMPO" label - read only
                    return CursorContextFactory.readOnly()
                }
                // On tempo value (000-999)
                return CursorContext(
                    valueType = CursorValueType.HEX_BYTE,  // Actually decimal, but similar behavior
                    capabilities = CursorCapabilities(
                        canIncrement = true,
                        canDecrement = true,
                        canIncrementFast = true,
                        canDecrementFast = true
                    ),
                    currentValue = state.project.tempo,
                    minValue = 20,
                    maxValue = 999,
                    smallStep = 1,
                    largeStep = 10,
                    emptyValue = -1
                )
            }
            1 -> {
                // TRANSPOSE row
                if (state.cursorColumn == 0) {
                    return CursorContextFactory.readOnly()
                }
                // Transpose value (00-FF)
                return CursorContextFactory.hexByte(
                    currentValue = state.project.transpose,
                    min = 0,
                    max = 255
                )
            }
            2 -> {
                // NAME row - per-character editing
                if (state.cursorColumn == 0) {
                    return CursorContextFactory.readOnly()
                }
                // Get the character index (column 1 = char 0, column 2 = char 1, etc.)
                val charIndex = state.cursorColumn - 1
                if (charIndex >= 12) {
                    return CursorContextFactory.none()
                }

                // Get current character (or space if beyond name length)
                val currentChar = if (charIndex < state.project.name.length) {
                    state.project.name[charIndex]
                } else {
                    ' '  // Empty slot
                }

                // Use CHARACTER type for text editing
                // Cycles through: A-Z, 0-9, underscore, dash, space
                return CursorContextFactory.character(currentChar)
            }
            3 -> {
                // PROJECT row (LOAD/SAVE/NEW) - not value editing, read-only for now
                return CursorContextFactory.readOnly()
            }
            4 -> {
                // EXPORT row (WAV MIX) - actionable button
                if (state.cursorColumn == 0) {
                    return CursorContextFactory.readOnly()
                }
                // WAV MIX button - can be activated with A button
                return CursorContextFactory.readOnly()  // Action handled in MainActivity
            }
            5 -> {
                // CLEAN row - press A to clean (read-only, action handled in MainActivity)
                return CursorContextFactory.readOnly()
            }
            7 -> {
                // LAYOUT row - A button cycles the mode (handled in MainActivity)
                return CursorContextFactory.readOnly()
            }
            8 -> {
                // SCALING row - A button cycles scaling mode (handled in MainActivity)
                return CursorContextFactory.readOnly()
            }
            9 -> {
                // BUTTON SOUND row - A+UP = ON, A+DOWN = OFF
                if (state.cursorColumn == 0) return CursorContextFactory.readOnly()
                return CursorContext(
                    valueType = CursorValueType.HEX_BYTE,
                    capabilities = CursorCapabilities(
                        canIncrement = true,
                        canDecrement = true,
                        canIncrementFast = false,
                        canDecrementFast = false
                    ),
                    currentValue = if (state.buttonSoundEnabled) 1 else 0,
                    minValue = 0,
                    maxValue = 1,
                    smallStep = 1,
                    largeStep = 1,
                    emptyValue = -1
                )
            }
            10 -> {
                // BUTTON VOLUME row - 00-FF
                if (state.cursorColumn == 0) return CursorContextFactory.readOnly()
                return CursorContextFactory.hexByte(
                    currentValue = state.buttonSoundVolume,
                    min = 0,
                    max = 255
                )
            }
            11 -> {
                // BUTTON VIBRO row - A+UP = ON, A+DOWN = OFF
                if (state.cursorColumn == 0) return CursorContextFactory.readOnly()
                return CursorContext(
                    valueType = CursorValueType.HEX_BYTE,
                    capabilities = CursorCapabilities(
                        canIncrement = true,
                        canDecrement = true,
                        canIncrementFast = false,
                        canDecrementFast = false
                    ),
                    currentValue = if (state.buttonVibroEnabled) 1 else 0,
                    minValue = 0,
                    maxValue = 1,
                    smallStep = 1,
                    largeStep = 1,
                    emptyValue = -1
                )
            }
            12 -> {
                // VIBRO POWER row - 00-FF
                if (state.cursorColumn == 0) return CursorContextFactory.readOnly()
                return CursorContextFactory.hexByte(
                    currentValue = state.vibroPower,
                    min = 0,
                    max = 255
                )
            }
            13 -> {
                // KEYBOARD INSERT MODE row - BEFORE (0) / AFTER (1)
                if (state.cursorColumn == 0) return CursorContextFactory.readOnly()
                return CursorContextFactory.readOnly()  // Action handled in MainActivity (A button)
            }
            else -> return CursorContextFactory.none()
        }
    }

    /**
     * Handle input action for project screen.
     */
    fun handleInput(
        state: ProjectState,
        action: com.example.pockettracker.core.logic.InputAction
    ): InputResult {
        when (state.cursorRow) {
            0 -> {
                // TEMPO row
                when (action) {
                    is com.example.pockettracker.core.logic.InputAction.SET_VALUE -> {
                        state.project.tempo = action.value.coerceIn(20, 999)
                    }
                    else -> { /* Other actions not applicable */ }
                }
            }
            1 -> {
                // TRANSPOSE row
                when (action) {
                    is com.example.pockettracker.core.logic.InputAction.SET_VALUE -> {
                        state.project.transpose = action.value.coerceIn(0, 255)
                    }
                    else -> { /* Other actions not applicable */ }
                }
            }
            2 -> {
                // NAME row - per-character editing
                val charIndex = state.cursorColumn - 1
                if (charIndex < 0 || charIndex >= 12) return InputResult(modified = false)

                when (action) {
                    is com.example.pockettracker.core.logic.InputAction.SET_VALUE -> {
                        // Set character at position
                        val char = action.value.toChar()
                        val sb = StringBuilder(state.project.name.padEnd(12, ' '))
                        sb.setCharAt(charIndex, char)
                        state.project.name = sb.toString().trimEnd()  // Remove trailing spaces
                    }
                    is com.example.pockettracker.core.logic.InputAction.DELETE -> {
                        // Delete character (replace with space)
                        if (charIndex < state.project.name.length) {
                            val sb = StringBuilder(state.project.name.padEnd(12, ' '))
                            sb.setCharAt(charIndex, ' ')
                            state.project.name = sb.toString().trimEnd()
                        }
                    }
                    else -> { /* Other actions not applicable */ }
                }
            }
            3 -> {
                // PROJECT row (LOAD/SAVE/NEW) - handled elsewhere
            }
            9 -> {
                // BUTTON SOUND — carry new boolean back to MainActivity via InputResult
                when (action) {
                    is com.example.pockettracker.core.logic.InputAction.SET_VALUE -> {
                        return InputResult(
                            modified = true,
                            buttonSoundEnabled = action.value > 0
                        )
                    }
                    else -> {}
                }
            }
            10 -> {
                // BUTTON VOLUME — carry new value back to MainActivity via InputResult
                when (action) {
                    is com.example.pockettracker.core.logic.InputAction.SET_VALUE -> {
                        return InputResult(
                            modified = true,
                            buttonSoundVolume = action.value.coerceIn(0, 255)
                        )
                    }
                    else -> {}
                }
            }
            11 -> {
                // BUTTON VIBRO — carry new boolean back to MainActivity via InputResult
                when (action) {
                    is com.example.pockettracker.core.logic.InputAction.SET_VALUE -> {
                        return InputResult(
                            modified = true,
                            buttonVibroEnabled = action.value > 0
                        )
                    }
                    else -> {}
                }
            }
            12 -> {
                // VIBRO POWER — carry new value back to MainActivity via InputResult
                when (action) {
                    is com.example.pockettracker.core.logic.InputAction.SET_VALUE -> {
                        return InputResult(
                            modified = true,
                            vibroPower = action.value.coerceIn(0, 255)
                        )
                    }
                    else -> {}
                }
            }
        }

        return InputResult(
            modified = action !is com.example.pockettracker.core.logic.InputAction.NONE
        )
    }

    /**
     * @param modified      True when something was changed (triggers projectVersion increment)
     * @param buttonSoundEnabled  Non-null when the user toggled BTN SOUND (row 9)
     * @param buttonVibroEnabled  Non-null when the user toggled BTN VIBRO (row 10)
     */
    data class InputResult(
        val modified: Boolean,
        val buttonSoundEnabled: Boolean? = null,
        val buttonSoundVolume: Int? = null,
        val buttonVibroEnabled: Boolean? = null,
        val vibroPower: Int? = null,
        val insertBefore: Boolean? = null   // Non-null when user toggled KB INSERT row
    )
}

/**
 * STATE DATA FOR PROJECT SCREEN
 *
 * @param project The project being edited
 * @param cursorRow Which row (0=TEMPO, 1=TRANSPOSE, 2=NAME, 3=PROJECT, etc.)
 * @param cursorColumn Which column:
 *   - 0 = Parameter name (left column)
 *   - 1+ = Value columns (specific to each row)
 *   For NAME row: 1-12 = character position
 *   For PROJECT row: 1=LOAD, 2=SAVE, 3=NEW
 * @param statusMessage Status message to show at bottom
 * @param isSuccess True if status is success, false if error
 */
data class ProjectState(
    val project: Project,
    val cursorRow: Int = 0,
    val cursorColumn: Int = 1,  // Start on value, not name
    val statusMessage: String = "",
    val isSuccess: Boolean = true,
    val isRendering: Boolean = false,
    val renderProgress: Float = 0f,
    val layoutMode: DeviceAdapter.LayoutMode = DeviceAdapter.LayoutMode.FULL,
    val scalingMode: DeviceAdapter.ScalingMode = DeviceAdapter.ScalingMode.INTEGER,
    val buttonSoundEnabled: Boolean = false,
    val buttonSoundVolume: Int = 255,
    val buttonVibroEnabled: Boolean = false,
    val vibroPower: Int = 255,
    val insertBefore: Boolean = true   // Keyboard insert mode: true=BEFORE cursor, false=AFTER
)