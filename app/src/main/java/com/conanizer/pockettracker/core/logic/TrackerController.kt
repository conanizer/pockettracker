package com.conanizer.pockettracker.core.logic

import com.conanizer.pockettracker.core.data.MAIN_ROW_SCREENS
import com.conanizer.pockettracker.core.data.Note
import com.conanizer.pockettracker.core.data.Project
import com.conanizer.pockettracker.core.data.Chain
import com.conanizer.pockettracker.core.data.Phrase
import com.conanizer.pockettracker.core.data.Table
import com.conanizer.pockettracker.core.data.Groove
import com.conanizer.pockettracker.core.data.Instrument
import com.conanizer.pockettracker.core.data.InstrumentType
import com.conanizer.pockettracker.core.data.ScreenType
import com.conanizer.pockettracker.core.storage.FileInfo

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
    var project = Project(version = 1)
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

    // Per-screen saved cursor positions for SONG/CHAIN/PHRASE (used by REMEMBER cursor mode)
    var songCursorRow = 0
    var songCursorColumn = 1
    var chainCursorRow = 0
    var chainCursorColumn = 1
    var phraseCursorRow = 0
    var phraseCursorColumn = 1

    // Scroll position for SONG screen (0-255 range cursor with 16-row viewport)
    var songScrollPosition = 0
        set(value) {
            field = value.coerceIn(0, 240)
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

    // Settings screen cursor (opened from PROJECT row 6)
    var settingsCursorRow = 0
        set(value) {
            field = value
            stateObserver.onStateChanged()
        }

    var settingsCursorColumn = 1
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
                resetEditingContext()

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
                resetEditingContext()

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
        // Stop playback so no voices are reading samples while we clear them
        playbackController.stop()

        project = Project(version = 1)
        projectVersion++
        statusMessage = "NEW PROJECT"
        statusSuccess = true

        // Clear audio engine samples so empty instruments don't play stale audio
        instrumentController.clearAllSamples()

        resetEditingContext()
    }

    /**
     * Reset all editing context (lastEdited*, current*, cursors) to defaults.
     * Called on NEW project and LOAD project.
     */
    private fun resetEditingContext() {
        // Reset all "remember last" editing context
        lastEditedChain = 0
        lastEditedPhrase = 0
        lastEditedNote = Note.fromString("C-4")
        lastEditedVolume = 0xFF
        lastEditedTranspose = 0
        lastEditedInstrument = 0
        lastEditedTable = 0
        currentChain = 0
        currentPhrase = 0
        currentInstrument = 0
        currentTable = 0
        currentGroove = 0

        // Reset all cursor positions
        cursorRow = 0
        cursorColumn = 1
        songScrollPosition = 0
        instrumentCursorRow = 0
        instrumentCursorColumn = 1
        mixerCursorColumn = 0
        tableCursorRow = 0
        tableCursorColumn = 1
        grooveCursorRow = 0
        modCursorRow = 0
        modCursorPair = 0
        modCursorSide = 0
        projectCursorRow = 0
        projectCursorColumn = 1

        // Reset per-screen saved cursor positions (used by CURSOR REMEMBER setting)
        resetCursorRememberPositions()
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

    /**
     * Reset the per-screen saved cursor positions back to their defaults.
     * Called on NEW project and LOAD project so stale positions don't carry over.
     */
    fun resetCursorRememberPositions() {
        songCursorRow = 0; songCursorColumn = 1
        chainCursorRow = 0; chainCursorColumn = 1
        phraseCursorRow = 0; phraseCursorColumn = 1
    }

    /**
     * Save the current shared cursor (cursorRow/cursorColumn) for SONG/CHAIN/PHRASE screens.
     * Other screens already use dedicated cursor variables and don't need saving here.
     */
    fun saveCursorForScreen(screen: ScreenType) {
        when (screen) {
            ScreenType.SONG   -> { songCursorRow  = cursorRow; songCursorColumn  = cursorColumn }
            ScreenType.CHAIN  -> { chainCursorRow = cursorRow; chainCursorColumn = cursorColumn }
            ScreenType.PHRASE -> { phraseCursorRow = cursorRow; phraseCursorColumn = cursorColumn }
            else -> {}
        }
    }

    /**
     * Restore cursor for [screen] based on [remember] mode.
     *
     * REMEMBER = restore saved per-screen cursor position.
     * REFRESH  = reset cursor to default (row 0, col 1) for all main editing screens.
     */
    fun restoreCursorForScreen(screen: ScreenType, remember: Boolean) {
        if (remember) {
            when (screen) {
                ScreenType.SONG   -> { cursorRow = songCursorRow;   cursorColumn = songCursorColumn }
                ScreenType.CHAIN  -> { cursorRow = chainCursorRow;  cursorColumn = chainCursorColumn }
                ScreenType.PHRASE -> { cursorRow = phraseCursorRow; cursorColumn = phraseCursorColumn }
                // INSTRUMENT, MIXER, TABLE, GROOVE, MODS already have dedicated cursor vars
                // that persist naturally — nothing extra needed in REMEMBER mode.
                else -> {}
            }
        } else {
            // REFRESH — reset every main editing screen cursor to its default position
            when (screen) {
                ScreenType.SONG, ScreenType.CHAIN, ScreenType.PHRASE -> {
                    val (r, c) = getDefaultCursorPosition(screen)
                    cursorRow = r; cursorColumn = c
                }
                ScreenType.INSTRUMENT -> { instrumentCursorRow = 0; instrumentCursorColumn = 1 }
                ScreenType.MIXER      -> { mixerCursorColumn = 0 }
                ScreenType.TABLE      -> { tableCursorRow = 0; tableCursorColumn = 1 }
                ScreenType.GROOVE     -> { grooveCursorRow = 0 }
                ScreenType.MODS       -> { modCursorRow = 0; modCursorPair = 0; modCursorSide = 0 }
                else -> {}
            }
        }
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
                    6  // Wrap to bottom (rows 0-6)
                }
                projectCursorColumn = 1  // Reset to first value column
            }
            ScreenType.SETTINGS -> {
                settingsCursorRow = if (settingsCursorRow > 0) settingsCursorRow - 1 else 7
                settingsCursorColumn = 1
            }
            ScreenType.INSTRUMENT -> {
                val oldRow = instrumentCursorRow
                val oldColumn = instrumentCursorColumn
                val isSoundFont = project.instruments[currentInstrument].instrumentType == InstrumentType.SOUNDFONT
                val maxRow = if (isSoundFont) 11 else 15
                instrumentCursorRow = when {
                    instrumentCursorRow == 7 -> 5          // Skip spacer (row 6) going up
                    !isSoundFont && instrumentCursorRow == 11 -> 9  // Skip SAMPLER spacer (row 10)
                    instrumentCursorRow > 0 -> instrumentCursorRow - 1
                    else -> maxRow                          // Wrap from top to bottom
                }
                // Dual-param rows: 0=TYPE+LOAD+SAVE, 3=ROOT+VOL, 4=DETUNE+PAN
                // 7=DRIVE+FILTER or BANK+PRESET, 8=CRUSH+CUT (SAMPLER), 9=DWNSMPL+RES (SAMPLER)
                // SOUNDFONT adds: 9=ATK+DEC, 10=SUS+REL, 11=CUT+RES
                val dualParamRows = if (isSoundFont) setOf(0, 3, 4, 7, 9, 10, 11) else setOf(0, 3, 4, 7, 8, 9)
                instrumentCursorColumn = if (oldRow in dualParamRows && instrumentCursorRow in dualParamRows && oldColumn == 3) {
                    3
                } else {
                    1
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
            ScreenType.SONG -> {
                // SONG screen: 256 rows (0-255), clamp at 0, scroll to keep cursor visible
                if (cursorRow > 0) {
                    cursorRow--
                    if (cursorRow < songScrollPosition) {
                        songScrollPosition = cursorRow
                    }
                }
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
            ScreenType.SETTINGS -> {
                settingsCursorRow = if (settingsCursorRow < 7) settingsCursorRow + 1 else 0
                settingsCursorColumn = 1
            }
            ScreenType.INSTRUMENT -> {
                val oldRow = instrumentCursorRow
                val oldColumn = instrumentCursorColumn
                val isSoundFont = project.instruments[currentInstrument].instrumentType == InstrumentType.SOUNDFONT
                val maxRow = if (isSoundFont) 11 else 15
                instrumentCursorRow = when {
                    instrumentCursorRow == 5 -> 7          // Skip spacer (row 6) going down
                    !isSoundFont && instrumentCursorRow == 9 -> 11  // Skip SAMPLER spacer (row 10)
                    instrumentCursorRow < maxRow -> instrumentCursorRow + 1
                    else -> 0                              // Wrap from bottom to top
                }
                val dualParamRows = if (isSoundFont) setOf(0, 3, 4, 7, 9, 10, 11) else setOf(0, 3, 4, 7, 8, 9)
                instrumentCursorColumn = if (oldRow in dualParamRows && instrumentCursorRow in dualParamRows && oldColumn == 3) {
                    3
                } else {
                    1
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
            ScreenType.SONG -> {
                // SONG screen: 256 rows (0-255), clamp at 255, scroll to keep cursor visible
                if (cursorRow < 255) {
                    cursorRow++
                    if (cursorRow >= songScrollPosition + 16) {
                        songScrollPosition = cursorRow - 15
                    }
                }
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
            ScreenType.SETTINGS -> {
                // All rows have only column 1 (no multi-column values)
                settingsCursorColumn = 1
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
            ScreenType.SETTINGS -> {
                // All rows have only column 1 (no multi-column values)
                settingsCursorColumn = 1
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
     * Move SONG cursor up by 16 rows (page up), clamped at 0.
     */
    fun moveSongBigUp() {
        val newRow = (cursorRow - 16).coerceAtLeast(0)
        cursorRow = newRow
        songScrollPosition = newRow
    }

    /**
     * Move SONG cursor down by 16 rows (page down), clamped at 255.
     */
    fun moveSongBigDown() {
        val newRow = (cursorRow + 16).coerceAtMost(255)
        cursorRow = newRow
        songScrollPosition = (newRow - 15).coerceAtLeast(0)
    }

    /** Collect used chain/phrase sets from the song (shared by both clean methods). */
    private fun collectUsedRefs(): Triple<Set<Int>, Set<Int>, Set<Int>> {
        val usedChainIds = mutableSetOf<Int>()
        for (track in project.tracks) {
            for (ref in track.chainRefs) { if (ref >= 0) usedChainIds.add(ref) }
        }
        val usedPhraseIds = mutableSetOf<Int>()
        for (chainId in usedChainIds) {
            for (ref in project.chains[chainId].phraseRefs) { if (ref >= 0) usedPhraseIds.add(ref) }
        }
        val usedInstrumentIds = mutableSetOf<Int>()
        for (phraseId in usedPhraseIds) {
            for (step in project.phrases[phraseId].steps) {
                if (step.note != Note.EMPTY) usedInstrumentIds.add(step.instrument)
            }
        }
        return Triple(usedChainIds, usedPhraseIds, usedInstrumentIds)
    }

    /**
     * Clean unused chains and phrases only (SEQ clean).
     */
    fun cleanUnusedSeq() {
        val (usedChainIds, usedPhraseIds, _) = collectUsedRefs()
        for (i in project.chains.indices) {
            if (i !in usedChainIds) project.chains[i] = Chain(id = i)
        }
        for (i in project.phrases.indices) {
            if (i !in usedPhraseIds) project.phrases[i] = Phrase(id = i)
        }
        projectVersion++
        statusMessage = "SEQ CLEANED"
        statusSuccess = true
    }

    /**
     * Clean unused instruments, tables, and grooves (INST clean).
     */
    fun cleanUnusedInst() {
        val (usedChainIds, usedPhraseIds, usedInstrumentIds) = collectUsedRefs()

        val usedTableIds = mutableSetOf<Int>()
        val usedGrooveIds = mutableSetOf<Int>()
        usedGrooveIds.add(0) // Groove 0 always kept

        for (phraseId in usedPhraseIds) {
            for (step in project.phrases[phraseId].steps) {
                val fxPairs = listOf(
                    step.fx1Type to step.fx1Value,
                    step.fx2Type to step.fx2Value,
                    step.fx3Type to step.fx3Value
                )
                for ((fxType, fxValue) in fxPairs) {
                    if (fxType == EffectProcessor.FX_TBL) usedTableIds.add(fxValue and 0xFF)
                    if (fxType == EffectProcessor.FX_GRV) usedGrooveIds.add(fxValue and 0xFF)
                }
            }
        }

        // Implicit table mapping (instrument i → table i) + explicit tableId override
        for (instId in usedInstrumentIds) {
            usedTableIds.add(instId)
            val inst = project.instruments[instId]
            if (inst.tableId >= 0) usedTableIds.add(inst.tableId)
        }

        // Clear all unused instruments (any slot that isn't referenced by the active song)
        for (i in project.instruments.indices) {
            if (i !in usedInstrumentIds) {
                project.instruments[i] = Instrument(id = i)
            }
        }

        for (i in project.tables.indices) {
            if (i !in usedTableIds) project.tables[i] = Table(id = i)
        }

        for (i in project.grooves.indices) {
            if (i !in usedGrooveIds) project.grooves[i] = Groove(id = i)
        }

        projectVersion++
        statusMessage = "INST CLEANED"
        statusSuccess = true
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
            2 -> 20  // NAME: up to column 20 (one per character)
            3 -> 3   // LOAD/SAVE/NEW buttons: up to column 3
            5 -> 2   // CLEAN SEQ/INST buttons: up to column 2
            else -> 1
        }
        return (currentColumn + 1).coerceAtMost(maxColumn)
    }

    /**
     * Helper to get next column left for INSTRUMENT screen.
     * Row 0 (TYPE+LOAD+SAVE) steps through cols 1→2→3; all others jump 3→1.
     */
    private fun getInstrumentCursorLeftColumn(row: Int, currentColumn: Int): Int {
        if (row == 0) return (currentColumn - 1).coerceAtLeast(1)  // step: 3→2→1
        val isSoundFont = project.instruments[currentInstrument].instrumentType == InstrumentType.SOUNDFONT
        val dualParamRows = if (isSoundFont) setOf(3, 4, 7, 9, 10, 11) else setOf(3, 4, 7, 8, 9)
        return if (row in dualParamRows) 1 else 1
    }

    /**
     * Helper to get next column right for INSTRUMENT screen.
     * Row 0 (TYPE+LOAD+SAVE) steps through cols 1→2→3; all others jump 1→3.
     */
    private fun getInstrumentCursorRightColumn(row: Int, currentColumn: Int): Int {
        if (row == 0) return (currentColumn + 1).coerceAtMost(3)  // step: 1→2→3
        val isSoundFont = project.instruments[currentInstrument].instrumentType == InstrumentType.SOUNDFONT
        val dualParamRows = if (isSoundFont) setOf(3, 4, 7, 9, 10, 11) else setOf(3, 4, 7, 8, 9)
        return if (row in dualParamRows) 3 else 1
    }

}
