package com.example.pockettracker.core.logic

import androidx.compose.runtime.*
import com.example.pockettracker.Project
import com.example.pockettracker.ScreenType
import com.example.pockettracker.core.storage.FileInfo

/**
 * TRACKER CONTROLLER
 *
 * Main coordinator for all tracker logic.
 * Owns global state and delegates operations to specialist controllers.
 *
 * ✅ PLATFORM-AGNOSTIC - No Android dependencies
 *
 * Architecture:
 * - TrackerController (this) - Owns state, coordinates operations
 * - FileController - Save/load projects
 * - PlaybackController - Audio playback
 * - InstrumentController - Sample management
 * - EffectProcessor - Effect calculations (stubs)
 * - ClipboardManager - Copy/paste (stubs)
 * - InputController - Button handling (TODO: add tomorrow)
 */
class TrackerController(
    val fileController: FileController,
    val playbackController: PlaybackController,
    val instrumentController: InstrumentController,
    val effectProcessor: EffectProcessor,
    val clipboardManager: ClipboardManager
    // TODO: Add inputController tomorrow when we create it
) {

    // ========================================
    // GLOBAL STATE (owned by coordinator)
    // ========================================

    /**
     * Current project (all tracker data).
     */
    var project by mutableStateOf(Project())

    /**
     * Current screen being displayed.
     */
    var currentScreen by mutableStateOf(ScreenType.PHRASE)

    /**
     * Project version counter (for forcing Compose recomposition).
     */
    var projectVersion by mutableIntStateOf(0)

    /**
     * Status message for user feedback.
     */
    var statusMessage by mutableStateOf("")

    /**
     * Whether last operation succeeded.
     */
    var statusSuccess by mutableStateOf(true)

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
    }
}
