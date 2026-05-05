package com.conanizer.pockettracker

import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.geometry.Size
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.drawscope.DrawScope
import com.conanizer.pockettracker.core.data.Chain

/**
 * CHAIN EDITOR MODULE
 *
 * 16 phrase slots with optional transpose per slot.
 * Transpose encoding: 00=−128 semi, 80=0 (no change), FF=+127 semi.
 *
 * Size: 510×392 px
 */
class ChainEditorModule : TrackerModule {
    override val width  = 510
    override val height = 392

    private val FONT_SCALE   = 3
    private val CHAR_SPACING = 2
    private val ROW_HEIGHT   = 21
    private val TEXT_PADDING = 3

    override fun DrawScope.draw(x: Int, y: Int, scale: Int, state: Any?) {
        val chainState = state as? ChainEditorState ?: return

        drawRect(
            color   = Color(0xFF0a0a0a),
            topLeft = Offset((x * scale).toFloat(), (y * scale).toFloat()),
            size    = Size((width * scale).toFloat(), (height * scale).toFloat())
        )

        var colX  = x + 10
        val stepX = colX; colX += 30 + 10
        val phX   = colX; colX += 30 + 20
        val tspX  = colX

        var rowY = y + TEXT_PADDING
        drawBitmapText("CHAIN ${chainState.chain.id.toHex2()}", x + 10, rowY, scale, Color.Cyan, CHAR_SPACING, FONT_SCALE)

        rowY = y + ROW_HEIGHT + 14 + TEXT_PADDING
        drawBitmapText("PH",  phX,  rowY, scale, Color.Gray, CHAR_SPACING, FONT_SCALE)
        drawBitmapText("TSP", tspX, rowY, scale, Color.Gray, CHAR_SPACING, FONT_SCALE)

        for (index in 0..15) {
            drawChainRow(x, y, scale, index, chainState, stepX, phX, tspX)
        }
    }

    private fun DrawScope.drawChainRow(
        x: Int, y: Int, scale: Int,
        index: Int,
        state: ChainEditorState,
        stepX: Int, phX: Int, tspX: Int
    ) {
        val dataRowY = y + ROW_HEIGHT + 14 + ROW_HEIGHT + (index * ROW_HEIGHT)
        val phraseRef = state.chain.phraseRefs[index]
        val isEmpty   = phraseRef == -1
        val isRowSelected = state.selectionMode && (1..2).any { col -> state.isCellSelected(index, col) }

        drawRect(
            color   = rowBgColor(index, state.cursorRow, state.playbackRow, state.isPlaying, isRowSelected),
            topLeft = Offset((x * scale).toFloat(), (dataRowY * scale).toFloat()),
            size    = Size((width * scale).toFloat(), (ROW_HEIGHT * scale).toFloat())
        )

        val textY = dataRowY + TEXT_PADDING

        drawBitmapText(
            text = index.toString(16).uppercase(),
            x = stepX, y = textY, scale = scale,
            color = when {
                index == state.cursorRow && state.cursorColumn == 0 -> Color.Yellow
                index % 4 == 0 -> Color(0xFFAAAAAA)
                else -> Color(0xFF666666)
            },
            spacing = CHAR_SPACING, fontScale = FONT_SCALE
        )

        drawBitmapText(
            text = if (isEmpty) "--" else phraseRef.toHex2(),
            x = phX, y = textY, scale = scale,
            color = when {
                index == state.cursorRow && state.cursorColumn == 1 -> Color.Yellow
                state.selectionMode && state.isCellSelected(index, 1) -> Color(0xFF00DD00)
                isEmpty -> Color(0xFF444444)
                else -> Color.White
            },
            spacing = CHAR_SPACING, fontScale = FONT_SCALE
        )

        val transposeValue = state.chain.transposeValues[index]
        drawBitmapText(
            text = if (isEmpty) "--" else transposeValue.toHex2(),
            x = tspX, y = textY, scale = scale,
            color = when {
                index == state.cursorRow && state.cursorColumn == 2 -> Color.Yellow
                state.selectionMode && state.isCellSelected(index, 2) -> Color(0xFF00DD00)
                isEmpty -> Color(0xFF444444)
                transposeValue == 0x80 -> Color(0xFF888888)  // 0x80 = no transpose, dim it
                else -> Color(0xFFaaaaaa)
            },
            spacing = CHAR_SPACING, fontScale = FONT_SCALE
        )
    }

    fun getCursorContext(state: ChainEditorState): CursorContext {
        return when (state.cursorColumn) {
            0 -> CursorContextFactory.readOnly()
            1 -> CursorContextFactory.phraseRef(state.chain.phraseRefs[state.cursorRow], canCreate = true)
            2 -> CursorContextFactory.transpose(
                state.chain.transposeValues[state.cursorRow],
                isEmpty = state.chain.phraseRefs[state.cursorRow] == -1
            )
            else -> CursorContextFactory.none()
        }
    }

    fun handleInput(
        state: ChainEditorState,
        action: com.conanizer.pockettracker.core.logic.InputAction
    ): InputResult {
        val chain = state.chain
        var lastEditedPhrase: Int? = null
        var lastEditedTranspose: Int? = null

        when (action) {
            is com.conanizer.pockettracker.core.logic.InputAction.SET_VALUE -> {
                when (state.cursorColumn) {
                    1 -> { chain.phraseRefs[state.cursorRow] = action.value; lastEditedPhrase = action.value }
                    2 -> { chain.transposeValues[state.cursorRow] = action.value; lastEditedTranspose = action.value }
                }
            }
            is com.conanizer.pockettracker.core.logic.InputAction.DELETE -> {
                if (state.cursorColumn == 1) clearChainSlot(chain, state.cursorRow)
            }
            is com.conanizer.pockettracker.core.logic.InputAction.INSERT_DEFAULT -> {
                if (state.cursorColumn == 1) {
                    chain.phraseRefs[state.cursorRow] = 0
                    chain.transposeValues[state.cursorRow] = 0x00
                    lastEditedPhrase = 0
                    lastEditedTranspose = 0
                }
            }
            else -> {}
        }

        return InputResult(
            modified = action !is com.conanizer.pockettracker.core.logic.InputAction.NONE,
            lastEditedPhrase = lastEditedPhrase,
            lastEditedTranspose = lastEditedTranspose
        )
    }

    data class InputResult(
        val modified: Boolean,
        val lastEditedPhrase: Int? = null,
        val lastEditedTranspose: Int? = null
    )
}

data class ChainEditorState(
    val chain: Chain,
    val cursorRow: Int,
    val cursorColumn: Int,
    val playbackRow: Int = 0,
    val isPlaying: Boolean = false,
    val selectionMode: Boolean = false,
    val isCellSelected: (Int, Int) -> Boolean = { _, _ -> false }
)
