package com.conanizer.pockettracker.ui.modules

import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.geometry.Size
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.drawscope.DrawScope
import com.conanizer.pockettracker.ui.theme.AppTheme
import com.conanizer.pockettracker.input.CursorContext
import com.conanizer.pockettracker.input.CursorContextFactory
import com.conanizer.pockettracker.ui.TrackerModule
import com.conanizer.pockettracker.ui.clearChainSlot
import com.conanizer.pockettracker.core.data.Chain
import com.conanizer.pockettracker.core.logic.InputAction
import com.conanizer.pockettracker.ui.drawBitmapText
import com.conanizer.pockettracker.ui.rowBgColor
import com.conanizer.pockettracker.ui.toHex2

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

        val t = chainState.appTheme
        drawRect(
            color   = Color(t.background),
            topLeft = Offset((x * scale).toFloat(), (y * scale).toFloat()),
            size    = Size((width * scale).toFloat(), (height * scale).toFloat())
        )

        var colX  = x + 10
        val stepX = colX; colX += 30 + 10
        val phX   = colX; colX += 30 + 20
        val tspX  = colX

        var rowY = y + TEXT_PADDING
        drawBitmapText("CHAIN ${chainState.chain.id.toHex2()}", x + 10, rowY, scale,
            Color(t.textTitle), CHAR_SPACING, FONT_SCALE)

        rowY = y + ROW_HEIGHT + 14 + TEXT_PADDING
        drawBitmapText("PH",  phX,  rowY, scale, Color(t.textParam), CHAR_SPACING, FONT_SCALE)
        drawBitmapText("TSP", tspX, rowY, scale, Color(t.textParam), CHAR_SPACING, FONT_SCALE)

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

        val t = state.appTheme
        drawRect(
            color   = rowBgColor(
                index,
                state.cursorRow,
                state.playbackRow,
                state.isPlaying,
                isRowSelected,
                t
            ),
            topLeft = Offset((x * scale).toFloat(), (dataRowY * scale).toFloat()),
            size    = Size((width * scale).toFloat(), (ROW_HEIGHT * scale).toFloat())
        )

        val textY = dataRowY + TEXT_PADDING

        drawBitmapText(
            text = index.toString(16).uppercase(),
            x = stepX, y = textY, scale = scale,
            color = when {
                index == state.cursorRow && state.cursorColumn == 0 -> Color(t.textCursor)
                index % 4 == 0 -> Color(t.textParam)
                else -> Color(t.textEmpty)
            },
            spacing = CHAR_SPACING, fontScale = FONT_SCALE
        )

        drawBitmapText(
            text = if (isEmpty) "--" else phraseRef.toHex2(),
            x = phX, y = textY, scale = scale,
            color = when {
                index == state.cursorRow && state.cursorColumn == 1 -> Color(t.textCursor)
                state.selectionMode && state.isCellSelected(index, 1) -> Color(t.vizWave)
                isEmpty -> Color(t.textEmpty)
                else -> Color(t.textValue)
            },
            spacing = CHAR_SPACING, fontScale = FONT_SCALE
        )

        val transposeValue = state.chain.transposeValues[index]
        drawBitmapText(
            text = if (isEmpty) "--" else transposeValue.toHex2(),
            x = tspX, y = textY, scale = scale,
            color = when {
                index == state.cursorRow && state.cursorColumn == 2 -> Color(t.textCursor)
                state.selectionMode && state.isCellSelected(index, 2) -> Color(t.vizWave)
                isEmpty -> Color(t.textEmpty)
                else -> Color(t.textParam)
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
                isEmpty = state.chain.phraseRefs[state.cursorRow] == -1,
                default = 0x00
            )
            else -> CursorContextFactory.none()
        }
    }

    fun handleInput(
        state: ChainEditorState,
        action: InputAction
    ): InputResult {
        val chain = state.chain
        var lastEditedPhrase: Int? = null
        var lastEditedTranspose: Int? = null

        when (action) {
            is InputAction.SET_VALUE -> {
                when (state.cursorColumn) {
                    1 -> { chain.phraseRefs[state.cursorRow] = action.value; lastEditedPhrase = action.value }
                    2 -> { chain.transposeValues[state.cursorRow] = action.value; lastEditedTranspose = action.value }
                }
            }
            is InputAction.DELETE -> {
                if (state.cursorColumn == 1) clearChainSlot(chain, state.cursorRow)
            }
            is InputAction.INSERT_DEFAULT -> {
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
            modified = action !is InputAction.NONE,
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
    val isCellSelected: (Int, Int) -> Boolean = { _, _ -> false },
    val appTheme: AppTheme = AppTheme.Companion.CLASSIC
)
