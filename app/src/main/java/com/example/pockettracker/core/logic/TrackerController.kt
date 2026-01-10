package com.example.pockettracker.core.logic

import com.example.pockettracker.Note
import com.example.pockettracker.Project
import com.example.pockettracker.ScreenType
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
        private set

    // Cursor positions
    var cursorRow = 0
        private set

    var cursorColumn = 1
        private set

    // Project screen cursor
    var projectCursorRow = 0
        private set

    var projectCursorColumn = 1
        private set

    // Instrument screen cursor
    var instrumentCursorRow = 0
        private set

    var instrumentCursorColumn = 1
        private set

    // Current editing context
    var currentChain = 0
        private set

    var currentPhrase = 0
        private set

    // Last edited values (for quick insert)
    var lastEditedPhrase = 0
        private set

    var lastEditedChain = 0
        private set

    var lastEditedNote = Note.fromString("C-4")
        private set

    var lastEditedVolume = 0xFF
        private set

    var lastEditedTranspose = 0
        private set

    // Instrument references
    var currentInstrument = 0
        private set

    var lastEditedInstrument = 0
        private set

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
    }

    /**
     * Play current chain.
     */
    fun playChain(chainId: Int, loop: Boolean = true) {
        playbackController.playChain(project, chainId, loop)
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

    // ========================================
    // INSTRUMENT OPERATIONS (delegate to InstrumentController)
    // ========================================

    /**
     * Load sample into instrument.
     */
    fun loadSampleIntoInstrument(filePath: String) {
        val result = instrumentController.loadSampleFromFile(project, filePath)

        when (result) {
            is com.example.pockettracker.core.logic.LoadResult.Success -> {
                // Project is modified in-place by InstrumentController
                projectVersion++
                statusMessage = "SAMPLE LOADED"
                statusSuccess = true
            }
            is com.example.pockettracker.core.logic.LoadResult.Error -> {
                statusMessage = result.message
                statusSuccess = false
            }
        }
    }

    /**
     * Preview instrument with current parameters.
     */
    fun previewInstrument() {
        instrumentController.previewInstrument(project)
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
        if (currentScreen !in com.example.pockettracker.MAIN_ROW_SCREENS) {
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
        if (currentScreen !in com.example.pockettracker.MAIN_ROW_SCREENS) {
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

    // === SIMPLE SETTER METHODS ===
    fun setCursor(row: Int, column: Int) {
        cursorRow = row
        cursorColumn = column
        notifyStateChanged()
    }

    fun setProjectCursor(row: Int, column: Int) {
        projectCursorRow = row
        projectCursorColumn = column
        notifyStateChanged()
    }

    fun setInstrumentCursor(row: Int, column: Int) {
        instrumentCursorRow = row
        instrumentCursorColumn = column
        notifyStateChanged()
    }

    fun setCurrentChain(chain: Int) {
        currentChain = chain
        lastEditedChain = chain
        notifyStateChanged()
    }

    fun setCurrentPhrase(phrase: Int) {
        currentPhrase = phrase
        lastEditedPhrase = phrase
        notifyStateChanged()
    }

    fun setCurrentInstrument(instrument: Int) {
        currentInstrument = instrument
        lastEditedInstrument = instrument
        notifyStateChanged()
    }

    fun updateLastEditedNote(note: Note) {
        lastEditedNote = note
    }

    fun updateLastEditedVolume(volume: Int) {
        lastEditedVolume = volume
    }

    fun updateLastEditedTranspose(transpose: Int) {
        lastEditedTranspose = transpose
    }


    // === PRIVATE HELPER ===
    private fun notifyStateChanged() {
        projectVersion++
        stateObserver.onStateChanged()
    }
}
