package com.example.pockettracker.core.logic

import com.example.pockettracker.core.data.MAIN_ROW_SCREENS
import com.example.pockettracker.core.data.Note
import com.example.pockettracker.core.data.Project
import com.example.pockettracker.core.data.ScreenType
import com.example.pockettracker.core.storage.FileInfo

/**
 * TRACKER CONTROLLER
 *
 * Main coordinator for all tracker logic.
 * Owns global state and delegates operations to specialist controllers.
 *
 * ✅ PLATFORM-AGNOSTIC - No Android dependencies!
 *
 * Architecture:
 * - TrackerController (this) - Owns state, coordinates operations
 * - FileController - Save/load projects
 * - PlaybackController - Audio playback
 * - InstrumentController - Sample management
 * - EffectProcessor - Effect calculations (stubs)
 * - ClipboardManager - Copy/paste (stubs)
 * - InputController - Button handling
 *
 * Updated in Phase 5 to remove Compose state dependencies.
 * State changes are communicated to UI layer via callbacks or version counter.
 */
class TrackerController(
    val fileController: FileController,
    val playbackController: PlaybackController,
    val instrumentController: InstrumentController,
    val effectProcessor: EffectProcessor,
    val clipboardManager: ClipboardManager,
    val inputController: InputController,
    private val stateObserver: StateObserver
) {

    // ========================================
    // GLOBAL STATE (owned by coordinator)
    // ========================================

    /**
     * Current project (all tracker data).
     */
    var project = Project()
        set(value) {
            field = value
            stateObserver.onStateChanged()
        }

    /**
     * Current screen being displayed.
     */
    var currentScreen = ScreenType.PHRASE
        set(value) {
            field = value
            stateObserver.onStateChanged()
            if (value == ScreenType.INSTRUMENT) {
                instrumentController.syncToLastEdited(project)
            }
            if (value == ScreenType.TABLE) {
                // Sync table to match current instrument
                currentTable = currentInstrument
            }
        }

    /**
     * Project version counter (for forcing UI recomposition).
     * Increment this whenever project data changes.
     */
    var projectVersion = 0
        set(value) {
            field = value
            stateObserver.onStateChanged()
        }

    /**
     * Status message for user feedback.
     */
    var statusMessage = ""
        set(value) {
            field = value
            stateObserver.onStateChanged()
        }

    /**
     * Whether last operation succeeded.
     */
    var statusSuccess = true
        set(value) {
            field = value
            stateObserver.onStateChanged()
        }

    // === NEW STATE (moved from MainActivity) ===
    // Navigation
    var previousColumn = 2
        set(value) {
            field = value
            stateObserver.onStateChanged()
        }

    // Cursor positions
    var cursorRow = 0
        set(value) {
            field = value
            stateObserver.onStateChanged()
        }

    var cursorColumn = 1
        set(value) {
            field = value
            stateObserver.onStateChanged()
        }

    // Project screen cursor
    var projectCursorRow = 0
        set(value) {
            field = value
            stateObserver.onStateChanged()
        }

    var projectCursorColumn = 1
        set(value) {
            field = value
            stateObserver.onStateChanged()
        }

    // Instrument screen cursor
    var instrumentCursorRow = 0
        set(value) {
            field = value
            stateObserver.onStateChanged()
        }

    var instrumentCursorColumn = 1
        set(value) {
            field = value
            stateObserver.onStateChanged()
        }

    // Mixer screen cursor (0-7 = tracks, 8 = master)
    var mixerCursorColumn = 0
        set(value) {
            field = value.coerceIn(0, 8)
            stateObserver.onStateChanged()
        }

    // Table screen cursor
    var tableCursorRow = 0
        set(value) {
            field = value.coerceIn(0, 15)
            stateObserver.onStateChanged()
        }

    var tableCursorColumn = 1  // Start on transpose column
        set(value) {
            field = value.coerceIn(0, 8)  // 0=step, 1=transpose, 2=vol, 3-8=fx
            stateObserver.onStateChanged()
        }

    // Current table being edited
    var currentTable = 0
        set(value) {
            field = value.coerceIn(0, 255)
            lastEditedTable = value
            stateObserver.onStateChanged()
        }

    var lastEditedTable = 0

    // Groove screen cursor
    var grooveCursorRow = 0
        set(value) {
            field = value.coerceIn(0, 15)
            stateObserver.onStateChanged()
        }

    // Current groove being edited
    var currentGroove = 0
        set(value) {
            field = value.coerceIn(0, 255)
            stateObserver.onStateChanged()
        }

    // Modulation screen cursor
    var modCursorRow = 0
        set(value) {
            field = value
            stateObserver.onStateChanged()
        }

    var modCursorPair = 0
        set(value) {
            field = value.coerceIn(0, 1)
            stateObserver.onStateChanged()
        }

    var modCursorSide = 0
        set(value) {
            field = value.coerceIn(0, 1)
            stateObserver.onStateChanged()
        }

    // Current editing context
    var currentChain = 0
        set(value) {
            field = value
            lastEditedChain = value
            stateObserver.onStateChanged()
        }

    var currentPhrase = 0
        set(value) {
            field = value
            lastEditedPhrase = value
            stateObserver.onStateChanged()
        }

    // Last edited values (for quick insert)
    var lastEditedPhrase = 0

    var lastEditedChain = 0

    var lastEditedNote = Note.fromString("C-4")

    var lastEditedVolume = 0xFF

    var lastEditedTranspose = 0

    // Instrument references
    var currentInstrument = 0
        set(value) {
            field = value
            lastEditedInstrument = value
            stateObserver.onStateChanged()
        }

    var lastEditedInstrument = 0

    // ========================================
    // FILE OPERATIONS (delegate to FileController)
    // ========================================

    /**
     * Save current project.
     */
    fun saveProject(filename: String): FileController.SaveResult {
        val result = fileController.saveProject(project, filename)

        when (result) {
            is FileController.SaveResult.Success -> {
                statusMessage = "SAVED"
                statusSuccess = true
            }
            is FileController.SaveResult.Error -> {
                statusMessage = "SAVE FAILED"
                statusSuccess = false
            }
        }

        return result
    }

    /**
     * Load project from filename.
     */
    fun loadProject(filename: String): FileController.LoadResult {
        val result = fileController.loadProject(filename)

        when (result) {
            is FileController.LoadResult.Success -> {
                project = result.project
                projectVersion++
                statusMessage = "LOADED: $filename"
                statusSuccess = true

                // TODO: Reload project samples if needed in future
            }
            is FileController.LoadResult.Error -> {
                statusMessage = "LOAD FAILED"
                statusSuccess = false
            }
        }

        return result
    }

    /**
     * Load project from FileInfo.
     */
    fun loadProject(fileInfo: FileInfo): FileController.LoadResult {
        val result = fileController.loadProject(fileInfo)

        when (result) {
            is FileController.LoadResult.Success -> {
                project = result.project
                projectVersion++
                statusMessage = "LOADED: ${fileInfo.nameWithoutExtension}"
                statusSuccess = true

                // TODO: Reload project samples if needed in future
            }
            is FileController.LoadResult.Error -> {
                statusMessage = "LOAD FAILED"
                statusSuccess = false
            }
        }

        return result
    }

    /**
     * Create new project.
     */
    fun newProject() {
        project = Project()
        projectVersion++
        statusMessage = "NEW PROJECT"
        statusSuccess = true
    }

    /**
     * List all project files.
     */
    fun listProjects(): List<FileInfo> {
        return fileController.listProjects()
    }

    // ========================================
    // PLAYBACK OPERATIONS (delegate to PlaybackController)
    // ========================================

    /**
     * Play current phrase.
     */
    fun playPhrase(phraseId: Int, loop: Boolean = true) {
        playbackController.playPhrase(project, phraseId, loop)
        currentPhrase = phraseId
    }

    /**
     * Play current chain.
     */
    fun playChain(chainId: Int, loop: Boolean = true) {
        playbackController.playChain(project, chainId, loop)
        currentChain = chainId
    }

    /**
     * Play entire song.
     */
    fun playSong(startRow: Int = 0, loop: Boolean = true) {
        playbackController.playSong(project, startRow, loop)
    }

    /**
     * Stop all playback.
     */
    fun stopPlayback() {
        playbackController.stop()
    }

    /**
     * Check if currently playing.
     */
    fun isPlaying(): Boolean {
        return playbackController.isPlaying
    }

    fun togglePlayback() {
        if (isPlaying()) {
            stopPlayback()
        } else {
            // Start playback based on current screen
            when (currentScreen) {
                ScreenType.PHRASE -> playPhrase(currentPhrase)
                ScreenType.CHAIN -> playChain(currentChain)
                ScreenType.SONG -> playSong()
                else -> playPhrase(currentPhrase) // default
            }
        }
    }

    // ========================================
    // INSTRUMENT OPERATIONS (delegate to InstrumentController)
    // ========================================

    /**
     * Load sample into instrument.
     */
    fun loadSampleIntoInstrument(filePath: String) {
        val result = instrumentController.loadSampleFromFile(project, filePath)

        when (result) {
            is LoadResult.Success -> {
                // Project is modified in-place by InstrumentController
                projectVersion++
                statusMessage = "SAMPLE LOADED"
                statusSuccess = true
            }
            is LoadResult.Error -> {
                statusMessage = result.message
                statusSuccess = false
            }
        }
    }

    /**
     * Preview instrument with current parameters.
     */
    fun previewInstrument() {
        // Sync InstrumentController to TrackerController's currentInstrument before previewing
        instrumentController.currentInstrument = currentInstrument
        instrumentController.previewInstrument(project)
    }

    /**
     * Preview instrument with its associated table.
     * For now, just previews the instrument (table processing not yet implemented).
     */
    fun previewInstrumentWithTable(instrumentId: Int, tableId: Int) {
        instrumentController.previewInstrumentWithTable(project, instrumentId, tableId)
    }

    /**
     * Preview sample file before loading.
     */
    fun previewSampleFile(filePath: String): Boolean {
        return instrumentController.previewSampleFile(filePath)
    }

    /**
     * Update instrument playback parameters.
     */
    fun updateInstrumentPlaybackParams(instrumentId: Int) {
        val instrument = project.instruments[instrumentId]
        instrumentController.updatePlaybackParams(instrument)
        projectVersion++
    }

    // ========================================
    // SCREEN NAVIGATION (coordinator responsibility)
    // ========================================

    /**
     * Navigate to a different screen.
     */
    fun navigateToScreen(screen: ScreenType) {
        currentScreen = screen
    }

    // ========================================
    // CLIPBOARD OPERATIONS (delegate to ClipboardManager)
    // ========================================

    /**
     * Check if clipboard has data.
     */
    fun hasClipboardData(): Boolean {
        return clipboardManager.hasData()
    }

    /**
     * Get clipboard info for UI display.
     */
    fun getClipboardInfo(): String {
        return clipboardManager.getClipboardInfo()
    }

    // Copy/paste operations will be fully implemented in Milestone 2.5

    // ========================================
    // PROJECT DATA ACCESS HELPERS
    // ========================================

    /**
     * Force Compose to recompose when project data changes.
     * Call this after modifying project properties directly.
     */
    fun notifyProjectChanged() {
        projectVersion++
    }

    /**
     * Clear status message.
     */
    fun clearStatus() {
        statusMessage = ""
        statusSuccess = true
    }

    // ========================================
    // SCREEN NAVIGATION
    // ========================================
    // Migrated from MainActivity during cleanup (January 2025)

    /**
     * Get the column (0-4) for a given screen in the 5×5 navigation grid.
     */
    fun getScreenColumn(screen: ScreenType): Int {
        return when (screen) {
            ScreenType.SONG -> 0
            ScreenType.CHAIN -> 1
            ScreenType.PHRASE, ScreenType.GROOVE, ScreenType.SCALE -> 2
            ScreenType.INSTRUMENT, ScreenType.MODS, ScreenType.INST_POOL -> 3
            ScreenType.TABLE -> 4
            // Shared screens (Project, Mixer, Effects) don't have fixed columns
            else -> -1  // Will use previousColumn
        }
    }

    /**
     * Get the main screen (row 2) for a given column.
     */
    fun getMainScreenForColumn(column: Int): ScreenType {
        return when (column) {
            0 -> ScreenType.SONG
            1 -> ScreenType.CHAIN
            2 -> ScreenType.PHRASE
            3 -> ScreenType.INSTRUMENT
            4 -> ScreenType.TABLE
            else -> ScreenType.PHRASE
        }
    }

    /**
     * Navigate UP in the 5×5 grid.
     * Returns: Pair(newScreen, newColumn)
     */
    fun navigateUp(currentScreen: ScreenType, previousColumn: Int): Pair<ScreenType, Int> {
        val currentCol = if (getScreenColumn(currentScreen) == -1) {
            previousColumn
        } else {
            getScreenColumn(currentScreen)
        }

        return when (currentScreen) {
            // FROM ROW 4 (Effects) → UP TO ROW 3 (Mixer)
            ScreenType.EFFECTS -> Pair(ScreenType.MIXER, currentCol)

            // FROM ROW 3 (Mixer) → UP TO ROW 2 (Main)
            ScreenType.MIXER -> {
                val mainScreen = getMainScreenForColumn(currentCol)
                Pair(mainScreen, currentCol)
            }

            // FROM ROW 2 (Main) → UP TO ROW 1
            ScreenType.SONG, ScreenType.CHAIN -> Pair(ScreenType.PROJECT, currentCol)
            ScreenType.PHRASE -> Pair(ScreenType.GROOVE, 2)
            ScreenType.INSTRUMENT -> Pair(ScreenType.MODS, 3)
            ScreenType.TABLE -> Pair(ScreenType.PROJECT, currentCol)

            // FROM ROW 1 → UP TO ROW 0
            ScreenType.PROJECT -> Pair(ScreenType.PROJECT, currentCol)  // Stay on PROJECT
            ScreenType.GROOVE -> Pair(ScreenType.SCALE, 2)
            ScreenType.MODS -> Pair(ScreenType.INST_POOL, 3)

            // FROM ROW 0 → Stay at top
            ScreenType.SCALE, ScreenType.INST_POOL -> Pair(currentScreen, currentCol)

            // Unhandled screens
            else -> Pair(currentScreen, currentCol)
        }
    }

    /**
     * Navigate DOWN in the 5×5 grid.
     */
    fun navigateDown(currentScreen: ScreenType, previousColumn: Int): Pair<ScreenType, Int> {
        val currentCol = if (getScreenColumn(currentScreen) == -1) {
            previousColumn
        } else {
            getScreenColumn(currentScreen)
        }

        return when (currentScreen) {
            // FROM ROW 0 → DOWN TO ROW 1
            ScreenType.SCALE -> Pair(ScreenType.GROOVE, 2)
            ScreenType.INST_POOL -> Pair(ScreenType.MODS, 3)

            // FROM ROW 1 → DOWN TO ROW 2 (Main)
            ScreenType.GROOVE -> Pair(ScreenType.PHRASE, 2)
            ScreenType.MODS -> Pair(ScreenType.INSTRUMENT, 3)
            ScreenType.PROJECT -> {
                val mainScreen = getMainScreenForColumn(currentCol)
                Pair(mainScreen, currentCol)
            }

            // FROM ROW 2 (Main) → DOWN TO ROW 3 (Mixer)
            ScreenType.SONG, ScreenType.CHAIN, ScreenType.PHRASE,
            ScreenType.INSTRUMENT, ScreenType.TABLE -> Pair(ScreenType.MIXER, currentCol)

            // FROM ROW 3 (Mixer) → DOWN TO ROW 4 (Effects)
            ScreenType.MIXER -> Pair(ScreenType.EFFECTS, currentCol)

            // FROM ROW 4 (Effects) → Stay at bottom
            ScreenType.EFFECTS -> Pair(ScreenType.EFFECTS, currentCol)

            // Unhandled screens
            else -> Pair(currentScreen, currentCol)
        }
    }

    /**
     * Navigate LEFT through main row screens (S C P I T).
     */
    fun navigateLeft(currentScreen: ScreenType, previousColumn: Int): Pair<ScreenType, Int> {
        // Shared rooms have no side doors!
        if (currentScreen in listOf(ScreenType.PROJECT, ScreenType.MIXER, ScreenType.EFFECTS)) {
            return Pair(currentScreen, previousColumn)
        }

        // If in a context screen, go to main row first
        if (currentScreen !in MAIN_ROW_SCREENS) {
            val mainScreen = getMainScreenForColumn(getScreenColumn(currentScreen))
            return Pair(mainScreen, getScreenColumn(currentScreen))
        }

        // Move left through S C P I T
        return when (currentScreen) {
            ScreenType.TABLE -> Pair(ScreenType.INSTRUMENT, 3)
            ScreenType.INSTRUMENT -> Pair(ScreenType.PHRASE, 2)
            ScreenType.PHRASE -> Pair(ScreenType.CHAIN, 1)
            ScreenType.CHAIN -> Pair(ScreenType.SONG, 0)
            ScreenType.SONG -> Pair(ScreenType.SONG, 0)  // Stay at leftmost
            else -> Pair(currentScreen, previousColumn)
        }
    }

    /**
     * Navigate RIGHT through main row screens (S C P I T).
     */
    fun navigateRight(currentScreen: ScreenType, previousColumn: Int): Pair<ScreenType, Int> {
        // Shared rooms have no side doors!
        if (currentScreen in listOf(ScreenType.PROJECT, ScreenType.MIXER, ScreenType.EFFECTS)) {
            return Pair(currentScreen, previousColumn)
        }

        // If in a context screen, go to main row first
        if (currentScreen !in MAIN_ROW_SCREENS) {
            val mainScreen = getMainScreenForColumn(getScreenColumn(currentScreen))
            return Pair(mainScreen, getScreenColumn(currentScreen))
        }

        // Move right through S C P I T
        return when (currentScreen) {
            ScreenType.SONG -> Pair(ScreenType.CHAIN, 1)
            ScreenType.CHAIN -> Pair(ScreenType.PHRASE, 2)
            ScreenType.PHRASE -> Pair(ScreenType.INSTRUMENT, 3)
            ScreenType.INSTRUMENT -> Pair(ScreenType.TABLE, 4)
            ScreenType.TABLE -> Pair(ScreenType.TABLE, 4)  // Stay at rightmost
            else -> Pair(currentScreen, previousColumn)
        }
    }

    /**
     * Get minimum editable column for a screen type.
     */
    fun getMinEditableColumn(screenType: ScreenType): Int {
        return when (screenType) {
            ScreenType.PHRASE -> 1
            ScreenType.CHAIN -> 1
            ScreenType.SONG -> 1
            ScreenType.PROJECT -> 1
            ScreenType.INSTRUMENT -> 1
            ScreenType.FILE_BROWSER -> 0
            else -> 0
        }
    }

    /**
     * Get minimum editable row for a screen type.
     */
    fun getMinEditableRow(screenType: ScreenType): Int {
        return 0  // All current screens start editing from row 0
    }

    /**
     * Get default cursor position for a screen.
     * Returns Pair(row, column) representing the first available/editable cell.
     */
    fun getDefaultCursorPosition(screenType: ScreenType): Pair<Int, Int> {
        val defaultRow = getMinEditableRow(screenType)
        val defaultColumn = getMinEditableColumn(screenType)
        return Pair(defaultRow, defaultColumn)
    }

    // ═══════════════════════════════════════════════════════════════════════════
    // CURSOR NAVIGATION (extracted from MainActivity)
    // ═══════════════════════════════════════════════════════════════════════════

    /**
     * Move cursor up on current screen with appropriate wrapping and bounds checking
     */
    fun moveCursorUp() {
        when (currentScreen) {
            ScreenType.PROJECT -> {
                projectCursorRow = if (projectCursorRow > 0) {
                    projectCursorRow - 1
                } else {
                    6  // Wrap to bottom
                }
                projectCursorColumn = 1  // Reset to first value column
            }
            ScreenType.INSTRUMENT -> {
                val oldRow = instrumentCursorRow
                val oldColumn = instrumentCursorColumn
                instrumentCursorRow = when {
                    instrumentCursorRow > 0 && instrumentCursorRow != 6 && instrumentCursorRow != 10 -> instrumentCursorRow - 1
                    instrumentCursorRow == 6 -> 4  // Skip spacer (row 5)
                    instrumentCursorRow == 10 -> 8  // Skip spacer (row 9)
                    else -> 14  // Wrap to bottom
                }

                // Dual-param rows (updated 2026-02-04):
                // Row 0: TYPE+[LOAD], Row 2: ROOT+VOL, Row 3: DETUNE+PAN
                // Row 6: DRIVE+FILTER, Row 7: CRUSH+CUT, Row 8: DWNSMPL+RES
                val dualParamRows = setOf(0, 2, 3, 6, 7, 8)
                // Preserve column 3 when navigating between dual-parameter rows
                instrumentCursorColumn = if (oldRow in dualParamRows && instrumentCursorRow in dualParamRows && oldColumn == 3) {
                    3  // Stay in column 3 when moving within dual-parameter rows
                } else {
                    1  // Reset to column 1 for all other cases
                }
            }
            ScreenType.TABLE -> {
                // Table: 16 rows (0-15) with wrapping
                tableCursorRow = if (tableCursorRow > 0) tableCursorRow - 1 else 15
            }
            ScreenType.GROOVE -> {
                // Groove: 16 rows (0-15) with wrapping
                grooveCursorRow = if (grooveCursorRow > 0) grooveCursorRow - 1 else 15
            }
            ScreenType.MODS -> {
                val inst = project.instruments[currentInstrument]
                val activeRowCount = inst.modSlots[modCursorPair * 2 + modCursorSide].rowCount()
                if (modCursorRow > 0) {
                    modCursorRow--
                } else if (modCursorPair > 0) {
                    // Move to bottom of pair 0, clamped to same side's rowCount
                    modCursorPair = 0
                    val targetSlotRows = inst.modSlots[0 * 2 + modCursorSide].rowCount()
                    modCursorRow = (targetSlotRows - 1).coerceAtLeast(0)
                }
                // At pair 0, row 0 — stay at top (no wrap)
            }
            else -> {
                // All other screens: simple cursor movement with wrapping (rows 0-15)
                cursorRow = if (cursorRow > 0) cursorRow - 1 else 15
            }
        }
    }

    /**
     * Move cursor down on current screen with appropriate wrapping and bounds checking
     */
    fun moveCursorDown() {
        when (currentScreen) {
            ScreenType.PROJECT -> {
                // Project has 7 rows (0-6) with wrapping
                projectCursorRow = if (projectCursorRow < 6) {
                    projectCursorRow + 1
                } else {
                    0  // Wrap to top
                }
                projectCursorColumn = 1  // Reset column
            }
            ScreenType.INSTRUMENT -> {
                val oldRow = instrumentCursorRow
                val oldColumn = instrumentCursorColumn
                instrumentCursorRow = when {
                    instrumentCursorRow < 14 && instrumentCursorRow != 4 && instrumentCursorRow != 8 -> instrumentCursorRow + 1
                    instrumentCursorRow == 4 -> 6  // Skip spacer (row 5)
                    instrumentCursorRow == 8 -> 10  // Skip spacer (row 9)
                    else -> 0  // Wrap to top
                }

                // Dual-param rows (updated 2026-02-04):
                // Row 0: TYPE+[LOAD], Row 2: ROOT+VOL, Row 3: DETUNE+PAN
                // Row 6: DRIVE+FILTER, Row 7: CRUSH+CUT, Row 8: DWNSMPL+RES
                val dualParamRows = setOf(0, 2, 3, 6, 7, 8)
                // Preserve column 3 when navigating between dual-parameter rows
                instrumentCursorColumn = if (oldRow in dualParamRows && instrumentCursorRow in dualParamRows && oldColumn == 3) {
                    3  // Stay in column 3 when moving within dual-parameter rows
                } else {
                    1  // Reset to column 1 for all other cases
                }
            }
            ScreenType.TABLE -> {
                // Table: 16 rows (0-15) with wrapping
                tableCursorRow = if (tableCursorRow < 15) tableCursorRow + 1 else 0
            }
            ScreenType.GROOVE -> {
                // Groove: 16 rows (0-15) with wrapping
                grooveCursorRow = if (grooveCursorRow < 15) grooveCursorRow + 1 else 0
            }
            ScreenType.MODS -> {
                val inst = project.instruments[currentInstrument]
                val activeRowCount = inst.modSlots[modCursorPair * 2 + modCursorSide].rowCount()
                if (modCursorRow < activeRowCount - 1) {
                    modCursorRow++
                } else if (modCursorPair < 1) {
                    modCursorPair = 1
                    modCursorRow = 0
                }
                // At pair 1, last row — stay at bottom (no wrap)
            }
            else -> {
                // Most screens have 16 rows (0-15) with wrapping
                cursorRow = if (cursorRow < 15) cursorRow + 1 else 0
            }
        }
    }

    /**
     * Move cursor left on current screen with appropriate bounds checking
     */
    fun moveCursorLeft() {
        when (currentScreen) {
            ScreenType.PROJECT -> {
                projectCursorColumn = getProjectCursorLeftColumn(projectCursorRow, projectCursorColumn)
            }
            ScreenType.INSTRUMENT -> {
                // Instrument screen: handle left movement for screens with additional columns
                // Most rows: column 1 only (locked)
                // Rows with filter settings: toggle between value columns
                val minColumn = getInstrumentCursorLeftColumn(instrumentCursorRow, instrumentCursorColumn)
                if (instrumentCursorColumn > minColumn) {
                    instrumentCursorColumn = minColumn
                }
            }
            ScreenType.MIXER -> {
                // Mixer: move between track columns (0-7) and master (8)
                if (mixerCursorColumn > 0) mixerCursorColumn--
            }
            ScreenType.TABLE -> {
                // Table: columns 0=step (read-only), 1-8 editable
                // Move left if not at minimum editable column (1)
                if (tableCursorColumn > 1) tableCursorColumn--
            }
            ScreenType.MODS -> {
                modCursorSide = 0  // Switch to left slot
                val inst = project.instruments[currentInstrument]
                val leftRowCount = inst.modSlots[modCursorPair * 2].rowCount()
                modCursorRow = modCursorRow.coerceIn(0, (leftRowCount - 1).coerceAtLeast(0))
            }
            else -> {
                // Get minimum column for this screen
                val minColumn = when (currentScreen) {
                    ScreenType.SONG -> 1    // Column 0 is step number (not editable)
                    ScreenType.CHAIN -> 1
                    ScreenType.PHRASE -> 1
                    else -> 0
                }
                // Move left if not at minimum
                if (cursorColumn > minColumn) cursorColumn--
            }
        }
    }

    /**
     * Move cursor right on current screen with appropriate bounds checking
     */
    fun moveCursorRight() {
        when (currentScreen) {
            ScreenType.PROJECT -> {
                projectCursorColumn = getProjectCursorRightColumn(projectCursorRow, projectCursorColumn)
            }
            ScreenType.INSTRUMENT -> {
                // Instrument screen: handle right movement for screens with additional columns
                // Most rows: column 1 only (locked)
                // Rows with filter settings: toggle between value columns
                val maxColumn = getInstrumentCursorRightColumn(instrumentCursorRow, instrumentCursorColumn)
                if (instrumentCursorColumn < maxColumn) {
                    instrumentCursorColumn = maxColumn
                }
            }
            ScreenType.MIXER -> {
                // Mixer: move between track columns (0-7) and master (8)
                if (mixerCursorColumn < 8) mixerCursorColumn++
            }
            ScreenType.TABLE -> {
                // Table: columns 0=step, 1=transpose, 2=vol, 3-8=fx (0-8 total)
                // Move right if not at maximum column (8)
                if (tableCursorColumn < 8) tableCursorColumn++
            }
            ScreenType.MODS -> {
                modCursorSide = 1  // Switch to right slot
                val inst = project.instruments[currentInstrument]
                val rightRowCount = inst.modSlots[modCursorPair * 2 + 1].rowCount()
                modCursorRow = modCursorRow.coerceIn(0, (rightRowCount - 1).coerceAtLeast(0))
            }
            else -> {
                // Get maximum column for this screen
                val maxColumn = when (currentScreen) {
                    ScreenType.SONG -> 8     // 8 tracks
                    ScreenType.CHAIN -> 2    // PH + TSP columns
                    ScreenType.PHRASE -> 9   // Note + Vol + Inst + FX1(name+val) + FX2(name+val) + FX3(name+val) = 10 columns (0-9)
                    else -> 0
                }
                // Move right if not at maximum
                if (cursorColumn < maxColumn) cursorColumn++
            }
        }
    }

    /**
     * Helper to get next column left for PROJECT screen
     * Column 0 is always unreachable (label column)
     */
    private fun getProjectCursorLeftColumn(row: Int, currentColumn: Int): Int {
        // PROJECT screen column layout per row:
        // All rows: min column is 1 (column 0 is unreachable label)
        // Row 0-2: TEMPO/TRANSPOSE/NAME - columns 1-3 (value and up to 2 name letters)
        // Row 3: LOAD/SAVE/NEW buttons - columns 1-3 (LOAD, SAVE, NEW)
        // Row 4-6: no extra columns - column 1 only
        
        val minColumn = 1  // All rows start at column 1
        
        return (currentColumn - 1).coerceAtLeast(minColumn)
    }

    /**
     * Helper to get next column right for PROJECT screen
     * Column 0 is always unreachable (label column)
     */
    private fun getProjectCursorRightColumn(row: Int, currentColumn: Int): Int {
        // PROJECT screen column layout per row:
        // All rows: min column is 1 (column 0 is unreachable label)
        // Row 0-2: TEMPO/TRANSPOSE/NAME - columns 1-3 (value and up to 2 name letters)
        // Row 3: LOAD/SAVE/NEW buttons - columns 1-3 (LOAD, SAVE, NEW)
        // Row 4-6: no extra columns - column 1 only
        
        val maxColumn = when (row) {
            2 -> 12  // TEMPO/TRANSPOSE/NAME: up to column 3 (includes name letters)
            3 -> 3        // LOAD/SAVE/NEW buttons: up to column 3
            else -> 1     // Other rows: column 1 only
        }
        return (currentColumn + 1).coerceAtMost(maxColumn)
    }

    /**
     * Helper to get next column left for INSTRUMENT screen
     * Columns 0 and 2 are always unreachable (headers/labels)
     */
    private fun getInstrumentCursorLeftColumn(row: Int, currentColumn: Int): Int {
        // INSTRUMENT screen column layout per row (updated 2026-02-04):
        // Row 0: TYPE + [LOAD] - columns 1 and 3 (TYPE at col 1, LOAD button at col 3)
        // Row 1: NAME - column 1 only
        // Row 2: ROOT + VOL - columns 1 and 3
        // Row 3: DETUNE + PAN - columns 1 and 3
        // Row 4: TBL TIC - column 1 only
        // Row 5: SPACER (not reachable)
        // Row 6: DRIVE + FILTER - columns 1 and 3
        // Row 7: CRUSH + CUT - columns 1 and 3
        // Row 8: DWNSMPL + RES - columns 1 and 3
        // Row 9: SPACER (not reachable)
        // Row 10-14: single param rows - column 1 only

        val dualParamRows = setOf(0, 2, 3, 6, 7, 8)
        return if (row in dualParamRows) {
            // Dual-param rows: columns 1 and 3 (skip 2)
            if (currentColumn > 1) 1 else 1
        } else {
            // All other rows: column 1 only
            1
        }
    }

    /**
     * Helper to get next column right for INSTRUMENT screen
     * Columns 0 and 2 are always unreachable (headers/labels)
     */
    private fun getInstrumentCursorRightColumn(row: Int, currentColumn: Int): Int {
        // INSTRUMENT screen column layout per row (updated 2026-02-04):
        // Row 0: TYPE + [LOAD] - columns 1 and 3 (TYPE at col 1, LOAD button at col 3)
        // Row 1: NAME - column 1 only
        // Row 2: ROOT + VOL - columns 1 and 3
        // Row 3: DETUNE + PAN - columns 1 and 3
        // Row 4: TBL TIC - column 1 only
        // Row 5: SPACER (not reachable)
        // Row 6: DRIVE + FILTER - columns 1 and 3
        // Row 7: CRUSH + CUT - columns 1 and 3
        // Row 8: DWNSMPL + RES - columns 1 and 3
        // Row 9: SPACER (not reachable)
        // Row 10-14: single param rows - column 1 only

        val dualParamRows = setOf(0, 2, 3, 6, 7, 8)
        return if (row in dualParamRows) {
            // Dual-param rows: columns 1 and 3 (skip 2)
            if (currentColumn < 3) 3 else 3
        } else {
            // All other rows: column 1 only
            1
        }
    }

}
