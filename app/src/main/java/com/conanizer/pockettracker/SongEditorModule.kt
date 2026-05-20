package com.conanizer.pockettracker

import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.geometry.Size
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.drawscope.DrawScope
import com.conanizer.pockettracker.core.data.Project

/**
 * SONG EDITOR MODULE
 *
 * Top-level view: arrange chains across 8 tracks (256 rows, 16 visible at once).
 * cursorTrack is 1-8 (not 0-7).
 *
 * Size: 510×392 px
 */
class SongEditorModule : TrackerModule {
    override val width  = 510
    override val height = 392

    private val FONT_SCALE   = 3
    private val CHAR_SPACING = 2
    private val ROW_HEIGHT   = 21
    private val TEXT_PADDING = 3
    private val VISIBLE_ROWS = 16

    override fun DrawScope.draw(x: Int, y: Int, scale: Int, state: Any?) {
        val songState = state as? SongEditorState ?: return

        val t = songState.appTheme
        drawRect(
            color   = Color(t.background),
            topLeft = Offset((x * scale).toFloat(), (y * scale).toFloat()),
            size    = Size((width * scale).toFloat(), (height * scale).toFloat())
        )

        var colX = x + 10
        val stepX = colX; colX += 30 + 20
        val trackColumns = IntArray(8)
        for (i in 0..7) { trackColumns[i] = colX; colX += 30 + 20 }

        var rowY = y + TEXT_PADDING
        drawBitmapText("SONG: ${songState.project.name.take(20)}", x + 10, rowY, scale, Color(t.textTitle), CHAR_SPACING, FONT_SCALE)

        rowY = y + ROW_HEIGHT + 14 + TEXT_PADDING
        for (trackId in 0..7) {
            drawBitmapText("${trackId + 1}", trackColumns[trackId], rowY, scale, Color(t.textParam), CHAR_SPACING, FONT_SCALE)
        }

        for (rowIndex in 0 until VISIBLE_ROWS) {
            val absoluteRow = songState.scrollPosition + rowIndex
            drawSongRow(x, y, scale, rowIndex, absoluteRow, songState, stepX, trackColumns)
        }
    }

    private fun DrawScope.drawSongRow(
        x: Int, y: Int, scale: Int,
        rowIndex: Int,
        absoluteRow: Int,
        state: SongEditorState,
        stepX: Int,
        trackColumns: IntArray
    ) {
        val dataRowY = y + ROW_HEIGHT + 14 + ROW_HEIGHT + (rowIndex * ROW_HEIGHT)
        val isRowSelected = state.selectionMode && (1..8).any { col -> state.isCellSelected(absoluteRow, col) }

        val t = state.appTheme
        drawRect(
            color   = rowBgColor(absoluteRow, state.cursorRow, state.playbackRow, state.isPlaying, isRowSelected, t),
            topLeft = Offset((x * scale).toFloat(), (dataRowY * scale).toFloat()),
            size    = Size((width * scale).toFloat(), (ROW_HEIGHT * scale).toFloat())
        )

        val textY = dataRowY + TEXT_PADDING

        drawBitmapText(
            text = absoluteRow.toHex2(),
            x = stepX, y = textY, scale = scale,
            color = if (absoluteRow % 4 == 0) Color(t.textParam) else Color(t.textEmpty),
            spacing = CHAR_SPACING, fontScale = FONT_SCALE
        )

        for (trackId in 0..7) {
            val track   = state.project.tracks[trackId]
            val chainId = if (absoluteRow < track.chainRefs.size) track.chainRefs[absoluteRow] else -1
            val selectionCol = trackId + 1

            drawBitmapText(
                text = if (chainId == -1) "--" else chainId.toHex2(),
                x = trackColumns[trackId], y = textY, scale = scale,
                color = when {
                    absoluteRow == state.cursorRow && trackId == (state.cursorTrack - 1) -> Color(t.textCursor)
                    state.selectionMode && state.isCellSelected(absoluteRow, selectionCol) -> Color(0xFF00DD00)
                    chainId == -1 -> Color(t.textEmpty)
                    else -> Color(t.textValue)
                },
                spacing = CHAR_SPACING, fontScale = FONT_SCALE
            )
        }
    }

    fun getCursorContext(state: SongEditorState): CursorContext {
        if (state.cursorTrack < 1 || state.cursorTrack > 8) return CursorContextFactory.none()
        val track    = state.project.tracks[state.cursorTrack - 1]
        val chainRef = if (state.cursorRow < track.chainRefs.size) track.chainRefs[state.cursorRow] else -1
        return CursorContextFactory.chainRef(chainRef, canCreate = true)
    }

    fun handleInput(
        state: SongEditorState,
        action: com.conanizer.pockettracker.core.logic.InputAction
    ): InputResult {
        val trackIndex = state.cursorTrack - 1
        if (trackIndex < 0 || trackIndex >= state.project.tracks.size) return InputResult(modified = false)

        val track = state.project.tracks[trackIndex]
        var lastEditedChain: Int? = null

        when (action) {
            is com.conanizer.pockettracker.core.logic.InputAction.SET_VALUE -> {
                while (track.chainRefs.size <= state.cursorRow) track.chainRefs.add(-1)
                track.chainRefs[state.cursorRow] = action.value
                lastEditedChain = action.value
            }
            is com.conanizer.pockettracker.core.logic.InputAction.DELETE -> {
                clearSongChainRef(track, state.cursorRow)
            }
            is com.conanizer.pockettracker.core.logic.InputAction.INSERT_DEFAULT -> {
                while (track.chainRefs.size <= state.cursorRow) track.chainRefs.add(-1)
                track.chainRefs[state.cursorRow] = 0
                lastEditedChain = 0
            }
            else -> {}
        }

        return InputResult(
            modified = action !is com.conanizer.pockettracker.core.logic.InputAction.NONE,
            lastEditedChain = lastEditedChain
        )
    }

    data class InputResult(
        val modified: Boolean,
        val lastEditedChain: Int? = null
    )
}

data class SongEditorState(
    val project: Project,
    val cursorRow: Int,
    val cursorTrack: Int,           // 1-8 (not 0-7)
    val scrollPosition: Int = 0,
    val isPlaying: Boolean = false,
    val playbackRow: Int = 0,
    val selectionMode: Boolean = false,
    val isCellSelected: (Int, Int) -> Boolean = { _, _ -> false },
    val appTheme: AppTheme = AppTheme.CLASSIC
)
