package com.conanizer.pockettracker

import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.geometry.Size
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.drawscope.DrawScope
import com.conanizer.pockettracker.core.data.Project

/**
 * SONG EDITOR MODULE - FINAL DESIGN
 *
 * This is the "top level" view where you arrange chains into tracks
 *
 * Layout:
 * - Step column (00-FF, read-only, no header)
 * - 8 track columns (chain references, headers: 1-8)
 * - 16 visible rows (scrollable)
 * - Same spacing as Chain/Phrase screens
 *
 * Example view:
 * SONG: MYSONG       BPM: 128
 *     1   2   3   4   5   6   7   8
 * 00  00  --  --  --  --  --  --  --
 * 01  01  --  --  --  --  --  --  --
 * 02  02  --  --  --  --  --  --  --
 * 03  --  --  --  --  --  --  --  --
 * ...
 *
 * Size: 620×392 pixels (same as phrase/chain)
 * State type: SongEditorState
 */
class SongEditorModule : TrackerModule {
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

    // Number of visible rows (16 chains visible at once)
    private val VISIBLE_ROWS = 16

    /**
     * Main draw function - renders the song arrangement view
     *
     * @param x X position in design pixels
     * @param y Y position in design pixels
     * @param scale Screen scale factor
     * @param state SongEditorState - contains project and cursor info
     */
    override fun DrawScope.draw(x: Int, y: Int, scale: Int, state: Any?) {
        // Cast state or return if wrong type
        val songState = state as? SongEditorState ?: return

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
        // Match phrase/chain spacing EXACTLY!
        // ===================================
        var colX = x + 10  // 10px left padding (same as phrase/chain)

        // STEP column (00-FF, 2 chars)
        val stepX = colX
        colX += 30 + 20  // 30px for 2 chars + 10px gap

        // Track columns (8 tracks, each showing 2-char chain ID)
        val trackColumns = IntArray(8)
        for (i in 0..7) {
            trackColumns[i] = colX
            colX += 30 + 20  // 30px for 2 chars + 15px gap between tracks
        }

        // ===================================
        // STEP 3: Draw header "SONG: NAME"
        // (BPM is now shown in the right bar track note monitor)
        // ===================================
        var rowY = y + TEXT_PADDING

        // Song name
        val songName = songState.project.name.take(10)  // Max 10 chars
        drawBitmapText(
            text = "SONG: $songName",
            x = x + 10,
            y = rowY,
            scale = scale,
            color = Color.Cyan,
            spacing = CHAR_SPACING,
            fontScale = FONT_SCALE
        )

        // ===================================
        // STEP 4: Move down with spacer
        // ===================================
        rowY = y + ROW_HEIGHT + 14 + TEXT_PADDING

        // ===================================
        // STEP 5: Draw track headers (1-8, no header for step column)
        // ===================================
        for (trackId in 0..7) {
            drawBitmapText(
                text = "${trackId + 1}",  // 1-8 (not 0-7)
                x = trackColumns[trackId],
                y = rowY,
                scale = scale,
                color = Color.Gray,
                spacing = CHAR_SPACING,
                fontScale = FONT_SCALE
            )
        }

        // ===================================
        // STEP 6: Draw song grid (16 visible rows × 8 tracks)
        // ===================================
        for (rowIndex in 0 until VISIBLE_ROWS) {
            // Calculate absolute position (accounting for scroll)
            val absoluteRow = songState.scrollPosition + rowIndex

            drawSongRow(
                x = x,
                y = y,
                scale = scale,
                rowIndex = rowIndex,        // Visual row (0-15)
                absoluteRow = absoluteRow,  // Actual position in song
                state = songState,
                stepX = stepX,
                trackColumns = trackColumns
            )
        }
    }

