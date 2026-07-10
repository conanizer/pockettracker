package com.conanizer.pockettracker.core.logic

import com.conanizer.pockettracker.BuildConfig
import com.conanizer.pockettracker.core.data.MAIN_ROW_SCREENS
import com.conanizer.pockettracker.core.data.Note
import com.conanizer.pockettracker.core.data.Project
import com.conanizer.pockettracker.core.data.Chain
import com.conanizer.pockettracker.core.data.Phrase
import com.conanizer.pockettracker.core.data.Table
import com.conanizer.pockettracker.core.data.Groove
import com.conanizer.pockettracker.core.data.Instrument
import com.conanizer.pockettracker.core.data.InstrumentRowKind
import com.conanizer.pockettracker.core.data.InstrumentRowLayout
import com.conanizer.pockettracker.core.data.InstrumentType
import com.conanizer.pockettracker.core.data.ScreenType
import com.conanizer.pockettracker.core.storage.FileInfo
import kotlin.properties.ReadWriteProperty

class TrackerController(
    val fileController: FileController,
    val playbackController: PlaybackController,
    val instrumentController: InstrumentController,
    val effectProcessor: EffectProcessor,
    val clipboardManager: ClipboardManager,
    val inputController: InputController,
    private val stateObserver: StateObserver
) {

    // Notify-on-set delegate (see StateObserver.observed). Properties with additional side
    // effects (lastEdited* mirrors, screen-change hooks) keep explicit setters below.
    private fun <T> observed(initial: T, transform: (T) -> T = { it }): ReadWriteProperty<Any?, T> =
        stateObserver.observed(initial, transform)

    var project by observed(Project(version = 1))

    // True while the INSTRUMENT screen was reached via R+RIGHT from the Instrument Pool — makes
    // R+LEFT from INSTRUMENT jump back to the pool (instead of PHRASE). Cleared on any move off
    // INSTRUMENT (handled in the currentScreen setter below).
    var instrumentFromPool = false

    var currentScreen = ScreenType.SONG
        set(value) {
            field = value
            stateObserver.onStateChanged()
            if (value != ScreenType.INSTRUMENT) instrumentFromPool = false
            if (value == ScreenType.INSTRUMENT) {
                instrumentController.syncToLastEdited(project)
            }
            if (value == ScreenType.TABLE) {
                // Sync table to match current instrument
                currentTable = currentInstrument
            }
        }

    var projectVersion by observed(0)

    // Set to match projectVersion after save/load/new so isProjectDirty works correctly.
    var savedProjectVersion = 0
    val isProjectDirty get() = projectVersion != savedProjectVersion

    /**
     * Re-sync the dirty baseline to the current version. Callers that bump projectVersion purely to
     * force a redraw *after* a load completes must call this, otherwise a freshly-loaded (unedited)
     * project reads as dirty — which both shows a spurious "unsaved changes" prompt and triggers a
     * phantom autosave/recovery. NOT used by recoverFromAutosave (that stays dirty).
     */
    fun markProjectClean() { savedProjectVersion = projectVersion }

    var statusMessage by observed("")

    var statusSuccess by observed(true)

    var previousColumn by observed(2)

    var cursorRow by observed(0)

    // Per-screen saved cursor positions for SONG/CHAIN/PHRASE (used by REMEMBER cursor mode)
    var songCursorRow = 0
    var songCursorColumn = 1
    var chainCursorRow = 0
    var chainCursorColumn = 1
    var phraseCursorRow = 0
    var phraseCursorColumn = 1

    // Scroll position for SONG screen (0-255 range cursor with 16-row viewport)
    var songScrollPosition by observed(0) { it.coerceIn(0, 240) }

    var cursorColumn by observed(1)

    // Project screen cursor
    var projectCursorRow by observed(0)

    var projectCursorColumn by observed(1)

    var settingsCursorRow by observed(0)

    var settingsCursorColumn by observed(1)

    // True when the current layout has a selectable device skin (theme), enabling the LAYOUT row's
    // second column. Plain Boolean (no Android types) — kept in sync by the UI layer on layout change.
    var settingsLayoutHasThemes = false

    var instrumentCursorRow by observed(0)

    var instrumentCursorColumn by observed(1)

    // Instrument Pool cursor column (0 = NAME, 1 = VOL, 2 = REV, 3 = DEL, 4 = EQ).
    // The selected ROW is `currentInstrument` (shared with the INSTRUMENT view).
    var poolCursorColumn by observed(0) { it.coerceIn(0, 4) }

    // Mixer screen cursor (0-7 = tracks, 8 = master)
    var mixerCursorColumn by observed(0) { it.coerceIn(0, 8) }

    // Mixer master sub-row (reachable from any column via DPAD UP/DOWN)
    // 0 = volume row, 1 = MASTER FX selector, 2 = effect depth, 3 = LIM pre-gain
    var mixerMasterRow by observed(0) { it.coerceIn(0, 3) }

    var effectsCursorRow by observed(0) { it.coerceIn(0, 7) }

    var tableCursorRow by observed(0) { it.coerceIn(0, 15) }

    var tableCursorColumn by observed(1) { it.coerceIn(0, 8) }  // 0=step, 1=transpose, 2=vol, 3-8=fx

    var currentTable = 0
        set(value) {
            field = value.coerceIn(0, project.tables.lastIndex)  // pool is 128, not 256
            lastEditedTable = field
            stateObserver.onStateChanged()
        }

    var lastEditedTable = 0

    var grooveCursorRow by observed(0) { it.coerceIn(0, 15) }

    var currentGroove by observed(0) { it.coerceIn(0, project.grooves.lastIndex) }  // pool is 128, not 256

    var modCursorRow by observed(0)

    var modCursorPair by observed(0) { it.coerceIn(0, 1) }

    var modCursorSide by observed(0) { it.coerceIn(0, 1) }

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

    var lastEditedVolume = 0x7F

    var lastEditedTranspose = 0

    var currentInstrument = 0
        set(value) {
            field = value.coerceIn(0, project.instruments.lastIndex)  // clamp at the source (pool is 128)
            lastEditedInstrument = field
            stateObserver.onStateChanged()
        }

    var lastEditedInstrument = 0

    fun saveProject(filename: String): FileController.SaveResult {
        val result = fileController.saveProject(project, filename)

        when (result) {
            is FileController.SaveResult.Success -> {
                savedProjectVersion = projectVersion
                fileController.clearAutosave()  // work is now safely in a real file
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
        handleLoadResult(result, filename)
        return result
    }

    fun loadProject(fileInfo: FileInfo): FileController.LoadResult {
        val result = fileController.loadProject(fileInfo)
        handleLoadResult(result, fileInfo.nameWithoutExtension)
        return result
    }

    /**
     * Shared success/error tail for both loadProject overloads. On success the working project becomes
     * clean (matches a real file), the autosave is cleared, and the editing context resets; on error
     * only the status line is set. Sample reload after a successful load is driven separately by the UI
     * (AppInputDispatcher.reloadProjectSamples) once the audio backend is ready.
     */
    private fun handleLoadResult(result: FileController.LoadResult, label: String) {
        when (result) {
            is FileController.LoadResult.Success -> {
                project = result.project
                projectVersion++
                savedProjectVersion = projectVersion
                fileController.clearAutosave()  // working state now matches a real file
                statusMessage = "LOADED: $label"
                statusSuccess = true
                resetEditingContext()
            }
            is FileController.LoadResult.Error -> {
                statusMessage = "LOAD FAILED"
                statusSuccess = false
            }
        }
    }

    fun newProject() {
        // Stop before clearing so no voices are reading samples while we free them.
        playbackController.stop()

        project = Project(version = 1)
        projectVersion++
        savedProjectVersion = projectVersion
        fileController.clearAutosave()  // fresh project, nothing to recover
        statusMessage = "NEW PROJECT"
        statusSuccess = true

        instrumentController.clearAllSamples()
        instrumentController.clearAllSoundfonts()   // free cached SF2s too

        resetEditingContext()
    }

    /**
     * Load the crash-recovery autosave into the working project, leaving it DIRTY (projectVersion is
     * bumped but savedProjectVersion is NOT, so the user is nudged to Save it under a real name) and
     * deliberately NOT clearing the autosave file. The caller must reload samples afterward (the
     * autosave stores sample paths, not PCM — see AppInputDispatcher).
     */
    fun recoverFromAutosave(): Boolean {
        return when (val result = fileController.loadAutosave()) {
            is FileController.LoadResult.Success -> {
                project = result.project
                projectVersion++          // recompose + mark dirty (savedProjectVersion stays behind)
                statusMessage = "RECOVERED"
                statusSuccess = true
                resetEditingContext()
                true
            }
            is FileController.LoadResult.Error -> {
                statusMessage = "RECOVER FAILED"
                statusSuccess = false
                false
            }
        }
    }

    private fun resetEditingContext() {
        lastEditedChain = 0
        lastEditedPhrase = 0
        lastEditedNote = Note.fromString("C-4")
        lastEditedVolume = 0x7F
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

    fun playPhrase(phraseId: Int) {
        playbackController.playPhrase(project, phraseId)
        currentPhrase = phraseId
    }

    fun playChain(chainId: Int) {
        playbackController.playChain(project, chainId)
        currentChain = chainId
    }

    fun playSong(startRow: Int = 0) {
        playbackController.playSong(project, startRow)
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
                ScreenType.SONG -> playSong(startRow = cursorRow)  // start from highlighted row
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

        // Row-0 instrument (entered from the pool): nothing above it — stay.
        if (currentScreen == ScreenType.INSTRUMENT && instrumentFromPool) return Pair(ScreenType.INSTRUMENT, 3)

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

        // Row-0 instrument (entered from the pool) drops to MODS, like the pool to its left.
        if (currentScreen == ScreenType.INSTRUMENT && instrumentFromPool) return Pair(ScreenType.MODS, 3)

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
        // EFFECTS has no side doors
        if (currentScreen == ScreenType.EFFECTS) return Pair(currentScreen, previousColumn)

        // Instrument Pool fast-jump pair: INST_POOL ←→ INSTRUMENT (R+RIGHT/R+LEFT).
        // From the pool, R+LEFT exits left to PHRASE. From an INSTRUMENT entered via the pool, R+LEFT
        // returns to the pool (normal INSTRUMENT R+LEFT still goes to PHRASE — see instrumentFromPool).
        if (currentScreen == ScreenType.INST_POOL) return Pair(ScreenType.PHRASE, 2)
        if (currentScreen == ScreenType.INSTRUMENT && instrumentFromPool) return Pair(ScreenType.INST_POOL, 3)

        // Row 1 (PROJECT, GROOVE, MODS) and Row 3 (MIXER): jump to row-2 screen one column left
        val contextCol = when (currentScreen) {
            ScreenType.PROJECT -> previousColumn
            ScreenType.GROOVE  -> 2
            ScreenType.MODS    -> 3
            ScreenType.MIXER   -> 2
            else -> -1
        }
        if (contextCol >= 0) {
            val targetCol = (contextCol - 1).coerceAtLeast(0)
            return Pair(getMainScreenForColumn(targetCol), targetCol)
        }

        // Other non-main-row screens (SCALE, INST_POOL): drop to main row in same column
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
        // EFFECTS has no side doors
        if (currentScreen == ScreenType.EFFECTS) return Pair(currentScreen, previousColumn)

        // Instrument Pool fast-jump: R+RIGHT from the pool jumps to the INSTRUMENT screen and marks it
        // so R+LEFT returns to the pool. (The setter clears the flag on any other move off INSTRUMENT.)
        if (currentScreen == ScreenType.INST_POOL) { instrumentFromPool = true; return Pair(ScreenType.INSTRUMENT, 3) }
        // Row-0 instrument (from the pool): nothing to its right — stay (don't fall through to TABLE).
        if (currentScreen == ScreenType.INSTRUMENT && instrumentFromPool) return Pair(ScreenType.INSTRUMENT, 3)

        // Row 1 (PROJECT, GROOVE, MODS) and Row 3 (MIXER): jump to row-2 screen one column right
        val contextCol = when (currentScreen) {
            ScreenType.PROJECT -> previousColumn
            ScreenType.GROOVE  -> 2
            ScreenType.MODS    -> 3
            ScreenType.MIXER   -> 2
            else -> -1
        }
        if (contextCol >= 0) {
            val targetCol = (contextCol + 1).coerceAtMost(4)
            return Pair(getMainScreenForColumn(targetCol), targetCol)
        }

        // Other non-main-row screens (SCALE, INST_POOL): drop to main row in same column
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

    fun getDefaultCursorPosition(screenType: ScreenType): Pair<Int, Int> {
        // Every screen's default cursor row is 0; only the default column varies by screen.
        return Pair(0, getMinEditableColumn(screenType))
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
                ScreenType.INST_POOL  -> { poolCursorColumn = 0 }
                else -> {}
            }
        }
    }

    /** Move the Instrument Pool selection (= currentInstrument), keeping the controllers in sync.
     *  Single-step DPAD wraps 00↔7F; B+UP/DOWN paging clamps at the ends (like the song screen). */
    private fun setPoolSelection(n: Int) {
        currentInstrument = n                     // setter also updates lastEditedInstrument + notifies
        instrumentController.currentInstrument = n
    }
    private fun movePoolSelection(delta: Int) {
        val size = project.instruments.size
        setPoolSelection(((currentInstrument + delta) % size + size) % size)  // wrap
    }
    fun poolBigUp()   = setPoolSelection((currentInstrument - 16).coerceIn(0, project.instruments.size - 1))
    fun poolBigDown() = setPoolSelection((currentInstrument + 16).coerceIn(0, project.instruments.size - 1))

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
                var prev = if (settingsCursorRow > 0) settingsCursorRow - 1 else 12
                if (!BuildConfig.DEBUG && prev == 12) prev = 11 // TRACE hidden in release
                if (!BuildConfig.DEBUG && prev == 2) prev = 1   // OVERLAY hidden in release
                settingsCursorRow = prev
                settingsCursorColumn = 1
            }
            ScreenType.INSTRUMENT -> {
                val oldRow = instrumentCursorRow
                val oldColumn = instrumentCursorColumn
                val rows = instrumentRows()
                instrumentCursorRow = instrumentRowStep(rows, instrumentCursorRow, -1)
                instrumentCursorColumn = instrumentColumnFor(rows, instrumentCursorRow, oldRow, oldColumn)
            }
            ScreenType.TABLE -> {
                tableCursorRow = if (tableCursorRow > 0) tableCursorRow - 1 else 15
            }
            ScreenType.GROOVE -> {
                grooveCursorRow = if (grooveCursorRow > 0) grooveCursorRow - 1 else 15
            }
            ScreenType.MODS -> {
                val inst = project.instruments[currentInstrument]
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
                when {
                    // REV / DEL (send row) → first track
                    mixerMasterRow == 1 && (mixerCursorColumn == 0 || mixerCursorColumn == 1) -> {
                        mixerMasterRow = 0
                        mixerCursorColumn = 0
                    }
                    // Master column: EQ→MIX, OTT→EQ, LIM→OTT (col 8 stays)
                    mixerMasterRow > 0 -> mixerMasterRow--
                }
            }
            ScreenType.INST_POOL -> movePoolSelection(-1)
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
                var next = if (settingsCursorRow < 12) settingsCursorRow + 1 else 0
                if (!BuildConfig.DEBUG && next == 2) next = 3   // OVERLAY hidden in release
                if (!BuildConfig.DEBUG && next == 12) next = 0  // TRACE hidden in release
                settingsCursorRow = next
                settingsCursorColumn = 1
            }
            ScreenType.INSTRUMENT -> {
                val oldRow = instrumentCursorRow
                val oldColumn = instrumentCursorColumn
                val rows = instrumentRows()
                instrumentCursorRow = instrumentRowStep(rows, instrumentCursorRow, +1)
                instrumentCursorColumn = instrumentColumnFor(rows, instrumentCursorRow, oldRow, oldColumn)
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
                when {
                    // Track meters → REV (send row, col 0)
                    mixerMasterRow == 0 && mixerCursorColumn < 8 -> {
                        mixerMasterRow = 1
                        mixerCursorColumn = 0
                    }
                    // Master column: MIX→EQ→OTT→LIM (col 8 stays)
                    mixerCursorColumn == 8 && mixerMasterRow < 3 -> mixerMasterRow++
                    // REV / DEL: nothing below → stay
                }
            }
            ScreenType.INST_POOL -> movePoolSelection(1)
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
                when {
                    mixerMasterRow == 0 -> {
                        // Track row wraps: track 0 → master, master → track 0
                        mixerCursorColumn = if (mixerCursorColumn > 0) mixerCursorColumn - 1 else 8
                    }
                    // DEL → REV
                    mixerMasterRow == 1 && mixerCursorColumn == 1 -> mixerCursorColumn = 0
                    // Master column (EQ/OTT/DST/LIM) → DEL
                    mixerCursorColumn == 8 -> {
                        mixerMasterRow = 1
                        mixerCursorColumn = 1
                    }
                    // REV (col 0): nothing to the left → stay
                }
            }
            ScreenType.INST_POOL -> {
                if (poolCursorColumn > 0) poolCursorColumn--
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
                // rows 2 (OVERLAY), 3 (BTN SOUND), 4 (BTN VIBRO), 10 (TEMPLATE) have a second column,
                // and row 0 (LAYOUT) gains a theme column when the current layout is skinned.
                val hasSecondCol = settingsCursorRow in setOf(2, 3, 4, 10) ||
                        (settingsCursorRow == 0 && settingsLayoutHasThemes)
                settingsCursorColumn = if (hasSecondCol && settingsCursorColumn < 2) 2 else settingsCursorColumn
            }
            ScreenType.INSTRUMENT -> {
                val maxColumn = getInstrumentCursorRightColumn(instrumentCursorRow, instrumentCursorColumn)
                if (instrumentCursorColumn < maxColumn) {
                    instrumentCursorColumn = maxColumn
                }
            }
            ScreenType.MIXER -> {
                when {
                    mixerMasterRow == 0 -> {
                        // Track row wraps: master → track 0, track 7 → master
                        mixerCursorColumn = if (mixerCursorColumn < 8) mixerCursorColumn + 1 else 0
                    }
                    // REV → DEL
                    mixerMasterRow == 1 && mixerCursorColumn == 0 -> mixerCursorColumn = 1
                    // DEL → EQ (master column)
                    mixerMasterRow == 1 && mixerCursorColumn == 1 -> mixerCursorColumn = 8
                    // Master column (col 8) already rightmost → stay
                }
            }
            ScreenType.INST_POOL -> {
                if (poolCursorColumn < 4) poolCursorColumn++
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

    /** Scroll the 16-row song window so [row] is visible. Used when expanding a selection past
     *  the visible window (selection mode keeps the cursor anchored, so it can't drive the scroll). */
    fun scrollSongToRow(row: Int) {
        if (row < songScrollPosition) songScrollPosition = row
        else if (row >= songScrollPosition + 16) songScrollPosition = row - 15
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
        // Only steps WITH a note count their instrument: a note-less step's instrument number never
        // triggers or configures anything at playback (scheduleStepWithEffects reads it only when
        // hasNote), so an instrument referenced solely by note-less steps is genuinely unused.
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
                for (slot in 1..3) {
                    val (fxType, fxValue) = step.fx(slot)
                    if (fxType == EffectProcessor.FX_TBL) usedTableIds.add(fxValue and 0xFF)
                    if (fxType == EffectProcessor.FX_GRV) usedGrooveIds.add(fxValue and 0xFF)
                }
            }
        }

        // Implicit table mapping (instrument i → table i) + explicit tableId override
        for (instId in usedInstrumentIds) {
            val inst = project.instruments.getOrNull(instId) ?: continue
            usedTableIds.add(instId)
            if (inst.tableId >= 0) usedTableIds.add(inst.tableId)
        }

        // A table's own rows can carry TBL/GRV FX (the editor allows all effects in a table column).
        // Walk referenced tables transitively so a groove used only from inside a table — or a table
        // chained from another via a TBL row — isn't wiped out from under a still-referenced table.
        val tableWorklist = ArrayDeque(usedTableIds)
        while (tableWorklist.isNotEmpty()) {
            val table = project.tables.getOrNull(tableWorklist.removeFirst()) ?: continue
            for (row in table.rows) {
                for ((fxType, fxValue) in listOf(
                    row.fx1Type to row.fx1Value, row.fx2Type to row.fx2Value, row.fx3Type to row.fx3Value
                )) {
                    when (fxType) {
                        EffectProcessor.FX_TBL -> {
                            val ref = fxValue and 0xFF
                            if (usedTableIds.add(ref)) tableWorklist.addLast(ref)
                        }
                        EffectProcessor.FX_GRV -> usedGrooveIds.add(fxValue and 0xFF)
                    }
                }
            }
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

    // ── INSTRUMENT screen cursor geometry ────────────────────────────────────────────────
    // All four movement paths (up/down/left/right) walk InstrumentRowLayout — the single
    // row-descriptor table. The drawn layout lives in InstrumentModule; keep the table in sync.

    private fun instrumentRows(): Array<InstrumentRowKind> = InstrumentRowLayout.rows(
        project.instruments[currentInstrument].instrumentType == InstrumentType.SOUNDFONT
    )

    /** Step [delta] = ±1 rows with wrap, skipping SPACER rows. */
    private fun instrumentRowStep(rows: Array<InstrumentRowKind>, from: Int, delta: Int): Int {
        var r = from
        do {
            r += delta
            if (r < 0) r = rows.size - 1
            if (r >= rows.size) r = 0
        } while (rows[r] == InstrumentRowKind.SPACER)
        return r
    }

    /** Column after a vertical move: SOURCE snaps to LOAD; the right-hand column (3) is kept
     *  when moving between rows that have one; TRIPLE additionally keeps column 5. */
    private fun instrumentColumnFor(
        rows: Array<InstrumentRowKind>, newRow: Int, oldRow: Int, oldColumn: Int
    ): Int {
        fun hasRightColumn(k: InstrumentRowKind?) =
            k == InstrumentRowKind.DUAL || k == InstrumentRowKind.NAME || k == InstrumentRowKind.TRIPLE
        val old = rows.getOrNull(oldRow)
        return when (rows[newRow]) {
            InstrumentRowKind.SOURCE -> 2
            InstrumentRowKind.TRIPLE -> when {
                hasRightColumn(old) && oldColumn == 3 -> 3
                old == InstrumentRowKind.TRIPLE && oldColumn == 5 -> 5
                else -> 1
            }
            InstrumentRowKind.DUAL,
            InstrumentRowKind.NAME -> if (hasRightColumn(old) && oldColumn >= 3) 3 else 1
            else -> 1
        }
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
        val rows = instrumentRows()
        return when (rows.getOrElse(row) { InstrumentRowKind.SINGLE }) {
            InstrumentRowKind.NAME   -> (currentColumn - 1).coerceAtLeast(1)  // 3→2→1
            InstrumentRowKind.SOURCE -> (currentColumn - 1).coerceAtLeast(2)  // 3→2 (min col 2)
            InstrumentRowKind.TRIPLE -> (currentColumn - 2).coerceAtLeast(1)  // 5→3→1
            InstrumentRowKind.DUAL   -> 1                                     // jump 3→1
            else -> 1
        }
    }

    private fun getInstrumentCursorRightColumn(row: Int, currentColumn: Int): Int {
        val isSoundFont = project.instruments[currentInstrument].instrumentType == InstrumentType.SOUNDFONT
        val rows = instrumentRows()
        return when (rows.getOrElse(row) { InstrumentRowKind.SINGLE }) {
            InstrumentRowKind.NAME   -> (currentColumn + 1).coerceAtMost(3)   // 1→2→3
            // Sampler has LOAD (2) + EDIT (3); SF has LOAD only (no editable waveform), so cap
            // at col 2 — otherwise right-from-LOAD lands on the hidden EDIT and the cursor vanishes.
            InstrumentRowKind.SOURCE -> (currentColumn + 1).coerceAtMost(if (isSoundFont) 2 else 3)
            InstrumentRowKind.TRIPLE -> (currentColumn + 2).coerceAtMost(5)   // 1→3→5
            InstrumentRowKind.DUAL   -> 3                                     // jump 1→3
            else -> 1
        }
    }

}
