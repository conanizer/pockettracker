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

class TrackerController(
    val fileController: FileController,
    val playbackController: PlaybackController,
    val instrumentController: InstrumentController,
    val effectProcessor: EffectProcessor,
    val clipboardManager: ClipboardManager,
    val inputController: InputController,
    private val stateObserver: StateObserver
) {

    var project = Project(version = 1)
        set(value) {
            field = value
            stateObserver.onStateChanged()
        }

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

    var projectVersion = 0
        set(value) {
            field = value
            stateObserver.onStateChanged()
        }

    // Set to match projectVersion after save/load/new so isProjectDirty works correctly.
    var savedProjectVersion = 0
    val isProjectDirty get() = projectVersion != savedProjectVersion

    var statusMessage = ""
        set(value) {
            field = value
            stateObserver.onStateChanged()
        }

    var statusSuccess = true
        set(value) {
            field = value
            stateObserver.onStateChanged()
        }

    var previousColumn = 2
        set(value) {
            field = value
            stateObserver.onStateChanged()
        }

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

    // Mixer master sub-row (reachable from any column via DPAD UP/DOWN)
    // 0 = volume row, 1 = MASTER FX selector, 2 = effect depth, 3 = LIM pre-gain
    var mixerMasterRow = 0
        set(value) {
            field = value.coerceIn(0, 3)
            stateObserver.onStateChanged()
        }

    var effectsCursorRow = 0
        set(value) {
            field = value.coerceIn(0, 7)
            stateObserver.onStateChanged()
        }

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

    var currentTable = 0
        set(value) {
            field = value.coerceIn(0, 255)
            lastEditedTable = value
            stateObserver.onStateChanged()
        }

    var lastEditedTable = 0

    var grooveCursorRow = 0
        set(value) {
            field = value.coerceIn(0, 15)
            stateObserver.onStateChanged()
        }

    var currentGroove = 0
        set(value) {
            field = value.coerceIn(0, 255)
            stateObserver.onStateChanged()
        }

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

    var lastEditedPhrase = 0

    var lastEditedChain = 0

    var lastEditedNote = Note.fromString("C-4")

    var lastEditedVolume = 0xFF

    var lastEditedTranspose = 0

    var currentInstrument = 0
        set(value) {
            field = value
            lastEditedInstrument = value
            stateObserver.onStateChanged()
        }

    var lastEditedInstrument = 0

    fun saveProject(filename: String): FileController.SaveResult {
        val result = fileController.saveProject(project, filename)

        when (result) {
            is FileController.SaveResult.Success -> {
                savedProjectVersion = projectVersion
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

    fun loadProject(filename: String): FileController.LoadResult {
        val result = fileController.loadProject(filename)

        when (result) {
            is FileController.LoadResult.Success -> {
                project = result.project
                projectVersion++
                savedProjectVersion = projectVersion
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

    fun loadProject(fileInfo: FileInfo): FileController.LoadResult {
        val result = fileController.loadProject(fileInfo)

        when (result) {
            is FileController.LoadResult.Success -> {
                project = result.project
                projectVersion++
                savedProjectVersion = projectVersion
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

    fun newProject() {
        // Stop before clearing so no voices are reading samples while we free them.
        playbackController.stop()

        project = Project(version = 1)
        projectVersion++
        savedProjectVersion = projectVersion
        statusMessage = "NEW PROJECT"
        statusSuccess = true

        instrumentController.clearAllSamples()

        resetEditingContext()
    }

    private fun resetEditingContext() {
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

        cursorRow = 0
        cursorColumn = 1
        songScrollPosition = 0
        instrumentCursorRow = 0
        instrumentCursorColumn = 1
        mixerCursorColumn = 0
        effectsCursorRow = 0
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

    fun listProjects(): List<FileInfo> {
        return fileController.listProjects()
    }

    fun playPhrase(phraseId: Int, loop: Boolean = true) {
        playbackController.playPhrase(project, phraseId, loop)
        currentPhrase = phraseId
    }

    fun playChain(chainId: Int, loop: Boolean = true) {
        playbackController.playChain(project, chainId, loop)
        currentChain = chainId
    }

    fun playSong(startRow: Int = 0, loop: Boolean = true) {
        playbackController.playSong(project, startRow, loop)
    }

    fun stopPlayback() {
        playbackController.stop()
    }

    fun isPlaying(): Boolean {
        return playbackController.isPlaying
    }

    fun togglePlayback() {
        if (isPlaying()) {
            stopPlayback()
        } else {
            when (currentScreen) {
                ScreenType.PHRASE -> playPhrase(currentPhrase)
                ScreenType.CHAIN -> playChain(currentChain)
                ScreenType.SONG -> playSong()
                else -> playPhrase(currentPhrase) // default
            }
        }
    }

    fun loadSampleIntoInstrument(filePath: String) {
        val result = instrumentController.loadSampleFromFile(project, filePath)

        when (result) {
            is LoadResult.Success -> {
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

    fun previewInstrument() {
        instrumentController.currentInstrument = currentInstrument
        instrumentController.previewInstrument(project)
    }

    fun previewInstrumentWithTable(instrumentId: Int, tableId: Int) {
        instrumentController.previewInstrumentWithTable(project, instrumentId, tableId)
    }

    fun previewSampleFile(filePath: String): Boolean {
        return instrumentController.previewSampleFile(filePath)
    }

    fun updateInstrumentPlaybackParams(instrumentId: Int) {
        val instrument = project.instruments[instrumentId]
        instrumentController.updatePlaybackParams(instrument)
        projectVersion++
    }

    fun navigateToScreen(screen: ScreenType) {
        currentScreen = screen
    }

    fun hasClipboardData(): Boolean {
        return clipboardManager.hasData()
    }

    fun getClipboardInfo(): String {
        return clipboardManager.getClipboardInfo()
    }

    fun notifyProjectChanged() {
        projectVersion++
    }

    fun clearStatus() {
        statusMessage = ""
        statusSuccess = true
    }

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

    fun getMinEditableRow(screenType: ScreenType): Int {
        return 0
    }

    fun getDefaultCursorPosition(screenType: ScreenType): Pair<Int, Int> {
        val defaultRow = getMinEditableRow(screenType)
        val defaultColumn = getMinEditableColumn(screenType)
        return Pair(defaultRow, defaultColumn)
    }

    fun resetCursorRememberPositions() {
        songCursorRow = 0; songCursorColumn = 1
        chainCursorRow = 0; chainCursorColumn = 1
        phraseCursorRow = 0; phraseCursorColumn = 1
    }

    fun saveCursorForScreen(screen: ScreenType) {
        when (screen) {
            ScreenType.SONG   -> { songCursorRow  = cursorRow; songCursorColumn  = cursorColumn }
            ScreenType.CHAIN  -> { chainCursorRow = cursorRow; chainCursorColumn = cursorColumn }
            ScreenType.PHRASE -> { phraseCursorRow = cursorRow; phraseCursorColumn = cursorColumn }
            else -> {}
        }
    }

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
                ScreenType.MIXER      -> { mixerCursorColumn = 0; mixerMasterRow = 0 }
                ScreenType.TABLE      -> { tableCursorRow = 0; tableCursorColumn = 1 }
                ScreenType.GROOVE     -> { grooveCursorRow = 0 }
                ScreenType.MODS       -> { modCursorRow = 0; modCursorPair = 0; modCursorSide = 0 }
                else -> {}
            }
        }
    }

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
                settingsCursorRow = if (settingsCursorRow > 0) settingsCursorRow - 1 else 11
                settingsCursorColumn = 1
            }
            ScreenType.INSTRUMENT -> {
                val oldRow = instrumentCursorRow
                val oldColumn = instrumentCursorColumn
                val isSoundFont = project.instruments[currentInstrument].instrumentType == InstrumentType.SOUNDFONT
                val maxRow = if (isSoundFont) 14 else 15
                val tripleRow = if (isSoundFont) 5 else 4
                val dualParamRows = if (isSoundFont) setOf(0, 6, 8, 9, 10) else setOf(0, 5, 7, 8, 9, 11, 12, 13, 14)
                instrumentCursorRow = when {
                    instrumentCursorRow == 3 -> 1                       // Skip spacer (row 2) both types
                    isSoundFont && instrumentCursorRow == 8 -> 6        // Skip SF spacer (row 7)
                    !isSoundFont && instrumentCursorRow == 7 -> 5       // Skip sampler spacer (row 6)
                    isSoundFont && instrumentCursorRow == 12 -> 10      // Skip SF spacer (row 11)
                    !isSoundFont && instrumentCursorRow == 11 -> 9      // Skip sampler spacer (row 10)
                    instrumentCursorRow > 0 -> instrumentCursorRow - 1
                    else -> maxRow
                }
                instrumentCursorColumn = when {
                    instrumentCursorRow == 3 -> 2
                    instrumentCursorRow == tripleRow -> when {
                        (oldRow in dualParamRows || oldRow == tripleRow) && oldColumn == 3 -> 3
                        oldRow == tripleRow && oldColumn == 5 -> 5
                        else -> 1
                    }
                    instrumentCursorRow in dualParamRows -> if ((oldRow in dualParamRows || oldRow == tripleRow) && oldColumn >= 3) 3 else 1
                    else -> 1
                }
            }
            ScreenType.TABLE -> {
                tableCursorRow = if (tableCursorRow > 0) tableCursorRow - 1 else 15
            }
            ScreenType.GROOVE -> {
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
            ScreenType.MIXER -> {
                if (mixerMasterRow > 0) mixerMasterRow--
            }
            ScreenType.EFFECTS -> {
                if (effectsCursorRow > 0) effectsCursorRow--
            }
            ScreenType.SONG -> {
                if (cursorRow > 0) {
                    cursorRow--
                    if (cursorRow < songScrollPosition) {
                        songScrollPosition = cursorRow
                    }
                }
            }
            else -> {
                cursorRow = if (cursorRow > 0) cursorRow - 1 else 15
            }
        }
    }

    fun moveCursorDown() {
        when (currentScreen) {
            ScreenType.PROJECT -> {
                projectCursorRow = if (projectCursorRow < 6) {
                    projectCursorRow + 1
                } else {
                    0
                }
                projectCursorColumn = 1
            }
            ScreenType.SETTINGS -> {
                settingsCursorRow = if (settingsCursorRow < 11) settingsCursorRow + 1 else 0
                settingsCursorColumn = 1
            }
            ScreenType.INSTRUMENT -> {
                val oldRow = instrumentCursorRow
                val oldColumn = instrumentCursorColumn
                val isSoundFont = project.instruments[currentInstrument].instrumentType == InstrumentType.SOUNDFONT
                val maxRow = if (isSoundFont) 14 else 15
                val tripleRow = if (isSoundFont) 5 else 4
                val dualParamRows = if (isSoundFont) setOf(0, 6, 8, 9, 10) else setOf(0, 5, 7, 8, 9, 11, 12, 13, 14)
                instrumentCursorRow = when {
                    instrumentCursorRow == 1 -> 3                       // Skip spacer (row 2) both types
                    isSoundFont && instrumentCursorRow == 6 -> 8        // Skip SF spacer (row 7)
                    !isSoundFont && instrumentCursorRow == 5 -> 7       // Skip sampler spacer (row 6)
                    isSoundFont && instrumentCursorRow == 10 -> 12      // Skip SF spacer (row 11)
                    !isSoundFont && instrumentCursorRow == 9 -> 11      // Skip sampler spacer (row 10)
                    instrumentCursorRow < maxRow -> instrumentCursorRow + 1
                    else -> 0
                }
                instrumentCursorColumn = when {
                    instrumentCursorRow == 3 -> 2
                    instrumentCursorRow == tripleRow -> when {
                        (oldRow in dualParamRows || oldRow == tripleRow) && oldColumn == 3 -> 3
                        oldRow == tripleRow && oldColumn == 5 -> 5
                        else -> 1
                    }
                    instrumentCursorRow in dualParamRows -> if ((oldRow in dualParamRows || oldRow == tripleRow) && oldColumn >= 3) 3 else 1
                    else -> 1
                }
            }
            ScreenType.TABLE -> {
                tableCursorRow = if (tableCursorRow < 15) tableCursorRow + 1 else 0
            }
            ScreenType.GROOVE -> {
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
            ScreenType.MIXER -> {
                if (mixerMasterRow < 3) {
                    mixerMasterRow++
                    // Send rows (1-2) only have col 0 (REV/DEL) and col 8 (master).
                    // Track cols 1-7 snap to col 0 (send); col 8 stays on master.
                    if (mixerMasterRow > 0 && mixerCursorColumn in 1..7) mixerCursorColumn = 0
                    // LIM row (3) is master-only: col 0 has no send, snap to master.
                    if (mixerMasterRow == 3 && mixerCursorColumn == 0) mixerCursorColumn = 8
                }
            }
            ScreenType.EFFECTS -> {
                if (effectsCursorRow < 7) effectsCursorRow++
            }
            ScreenType.SONG -> {
                if (cursorRow < 255) {
                    cursorRow++
                    if (cursorRow >= songScrollPosition + 16) {
                        songScrollPosition = cursorRow - 15
                    }
                }
            }
            else -> {
                cursorRow = if (cursorRow < 15) cursorRow + 1 else 0
            }
        }
    }

    fun moveCursorLeft() {
        when (currentScreen) {
            ScreenType.PROJECT -> {
                projectCursorColumn = getProjectCursorLeftColumn(projectCursorRow, projectCursorColumn)
            }
            ScreenType.SETTINGS -> {
                settingsCursorColumn = 1
            }
            ScreenType.INSTRUMENT -> {
                val minColumn = getInstrumentCursorLeftColumn(instrumentCursorRow, instrumentCursorColumn)
                if (instrumentCursorColumn > minColumn) {
                    instrumentCursorColumn = minColumn
                }
            }
            ScreenType.MIXER -> {
                if (mixerMasterRow == 0) {
                    // Track row wraps: track 0 → master, master → track 0
                    mixerCursorColumn = if (mixerCursorColumn > 0) mixerCursorColumn - 1 else 8
                } else {
                    // Send rows: only col 0 (send) and col 8 (master) — wrap between them
                    if (mixerCursorColumn == 8) mixerCursorColumn = 0
                }
            }
            ScreenType.TABLE -> {
                if (tableCursorColumn > 1) tableCursorColumn--
            }
            ScreenType.MODS -> {
                modCursorSide = 0  // Switch to left slot
                val inst = project.instruments[currentInstrument]
                val leftRowCount = inst.modSlots[modCursorPair * 2].rowCount()
                modCursorRow = modCursorRow.coerceIn(0, (leftRowCount - 1).coerceAtLeast(0))
            }
            else -> {
                val minColumn = when (currentScreen) {
                    ScreenType.SONG -> 1
                    ScreenType.CHAIN -> 1
                    ScreenType.PHRASE -> 1
                    else -> 0
                }
                if (cursorColumn > minColumn) cursorColumn--
            }
        }
    }

    fun moveCursorRight() {
        when (currentScreen) {
            ScreenType.PROJECT -> {
                projectCursorColumn = getProjectCursorRightColumn(projectCursorRow, projectCursorColumn)
            }
            ScreenType.SETTINGS -> {
                settingsCursorColumn = if (settingsCursorRow == 11) 2 else 1
            }
            ScreenType.INSTRUMENT -> {
                val maxColumn = getInstrumentCursorRightColumn(instrumentCursorRow, instrumentCursorColumn)
                if (instrumentCursorColumn < maxColumn) {
                    instrumentCursorColumn = maxColumn
                }
            }
            ScreenType.MIXER -> {
                if (mixerMasterRow == 0) {
                    // Track row wraps: master → track 0, track 7 → master
                    mixerCursorColumn = if (mixerCursorColumn < 8) mixerCursorColumn + 1 else 0
                } else {
                    // Send rows: only col 0 (send) and col 8 (master) — wrap between them
                    if (mixerCursorColumn == 0) mixerCursorColumn = 8
                }
            }
            ScreenType.TABLE -> {
                if (tableCursorColumn < 8) tableCursorColumn++
            }
            ScreenType.MODS -> {
                modCursorSide = 1  // Switch to right slot
                val inst = project.instruments[currentInstrument]
                val rightRowCount = inst.modSlots[modCursorPair * 2 + 1].rowCount()
                modCursorRow = modCursorRow.coerceIn(0, (rightRowCount - 1).coerceAtLeast(0))
            }
            else -> {
                val maxColumn = when (currentScreen) {
                    ScreenType.SONG -> 8
                    ScreenType.CHAIN -> 2
                    ScreenType.PHRASE -> 9
                    else -> 0
                }
                if (cursorColumn < maxColumn) cursorColumn++
            }
        }
    }

    fun moveSongBigUp() {
        val newRow = (cursorRow - 16).coerceAtLeast(0)
        cursorRow = newRow
        songScrollPosition = newRow
    }

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

    private fun getProjectCursorLeftColumn(row: Int, currentColumn: Int): Int {
        return (currentColumn - 1).coerceAtLeast(1)
    }

    private fun getProjectCursorRightColumn(row: Int, currentColumn: Int): Int {
        val maxColumn = when (row) {
            2 -> 20  // NAME: up to column 20 (one per character)
            3 -> 3   // LOAD/SAVE/NEW buttons: up to column 3
            4 -> 2   // MIX/STEMS buttons: up to column 2
            5 -> 2   // CLEAN SEQ/INST buttons: up to column 2
            else -> 1
        }
        return (currentColumn + 1).coerceAtMost(maxColumn)
    }

    private fun getInstrumentCursorLeftColumn(row: Int, currentColumn: Int): Int {
        val isSoundFont = project.instruments[currentInstrument].instrumentType == InstrumentType.SOUNDFONT
        val tripleRow = if (isSoundFont) 5 else 4
        val dualParamRows = if (isSoundFont) setOf(0, 6, 8, 9, 10) else setOf(0, 5, 7, 8, 9, 11, 12, 13, 14)
        return when {
            row == 0 -> (currentColumn - 1).coerceAtLeast(1)           // 3→2→1
            row == 3 -> (currentColumn - 1).coerceAtLeast(2)           // 3→2 (min col 2)
            row == tripleRow -> (currentColumn - 2).coerceAtLeast(1)   // 5→3→1
            row in dualParamRows -> 1                                   // jump 3→1
            else -> 1
        }
    }

    private fun getInstrumentCursorRightColumn(row: Int, currentColumn: Int): Int {
        val isSoundFont = project.instruments[currentInstrument].instrumentType == InstrumentType.SOUNDFONT
        val tripleRow = if (isSoundFont) 5 else 4
        val dualParamRows = if (isSoundFont) setOf(0, 6, 8, 9, 10) else setOf(0, 5, 7, 8, 9, 11, 12, 13, 14)
        return when {
            row == 0 -> (currentColumn + 1).coerceAtMost(3)            // 1→2→3
            row == 3 -> (currentColumn + 1).coerceAtMost(3)            // 2→3 (max col 3)
            row == tripleRow -> (currentColumn + 2).coerceAtMost(5)    // 1→3→5
            row in dualParamRows -> 3                                   // jump 1→3
            else -> 1
        }
    }

}
