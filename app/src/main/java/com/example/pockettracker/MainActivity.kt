package com.example.pockettracker

import android.Manifest
import android.content.Intent
import android.content.pm.PackageManager
import android.net.Uri
import android.os.Build
import android.os.Bundle
import android.os.Environment
import android.provider.Settings
import android.util.Log
import android.view.Window
import android.view.WindowManager
import android.view.WindowInsets
import android.view.WindowInsetsController
import android.view.View
import androidx.activity.ComponentActivity
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.compose.setContent
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.foundation.layout.*
import androidx.compose.material3.Surface
import androidx.compose.runtime.*
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.platform.LocalContext
import androidx.core.content.ContextCompat
import com.example.pockettracker.ui.theme.PockettrackerTheme
import androidx.compose.ui.focus.FocusRequester
import com.example.pockettracker.core.logic.InstrumentController
import com.example.pockettracker.core.logic.PlaybackController
import com.example.pockettracker.core.logic.FileController
import com.example.pockettracker.core.logic.InputAction
import com.example.pockettracker.core.audio.AudioEngine
import com.example.pockettracker.core.data.MAIN_ROW_SCREENS
import com.example.pockettracker.core.data.Note
import com.example.pockettracker.core.data.Project
import com.example.pockettracker.core.data.ScreenType
import com.example.pockettracker.core.logic.ClipboardManager
import com.example.pockettracker.platform.android.OboeAudioBackend
import com.example.pockettracker.platform.android.AndroidResourceLoader
import com.example.pockettracker.platform.android.AndroidFileSystem
import com.example.pockettracker.core.storage.FileInfo
import com.example.pockettracker.core.storage.FileSortMode
import java.io.File

// ═══════════════════════════════════════════════════════════════════════════
// HELPER FUNCTIONS
// ═══════════════════════════════════════════════════════════════════════════

/**
 * Convert java.io.File to platform-agnostic FileInfo.
 * Temporary helper during refactoring - FileBrowserModule will eventually use FileInfo directly.
 */
fun File.toFileInfo(): FileInfo = FileInfo(
    path = absolutePath,
    name = name,
    extension = extension,
    isDirectory = isDirectory,
    size = if (isFile) length() else 0L,
    lastModified = lastModified()
)

// ═══════════════════════════════════════════════════════════════════════════
// MAIN ACTIVITY
// ═══════════════════════════════════════════════════════════════════════════

/**
 * MAIN ACTIVITY
 *
 * This is the entry point of the app. It runs ONCE when the app starts.
 *
 * What it does:
 * 1. Creates a DeviceAdapter to detect what kind of device we're on
 * 2. Calculates the best layout (full screen vs virtual buttons, portrait vs landscape)
 * 3. Logs the detection results so we can see them in Logcat
 * 4. Passes the layout configuration to PocketTrackerApp
 */
class MainActivity : ComponentActivity() {
    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)

        // SIMPLE: Just set the window to fullscreen
        // This should work on most Android gaming devices
        requestWindowFeature(Window.FEATURE_NO_TITLE)
        window.setFlags(
            WindowManager.LayoutParams.FLAG_FULLSCREEN,
            WindowManager.LayoutParams.FLAG_FULLSCREEN
        )

        // Your existing code...
        val deviceAdapter = DeviceAdapter(this)
        val layout = deviceAdapter.calculateLayout()

        Log.d("DeviceAdapter", "=== DEVICE DETECTION ===")
        Log.d("DeviceAdapter", deviceAdapter.getConfigDescription(layout))
        Log.d("DeviceAdapter", "======================")

        setContent {
            PockettrackerTheme {
                Surface(
                    modifier = Modifier.fillMaxSize(),
                    color = Color.Black
                ) {
                    PocketTrackerApp(layoutConfig = layout)
                }
            }
        }
    }
}

// ═══════════════════════════════════════════════════════════════════════════
// POCKET TRACKER APP
// ═══════════════════════════════════════════════════════════════════════════

/**
 * POCKET TRACKER APP
 *
 * This is the main composable function that contains all the app logic.
 * A "composable" is a function that creates UI elements.
 *
 * @param layoutConfig - Information about the device (from DeviceAdapter)
 *                       This tells us if we need virtual buttons and what orientation
 *
 * What it does:
 * 1. Sets up all the app state (cursor position, current screen, etc.)
 * 2. Creates button handlers (what happens when each button is pressed)
 * 3. Chooses which layout to show based on layoutConfig
 */
