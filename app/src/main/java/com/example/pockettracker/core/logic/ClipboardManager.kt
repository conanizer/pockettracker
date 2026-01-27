package com.example.pockettracker.core.logic

import com.example.pockettracker.core.logging.ILogger
import com.example.pockettracker.core.data.Project
import com.example.pockettracker.core.data.Note
import com.example.pockettracker.core.data.PhraseStep

/**
 * CLIPBOARD MANAGER
 *
 * Manages clipboard for copy/paste operations.
 *
 * ✅ PLATFORM-AGNOSTIC - No Android dependencies!
 *
 * LGPT-Style Copy/Paste (Milestone 2.5):
 * - SELECT + B: Enter/cycle selection mode (CELL → ROW → SCREEN)
 * - B (in selection mode): Copy selection + exit
 * - SELECT + A (in selection mode): Cut (copy + delete) + exit
 * - SELECT + A (outside selection): Paste at cursor
 */
class ClipboardManager(
    private val logger: ILogger
) {
    private val TAG = "ClipboardManager"

    // ========================================
    // CLIPBOARD DATA TYPES
    // ========================================

    /**
     * Clipboard item for a single phrase cell (one field of a step).
     * Stores column position so paste can apply correct field.
     */
    data class PhraseStepClipItem(
        val row: Int,           // Row in source phrase (0-15)
        val column: Int,        // Column type (1=note, 2=volume, 3=instrument, 4-9=fx)
        val note: Note?,        // Note value (column 1)
        val volume: Int?,       // Volume value (column 2)
        val instrument: Int?,   // Instrument value (column 3)
        val fxType: Int?,       // FX type (columns 4,6,8)
        val fxValue: Int?       // FX value (columns 5,7,9)
    )

    /**
     * Container for phrase steps clipboard data.
     */
    data class PhraseStepsData(
        val items: List<PhraseStepClipItem>,
        val sourcePhrase: Int,
        val width: Int,         // Number of columns
        val height: Int         // Number of rows
    )

    /**
     * Clipboard item for a chain row.
     */
    data class ChainRowClipItem(
        val row: Int,           // Row in source chain (0-15)
        val column: Int,        // 1=phraseRef, 2=transpose
        val phraseRef: Int?,    // Phrase reference (-1 = empty)
        val transpose: Int?     // Transpose value
    )

    /**
     * Container for chain rows clipboard data.
     */
    data class ChainRowsData(
        val items: List<ChainRowClipItem>,
        val sourceChain: Int,
        val width: Int,
        val height: Int
    )

    /**
     * Clipboard item for a song cell (chain reference in a track).
     */
    data class SongCellClipItem(
        val row: Int,           // Row in source song (0-255)
        val column: Int,        // Track number (1-8)
        val chainRef: Int       // Chain reference (-1 = empty)
    )

    /**
     * Container for song cells clipboard data.
     */
    data class SongCellsData(
        val items: List<SongCellClipItem>,
        val width: Int,         // Number of tracks
        val height: Int         // Number of rows
    )

    /**
     * Current clipboard contents.
     * Null = clipboard is empty.
     */
    var clipboard: ClipboardData? = null
        private set

    /**
     * Clipboard data container.
     */
    data class ClipboardData(
        val type: ClipboardType,
        val data: Any,  // PhraseStepsData or ChainRowsData
        val width: Int,
        val height: Int
    )

    /**
     * Types of clipboard content.
     */
    enum class ClipboardType {
        PHRASE_STEPS,   // Selection of phrase steps (rows × columns)
        CHAIN_ROWS,     // Selection of chain rows
        SONG_CELLS      // Selection of song cells (chain refs across tracks)
    }

    // ========================================
    // COPY OPERATIONS
    // ========================================

    /**
     * Copy phrase steps to clipboard.
     */
    fun copyPhraseSteps(
        project: Project,
        phraseId: Int,
        startRow: Int,
        startColumn: Int,
        endRow: Int,
        endColumn: Int
    ) {
        val phrase = project.phrases[phraseId]
        val items = mutableListOf<PhraseStepClipItem>()

        val minRow = minOf(startRow, endRow)
        val maxRow = maxOf(startRow, endRow)
        val minCol = minOf(startColumn, endColumn)
        val maxCol = maxOf(startColumn, endColumn)

        for (row in minRow..maxRow) {
            val step = phrase.steps[row]
            for (col in minCol..maxCol) {
                val item = when (col) {
                    1 -> PhraseStepClipItem(row - minRow, col, step.note, null, null, null, null)
                    2 -> PhraseStepClipItem(row - minRow, col, null, step.volume, null, null, null)
                    3 -> PhraseStepClipItem(row - minRow, col, null, null, step.instrument, null, null)
                    4 -> PhraseStepClipItem(row - minRow, col, null, null, null, step.fx1Type, null)
                    5 -> PhraseStepClipItem(row - minRow, col, null, null, null, null, step.fx1Value)
                    6 -> PhraseStepClipItem(row - minRow, col, null, null, null, step.fx2Type, null)
                    7 -> PhraseStepClipItem(row - minRow, col, null, null, null, null, step.fx2Value)
                    8 -> PhraseStepClipItem(row - minRow, col, null, null, null, step.fx3Type, null)
                    9 -> PhraseStepClipItem(row - minRow, col, null, null, null, null, step.fx3Value)
                    else -> continue
                }
                items.add(item)
            }
        }

        val data = PhraseStepsData(
            items = items,
            sourcePhrase = phraseId,
            width = maxCol - minCol + 1,
            height = maxRow - minRow + 1
        )

        clipboard = ClipboardData(ClipboardType.PHRASE_STEPS, data, data.width, data.height)
        logger.d(TAG, "📋 Copied phrase steps: ${data.width}x${data.height} from phrase $phraseId")
    }

    /**
     * Copy chain rows to clipboard.
     */
    fun copyChainRows(
        project: Project,
        chainId: Int,
        startRow: Int,
        startColumn: Int,
        endRow: Int,
        endColumn: Int
    ) {
        val chain = project.chains[chainId]
        val items = mutableListOf<ChainRowClipItem>()

        val minRow = minOf(startRow, endRow)
        val maxRow = maxOf(startRow, endRow)
        val minCol = minOf(startColumn, endColumn)
        val maxCol = maxOf(startColumn, endColumn)

        for (row in minRow..maxRow) {
            for (col in minCol..maxCol) {
                val item = when (col) {
                    1 -> ChainRowClipItem(row - minRow, col, chain.phraseRefs[row], null)
                    2 -> ChainRowClipItem(row - minRow, col, null, chain.transposeValues[row])
                    else -> continue
                }
                items.add(item)
            }
        }

        val data = ChainRowsData(
            items = items,
            sourceChain = chainId,
            width = maxCol - minCol + 1,
            height = maxRow - minRow + 1
        )

        clipboard = ClipboardData(ClipboardType.CHAIN_ROWS, data, data.width, data.height)
        logger.d(TAG, "📋 Copied chain rows: ${data.width}x${data.height} from chain $chainId")
    }

    /**
     * Copy song cells to clipboard.
     * Note: columns are 1-8 (track numbers), rows are 0-255 (song rows)
     */
    fun copySongCells(
        project: Project,
        startRow: Int,
        startColumn: Int,  // 1-8 (track number)
        endRow: Int,
        endColumn: Int     // 1-8 (track number)
    ) {
        val items = mutableListOf<SongCellClipItem>()

        val minRow = minOf(startRow, endRow)
        val maxRow = maxOf(startRow, endRow)
        val minCol = minOf(startColumn, endColumn)
        val maxCol = maxOf(startColumn, endColumn)

        for (row in minRow..maxRow) {
            for (col in minCol..maxCol) {
                // Track index is col - 1 (columns are 1-8, track indices are 0-7)
                val trackIndex = col - 1
                if (trackIndex < 0 || trackIndex >= 8) continue

                val track = project.tracks[trackIndex]
                val chainRef = if (row < track.chainRefs.size) {
                    track.chainRefs[row]
                } else {
                    -1  // Empty
                }

                items.add(SongCellClipItem(row - minRow, col, chainRef))
            }
        }

        val data = SongCellsData(
            items = items,
            width = maxCol - minCol + 1,
            height = maxRow - minRow + 1
        )

        clipboard = ClipboardData(ClipboardType.SONG_CELLS, data, data.width, data.height)
        logger.d(TAG, "📋 Copied song cells: ${data.width}x${data.height}")
    }

    // ========================================
    // PASTE OPERATIONS
    // ========================================

    /**
     * Paste clipboard contents at cursor position.
     *
     * @param project The project to paste into
     * @param targetScreen Which screen (PHRASE, CHAIN)
     * @param targetId Current phrase/chain ID
     * @param cursorRow Current cursor row
     * @param cursorColumn Current cursor column
     * @return PasteResult indicating success or failure
     */
    fun paste(
        project: Project,
        targetScreen: String,
        targetId: Int,
        cursorRow: Int,
        cursorColumn: Int
    ): PasteResult {
        val clip = clipboard ?: return PasteResult.NoClipboard

        return when (clip.type) {
            ClipboardType.PHRASE_STEPS -> {
                if (targetScreen != "PHRASE") {
                    return PasteResult.Error("Can only paste phrase data to phrase screen")
                }
                pastePhraseSteps(project, targetId, cursorRow, cursorColumn, clip.data as PhraseStepsData)
            }
            ClipboardType.CHAIN_ROWS -> {
                if (targetScreen != "CHAIN") {
                    return PasteResult.Error("Can only paste chain data to chain screen")
                }
                pasteChainRows(project, targetId, cursorRow, cursorColumn, clip.data as ChainRowsData)
            }
            ClipboardType.SONG_CELLS -> {
                if (targetScreen != "SONG") {
                    return PasteResult.Error("Can only paste song data to song screen")
                }
                pasteSongCells(project, cursorRow, cursorColumn, clip.data as SongCellsData)
            }
        }
    }

    /**
     * Paste phrase steps at cursor position.
     */
    private fun pastePhraseSteps(
        project: Project,
        phraseId: Int,
        cursorRow: Int,
        cursorColumn: Int,
        data: PhraseStepsData
    ): PasteResult {
        val phrase = project.phrases[phraseId]
        var itemsPasted = 0

        for (item in data.items) {
            val targetRow = cursorRow + item.row
            val targetCol = cursorColumn + (item.column - data.items.minOf { it.column })

            // Skip if out of bounds
            if (targetRow < 0 || targetRow >= 16) continue
            if (targetCol < 1 || targetCol > 9) continue

            val step = phrase.steps[targetRow]

            when (targetCol) {
                1 -> item.note?.let { step.note = it; itemsPasted++ }
                2 -> item.volume?.let { step.volume = it; itemsPasted++ }
                3 -> item.instrument?.let { step.instrument = it; itemsPasted++ }
                4 -> item.fxType?.let { step.fx1Type = it; itemsPasted++ }
                5 -> item.fxValue?.let { step.fx1Value = it; itemsPasted++ }
                6 -> item.fxType?.let { step.fx2Type = it; itemsPasted++ }
                7 -> item.fxValue?.let { step.fx2Value = it; itemsPasted++ }
                8 -> item.fxType?.let { step.fx3Type = it; itemsPasted++ }
                9 -> item.fxValue?.let { step.fx3Value = it; itemsPasted++ }
            }
        }

        logger.d(TAG, "📋 Pasted $itemsPasted phrase items to phrase $phraseId at ($cursorRow, $cursorColumn)")
        return PasteResult.Success(itemsPasted)
    }

    /**
     * Paste chain rows at cursor position.
     */
    private fun pasteChainRows(
        project: Project,
        chainId: Int,
        cursorRow: Int,
        cursorColumn: Int,
        data: ChainRowsData
    ): PasteResult {
        val chain = project.chains[chainId]
        var itemsPasted = 0

        for (item in data.items) {
            val targetRow = cursorRow + item.row
            val targetCol = cursorColumn + (item.column - data.items.minOf { it.column })

            // Skip if out of bounds
            if (targetRow < 0 || targetRow >= 16) continue
            if (targetCol < 1 || targetCol > 2) continue

            when (targetCol) {
                1 -> item.phraseRef?.let { chain.phraseRefs[targetRow] = it; itemsPasted++ }
                2 -> item.transpose?.let { chain.transposeValues[targetRow] = it; itemsPasted++ }
            }
        }

        logger.d(TAG, "📋 Pasted $itemsPasted chain items to chain $chainId at ($cursorRow, $cursorColumn)")
        return PasteResult.Success(itemsPasted)
    }

    /**
     * Paste song cells at cursor position.
     * Cursor column is 1-8 (track number).
     */
    private fun pasteSongCells(
        project: Project,
        cursorRow: Int,
        cursorColumn: Int,  // 1-8 (track number)
        data: SongCellsData
    ): PasteResult {
        var itemsPasted = 0
        val minSourceCol = data.items.minOfOrNull { it.column } ?: return PasteResult.Success(0)

        for (item in data.items) {
            val targetRow = cursorRow + item.row
            val targetCol = cursorColumn + (item.column - minSourceCol)

            // Skip if out of bounds (columns are 1-8)
            if (targetRow < 0 || targetRow >= 256) continue
            if (targetCol < 1 || targetCol > 8) continue

            // Track index is targetCol - 1
            val trackIndex = targetCol - 1
            val track = project.tracks[trackIndex]

            // Ensure track has enough rows
            while (track.chainRefs.size <= targetRow) {
                track.chainRefs.add(-1)
            }

            track.chainRefs[targetRow] = item.chainRef
            itemsPasted++
        }

        logger.d(TAG, "📋 Pasted $itemsPasted song cells at ($cursorRow, track $cursorColumn)")
        return PasteResult.Success(itemsPasted)
    }

    // ========================================
    // CUT OPERATION (copy + delete)
    // ========================================

    /**
     * Cut phrase steps (copy + clear source).
     */
    fun cutPhraseSteps(
        project: Project,
        phraseId: Int,
        startRow: Int,
        startColumn: Int,
        endRow: Int,
        endColumn: Int
    ): CutResult {
        // First copy
        copyPhraseSteps(project, phraseId, startRow, startColumn, endRow, endColumn)

        // Then clear source
        val phrase = project.phrases[phraseId]
        val minRow = minOf(startRow, endRow)
        val maxRow = maxOf(startRow, endRow)
        val minCol = minOf(startColumn, endColumn)
        val maxCol = maxOf(startColumn, endColumn)
        var itemsCut = 0

        for (row in minRow..maxRow) {
            val step = phrase.steps[row]
            for (col in minCol..maxCol) {
                when (col) {
                    1 -> { step.note = Note.EMPTY; itemsCut++ }
                    2 -> { step.volume = 0xFF; itemsCut++ }
                    3 -> { step.instrument = 0; itemsCut++ }
                    4 -> { step.fx1Type = 0; itemsCut++ }
                    5 -> { step.fx1Value = 0; itemsCut++ }
                    6 -> { step.fx2Type = 0; itemsCut++ }
                    7 -> { step.fx2Value = 0; itemsCut++ }
                    8 -> { step.fx3Type = 0; itemsCut++ }
                    9 -> { step.fx3Value = 0; itemsCut++ }
                }
            }
        }

        logger.d(TAG, "✂️ Cut $itemsCut phrase items from phrase $phraseId")
        return CutResult.Success(itemsCut)
    }

    /**
     * Cut chain rows (copy + clear source).
     */
    fun cutChainRows(
        project: Project,
        chainId: Int,
        startRow: Int,
        startColumn: Int,
        endRow: Int,
        endColumn: Int
    ): CutResult {
        // First copy
        copyChainRows(project, chainId, startRow, startColumn, endRow, endColumn)

        // Then clear source
        val chain = project.chains[chainId]
        val minRow = minOf(startRow, endRow)
        val maxRow = maxOf(startRow, endRow)
        val minCol = minOf(startColumn, endColumn)
        val maxCol = maxOf(startColumn, endColumn)
        var itemsCut = 0

        for (row in minRow..maxRow) {
            for (col in minCol..maxCol) {
                when (col) {
                    1 -> { chain.phraseRefs[row] = -1; itemsCut++ }  // -1 = empty
                    2 -> { chain.transposeValues[row] = 0x00; itemsCut++ }
                }
            }
        }

        logger.d(TAG, "✂️ Cut $itemsCut chain items from chain $chainId")
        return CutResult.Success(itemsCut)
    }

    /**
     * Cut song cells (copy + clear source).
     */
    fun cutSongCells(
        project: Project,
        startRow: Int,
        startColumn: Int,  // 1-8 (track number)
        endRow: Int,
        endColumn: Int     // 1-8 (track number)
    ): CutResult {
        // First copy
        copySongCells(project, startRow, startColumn, endRow, endColumn)

        // Then clear source
        val minRow = minOf(startRow, endRow)
        val maxRow = maxOf(startRow, endRow)
        val minCol = minOf(startColumn, endColumn)
        val maxCol = maxOf(startColumn, endColumn)
        var itemsCut = 0

        for (row in minRow..maxRow) {
            for (col in minCol..maxCol) {
                val trackIndex = col - 1
                if (trackIndex < 0 || trackIndex >= 8) continue

                val track = project.tracks[trackIndex]
                if (row < track.chainRefs.size) {
                    track.chainRefs[row] = -1  // Clear to empty
                    itemsCut++
                }
            }
        }

        logger.d(TAG, "✂️ Cut $itemsCut song cells")
        return CutResult.Success(itemsCut)
    }

    // ========================================
    // DELETE OPERATIONS (clear without copy)
    // ========================================

    /**
     * Delete phrase steps (clear without copying to clipboard).
     * Used by A+B in selection mode.
     */
    fun deletePhraseSteps(
        project: Project,
        phraseId: Int,
        startRow: Int,
        startColumn: Int,
        endRow: Int,
        endColumn: Int
    ): DeleteResult {
        val phrase = project.phrases[phraseId]
        val minRow = minOf(startRow, endRow)
        val maxRow = maxOf(startRow, endRow)
        val minCol = minOf(startColumn, endColumn)
        val maxCol = maxOf(startColumn, endColumn)
        var itemsDeleted = 0

        for (row in minRow..maxRow) {
            val step = phrase.steps[row]
            for (col in minCol..maxCol) {
                when (col) {
                    1 -> { step.note = Note.EMPTY; itemsDeleted++ }
                    2 -> { step.volume = 0xFF; itemsDeleted++ }
                    3 -> { step.instrument = 0; itemsDeleted++ }
                    4 -> { step.fx1Type = 0; itemsDeleted++ }
                    5 -> { step.fx1Value = 0; itemsDeleted++ }
                    6 -> { step.fx2Type = 0; itemsDeleted++ }
                    7 -> { step.fx2Value = 0; itemsDeleted++ }
                    8 -> { step.fx3Type = 0; itemsDeleted++ }
                    9 -> { step.fx3Value = 0; itemsDeleted++ }
                }
            }
        }

        logger.d(TAG, "🗑️ Deleted $itemsDeleted phrase items from phrase $phraseId")
        return DeleteResult.Success(itemsDeleted)
    }

    /**
     * Delete chain rows (clear without copying to clipboard).
     * Used by A+B in selection mode.
     */
    fun deleteChainRows(
        project: Project,
        chainId: Int,
        startRow: Int,
        startColumn: Int,
        endRow: Int,
        endColumn: Int
    ): DeleteResult {
        val chain = project.chains[chainId]
        val minRow = minOf(startRow, endRow)
        val maxRow = maxOf(startRow, endRow)
        val minCol = minOf(startColumn, endColumn)
        val maxCol = maxOf(startColumn, endColumn)
        var itemsDeleted = 0

        for (row in minRow..maxRow) {
            for (col in minCol..maxCol) {
                when (col) {
                    1 -> { chain.phraseRefs[row] = -1; itemsDeleted++ }  // -1 = empty
                    2 -> { chain.transposeValues[row] = 0x00; itemsDeleted++ }
                }
            }
        }

        logger.d(TAG, "🗑️ Deleted $itemsDeleted chain items from chain $chainId")
        return DeleteResult.Success(itemsDeleted)
    }

    /**
     * Delete song cells (clear without copying to clipboard).
     * Used by A+B in selection mode.
     */
    fun deleteSongCells(
        project: Project,
        startRow: Int,
        startColumn: Int,  // 1-8 (track number)
        endRow: Int,
        endColumn: Int     // 1-8 (track number)
    ): DeleteResult {
        val minRow = minOf(startRow, endRow)
        val maxRow = maxOf(startRow, endRow)
        val minCol = minOf(startColumn, endColumn)
        val maxCol = maxOf(startColumn, endColumn)
        var itemsDeleted = 0

        for (row in minRow..maxRow) {
            for (col in minCol..maxCol) {
                val trackIndex = col - 1
                if (trackIndex < 0 || trackIndex >= 8) continue

                val track = project.tracks[trackIndex]
                if (row < track.chainRefs.size) {
                    track.chainRefs[row] = -1  // Clear to empty
                    itemsDeleted++
                }
            }
        }

        logger.d(TAG, "🗑️ Deleted $itemsDeleted song cells")
        return DeleteResult.Success(itemsDeleted)
    }

    // ========================================
    // UTILITY METHODS
    // ========================================

    /**
     * Clear clipboard.
     */
    fun clear() {
        clipboard = null
        logger.d(TAG, "🗑️ Clipboard cleared")
    }

    /**
     * Check if clipboard has data.
     */
    fun hasData(): Boolean = clipboard != null

    /**
     * Get clipboard info string for UI display.
     */
    fun getClipboardInfo(): String {
        val clip = clipboard ?: return ""
        val typeStr = when (clip.type) {
            ClipboardType.PHRASE_STEPS -> "PHR"
            ClipboardType.CHAIN_ROWS -> "CHN"
            ClipboardType.SONG_CELLS -> "SNG"
        }
        return "$typeStr:${clip.width}x${clip.height}"
    }

    // ========================================
    // RESULT TYPES
    // ========================================

    /**
     * Result of paste operation.
     */
    sealed class PasteResult {
        object NoClipboard : PasteResult()
        data class Success(val itemsPasted: Int) : PasteResult()
        data class Error(val message: String) : PasteResult()
    }

    /**
     * Result of cut operation.
     */
    sealed class CutResult {
        data class Success(val itemsCut: Int) : CutResult()
        data class Error(val message: String) : CutResult()
    }

    /**
     * Result of delete operation (clear without copy).
     */
    sealed class DeleteResult {
        data class Success(val itemsDeleted: Int) : DeleteResult()
        data class Error(val message: String) : DeleteResult()
    }
}
