package com.example.pockettracker

import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.geometry.Size
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.drawscope.DrawScope

/**
 * CHAIN EDITOR MODULE - IMPROVED VERSION
 *
 * A chain is a sequence of 16 phrases with optional transpose
 *
 * Columns:
 * - STEP (hidden label, just row number in hex)
 * - PH (Phrase reference 00-FF or --)
 * - TSP (Transpose in semitones, 00-FF where 80 = no transpose)
 *
 * Transpose:
 * - 00 = -128 semitones (down)
 * - 80 = 0 semitones (no change)
 * - FF = +127 semitones (up)
 * - Practically useful range: 70-90 (-16 to +16 semitones)
 *
 * Size: 620×392 pixels (same as phrase editor)
 * State type: ChainEditorState
 */
class ChainEditorModule : TrackerModule {
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
     * Main draw function - renders the chain editor
     */
    override fun DrawScope.draw(x: Int, y: Int, scale: Int, state: Any?) {
        // Cast state or return if wrong type
        val chainState = state as? ChainEditorState ?: return

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
        // Match phrase editor spacing!
        // ===================================
        var colX = x + 10  // 10px left padding (same as phrase)

        val stepX = colX
        colX += 30 + 10      // Step number: 2 chars (30px) + 10px gap

        val phX = colX
        colX += 30 + 20      // Phrase: 2 chars (30px) + 20px gap

        val tspX = colX      // Transpose: 2 chars (30px)

        // ===================================
        // STEP 3: Draw header "CHAIN 00"
        // ===================================
        var rowY = y + TEXT_PADDING

        // Convert chain ID to hex string
        val chainIdHex = chainState.chain.id
            .toString(16)
            .padStart(2, '0')
            .uppercase()

        drawBitmapText(
            text = "CHAIN $chainIdHex",
            x = x + 10,
            y = rowY,
            scale = scale,
            color = Color.Cyan,
            spacing = CHAR_SPACING,
            fontScale = FONT_SCALE
        )

        // ===================================
        // STEP 4: Spacer after header
        // ===================================
        rowY = y + ROW_HEIGHT + 14 + TEXT_PADDING

        // ===================================
        // STEP 5: Draw column headers
        // No "STEP" label - just show PH and TSP
        // ===================================
        drawBitmapText(
            text = "PH",  // Phrase column header
            x = phX,
            y = rowY,
            scale = scale,
            color = Color.Gray,
            spacing = CHAR_SPACING,
            fontScale = FONT_SCALE
        )

        drawBitmapText(
            text = "TSP",  // Transpose column header
            x = tspX,
            y = rowY,
            scale = scale,
            color = Color.Gray,
            spacing = CHAR_SPACING,
            fontScale = FONT_SCALE
        )

        // ===================================
        // STEP 6: Draw 16 data rows
        // ===================================
        for (index in 0..15) {
            // Get phrase reference at this slot
            val phraseRef = chainState.chain.phraseRefs[index]

            // Get transpose value at this slot (IntArray not in Chain yet!)
            // For now we'll use 0x80 (no transpose) as default
            // You'll need to add transposeValues: IntArray to Chain data class
            val transposeValue = chainState.chain.transposeValues[index]  // TODO: Add to Chain data structure

            drawChainRow(
                x = x,
                y = y,
                scale = scale,
                index = index,
                phraseRef = phraseRef,
                transposeValue = transposeValue,
                state = chainState,
                stepX = stepX,
                phX = phX,
                tspX = tspX
            )
        }
    }

    /**
     * Draw a single row in the chain
     *
     * Shows: [step number] [phrase ref] [transpose]
     */
    private fun DrawScope.drawChainRow(
        x: Int,
        y: Int,
        scale: Int,
        index: Int,
        phraseRef: Int,
        transposeValue: Int,
        state: ChainEditorState,
        stepX: Int,
        phX: Int,
        tspX: Int
    ) {
        // ===================================
        // STEP 1: Calculate Y position
        // ===================================
        val dataRowY = y + ROW_HEIGHT + 14 + ROW_HEIGHT + (index * ROW_HEIGHT)

        // ===================================
        // STEP 2: Row background color
        // ===================================
        val bgColor = when {
            // Cursor on this row
            index == state.cursorRow -> Color(0xFF333333)

            // Every 4th row
            index % 4 == 0 -> Color(0xFF151515)

            // Default
            else -> Color(0xFF0a0a0a)
        }

        // Draw row background
        drawRect(
            color = bgColor,
            topLeft = Offset((x * scale).toFloat(), (dataRowY * scale).toFloat()),
            size = Size((width * scale).toFloat(), (ROW_HEIGHT * scale).toFloat())
        )

        val textY = dataRowY + TEXT_PADDING

        // ===================================
        // COLUMN 0: STEP NUMBER (no header label)
        // ===================================
        drawBitmapText(
            text = index.toString(16).padStart(1, '0').uppercase(),  // 00-0F
            x = stepX,
            y = textY,
            scale = scale,
            // Highlight if cursor is on this row and STEP column
            color = if (index == state.cursorRow && state.cursorColumn == 0)
                Color.Yellow
            else
                Color(0xFF666666),  // Gray
            spacing = CHAR_SPACING,
            fontScale = FONT_SCALE
        )

        // ===================================
        // COLUMN 1: PHRASE REFERENCE (PH)
        // ===================================
        val isEmpty = phraseRef == 0xFF

        val phraseText = if (isEmpty) {
            "--"
        } else {
            phraseRef.toString(16).padStart(2, '0').uppercase()
        }

        val phraseColor = when {
            index == state.cursorRow && state.cursorColumn == 1 -> Color.Yellow
            isEmpty -> Color(0xFF444444)
            else -> Color.White
        }

        drawBitmapText(
            text = phraseText,
            x = phX,
            y = textY,
            scale = scale,
            color = phraseColor,
            spacing = CHAR_SPACING,
            fontScale = FONT_SCALE
        )

        // ===================================
        // COLUMN 2: TRANSPOSE (TSP)
        // ===================================
        // Only show transpose if phrase is not empty
        val transposeText = if (isEmpty) {
            "--"
        } else {
            transposeValue.toString(16).padStart(2, '0').uppercase()
        }

        val transposeColor = when {
            index == state.cursorRow && state.cursorColumn == 2 -> Color.Yellow
            isEmpty -> Color(0xFF444444)
            transposeValue == 0x80 -> Color(0xFF888888)  // Dimmed when no transpose
            else -> Color(0xFFaaaaaa)  // Normal gray when transposed
        }

        drawBitmapText(
            text = transposeText,
            x = tspX,
            y = textY,
            scale = scale,
            color = transposeColor,
            spacing = CHAR_SPACING,
            fontScale = FONT_SCALE
        )
    }
}

/**
 * STATE DATA FOR CHAIN EDITOR
 *
 * @param chain The chain to display
 * @param cursorRow Which row (0-15)
 * @param cursorColumn Which column (0=step, 1=phrase, 2=transpose)
 */
data class ChainEditorState(
    val chain: Chain,
    val cursorRow: Int,
    val cursorColumn: Int
)