@Composable
fun PocketTrackerApp(layoutConfig: DeviceAdapter.LayoutConfig) {
    // Get Android context (needed for file access, audio, etc.)
    val context = LocalContext.current

    // ═══════════════════════════════════════════════════════════════════════
    // STORAGE PERMISSIONS REQUEST
    // ═══════════════════════════════════════════════════════════════════════

    // Request storage permissions for reading WAV files from Documents folder
    val permissionsToRequest = if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
        // Android 13+ (API 33+): Use granular media permissions
        arrayOf(Manifest.permission.READ_MEDIA_AUDIO)
    } else {
        // Android 12 and below: Use legacy storage permissions
        arrayOf(
            Manifest.permission.READ_EXTERNAL_STORAGE,
            Manifest.permission.WRITE_EXTERNAL_STORAGE
        )
    }

    val permissionLauncher = rememberLauncherForActivityResult(
        contract = ActivityResultContracts.RequestMultiplePermissions()
    ) { permissions ->
        val allGranted = permissions.values.all { it }
        if (allGranted) {
            Log.d("Permissions", "✅ Storage permissions granted!")
        } else {
            Log.w("Permissions", "❌ Storage permissions denied - some features may not work")
        }
    }

    // Check and request permissions on first composition
    //*
    LaunchedEffect(Unit) {
        val hasPermission = permissionsToRequest.all { permission ->
            ContextCompat.checkSelfPermission(context, permission) == PackageManager.PERMISSION_GRANTED
        }

        if (!hasPermission) {
            Log.d("Permissions", "Requesting storage permissions...")
            permissionLauncher.launch(permissionsToRequest)
        } else {
            Log.d("Permissions", "✅ Storage permissions already granted")
        }

        // Android 11+ (API 30+): Request MANAGE_EXTERNAL_STORAGE for full file access
        // This is needed to see files copied from other devices via USB/file manager
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {
            if (!Environment.isExternalStorageManager()) {
                Log.d("Permissions", "Requesting MANAGE_EXTERNAL_STORAGE permission...")
                try {
                    val intent = Intent(Settings.ACTION_MANAGE_APP_ALL_FILES_ACCESS_PERMISSION).apply {
                        data = Uri.parse("package:${context.packageName}")
                    }
                    context.startActivity(intent)
                } catch (e: Exception) {
                    // Fallback for devices that don't support the app-specific intent
                    Log.w("Permissions", "App-specific intent failed, trying general intent: ${e.message}")
                    val intent = Intent(Settings.ACTION_MANAGE_ALL_FILES_ACCESS_PERMISSION)
                    context.startActivity(intent)
                }
            } else {
                Log.d("Permissions", "✅ MANAGE_EXTERNAL_STORAGE already granted")
            }
        }
    }
    // ═══════════════════════════════════════════════════════════════════════
    // FILE SYSTEM SETUP (REFACTORED ARCHITECTURE - Phase 2 COMPLETE!)
    // ═══════════════════════════════════════════════════════════════════════

    // Step 1: Create platform-specific file system backend
    val fileSystem = remember { AndroidFileSystem(context) }

    // Step 2: Create platform-agnostic FileManager
    // ✅ No more Context dependency - fully portable!
    val fileManager = remember { FileManager(fileSystem) }

    // ═══════════════════════════════════════════════════════════════════════
    // PLATFORM BACKENDS (Phase 5: Complete Portability)
    // ═══════════════════════════════════════════════════════════════════════

    // Create logger (used by all controllers)
    val logger = remember { com.example.pockettracker.platform.android.AndroidLogger() }

    // Create state observer (triggers UI recomposition when controller state changes)
    // This is the bridge between platform-agnostic controllers and Compose's reactive UI
    var stateVersion by remember { mutableIntStateOf(0) }
    val stateObserver = remember {
        object : com.example.pockettracker.core.logic.StateObserver {
            override fun onStateChanged() {
                stateVersion++  // Trigger Compose recomposition
            }
        }
    }

    // Step 3: Create FileController (coordinates save/load operations)
    val fileController = remember { FileController(fileManager, logger) }

    // ═══════════════════════════════════════════════════════════════════════
    // AUDIO ENGINE SETUP (REFACTORED ARCHITECTURE - Phase 1 COMPLETE!)
    // ═══════════════════════════════════════════════════════════════════════

    // Step 1: Create platform-specific backends
    val audioBackend = remember { OboeAudioBackend() }
    val resourceLoader = remember { AndroidResourceLoader(context) }

    // Step 2: Create platform-agnostic AudioEngine
    // ✅ No more Context dependency - fully portable!
    val audioEngine = remember {
        AudioEngine(audioBackend, resourceLoader, logger).apply {
            create()
        }
    }

    // Step 3: Cleanup when app closes (important to prevent memory leaks)
    DisposableEffect(Unit) {
        onDispose {
            audioEngine.close()
        }
    }

    // InstrumentController: Manages all instrument operations
    // PHASE 4: Extracted from MainActivity to separate business logic
    // PHASE 5: Uses StateObserver for UI reactivity
    val instrumentController = remember {
        InstrumentController(audioEngine, logger, stateObserver)
    }

    // EffectProcessor: Processes effects (Milestone 2 - Kill effect implemented!)
    // PHASE 4: Extracted from MainActivity to separate business logic
    val effectProcessor = remember {
        com.example.pockettracker.core.logic.EffectProcessor(audioBackend, logger)
    }

    // PlaybackController: Manages all playback operations
    // PHASE 4: Extracted from MainActivity to separate business logic
    // PHASE 5: Uses StateObserver for UI reactivity
    // MILESTONE 2: Now includes EffectProcessor for effects support
    val playbackController = remember {
        PlaybackController(audioEngine, effectProcessor, logger, stateObserver)
    }

    // ClipboardManager: Handles copy/paste (stub for now, implementation in Milestone 2.5)
    // PHASE 4: Extracted from MainActivity to separate business logic
    val clipboardManager = remember {
        com.example.pockettracker.core.logic.ClipboardManager(logger)
    }

    // InputController: Handles button input
    // PHASE 4: Extracted from MainActivity to separate business logic
    // PHASE 5: Uses StateObserver for UI reactivity
    val inputController = remember {
        com.example.pockettracker.core.logic.InputController(logger, stateObserver)
    }

    // TrackerController: Main coordinator that owns state and delegates to controllers
    // PHASE 4: This is the MAIN COORDINATOR for all tracker logic
    // PHASE 5: Uses StateObserver for UI reactivity
    val trackerController = remember {
        com.example.pockettracker.core.logic.TrackerController(
            fileController = fileController,
            playbackController = playbackController,
            instrumentController = instrumentController,
            effectProcessor = effectProcessor,
            clipboardManager = clipboardManager,
            inputController = inputController,
            stateObserver = stateObserver
        ).apply {
            // Initialize with test project data
            project = Project().apply {
                // Add some test notes to phrase 0
                phrases[0].steps[0].note = Note.fromString("C-4")
                phrases[0].steps[0].instrument = 3
                phrases[0].steps[4].note = Note.fromString("E-4")
                phrases[0].steps[4].instrument = 3
                phrases[0].steps[8].note = Note.fromString("G-4")
                phrases[0].steps[8].instrument = 3
                phrases[0].steps[12].note = Note.fromString("C-5")
                phrases[0].steps[12].instrument = 3
            }
        }
    }

    // NOTE: GenericInputHandler has been migrated to InputController (Phase 4)
    // All input handling now goes through trackerController.inputController

    // ChainEditorModule: Used to get cursor context for chain editing
    val chainEditorModule = remember { ChainEditorModule() }

    // PhraseEditorModule: Used to get cursor context for phrase editing
    val phraseEditorModule = remember { PhraseEditorModule() }

    // SongEditorModule: Used to get cursor context for song editing
    val songEditorModule = remember { SongEditorModule() }

    // ProjectModule: Used to get cursor context for project editing
    val projectModule = remember { ProjectModule() }

    // InstrumentModule: Used for instrument editing screen
    val instrumentModule = remember { InstrumentModule() }

    // MixerModule: Used for mixer screen (8 tracks + master)
    val mixerModule = remember { MixerModule() }

    // Peak level buffers for mixer meters (updated periodically)
    val trackPeakBuffer = remember { FloatArray(8) }
    val masterPeakBuffer = remember { FloatArray(2) }

    // ═══════════════════════════════════════════════════════════════════════
    // STATE ALIASES (read from TrackerController, triggered by stateVersion)
    // ═══════════════════════════════════════════════════════════════════════
    //
    // All core state is now owned by TrackerController. These aliases create
    // a dependency on stateVersion so Compose recomposes when state changes.
    // For writes, use trackerController.xxx = value directly.

    // Core state aliases (depend on stateVersion for recomposition)
    // The stateVersion.let { } pattern creates a dependency on stateVersion
    val project = stateVersion.let { trackerController.project }
    val currentScreen = stateVersion.let { trackerController.currentScreen }
    val previousColumn = stateVersion.let { trackerController.previousColumn }
    val cursorRow = stateVersion.let { trackerController.cursorRow }
    val cursorColumn = stateVersion.let { trackerController.cursorColumn }
    val projectCursorRow = stateVersion.let { trackerController.projectCursorRow }
    val projectCursorColumn = stateVersion.let { trackerController.projectCursorColumn }
    val currentChain = stateVersion.let { trackerController.currentChain }
    val currentPhrase = stateVersion.let { trackerController.currentPhrase }
    val lastEditedPhrase = stateVersion.let { trackerController.lastEditedPhrase }
    val lastEditedChain = stateVersion.let { trackerController.lastEditedChain }
    val lastEditedNote = stateVersion.let { trackerController.lastEditedNote }
    val lastEditedVolume = stateVersion.let { trackerController.lastEditedVolume }
    val lastEditedTranspose = stateVersion.let { trackerController.lastEditedTranspose }
    val projectVersion = stateVersion.let { trackerController.projectVersion }

    // Status message aliases - use TrackerController's unified status
    val projectStatusMessage = stateVersion.let { trackerController.statusMessage }
    val projectStatusSuccess = stateVersion.let { trackerController.statusSuccess }

    // Auto-dismiss status messages after 5 seconds
    LaunchedEffect(trackerController.statusMessage) {
        if (trackerController.statusMessage.isNotEmpty()) {
            kotlinx.coroutines.delay(5000)
            trackerController.clearStatus()
        }
    }

    // Auto-dismiss instrument status (from InstrumentController)
    LaunchedEffect(instrumentController.statusMessage) {
        if (instrumentController.statusMessage.isNotEmpty()) {
            kotlinx.coroutines.delay(5000)
            instrumentController.clearStatus()
        }
    }

    // File browser module and state
    val fileBrowserModule = remember { FileBrowserModule() }
    var fileBrowserState by remember {
        mutableStateOf(
            FileBrowserModule.State(
                currentDirectory = File(fileManager.getProjectsDirectory()),
                items = emptyList(),
                fileExtension = "ptp"  // Only show .ptp project files
            )
        )
    }
    var previousScreen by remember { mutableStateOf(ScreenType.PROJECT) }


    // Initialize file browser item list when directory changes
    LaunchedEffect(fileBrowserState.currentDirectory, fileBrowserState.sortMode) {
        val items = fileBrowserModule.buildItemList(
            fileBrowserState.currentDirectory,
            fileBrowserState.fileExtension
        )
        fileBrowserState = fileBrowserState.copy(
            items = fileBrowserModule.sortItems(items, fileBrowserState.sortMode)
        )
    }

    // (Audio engine cleanup moved to line 168-172 with new architecture)

    // Update peak levels for mixer meters (every ~60ms = ~16fps update rate)
    LaunchedEffect(currentScreen) {
        if (currentScreen == ScreenType.MIXER) {
            while (true) {
                audioBackend.getTrackPeaks(trackPeakBuffer)
                audioBackend.getMasterPeaks(masterPeakBuffer)
                stateVersion++  // Trigger recomposition
                kotlinx.coroutines.delay(60)
            }
        }
    }

    // ═══════════════════════════════════════════════════════════════════════
    // INPUT ACTION HELPERS
    // ═══════════════════════════════════════════════════════════════════════

    /**
     * Apply InputAction to phrase step
     *
     * Handles value changes for phrase editing (notes, volume, instrument)
     */
    /**
     * Apply input action to file browser (character editing in RENAME/CREATE mode)
     */
    fun applyFileBrowserInputAction(action: InputAction) {
        when (action) {
            is InputAction.SET_VALUE -> {
                // Set character at renameCursor position
                val char = action.value.toChar()
                val buffer = fileBrowserState.renameBuffer.padEnd(12, ' ')
                val sb = StringBuilder(buffer)
                if (fileBrowserState.renameCursor < sb.length) {
                    sb.setCharAt(fileBrowserState.renameCursor, char)
                    fileBrowserState = fileBrowserState.copy(
                        renameBuffer = sb.toString().trimEnd()
                    )
                }
            }
            is InputAction.DELETE -> {
                // Delete character at cursor (replace with space, trim end)
                val buffer = fileBrowserState.renameBuffer.padEnd(12, ' ')
                val sb = StringBuilder(buffer)
                if (fileBrowserState.renameCursor < sb.length) {
                    sb.setCharAt(fileBrowserState.renameCursor, ' ')
                    fileBrowserState = fileBrowserState.copy(
                        renameBuffer = sb.toString().trimEnd()
                    )
                }
            }
            else -> { /* Other actions not applicable */ }
        }
    }

    /**
     * Reload all custom samples from a loaded project
     *
     * When a project is loaded from JSON, the sampleFilePath is preserved but the actual
     * WAV files aren't in memory. This function iterates through all instruments and
     * reloads any custom samples (instruments with non-null sampleFilePath).
     *
     * Also restores all instrument parameters (ROOT, DETUNE, START, END, REVERSE, LOOP).
     *
     * Should be called immediately after loading a project.
     */
    fun reloadProjectSamples() {
        var loadedCount = 0
        var failedCount = 0

        // IMPORTANT: Use trackerController.project directly, not the local 'project' val
        // The local 'project' is captured at composition time and may be stale
        trackerController.project.instruments.forEach { instrument ->
            if (instrument.sampleFilePath != null) {
                val filePath = instrument.sampleFilePath!!
                val success = audioEngine.loadSampleFromFile(instrument.id, filePath)

                if (success) {
                    // Restore instrument parameters after loading sample
                    audioEngine.updateInstrumentBaseFrequency(instrument)
                    audioEngine.updateInstrumentPlaybackParams(instrument)

                    loadedCount++
                    Log.d("ProjectLoad", "✅ Reloaded sample for instrument ${instrument.id.toString(16).padStart(2, '0')}: $filePath")
                } else {
                    failedCount++
                    Log.e("ProjectLoad", "❌ Failed to reload sample for instrument ${instrument.id.toString(16).padStart(2, '0')}: $filePath")
                }
            }
        }

        if (loadedCount > 0 || failedCount > 0) {
            Log.d("ProjectLoad", "Sample reload complete: $loadedCount loaded, $failedCount failed")
        }
    }

    /**
     * Generic input handler that routes button presses through cursor context
     *
     * This helper eliminates code duplication by:
     * 1. Getting the cursor context for the current screen
     * 2. Calling the provided handler function (handleAButton, handleBButton, etc.)
     * 3. Applying the resulting action to the appropriate screen
     *
     * Usage example:
     *   onAUp = { handleGenericInput { ctx -> trackerController.inputController.handleAButton(ctx) } }
     *   onADown = { handleGenericInput { ctx -> trackerController.inputController.handleBButton(ctx) } }
     *
     * @param handlerFunction Lambda that takes CursorContext and returns InputAction
     */
    fun handleGenericInput(handlerFunction: (CursorContext) -> InputAction) {
        // Read directly from trackerController to avoid stale captured values
        when (trackerController.currentScreen) {
            ScreenType.CHAIN -> {
                val chainState = ChainEditorState(
                    trackerController.project.chains[trackerController.currentChain],
                    trackerController.cursorRow,
                    trackerController.cursorColumn
                )
                val context = chainEditorModule.getCursorContext(chainState)
                val action = handlerFunction(context)
                val result = chainEditorModule.handleInput(chainState, action)
                if (result.modified) {
                    result.lastEditedPhrase?.let { trackerController.lastEditedPhrase = it }
                    result.lastEditedTranspose?.let { trackerController.lastEditedTranspose = it }
                    trackerController.projectVersion++
                }
            }
            ScreenType.PHRASE -> {
                val phraseState = PhraseEditorState(
                    trackerController.project.phrases[trackerController.currentPhrase],
                    trackerController.cursorRow,
                    trackerController.cursorColumn,
                    playbackRow = 0,
                    isPlaying = trackerController.isPlaying()
                )
                val context = phraseEditorModule.getCursorContext(phraseState)
                val action = handlerFunction(context)
                val result = phraseEditorModule.handleInput(phraseState, action, instrumentController)
                if (result.modified) {
                    result.lastEditedNote?.let { trackerController.lastEditedNote = it }
                    result.lastEditedVolume?.let { trackerController.lastEditedVolume = it }
                    trackerController.projectVersion++
                }
            }
            ScreenType.SONG -> {
                val songState = SongEditorState(
                    trackerController.project,
                    trackerController.cursorRow,
                    cursorTrack = trackerController.cursorColumn
                )
                val context = songEditorModule.getCursorContext(songState)
                val action = handlerFunction(context)
                val result = songEditorModule.handleInput(songState, action)
                if (result.modified) {
                    result.lastEditedChain?.let { trackerController.lastEditedChain = it }
                    trackerController.projectVersion++
                }
            }
            ScreenType.PROJECT -> {
                val projectState = ProjectState(
                    trackerController.project,
                    trackerController.projectCursorRow,
                    trackerController.projectCursorColumn,
                    trackerController.statusMessage,
                    trackerController.statusSuccess
                )
                val context = projectModule.getCursorContext(projectState)
                val action = handlerFunction(context)
                val result = projectModule.handleInput(projectState, action)
                if (result.modified) {
                    trackerController.projectVersion++
                }
            }
            ScreenType.FILE_BROWSER -> {
                // Only handle generic input when in RENAME or CREATE mode (character editing)
                if (fileBrowserState.mode == FileBrowserModule.BrowserMode.RENAME ||
                    fileBrowserState.mode == FileBrowserModule.BrowserMode.CREATE) {
                    val context = fileBrowserModule.getCursorContext(fileBrowserState)
                    val action = handlerFunction(context)
                    applyFileBrowserInputAction(action)
                }
            }
            ScreenType.INSTRUMENT -> {
                val instrumentState = InstrumentState(
                    trackerController.project.instruments[trackerController.currentInstrument],
                    trackerController.instrumentCursorRow,
                    trackerController.instrumentCursorColumn,
                    trackerController.statusMessage,
                    trackerController.statusSuccess
                )
                val context = instrumentModule.getCursorContext(instrumentState)
                val action = handlerFunction(context)
                val result = instrumentModule.handleInput(instrumentState, action, instrumentController)
                if (result.modified) {
                    trackerController.projectVersion++
                }
            }
            ScreenType.MIXER -> {
                val mixerState = MixerState(
                    trackerController.project,
                    trackerController.mixerCursorColumn,
                    trackPeakBuffer,
                    masterPeakBuffer
                )
                val context = mixerModule.getCursorContext(mixerState)
                val action = handlerFunction(context)
                val result = mixerModule.handleInput(mixerState, action) {
                    trackerController.projectVersion++
                }
                // Result handled in callback
            }
            else -> { /* Other screens not yet implemented */ }
        }
    }

    /**
     * Apply an InputAction based on current screen and context
     *
     * This dispatcher handles InputAction objects returned by InputController.
     * It bridges the gap between input interpretation and state management.
     *
     * Routes NAVIGATE actions to appropriate TrackerController methods.
     */
    fun applyInputAction(action: InputAction) {
        when (action) {
            is InputAction.NAVIGATE_UP -> {
                when (trackerController.currentScreen) {
                    ScreenType.FILE_BROWSER -> {
                        // File browser: move cursor up
                        if (fileBrowserState.items.isNotEmpty()) {
                            val newCursor = if (fileBrowserState.cursor > 0) {
                                fileBrowserState.cursor - 1
                            } else {
                                fileBrowserState.items.size - 1
                            }
                            val newScroll = when {
                                newCursor < fileBrowserState.scroll -> newCursor
                                newCursor >= fileBrowserState.scroll + FileBrowserModule.VISIBLE_ROWS ->
                                    (newCursor - FileBrowserModule.VISIBLE_ROWS + 1).coerceAtLeast(0)
                                else -> fileBrowserState.scroll
                            }
                            fileBrowserState = fileBrowserState.copy(cursor = newCursor, scroll = newScroll)
                        }
                    }
                    else -> trackerController.moveCursorUp()
                }
            }
            is InputAction.NAVIGATE_DOWN -> {
                when (trackerController.currentScreen) {
                    ScreenType.FILE_BROWSER -> {
                        // File browser: move cursor down
                        if (fileBrowserState.items.isNotEmpty()) {
                            val newCursor = if (fileBrowserState.cursor < fileBrowserState.items.size - 1) {
                                fileBrowserState.cursor + 1
                            } else {
                                0
                            }
                            val newScroll = when {
                                newCursor >= fileBrowserState.scroll + FileBrowserModule.VISIBLE_ROWS ->
                                    newCursor - FileBrowserModule.VISIBLE_ROWS + 1
                                newCursor < fileBrowserState.scroll -> newCursor
                                else -> fileBrowserState.scroll
                            }
                            fileBrowserState = fileBrowserState.copy(cursor = newCursor, scroll = newScroll)
                        }
                    }
                    else -> trackerController.moveCursorDown()
                }
            }
            is InputAction.NAVIGATE_LEFT -> {
                when (trackerController.currentScreen) {
                    ScreenType.FILE_BROWSER -> {
                        // File browser: page up or edit character position
                        when (fileBrowserState.mode) {
                            FileBrowserModule.BrowserMode.RENAME, FileBrowserModule.BrowserMode.CREATE -> {
                                if (fileBrowserState.renameCursor > 0) {
                                    fileBrowserState = fileBrowserState.copy(renameCursor = fileBrowserState.renameCursor - 1)
                                }
                            }
                            else -> {
                                if (fileBrowserState.items.isNotEmpty()) {
                                    val newCursor = (fileBrowserState.cursor - FileBrowserModule.VISIBLE_ROWS).coerceAtLeast(0)
                                    val newScroll = when {
                                        newCursor < fileBrowserState.scroll -> newCursor
                                        newCursor >= fileBrowserState.scroll + FileBrowserModule.VISIBLE_ROWS ->
                                            (newCursor - FileBrowserModule.VISIBLE_ROWS + 1).coerceAtLeast(0)
                                        else -> fileBrowserState.scroll
                                    }
                                    fileBrowserState = fileBrowserState.copy(cursor = newCursor, scroll = newScroll)
                                }
                            }
                        }
                    }
                    else -> trackerController.moveCursorLeft()
                }
            }
            is InputAction.NAVIGATE_RIGHT -> {
                when (trackerController.currentScreen) {
                    ScreenType.FILE_BROWSER -> {
                        // File browser: page down or edit character position
                        when (fileBrowserState.mode) {
                            FileBrowserModule.BrowserMode.RENAME, FileBrowserModule.BrowserMode.CREATE -> {
                                if (fileBrowserState.renameCursor < 11) {
                                    fileBrowserState = fileBrowserState.copy(renameCursor = fileBrowserState.renameCursor + 1)
                                }
                            }
                            else -> {
                                if (fileBrowserState.items.isNotEmpty()) {
                                    val newCursor = (fileBrowserState.cursor + FileBrowserModule.VISIBLE_ROWS)
                                        .coerceAtMost(fileBrowserState.items.size - 1)
                                    val newScroll = when {
                                        newCursor >= fileBrowserState.scroll + FileBrowserModule.VISIBLE_ROWS ->
                                            newCursor - FileBrowserModule.VISIBLE_ROWS + 1
                                        newCursor < fileBrowserState.scroll -> newCursor
                                        else -> fileBrowserState.scroll
                                    }
                                    fileBrowserState = fileBrowserState.copy(cursor = newCursor, scroll = newScroll)
                                }
                            }
                        }
                    }
                    else -> trackerController.moveCursorRight()
                }
            }
            // Other actions are handled by handleGenericInput or specific handlers
            else -> { /* No action needed for other types */ }
        }
    }

    /**
     * DPAD navigation handler - gets proper context and dispatches NAVIGATE action
     * 
     * Unlike handleGenericInput, this directly calls applyInputAction() because
     * modules don't understand NAVIGATE_* actions - those are screen-level navigation.
     *
     * @param handlerFunction Lambda that takes CursorContext and returns InputAction (typically a NAVIGATE_* action)
     */
    fun handleDPadNavigation(handlerFunction: () -> InputAction) {
        // Check if in selection mode first - expand selection instead of navigating
        if (trackerController.inputController.isSelectionModeActive()) {
            val action = handlerFunction()
            val direction = when (action) {
                InputAction.NAVIGATE_UP -> "UP"
                InputAction.NAVIGATE_DOWN -> "DOWN"
                InputAction.NAVIGATE_LEFT -> "LEFT"
                InputAction.NAVIGATE_RIGHT -> "RIGHT"
                else -> null
            }
            if (direction != null) {
                val maxColumn = when (trackerController.currentScreen) {
                    ScreenType.PHRASE -> 9
                    ScreenType.CHAIN -> 2
                    ScreenType.SONG -> 8
                    else -> 1
                }
                trackerController.inputController.expandSelection(direction, 15, maxColumn)
                return
            }
        }

        // DPAD navigation: Dispatch NAVIGATE_* actions directly via applyInputAction()
        // (NOT to module.handleInput() since modules don't understand NAVIGATE actions)
        //
        // Note: DPAD movement is screen-aware (handled by trackerController),
        // not cursor-state-aware. Movement bounds are enforced per-screen, not per-context.
        val action = handlerFunction()
        applyInputAction(action)
    }

    // ═══════════════════════════════════════════════════════════════════════
    // BUTTON HANDLERS
    // ═══════════════════════════════════════════════════════════════════════

    // Create a ButtonHandlers object that contains all button press logic
    // Remove dependencies - handlers can access current values directly from scope
    // This prevents InputMapper from being recreated and losing modifier states
    val buttonHandlers = remember {
        ButtonHandlers(
            onDPadUp = {
                handleDPadNavigation { trackerController.inputController.handleDPadUp() }
            },

            // ───────────────────────────────────────────────────────────────
            // D-PAD DOWN
            // ───────────────────────────────────────────────────────────────
            onDPadDown = {
                handleDPadNavigation { trackerController.inputController.handleDPadDown() }
            },

            // ───────────────────────────────────────────────────────────────
            // D-PAD LEFT
            // ───────────────────────────────────────────────────────────────
            onDPadLeft = {
                handleDPadNavigation { trackerController.inputController.handleDPadLeft() }
            },

            // ───────────────────────────────────────────────────────────────
            // D-PAD RIGHT
            // ───────────────────────────────────────────────────────────────
            onDPadRight = {
                handleDPadNavigation { trackerController.inputController.handleDPadRight() }
            },

// ───────────────────────────────────────────────────────────────
// BUTTON A - Primary action (insert/increment)
// ───────────────────────────────────────────────────────────────
            onButtonA = {
                // Read directly from trackerController to avoid stale captured values
                when (trackerController.currentScreen) {
                    // FILE BROWSER: Open folder, load file, or confirm actions
                    ScreenType.FILE_BROWSER -> {
                        when (fileBrowserState.mode) {
                            FileBrowserModule.BrowserMode.NORMAL -> {
                                // Open folder or load file
                                val item = fileBrowserState.items.getOrNull(fileBrowserState.cursor)
                                when (item) {
                                    is FileBrowserModule.BrowserItem.Parent -> {
                                        // Navigate to parent folder
                                        fileBrowserState = fileBrowserModule.navigateToParent(fileBrowserState)
                                    }
                                    is FileBrowserModule.BrowserItem.Folder -> {
                                        // Navigate into folder
                                        fileBrowserState = fileBrowserModule.navigateToFolder(
                                            fileBrowserState,
                                            item.file
                                        )
                                    }
                                    is FileBrowserModule.BrowserItem.FileItem -> {
                                        // Check which screen opened the file browser
                                        when (previousScreen) {
                                            ScreenType.PROJECT -> {
                                                // Load project file
                                                val result = trackerController.loadProject(item.file.toFileInfo())
                                                when (result) {
                                                    is FileController.LoadResult.Success -> {
                                                        trackerController.project = result.project
                                                        // Reload all custom samples from the loaded project
                                                        reloadProjectSamples()
                                                        trackerController.statusMessage = "LOADED: ${item.file.nameWithoutExtension}"
                                                        trackerController.statusSuccess = true
                                                        trackerController.projectVersion++
                                                        trackerController.currentScreen = previousScreen
                                                    }
                                                    is FileController.LoadResult.Error -> {
                                                        fileBrowserState = fileBrowserState.copy(
                                                            statusMessage = "LOAD FAILED",
                                                            statusSuccess = false
                                                        )
                                                    }
                                                }
                                            }
                                            ScreenType.INSTRUMENT -> {
                                                // Load WAV sample file using InstrumentController
                                                val result = instrumentController.loadSampleFromFile(
                                                    trackerController.project,
                                                    item.file.absolutePath
                                                )
                                                if (result is com.example.pockettracker.core.logic.LoadResult.Success) {
                                                    trackerController.projectVersion++
                                                    trackerController.currentScreen = previousScreen
                                                } else {
                                                    fileBrowserState = fileBrowserState.copy(
                                                        statusMessage = "LOAD FAILED",
                                                        statusSuccess = false
                                                    )
                                                }
                                            }
                                            else -> {
                                                // Unknown previous screen - try loading as project
                                                val result = trackerController.loadProject(item.file.toFileInfo())
                                                when (result) {
                                                    is FileController.LoadResult.Success -> {
                                                        trackerController.project = result.project
                                                        // Reload all custom samples from the loaded project
                                                        reloadProjectSamples()
                                                        trackerController.projectVersion++
                                                        trackerController.currentScreen = previousScreen
                                                    }
                                                    is FileController.LoadResult.Error -> {
                                                        fileBrowserState = fileBrowserState.copy(
                                                            statusMessage = "LOAD FAILED",
                                                            statusSuccess = false
                                                        )
                                                    }
                                                }
                                            }
                                        }
                                    }
                                    null -> { /* Empty list */ }
                                }
                            }
                            FileBrowserModule.BrowserMode.DELETE -> {
                                // Confirm delete
                                val item = fileBrowserState.items.getOrNull(fileBrowserState.cursor)
                                if (item != null && item !is FileBrowserModule.BrowserItem.Parent) {
                                    val deleted = fileManager.deleteFileOrFolder(item.file.absolutePath)
                                    if (deleted) {
                                        // Refresh list
                                        val newItems = fileBrowserModule.buildItemList(
                                            fileBrowserState.currentDirectory,
                                            fileBrowserState.fileExtension
                                        )
                                        val sortedItems = fileBrowserModule.sortItems(newItems, fileBrowserState.sortMode)

                                        // Adjust cursor if needed
                                        val newCursor = fileBrowserState.cursor.coerceAtMost(
                                            (newItems.size - 1).coerceAtLeast(0)
                                        )

                                        fileBrowserState = fileBrowserState.copy(
                                            items = sortedItems,
                                            cursor = newCursor,
                                            mode = FileBrowserModule.BrowserMode.NORMAL,
                                            statusMessage = "Deleted: ${item.displayName}",
                                            statusSuccess = true
                                        )
                                    } else {
                                        fileBrowserState = fileBrowserState.copy(
                                            mode = FileBrowserModule.BrowserMode.NORMAL,
                                            statusMessage = "Delete failed",
                                            statusSuccess = false
                                        )
                                    }
                                }
                            }
                            FileBrowserModule.BrowserMode.RENAME -> {
                                // Confirm rename (will implement in Phase 6)
                                fileBrowserState = fileBrowserState.copy(mode = FileBrowserModule.BrowserMode.NORMAL)
                            }
                            FileBrowserModule.BrowserMode.CREATE -> {
                                // Confirm create folder (will implement in Phase 6)
                                fileBrowserState = fileBrowserState.copy(mode = FileBrowserModule.BrowserMode.NORMAL)
                            }
                        }
                    }

                    // PROJECT SCREEN: Only handle action buttons (LOAD/SAVE/NEW)
                    // Value editing (TEMPO/TRANSPOSE/NAME) is handled by A+direction combos
                    ScreenType.PROJECT -> {
                        Log.d("ProjectScreen", "A pressed: row=${trackerController.projectCursorRow}, col=${trackerController.projectCursorColumn}")
                        when (trackerController.projectCursorRow) {
                            // ROW 3: PROJECT actions (LOAD/SAVE/NEW)
                            3 -> {
                                Log.d("ProjectScreen", "Row 3 action: column=${trackerController.projectCursorColumn}")
                                when (trackerController.projectCursorColumn) {
                                    1 -> {  // LOAD - Show file browser
                                        Log.d("ProjectScreen", "LOAD button pressed - switching to FILE_BROWSER")
                                        previousScreen = trackerController.currentScreen
                                        trackerController.currentScreen = ScreenType.FILE_BROWSER
                                        // Reset file browser state with correct directory and extension
                                        fileBrowserState = fileBrowserState.copy(
                                            currentDirectory = File(fileManager.getProjectsDirectory()),
                                            cursor = 0,
                                            scroll = 0,
                                            mode = FileBrowserModule.BrowserMode.NORMAL,
                                            fileExtension = "ptp",  // Filter for project files
                                            statusMessage = ""
                                        )
                                        Log.d("ProjectScreen", "File browser opened for .ptp files")
                                    }
                                    2 -> {  // SAVE
                                        val result = trackerController.saveProject(trackerController.project.name)
                                        when (result) {
                                            is FileController.SaveResult.Success -> {
                                                trackerController.statusMessage = "SAVED"
                                                trackerController.statusSuccess = true
                                            }
                                            is FileController.SaveResult.Error -> {
                                                trackerController.statusMessage = "SAVE FAILED"
                                                trackerController.statusSuccess = false
                                            }
                                        }
                                    }
                                    3 -> {  // NEW
                                        trackerController.project = Project()
                                        trackerController.statusMessage = "NEW PROJECT"
                                        trackerController.statusSuccess = true
                                    }
                                }
                            }
                            // Rows 4-6 (EXPORT, CLEAN, SYSTEM) - placeholder for now
                        }
                    }

                    // INSTRUMENT: Handle LOAD button
                    ScreenType.INSTRUMENT -> {
                        when (trackerController.instrumentCursorRow) {
                            1 -> {  // ROW 1: LOAD button
                                if (trackerController.instrumentCursorColumn == 1) {
                                    // Sync instrumentController before opening file browser
                                    instrumentController.currentInstrument = trackerController.currentInstrument
                                    val samplesDir = File(fileManager.getSamplesDirectory())
                                    Log.d("InstrumentScreen", "LOAD button pressed - opening file browser for instrument ${trackerController.currentInstrument}")
                                    Log.d("InstrumentScreen", "Samples directory: ${samplesDir.absolutePath}")
                                    Log.d("InstrumentScreen", "Directory exists: ${samplesDir.exists()}")
                                    Log.d("InstrumentScreen", "Directory can read: ${samplesDir.canRead()}")
                                    if (samplesDir.exists()) {
                                        val files = samplesDir.listFiles()
                                        Log.d("InstrumentScreen", "Files in directory: ${files?.size ?: 0}")
                                        files?.forEach { Log.d("InstrumentScreen", "  - ${it.name}") }
                                    }

                                    previousScreen = trackerController.currentScreen
                                    trackerController.currentScreen = ScreenType.FILE_BROWSER
                                    // Reset file browser state with correct directory and extension for WAV files
                                    fileBrowserState = FileBrowserModule.State(
                                        currentDirectory = samplesDir,
                                        items = fileBrowserModule.buildItemList(samplesDir, fileExtension = "wav"),
                                        cursor = 0,
                                        scroll = 0,
                                        mode = FileBrowserModule.BrowserMode.NORMAL,
                                        fileExtension = "wav",  // Only show WAV files
                                        statusMessage = ""
                                    )
                                    Log.d("InstrumentScreen", "File browser opened for .wav files, items: ${fileBrowserState.items.size}")
                                }
                            }
                            // Other rows use A+direction combos for value editing
                        }
                    }

                    // PHRASE: Quick insert last-used values on empty row
                    ScreenType.PHRASE -> {
                        // Quick-insert only works on Note column (column 1)
                        // FX columns (4-9) should not trigger note insertion
                        if (trackerController.cursorColumn == 1) {
                            // Build phrase state for the current cursor so we can get a CursorContext
                            val phraseState = PhraseEditorState(
                                trackerController.project.phrases[trackerController.currentPhrase],
                                trackerController.cursorRow,
                                trackerController.cursorColumn,
                                playbackRow = 0,
                                isPlaying = trackerController.isPlaying()
                            )
                            // Get cursor context to check if cell is empty via architecture
                            val context = phraseEditorModule.getCursorContext(phraseState)
                            if (context.capabilities.isEmpty) {
                                // Insert last-used values on empty note
                                val step = trackerController.project.phrases[trackerController.currentPhrase].steps[trackerController.cursorRow]
                                step.note = trackerController.lastEditedNote
                                step.instrument = trackerController.lastEditedInstrument
                                step.volume = trackerController.lastEditedVolume
                                trackerController.projectVersion++
                                Log.d("QuickInsert", "Inserted phrase step: note=${trackerController.lastEditedNote}, inst=${trackerController.lastEditedInstrument}, vol=${trackerController.lastEditedVolume}")
                            }
                        }
                        // For FX columns, A button behavior is handled by A+direction combos via handleGenericInput
                    }

                    // CHAIN: Quick insert last-used phrase + transpose on empty row
                    ScreenType.CHAIN -> {
                        val chain = trackerController.project.chains[trackerController.currentChain]
                        if (chain.isEmpty(trackerController.cursorRow)) {
                            // Insert last-used phrase and transpose
                            chain.phraseRefs[trackerController.cursorRow] = trackerController.lastEditedPhrase
                            chain.transposeValues[trackerController.cursorRow] = trackerController.lastEditedTranspose
                            trackerController.projectVersion++
                            Log.d("QuickInsert", "Inserted chain row: phrase=${trackerController.lastEditedPhrase}, transpose=${trackerController.lastEditedTranspose}")
                        }
                    }

                    // SONG: Quick insert last-used chain on empty row
                    ScreenType.SONG -> {
                        val track = trackerController.project.tracks[trackerController.cursorColumn - 1]
                        // Ensure track has enough rows
                        while (track.chainRefs.size <= trackerController.cursorRow) {
                            track.chainRefs.add(-1)
                        }
                        if (track.chainRefs[trackerController.cursorRow] == -1) {
                            // Insert last-used chain
                            track.chainRefs[trackerController.cursorRow] = trackerController.lastEditedChain
                            trackerController.projectVersion++
                            Log.d("QuickInsert", "Inserted song chain: chain=${trackerController.lastEditedChain} at track=${trackerController.cursorColumn-1}, row=${trackerController.cursorRow}")
                        }
                    }

                    else -> { /* Other screens not implemented yet */ }
                }
            },

// ───────────────────────────────────────────────────────────────
// BUTTON B - Secondary action
// ───────────────────────────────────────────────────────────────
            onButtonB = {
                // Check if in selection mode first - B = COPY
                if (trackerController.inputController.isSelectionModeActive()) {
                    val bounds = trackerController.inputController.getSelectionBounds()
                    if (bounds != null) {
                        when (trackerController.currentScreen) {
                            ScreenType.PHRASE -> {
                                clipboardManager.copyPhraseSteps(
                                    trackerController.project,
                                    trackerController.currentPhrase,
                                    bounds.topLeftRow, bounds.topLeftColumn,
                                    bounds.bottomRightRow, bounds.bottomRightColumn
                                )
                                Log.d("CopyPaste", "Copied phrase selection: ${bounds.width}x${bounds.height}")
                            }
                            ScreenType.CHAIN -> {
                                clipboardManager.copyChainRows(
                                    trackerController.project,
                                    trackerController.currentChain,
                                    bounds.topLeftRow, bounds.topLeftColumn,
                                    bounds.bottomRightRow, bounds.bottomRightColumn
                                )
                                Log.d("CopyPaste", "Copied chain selection: ${bounds.width}x${bounds.height}")
                            }
                            ScreenType.SONG -> {
                                clipboardManager.copySongCells(
                                    trackerController.project,
                                    bounds.topLeftRow, bounds.topLeftColumn,
                                    bounds.bottomRightRow, bounds.bottomRightColumn
                                )
                                Log.d("CopyPaste", "Copied song selection: ${bounds.width}x${bounds.height}")
                            }
                            else -> { }
                        }
                        trackerController.inputController.exitSelectionMode()
                    }
                    return@ButtonHandlers
                }

                when (trackerController.currentScreen) {
                    // FILE BROWSER: Cancel operation or go back
                    ScreenType.FILE_BROWSER -> {
                        when (fileBrowserState.mode) {
                            FileBrowserModule.BrowserMode.NORMAL -> {
                                // Go back to previous screen
                                Log.d("FileBrowser", "Returning to $previousScreen")
                                trackerController.currentScreen = previousScreen
                            }
                            else -> {
                                // Cancel current mode (DELETE/RENAME/CREATE)
                                Log.d("FileBrowser", "Cancelled ${fileBrowserState.mode} mode")
                                fileBrowserState = fileBrowserState.copy(
                                    mode = FileBrowserModule.BrowserMode.NORMAL,
                                    renameBuffer = "",
                                    renameCursor = 0
                                )
                            }
                        }
                    }

                    // SONG SCREEN:
                    ScreenType.SONG -> {
                        // All song value editing is now handled via generic input handler
                        // No need for manual cycling here
                    }

                    // CHAIN SCREEN:
                    ScreenType.CHAIN -> {
                        // No need for manual cycling here
                    }

                    // PHRASE SCREEN: Now handled by generic input (A+direction combos)
                    ScreenType.PHRASE -> {
                        // All phrase value editing is now handled via generic input handler
                        // No need for manual cycling here
                    }

                    // PROJECT SCREEN:
                    ScreenType.PROJECT -> {
                        when (trackerController.projectCursorRow) {
                            // ROW 0: TEMPO (decrease by 1)
                            0 -> {
                                // No need for manual cycling here
                            }
                        }
                    }

                    else -> { }
                }
            },

// ───────────────────────────────────────────────────────────────
// SELECT BUTTON - Clear/Delete or quick navigation
// ───────────────────────────────────────────────────────────────
            onSelect = {
                // Read directly from trackerController to avoid stale captured values
                when (trackerController.currentScreen) {
                    // SONG SCREEN: Clear chain reference
                    ScreenType.SONG -> {
                        val trackIndex = getTrackIndex(trackerController.cursorColumn)
                        clearSongChainRef(trackerController.project.tracks[trackIndex], trackerController.cursorRow)
                        trackerController.projectVersion++
                    }

                    // CHAIN SCREEN: Use generic input system (DELETE action)
                    ScreenType.CHAIN -> {
                        // Get cursor context from chain editor
                        val chainState = ChainEditorState(
                            trackerController.project.chains[trackerController.currentChain],
                            trackerController.cursorRow,
                            trackerController.cursorColumn
                        )
                        val context = chainEditorModule.getCursorContext(chainState)

                        // Handle SELECT as delete
                        val action = trackerController.inputController.handleSelect(context)

                        // Apply the action
                        when (action) {
                            is InputAction.DELETE -> {
                                // Clear the value at cursor position
                                if (trackerController.cursorColumn == 1) {
                                    clearChainSlot(trackerController.project.chains[trackerController.currentChain], trackerController.cursorRow)
                                    trackerController.projectVersion++
                                }
                            }
                            else -> { }
                        }
                    }

                    // PHRASE SCREEN: Clear step at cursor
                    ScreenType.PHRASE -> {
                        val step = trackerController.project.phrases[trackerController.currentPhrase].steps[trackerController.cursorRow]
                        when (trackerController.cursorColumn) {
                            1 -> {
                                // Clear note
                                step.note = Note.EMPTY
                                trackerController.projectVersion++
                            }
                            2 -> {
                                // Clear volume (set to max = FF)
                                step.volume = 0xFF
                                trackerController.projectVersion++
                            }
                            3 -> {
                                // Clear instrument (set to 0)
                                step.instrument = 0
                                trackerController.projectVersion++
                            }
                            4, 5 -> {
                                // Clear FX1
                                step.fx1Type = 0
                                step.fx1Value = 0
                                trackerController.projectVersion++
                            }
                            6, 7 -> {
                                // Clear FX2
                                step.fx2Type = 0
                                step.fx2Value = 0
                                trackerController.projectVersion++
                            }
                            8, 9 -> {
                                // Clear FX3
                                step.fx3Type = 0
                                step.fx3Value = 0
                                trackerController.projectVersion++
                            }
                        }
                    }

                    // FILE_BROWSER: SELECT button does nothing (combos handled separately)
                    ScreenType.FILE_BROWSER -> {
                        // Do nothing - SELECT combos (SELECT+A, SELECT+B, etc.) are handled in InputMapper
                    }

                    // OTHER SCREENS: Quick jump to main screen
                    else -> {
                        if (trackerController.currentScreen !in MAIN_ROW_SCREENS) {
                            trackerController.currentScreen = when (trackerController.previousColumn) {
                                0 -> ScreenType.SONG
                                1 -> ScreenType.CHAIN
                                2 -> ScreenType.PHRASE
                                3 -> ScreenType.INSTRUMENT
                                4 -> ScreenType.TABLE
                                else -> ScreenType.PHRASE
                            }
                        }
                    }
                }
            },

// ───────────────────────────────────────────────────────────────
// START BUTTON - Preview sample or toggle playback
// ───────────────────────────────────────────────────────────────
            onStart = {
                // Read directly from trackerController to avoid stale captured values
                when (trackerController.currentScreen) {
                    // PHASE 1 TEST: START on PROJECT screen triggers note queue test
                    ScreenType.PROJECT -> {
                        Log.d("NoteQueueTest", "🧪 START on PROJECT - Running note queue test...")
                        playbackController.testNoteQueue(trackerController.project)
                    }

                    // File browser: Preview selected WAV file
                    ScreenType.FILE_BROWSER -> {
                        Log.d("FileBrowser", "START pressed - previousScreen=$previousScreen, items=${fileBrowserState.items.size}")
                        if (previousScreen == ScreenType.INSTRUMENT && fileBrowserState.items.isNotEmpty()) {
                            val selectedItem = fileBrowserState.items[fileBrowserState.cursor]
                            val selectedFile = selectedItem.file
                            Log.d("FileBrowser", "Selected: ${selectedFile.name}, isFile=${selectedFile.isFile}, ext=${selectedFile.extension}")
                            if (selectedFile.isFile && selectedFile.extension.lowercase() == "wav") {
                                Log.d("FileBrowser", "Previewing sample: ${selectedFile.absolutePath}")
                                trackerController.previewSampleFile(selectedFile.absolutePath)
                            }
                        }
                    }

                    // Instrument screen: Preview instrument with all parameters
                    ScreenType.INSTRUMENT -> {
                        trackerController.previewInstrument()
                    }

                    // Other screens: Toggle playback USING TrackerController
                    else -> {
                        Log.d("Playback", "▶️ START pressed on ${trackerController.currentScreen}, isPlaying=${trackerController.isPlaying()}")
                        // Use TrackerController instead of direct PlaybackController
                        if (trackerController.isPlaying()) {
                            Log.d("Playback", "  → Stopping playback")
                            trackerController.stopPlayback()
                        } else {
                            when (trackerController.currentScreen) {
                                ScreenType.PHRASE -> {
                                    Log.d("Playback", "  → Starting phrase ${trackerController.currentPhrase}")
                                    trackerController.playPhrase(trackerController.currentPhrase)
                                    Log.d("Playback", "  → After playPhrase: isPlaying=${trackerController.isPlaying()}")
                                }

                                ScreenType.CHAIN -> {
                                    Log.d("Playback", "  → Starting chain ${trackerController.currentChain}")
                                    trackerController.playChain(trackerController.currentChain)
                                }

                                ScreenType.SONG -> {
                                    Log.d("Playback", "  → Starting song")
                                    trackerController.playSong()
                                }

                                ScreenType.MIXER -> {
                                    Log.d("Playback", "  → Starting song (from mixer)")
                                    trackerController.playSong()
                                }

                                else -> {
                                    Log.d("Playback", "  → Default togglePlayback")
                                    // Default to phrase playback
                                    trackerController.togglePlayback()  // NEW: Single method call
                                }
                            }
                        }
                    }
                }
            },

// ───────────────────────────────────────────────────────────────
// L BUTTON - Cancel selection mode (or hold modifier)
// ───────────────────────────────────────────────────────────────
            onL = {
                // L alone: Cancel selection mode without copying (M8-style)
                if (trackerController.inputController.isSelectionModeActive()) {
                    trackerController.inputController.exitSelectionMode()
                    Log.d("Selection", "L alone: Cancelled selection mode")
                }
                // Otherwise L is tracked as a hold modifier by InputMapper
                // Combinations like L+A, L+direction are handled in InputMapper
            },

// ───────────────────────────────────────────────────────────────
// R BUTTON - Hold modifier (tracked by InputMapper)
// ───────────────────────────────────────────────────────────────
            onR = {
                // R button alone
                // R is tracked as a hold modifier by InputMapper
                // Combinations like R+arrows for screen navigation handled in InputMapper
            },

// ───────────────────────────────────────────────────────────────
// A + DIRECTION COMBINATIONS (M8-style value editing)
// ───────────────────────────────────────────────────────────────
            onAUp = {
                // A+UP: Small increment (uses InputController)
                handleGenericInput { context -> trackerController.inputController.handleAButton(context) }
            },

            onADown = {
                // A+DOWN: Small decrement (uses InputController)
                handleGenericInput { context -> trackerController.inputController.handleBButton(context) }
            },

            onALeft = {
                // A+LEFT: Large decrement (uses InputController)
                handleGenericInput { context -> trackerController.inputController.handleALeft(context) }
            },

            onARight = {
                // A+RIGHT: Large increment (uses InputController)
                handleGenericInput { context -> trackerController.inputController.handleARight(context) }
            },

// ───────────────────────────────────────────────────────────────
// A + B COMBINATION (Delete selection or single cell)
// ───────────────────────────────────────────────────────────────
            onAB = {
                // A+B in selection mode: Delete selection (no clipboard)
                if (trackerController.inputController.isSelectionModeActive()) {
                    val bounds = trackerController.inputController.getSelectionBounds()
                    if (bounds != null) {
                        when (trackerController.currentScreen) {
                            ScreenType.PHRASE -> {
                                clipboardManager.deletePhraseSteps(
                                    trackerController.project,
                                    trackerController.currentPhrase,
                                    bounds.topLeftRow,
                                    bounds.topLeftColumn,
                                    bounds.bottomRightRow,
                                    bounds.bottomRightColumn
                                )
                                Log.d("Selection", "A+B: Deleted phrase selection")
                            }
                            ScreenType.CHAIN -> {
                                clipboardManager.deleteChainRows(
                                    trackerController.project,
                                    trackerController.currentChain,
                                    bounds.topLeftRow,
                                    bounds.topLeftColumn,
                                    bounds.bottomRightRow,
                                    bounds.bottomRightColumn
                                )
                                Log.d("Selection", "A+B: Deleted chain selection")
                            }
                            ScreenType.SONG -> {
                                clipboardManager.deleteSongCells(
                                    trackerController.project,
                                    bounds.topLeftRow,
                                    bounds.topLeftColumn,
                                    bounds.bottomRightRow,
                                    bounds.bottomRightColumn
                                )
                                Log.d("Selection", "A+B: Deleted song selection")
                            }
                            else -> { }
                        }
                        trackerController.inputController.exitSelectionMode()
                    }
                } else {
                    // A+B outside selection: Delete/clear single value at cursor
                    handleGenericInput { context -> trackerController.inputController.handleABCombo(context) }
                }
            },

// ───────────────────────────────────────────────────────────────
// B + DIRECTION COMBINATIONS (Item cycling: chain/phrase/instrument)
// ───────────────────────────────────────────────────────────────
            onBLeft = {
                // B+LEFT: Navigate to previous chain/phrase/instrument
                Log.d("Navigation", "B+LEFT: currentScreen=${trackerController.currentScreen}")
                when (trackerController.currentScreen) {
                    ScreenType.CHAIN -> {
                        // Previous chain (wrap around)
                        trackerController.currentChain = if (trackerController.currentChain > 0) trackerController.currentChain - 1 else 255
                        trackerController.lastEditedChain = trackerController.currentChain
                        Log.d("Navigation", "  -> Changed to chain ${trackerController.currentChain}")
                    }
                    ScreenType.PHRASE -> {
                        // Previous phrase (wrap around)
                        trackerController.currentPhrase = if (trackerController.currentPhrase > 0) trackerController.currentPhrase - 1 else 255
                        trackerController.lastEditedPhrase = trackerController.currentPhrase
                        Log.d("Navigation", "  -> Changed to phrase ${trackerController.currentPhrase}")
                    }
                    ScreenType.INSTRUMENT -> {
                        // Previous instrument (wrap around)
                        val newInst = if (trackerController.currentInstrument > 0)
                            trackerController.currentInstrument - 1 else 255
                        trackerController.currentInstrument = newInst
                        trackerController.lastEditedInstrument = newInst
                        instrumentController.currentInstrument = newInst  // Keep in sync!
                        Log.d("Navigation", "  -> Changed to instrument $newInst")
                    }
                    else -> { Log.d("Navigation", "  -> No action for screen ${trackerController.currentScreen}") }
                }
            },

            onBRight = {
                // B+RIGHT: Navigate to next chain/phrase/instrument
                Log.d("Navigation", "B+RIGHT: currentScreen=${trackerController.currentScreen}")
                when (trackerController.currentScreen) {
                    ScreenType.CHAIN -> {
                        // Next chain (wrap around)
                        trackerController.currentChain = if (trackerController.currentChain < 255) trackerController.currentChain + 1 else 0
                        trackerController.lastEditedChain = trackerController.currentChain
                        Log.d("Navigation", "  -> Changed to chain ${trackerController.currentChain}")
                    }
                    ScreenType.PHRASE -> {
                        // Next phrase (wrap around)
                        trackerController.currentPhrase = if (trackerController.currentPhrase < 255) trackerController.currentPhrase + 1 else 0
                        trackerController.lastEditedPhrase = trackerController.currentPhrase
                        Log.d("Navigation", "  -> Changed to phrase ${trackerController.currentPhrase}")
                    }
                    ScreenType.INSTRUMENT -> {
                        // Next instrument (wrap around)
                        val newInst = if (trackerController.currentInstrument < 255)
                            trackerController.currentInstrument + 1 else 0
                        trackerController.currentInstrument = newInst
                        trackerController.lastEditedInstrument = newInst
                        instrumentController.currentInstrument = newInst  // Keep in sync!
                        Log.d("Navigation", "  -> Changed to instrument $newInst")
                    }
                    else -> { Log.d("Navigation", "  -> No action for screen ${trackerController.currentScreen}") }
                }
            },

// ───────────────────────────────────────────────────────────────
// R + DIRECTION COMBINATIONS (Screen navigation)
// ───────────────────────────────────────────────────────────────
            onRUp = {
                // R+UP: Navigate to screen above in 5×5 grid OR cycle sort mode up in FILE_BROWSER
                if (trackerController.currentScreen == ScreenType.FILE_BROWSER) {
                    // File browser: cycle sort mode up
                    val modes = FileSortMode.values()
                    val currentIndex = modes.indexOf(fileBrowserState.sortMode)
                    val nextMode = modes[(currentIndex + 1) % modes.size]
                    fileBrowserState = fileBrowserState.copy(sortMode = nextMode)
                    Log.d("Navigation", "R+UP: Sort mode changed to $nextMode")
                } else {
                    val (newScreen, newCol) = trackerController.navigateUp(trackerController.currentScreen, trackerController.previousColumn)
                    if (newScreen != trackerController.currentScreen) {
                        // Screen changed - reset cursor to default position and exit selection mode
                        val (defaultRow, defaultCol) = trackerController.getDefaultCursorPosition(newScreen)
                        trackerController.cursorRow = defaultRow
                        trackerController.cursorColumn = defaultCol
                        trackerController.inputController.exitSelectionMode()
                    }
                    trackerController.currentScreen = newScreen
                    trackerController.previousColumn = newCol
                }
            },

            onRDown = {
                // R+DOWN: Navigate to screen below in 5×5 grid OR cycle sort mode down in FILE_BROWSER
                if (trackerController.currentScreen == ScreenType.FILE_BROWSER) {
                    // File browser: cycle sort mode down
                    val modes = FileSortMode.values()
                    val currentIndex = modes.indexOf(fileBrowserState.sortMode)
                    val prevMode = modes[(currentIndex - 1 + modes.size) % modes.size]
                    fileBrowserState = fileBrowserState.copy(sortMode = prevMode)
                    Log.d("Navigation", "R+DOWN: Sort mode changed to $prevMode")
                } else {
                    val (newScreen, newCol) = trackerController.navigateDown(trackerController.currentScreen, trackerController.previousColumn)
                    if (newScreen != trackerController.currentScreen) {
                        // Screen changed - reset cursor to default position and exit selection mode
                        val (defaultRow, defaultCol) = trackerController.getDefaultCursorPosition(newScreen)
                        trackerController.cursorRow = defaultRow
                        trackerController.cursorColumn = defaultCol
                        trackerController.inputController.exitSelectionMode()
                    }
                    trackerController.currentScreen = newScreen
                    trackerController.previousColumn = newCol
                }
            },

            onRLeft = {
                // R+LEFT: Navigate to screen on left OR go to parent folder in FILE_BROWSER
                if (trackerController.currentScreen == ScreenType.FILE_BROWSER) {
                    // File browser: navigate to parent folder
                    fileBrowserState = fileBrowserModule.navigateToParent(fileBrowserState)
                    Log.d("Navigation", "R+LEFT: Navigated to parent: ${fileBrowserState.currentDirectory.absolutePath}")
                } else {
                    val (newScreen, newCol) = trackerController.navigateLeft(trackerController.currentScreen, trackerController.previousColumn)
                    if (newScreen != trackerController.currentScreen) {
                        // Capture cursor value from current screen before leaving
                        when (trackerController.currentScreen) {
                            ScreenType.PHRASE -> {
                                // Leaving PHRASE: remember instrument under cursor if not empty
                                val phraseState = PhraseEditorState(
                                    trackerController.project.phrases[trackerController.currentPhrase],
                                    trackerController.cursorRow,
                                    trackerController.cursorColumn,
                                    playbackRow = 0,
                                    isPlaying = trackerController.isPlaying()
                                )
                                val context = phraseEditorModule.getCursorContext(phraseState)
                                if (!context.capabilities.isEmpty) {
                                    val step = trackerController.project.phrases[trackerController.currentPhrase].steps[trackerController.cursorRow]
                                    trackerController.lastEditedInstrument = step.instrument
                                }
                            }
                            ScreenType.CHAIN -> {
                                // Leaving CHAIN: remember phrase under cursor
                                val phraseRef = trackerController.project.chains[trackerController.currentChain].phraseRefs[trackerController.cursorRow]
                                if (phraseRef >= 0) {
                                    trackerController.lastEditedPhrase = phraseRef
                                }
                            }
                            ScreenType.SONG -> {
                                // Leaving SONG: remember chain under cursor
                                val track = trackerController.project.tracks[trackerController.cursorColumn - 1]
                                if (trackerController.cursorRow < track.chainRefs.size && track.chainRefs[trackerController.cursorRow] >= 0) {
                                    trackerController.lastEditedChain = track.chainRefs[trackerController.cursorRow]
                                }
                            }
                            else -> {}
                        }

                        // Sync cursor state based on which screen we're entering
                        when (newScreen) {
                            ScreenType.PHRASE -> {
                                // Entering PHRASE from CHAIN/INSTRUMENT: sync to last phrase
                                trackerController.currentPhrase = trackerController.lastEditedPhrase
                            }
                            ScreenType.CHAIN -> {
                                // Entering CHAIN from PHRASE/SONG: sync to last chain
                                trackerController.currentChain = trackerController.lastEditedChain
                            }
                            ScreenType.INSTRUMENT -> {
                                // Entering INSTRUMENT from PHRASE: sync to last instrument
                                trackerController.currentInstrument = trackerController.lastEditedInstrument
                            }
                            else -> {}
                        }

                        // Screen changed - reset cursor to default position and exit selection mode
                        val (defaultRow, defaultCol) = trackerController.getDefaultCursorPosition(newScreen)
                        trackerController.cursorRow = defaultRow
                        trackerController.cursorColumn = defaultCol
                        trackerController.inputController.exitSelectionMode()
                    }
                    trackerController.currentScreen = newScreen
                    trackerController.previousColumn = newCol
                }
            },

            onRRight = {
                // R+RIGHT: Navigate to screen on right in main row (disabled in FILE_BROWSER)
                // Read directly from trackerController to avoid stale captured values
                if (trackerController.currentScreen != ScreenType.FILE_BROWSER) {
                    val (newScreen, newCol) = trackerController.navigateRight(trackerController.currentScreen, trackerController.previousColumn)
                    if (newScreen != trackerController.currentScreen) {
                        // Capture cursor value from current screen before leaving
                        when (trackerController.currentScreen) {
                            ScreenType.PHRASE -> {
                                // Leaving PHRASE: remember instrument under cursor if not empty
                                val phraseState = PhraseEditorState(
                                    trackerController.project.phrases[trackerController.currentPhrase],
                                    trackerController.cursorRow,
                                    trackerController.cursorColumn,
                                    playbackRow = 0,
                                    isPlaying = trackerController.isPlaying()
                                )
                                val context = phraseEditorModule.getCursorContext(phraseState)
                                if (!context.capabilities.isEmpty) {
                                    val step = trackerController.project.phrases[trackerController.currentPhrase].steps[trackerController.cursorRow]
                                    trackerController.lastEditedInstrument = step.instrument
                                }
                            }
                            ScreenType.CHAIN -> {
                                // Leaving CHAIN: remember phrase under cursor
                                val phraseRef = trackerController.project.chains[trackerController.currentChain].phraseRefs[trackerController.cursorRow]
                                if (phraseRef >= 0) {
                                    trackerController.lastEditedPhrase = phraseRef
                                }
                            }
                            ScreenType.SONG -> {
                                // Leaving SONG: remember chain under cursor
                                val track = trackerController.project.tracks[trackerController.cursorColumn - 1]
                                if (trackerController.cursorRow < track.chainRefs.size && track.chainRefs[trackerController.cursorRow] >= 0) {
                                    trackerController.lastEditedChain = track.chainRefs[trackerController.cursorRow]
                                }
                            }
                            else -> {}
                        }

                        // Sync cursor state based on which screen we're entering
                        when (newScreen) {
                            ScreenType.INSTRUMENT -> {
                                // Entering INSTRUMENT from PHRASE: sync to last instrument
                                trackerController.currentInstrument = trackerController.lastEditedInstrument
                            }
                            ScreenType.PHRASE -> {
                                // Entering PHRASE from CHAIN: sync to last phrase
                                trackerController.currentPhrase = trackerController.lastEditedPhrase
                            }
                            ScreenType.CHAIN -> {
                                // Entering CHAIN from SONG: sync to last chain
                                trackerController.currentChain = trackerController.lastEditedChain
                            }
                            else -> {}
                        }

                        // Screen changed - reset cursor to default position and exit selection mode
                        val (defaultRow, defaultCol) = trackerController.getDefaultCursorPosition(newScreen)
                        trackerController.cursorRow = defaultRow
                        trackerController.cursorColumn = defaultCol
                        trackerController.inputController.exitSelectionMode()
                    }
                    trackerController.currentScreen = newScreen
                    trackerController.previousColumn = newCol
                }
            },

// ───────────────────────────────────────────────────────────────
// L + DIRECTION COMBINATIONS (Reserved for future use)
// Item cycling moved to B+direction, file browser actions moved to R+direction
// ───────────────────────────────────────────────────────────────
            onLLeft = {
                // L+LEFT: Reserved for future use (selection expand?)
            },

            onLRight = {
                // L+RIGHT: Reserved for future use (selection expand?)
            },

            onLUp = {
                // L+UP: Reserved for future use
            },

            onLDown = {
                // L+DOWN: Reserved for future use
            },

// ─────────────────────────────────────────────────────────────────────
// L+A: Cut (in selection) / Paste (outside selection)
// ─────────────────────────────────────────────────────────────────────
            onLA = {
                when (trackerController.currentScreen) {
                    ScreenType.PHRASE -> {
                        val action = trackerController.inputController.handleSelectA()
                        when (action) {
                            is InputAction.CUT -> {
                                // Cut selection
                                val bounds = trackerController.inputController.getSelectionBounds()
                                if (bounds != null) {
                                    clipboardManager.cutPhraseSteps(
                                        trackerController.project,
                                        trackerController.currentPhrase,
                                        bounds.topLeftRow, bounds.topLeftColumn,
                                        bounds.bottomRightRow, bounds.bottomRightColumn
                                    )
                                    trackerController.projectVersion++
                                    trackerController.inputController.exitSelectionMode()
                                    Log.d("CopyPaste", "Cut phrase selection")
                                }
                            }
                            is InputAction.PASTE -> {
                                // Paste at cursor
                                val result = clipboardManager.paste(
                                    trackerController.project,
                                    "PHRASE",
                                    trackerController.currentPhrase,
                                    trackerController.cursorRow,
                                    trackerController.cursorColumn
                                )
                                if (result is ClipboardManager.PasteResult.Success) {
                                    trackerController.projectVersion++
                                    Log.d("CopyPaste", "Pasted ${result.itemsPasted} items to phrase")
                                }
                            }
                            else -> { }
                        }
                    }
                    ScreenType.CHAIN -> {
                        val action = trackerController.inputController.handleSelectA()
                        when (action) {
                            is InputAction.CUT -> {
                                // Cut selection
                                val bounds = trackerController.inputController.getSelectionBounds()
                                if (bounds != null) {
                                    clipboardManager.cutChainRows(
                                        trackerController.project,
                                        trackerController.currentChain,
                                        bounds.topLeftRow, bounds.topLeftColumn,
                                        bounds.bottomRightRow, bounds.bottomRightColumn
                                    )
                                    trackerController.projectVersion++
                                    trackerController.inputController.exitSelectionMode()
                                    Log.d("CopyPaste", "Cut chain selection")
                                }
                            }
                            is InputAction.PASTE -> {
                                // Paste at cursor
                                val result = clipboardManager.paste(
                                    trackerController.project,
                                    "CHAIN",
                                    trackerController.currentChain,
                                    trackerController.cursorRow,
                                    trackerController.cursorColumn
                                )
                                if (result is ClipboardManager.PasteResult.Success) {
                                    trackerController.projectVersion++
                                    Log.d("CopyPaste", "Pasted ${result.itemsPasted} items to chain")
                                }
                            }
                            else -> { }
                        }
                    }
                    ScreenType.SONG -> {
                        val action = trackerController.inputController.handleSelectA()
                        when (action) {
                            is InputAction.CUT -> {
                                // Cut selection
                                val bounds = trackerController.inputController.getSelectionBounds()
                                if (bounds != null) {
                                    clipboardManager.cutSongCells(
                                        trackerController.project,
                                        bounds.topLeftRow, bounds.topLeftColumn,
                                        bounds.bottomRightRow, bounds.bottomRightColumn
                                    )
                                    trackerController.projectVersion++
                                    trackerController.inputController.exitSelectionMode()
                                    Log.d("CopyPaste", "Cut song selection")
                                }
                            }
                            is InputAction.PASTE -> {
                                // Paste at cursor
                                val result = clipboardManager.paste(
                                    trackerController.project,
                                    "SONG",
                                    0,  // Not used for song paste
                                    trackerController.cursorRow,
                                    trackerController.cursorColumn
                                )
                                if (result is ClipboardManager.PasteResult.Success) {
                                    trackerController.projectVersion++
                                    Log.d("CopyPaste", "Pasted ${result.itemsPasted} items to song")
                                }
                            }
                            else -> { }
                        }
                    }
                    else -> { }
                }
            },

// ─────────────────────────────────────────────────────────────────────
// L+B: Enter/cycle selection mode (CELL → ROW → SCREEN)
// ─────────────────────────────────────────────────────────────────────
            onLB = {
                when (trackerController.currentScreen) {
                    ScreenType.PHRASE -> {
                        // Enter/cycle selection mode (CELL → ROW → SCREEN)
                        trackerController.inputController.handleSelectB(
                            trackerController.cursorRow,
                            trackerController.cursorColumn,
                            9  // Max column for phrase (FX3 value)
                        )
                        Log.d("CopyPaste", "Selection: ${trackerController.inputController.getSelectionInfo()}")
                    }
                    ScreenType.CHAIN -> {
                        // Enter/cycle selection mode
                        trackerController.inputController.handleSelectB(
                            trackerController.cursorRow,
                            trackerController.cursorColumn,
                            2  // Max column for chain (transpose)
                        )
                        Log.d("CopyPaste", "Selection: ${trackerController.inputController.getSelectionInfo()}")
                    }
                    ScreenType.SONG -> {
                        // Enter/cycle selection mode
                        trackerController.inputController.handleSelectB(
                            trackerController.cursorRow,
                            trackerController.cursorColumn,
                            8  // Max column for song (track 8)
                        )
                        Log.d("CopyPaste", "Selection: ${trackerController.inputController.getSelectionInfo()}")
                    }
                    else -> { }
                }
            },

// ─────────────────────────────────────────────────────────────────────
// SELECT+A: Rename file/folder (file browser only)
// ─────────────────────────────────────────────────────────────────────
            onSelectA = {
                if (trackerController.currentScreen == ScreenType.FILE_BROWSER &&
                    fileBrowserState.mode == FileBrowserModule.BrowserMode.NORMAL) {
                    val item = fileBrowserState.items.getOrNull(fileBrowserState.cursor)
                    if (item != null && item !is FileBrowserModule.BrowserItem.Parent) {
                        fileBrowserState = fileBrowserState.copy(
                            mode = FileBrowserModule.BrowserMode.RENAME,
                            renameBuffer = item.file.nameWithoutExtension,
                            renameCursor = 0,
                            statusMessage = "",
                            statusSuccess = true
                        )
                    }
                }
            },

// ─────────────────────────────────────────────────────────────────────
// SELECT+B: Delete file/folder (file browser only)
// ─────────────────────────────────────────────────────────────────────
            onSelectB = {
                if (trackerController.currentScreen == ScreenType.FILE_BROWSER &&
                    fileBrowserState.mode == FileBrowserModule.BrowserMode.NORMAL) {
                    val item = fileBrowserState.items.getOrNull(fileBrowserState.cursor)
                    if (item != null && item !is FileBrowserModule.BrowserItem.Parent) {
                        fileBrowserState = fileBrowserState.copy(
                            mode = FileBrowserModule.BrowserMode.DELETE,
                            statusMessage = "",
                            statusSuccess = true
                        )
                    }
                }
            },

// ─────────────────────────────────────────────────────────────────────
// SELECT+R: Create new folder
// ─────────────────────────────────────────────────────────────────────
            onSelectR = {
                if (trackerController.currentScreen == ScreenType.FILE_BROWSER &&
                    fileBrowserState.mode == FileBrowserModule.BrowserMode.NORMAL) {
                    fileBrowserState = fileBrowserState.copy(
                        mode = FileBrowserModule.BrowserMode.CREATE,
                        renameBuffer = "NEWFOLDER",
                        renameCursor = 0,
                        statusMessage = "",
                        statusSuccess = true
                    )
                }
            }
        )
    }