    /**
     * Draw a single row of the song (shows chains for all 8 tracks)
     *
     * @param x Module X position
     * @param y Module Y position
     * @param scale Screen scale
     * @param rowIndex Visual row index (0-15)
     * @param absoluteRow Actual row in song (accounting for scroll)
     * @param state Full song state
     * @param stepX X position for step column
     * @param trackColumns X positions for each track column (array of 8)
     */
    private fun DrawScope.drawSongRow(
        x: Int,
        y: Int,
        scale: Int,
        rowIndex: Int,
        absoluteRow: Int,
        state: SongEditorState,
        stepX: Int,
        trackColumns: IntArray
    ) {
        // ===================================
        // STEP 1: Calculate Y position for this row
        // ===================================
        // Start after: header (21px) + spacer (14px) + track headers (21px)
        // Then add rowIndex × 21px
        val dataRowY = y + ROW_HEIGHT + 14 + ROW_HEIGHT + (rowIndex * ROW_HEIGHT)

        // ===================================
        // STEP 2: Determine background color
        // ===================================
        // Check if any cell in this row is selected (tracks are 1-8 in selection, mapped to columns 1-8)
        val isRowSelected = state.selectionMode && (1..8).any { col -> state.isCellSelected(absoluteRow, col) }

        val bgColor = when {
            // Playing row -> green highlight
            state.isPlaying && absoluteRow == state.playbackRow -> Color(0xFF004400)

            // Selection green
            isRowSelected -> Color(0xFF1a3a1a)

            // If cursor is on this row -> highlight
            absoluteRow == state.cursorRow -> Color(0xFF333333)

            // Every 4th row -> slightly lighter
            absoluteRow % 4 == 0 -> Color(0xFF151515)

            // Default
            else -> Color(0xFF0a0a0a)
        }

        // ===================================
        // STEP 3: Draw row background
        // ===================================
        drawRect(
            color = bgColor,
            topLeft = Offset((x * scale).toFloat(), (dataRowY * scale).toFloat()),
            size = Size((width * scale).toFloat(), (ROW_HEIGHT * scale).toFloat())
        )

        val textY = dataRowY + TEXT_PADDING

        // ===================================
        // STEP 4: Draw STEP column (00-FF, read-only)
        // ===================================
        drawBitmapText(
            text = absoluteRow.toString(16).padStart(2, '0').uppercase(),  // 00-FF
            x = stepX,
            y = textY,
            scale = scale,
            color = if (absoluteRow % 4 == 0) Color(0xFFAAAAAA) else Color(0xFF666666),
            spacing = CHAR_SPACING,
            fontScale = FONT_SCALE
        )

        // ===================================
        // STEP 5: Draw chain reference for each track
        // ===================================
        for (trackId in 0..7) {
            // Get this track's chain sequence
            val track = state.project.tracks[trackId]

            // Get chain ID at this position (or -1 if beyond end)
            val chainId = if (absoluteRow < track.chainRefs.size) {
                track.chainRefs[absoluteRow]
            } else {
                -1  // No chain at this position
            }

            // Format as text
            val chainText = if (chainId == -1) {
                "--"  // Empty
            } else {
                // Show chain ID in hex (e.g., 0 -> "00", 255 -> "FF")
                chainId.toString(16).padStart(2, '0').uppercase()
            }

            // Determine text color
            // Note: trackId is 0-7, but cursorTrack and selection columns are 1-8
            val selectionCol = trackId + 1  // Convert to selection column (1-8)
            val textColor = when {
                // Cursor is on this cell (track column starts at 1, not 0)
                absoluteRow == state.cursorRow &&
                        trackId == (state.cursorTrack - 1) -> Color.Yellow

                // Selection highlight (bright green)
                state.selectionMode && state.isCellSelected(absoluteRow, selectionCol) -> Color(0xFF00DD00)

                // Empty cell
                chainId == -1 -> Color(0xFF444444)

                // Normal chain reference
                else -> Color.White
            }

            // Draw the chain ID
            drawBitmapText(
                text = chainText,
                x = trackColumns[trackId],
                y = textY,
                scale = scale,
                color = textColor,
                spacing = CHAR_SPACING,
                fontScale = FONT_SCALE
            )
        }
    }

    /**
     * Get cursor context for current cursor position
     *
     * This tells the generic input system what kind of value we're on
     * and what actions are available.
     */
    fun getCursorContext(state: SongEditorState): CursorContext {
        // Step column is not selectable, only tracks 1-8
        if (state.cursorTrack < 1 || state.cursorTrack > 8) {
            return CursorContextFactory.none()
        }

        // Get the track (cursorTrack is 1-8, array index is 0-7)
        val track = state.project.tracks[state.cursorTrack - 1]

        // Get chain reference at current row
        val chainRef = if (state.cursorRow < track.chainRefs.size) {
            track.chainRefs[state.cursorRow]
        } else {
            -1  // Beyond current track length
        }

        // Return cursor context for chain reference
        return CursorContextFactory.chainRef(chainRef, canCreate = true)
    }

    /**
     * Handle input action for song editor.
     */
    fun handleInput(
        state: SongEditorState,
        action: com.conanizer.pockettracker.core.logic.InputAction
    ): InputResult {
        // Get track index (cursorTrack is 1-8, array index is 0-7)
        val trackIndex = state.cursorTrack - 1
        if (trackIndex < 0 || trackIndex >= state.project.tracks.size) {
            return InputResult(modified = false)
        }

        val track = state.project.tracks[trackIndex]
        var lastEditedChain: Int? = null

        when (action) {
            is com.conanizer.pockettracker.core.logic.InputAction.SET_VALUE -> {
                // Ensure track is long enough
                while (track.chainRefs.size <= state.cursorRow) {
                    track.chainRefs.add(-1)
                }
                track.chainRefs[state.cursorRow] = action.value
                lastEditedChain = action.value
            }
            is com.conanizer.pockettracker.core.logic.InputAction.DELETE -> {
                // Clear chain reference
                if (state.cursorRow < track.chainRefs.size) {
                    track.chainRefs[state.cursorRow] = -1
                }
            }
            is com.conanizer.pockettracker.core.logic.InputAction.INSERT_DEFAULT -> {
                // Insert chain 0 by default
                while (track.chainRefs.size <= state.cursorRow) {
                    track.chainRefs.add(-1)
                }
                track.chainRefs[state.cursorRow] = 0
                lastEditedChain = 0
            }
            else -> { /* NONE or unhandled - do nothing */ }
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

/**
 * STATE DATA FOR SONG EDITOR MODULE
 *
 * @param project The full project (contains all 8 tracks)
 * @param cursorRow Which row the cursor is on (absolute position, 0-255)
 * @param cursorTrack Which track/column the cursor is on (1-8, NOT 0-7!)
 * @param scrollPosition Vertical scroll offset (for showing more than 16 rows)
 * @param isPlaying Whether song playback is active
 * @param playbackRow Current playback position (0-255, synchronized across all tracks)
 * @param selectionMode Whether selection mode is active
 * @param isCellSelected Function to check if a cell is selected
 */
data class SongEditorState(
    val project: Project,       // Full project data
    val cursorRow: Int,         // Cursor row (absolute position 0-255)
    val cursorTrack: Int,       // Which track (1-8, step column is not selectable)
    val scrollPosition: Int = 0, // Scroll offset (0 = top of song)
    val isPlaying: Boolean = false, // Playback state
    val playbackRow: Int = 0,   // Current playback row (all tracks play in sync)
    val selectionMode: Boolean = false,
    val isCellSelected: (Int, Int) -> Boolean = { _, _ -> false }
)