// ═══════════════════════════════════════════════════════════════════════
// KEYBOARD INPUT MAPPING
// ═══════════════════════════════════════════════════════════════════════

// Create the input mapper to handle keyboard input
// This maps WASD/JK/UI/Shift/Space to game buttons
    val inputMapper = remember(buttonHandlers) {
        InputMapper(buttonHandlers, logInput = true)  // Enable logging for debugging
    }

// Focus requester to auto-focus the app for keyboard input
    val focusRequester = remember { FocusRequester() }

    //Auto-focus when app starts so keyboard works immediately
    LaunchedEffect(Unit) {
        // Small delay to ensure view is ready before requesting focus
        kotlinx.coroutines.delay(100)
        try {
            focusRequester.requestFocus()
        } catch (e: Exception) {
            // Ignore focus request errors
        }
    }

// ═══════════════════════════════════════════════════════════════════════
// CHOOSE LAYOUT BASED ON DEVICE DETECTION
// ═══════════════════════════════════════════════════════════════════════

// This is where the magic happens!
// Based on what DeviceAdapter detected, we show different layouts:

// Re-read controller properties when stateVersion changes
// This ensures layout functions recompose when controller state changes
    val isPlaying = stateVersion.let { trackerController.isPlaying() }
    val currentInstrument = stateVersion.let { trackerController.currentInstrument }
    val instrumentCursorRow = stateVersion.let { trackerController.instrumentCursorRow }
    val instrumentCursorColumn = stateVersion.let { trackerController.instrumentCursorColumn }
    val instrumentStatusMessage = stateVersion.let { trackerController.statusMessage }
    val instrumentStatusSuccess = stateVersion.let { trackerController.statusSuccess }

    // Selection/clipboard state for copy/paste
    val selectionInfo = stateVersion.let { trackerController.inputController.getSelectionInfo() }
    val clipboardInfo = stateVersion.let { clipboardManager.getClipboardInfo() }
    val selectionModeActive = stateVersion.let { trackerController.inputController.selectionMode }
    val isCellSelectedFn: (Int, Int) -> Boolean = { row, col ->
        trackerController.inputController.isCellSelected(row, col)
    }

    if (layoutConfig.needsVirtualButtons) {
        // SCENARIOS 2/3: Touchscreen devices need virtual buttons

        if (layoutConfig.isLandscape) {
            // LANDSCAPE: Buttons on left and right sides
            LandscapeLayoutWithVirtualButtons(
                layoutConfig = layoutConfig,
                currentScreen = currentScreen,
                project = project,
                audioEngine = audioEngine,
                cursorRow = cursorRow,
                cursorColumn = cursorColumn,
                isPlaying = isPlaying,
                previousColumn = previousColumn,
                currentChain = currentChain,
                currentPhrase = currentPhrase,
                projectCursorRow = projectCursorRow,
                projectCursorColumn = projectCursorColumn,
                projectStatusMessage = projectStatusMessage,
                projectStatusSuccess = projectStatusSuccess,
                buttonHandlers = buttonHandlers,
                inputMapper = inputMapper,
                focusRequester = focusRequester,
                projectVersion = projectVersion,
                currentInstrument = currentInstrument,
                instrumentCursorRow = instrumentCursorRow,
                instrumentCursorColumn = instrumentCursorColumn,
                instrumentStatusMessage = instrumentStatusMessage,
                instrumentStatusSuccess = instrumentStatusSuccess,
                fileBrowserState = fileBrowserState,
                playbackController = playbackController,
                selectionInfo = selectionInfo,
                clipboardInfo = clipboardInfo,
                selectionMode = selectionModeActive,
                isCellSelected = isCellSelectedFn,
                mixerCursorColumn = trackerController.mixerCursorColumn,
                trackPeaks = trackPeakBuffer,
                masterPeaks = masterPeakBuffer
            )
        } else {
            // PORTRAIT: Buttons below screen
            PortraitLayoutWithVirtualButtons(
                layoutConfig = layoutConfig,
                currentScreen = currentScreen,
                project = project,
                audioEngine = audioEngine,
                cursorRow = cursorRow,
                cursorColumn = cursorColumn,
                isPlaying = isPlaying,
                previousColumn = previousColumn,
                currentChain = currentChain,
                currentPhrase = currentPhrase,
                projectCursorRow = projectCursorRow,
                projectCursorColumn = projectCursorColumn,
                projectStatusMessage = projectStatusMessage,
                projectStatusSuccess = projectStatusSuccess,
                buttonHandlers = buttonHandlers,
                inputMapper = inputMapper,
                focusRequester = focusRequester,
                projectVersion = projectVersion,
                currentInstrument = currentInstrument,
                instrumentCursorRow = instrumentCursorRow,
                instrumentCursorColumn = instrumentCursorColumn,
                instrumentStatusMessage = instrumentStatusMessage,
                instrumentStatusSuccess = instrumentStatusSuccess,
                fileBrowserState = fileBrowserState,
                playbackController = playbackController,
                selectionInfo = selectionInfo,
                clipboardInfo = clipboardInfo,
                selectionMode = selectionModeActive,
                isCellSelected = isCellSelectedFn,
                mixerCursorColumn = trackerController.mixerCursorColumn,
                trackPeaks = trackPeakBuffer,
                masterPeaks = masterPeakBuffer
            )
        }
    } else {
        // SCENARIO 1: Gaming handheld with physical buttons
        // Full screen, NO virtual buttons!
        FullScreenLayout(
            layoutConfig = layoutConfig,
            currentScreen = currentScreen,
            project = project,
            audioEngine = audioEngine,
            cursorRow = cursorRow,
            cursorColumn = cursorColumn,
            isPlaying = isPlaying,
            previousColumn = previousColumn,
            currentChain = currentChain,
            currentPhrase = currentPhrase,
            projectCursorRow = projectCursorRow,
            projectCursorColumn = projectCursorColumn,
            projectStatusMessage = projectStatusMessage,
            projectStatusSuccess = projectStatusSuccess,
            inputMapper = inputMapper,
            focusRequester = focusRequester,
            projectVersion = projectVersion,
            currentInstrument = currentInstrument,
            instrumentCursorRow = instrumentCursorRow,
            instrumentCursorColumn = instrumentCursorColumn,
            instrumentStatusMessage = instrumentStatusMessage,
            instrumentStatusSuccess = instrumentStatusSuccess,
            fileBrowserState = fileBrowserState,
            playbackController = playbackController,
            selectionInfo = selectionInfo,
            clipboardInfo = clipboardInfo,
            selectionMode = selectionModeActive,
            isCellSelected = isCellSelectedFn,
            mixerCursorColumn = trackerController.mixerCursorColumn,
            trackPeaks = trackPeakBuffer,
            masterPeaks = masterPeakBuffer
        )
    }
}

// ═══════════════════════════════════════════════════════════════════════════
// NAVIGATION HELPER FUNCTIONS
// ═══════════════════════════════════════════════════════════════════════════
// NOTE: Navigation functions migrated to TrackerController (Phase 4 cleanup - January 2025)
// All navigation logic now in core/logic/TrackerController.kt

// ═══════════════════════════════════════════════════════════════════════════
// EDITOR HELPER FUNCTIONS
// ═══════════════════════════════════════════════════════════════════════════
// NOTE: Editor helpers migrated to EditorHelpers.kt (Phase 4 cleanup - January 2025)
// All phrase/chain/song/project editing helpers now in EditorHelpers.kt