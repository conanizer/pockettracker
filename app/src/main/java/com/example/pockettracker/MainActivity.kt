package com.example.pockettracker

import android.Manifest
import android.content.pm.PackageManager
import android.os.Build
import android.os.Bundle
import android.util.Log
import androidx.activity.ComponentActivity
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.compose.setContent
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.foundation.background
import androidx.compose.foundation.layout.*
import androidx.compose.material3.Button
import androidx.compose.material3.Surface
import androidx.compose.material3.Text
import androidx.compose.runtime.*
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import androidx.core.content.ContextCompat
import com.example.pockettracker.ui.theme.PockettrackerTheme
import androidx.compose.ui.unit.sp
import androidx.compose.foundation.focusable
import androidx.compose.ui.focus.FocusRequester
import androidx.compose.ui.focus.focusRequester

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

        // Step 1: Detect device type and calculate layout
        // This tells us if we need virtual buttons or not
        val deviceAdapter = DeviceAdapter(this)
        val layout = deviceAdapter.calculateLayout()

        // Step 2: Log the detection results so we can see what was detected
        Log.d("DeviceAdapter", "=== DEVICE DETECTION ===")
        Log.d("DeviceAdapter", deviceAdapter.getConfigDescription(layout))
        Log.d("DeviceAdapter", "======================")

        // Step 3: Set up the UI
        setContent {
            // This is Jetpack Compose - it defines what appears on screen
            PockettrackerTheme {
                Surface(
                    modifier = Modifier.fillMaxSize(),
                    color = Color.Black
                ) {
                    // Pass the layout config to our app!
                    // This is the KEY - we're telling PocketTrackerApp what kind of device we have
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
    }

    // ═══════════════════════════════════════════════════════════════════════
    // CREATE MANAGERS
    // ═══════════════════════════════════════════════════════════════════════

    // FileManager: Handles saving/loading projects
    // "remember" means "keep this object alive across recompositions"
    val fileManager = remember { FileManager(context) }

    // TrackerAudioEngine: Handles all audio playback
    // "apply { create() }" means "after creating, call the create() method"
    val audioEngine = remember {
        TrackerAudioEngine(context).apply {
            create()
        }
    }

    // GenericInputHandler: Handles button presses based on cursor context
    val genericInputHandler = remember { GenericInputHandler() }

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

    // ═══════════════════════════════════════════════════════════════════════
    // STATE VARIABLES
    // ═══════════════════════════════════════════════════════════════════════

    // State variables hold data that can change over time
    // When state changes, Compose automatically redraws the UI
    // "by remember { mutableStateOf(...) }" means "create a state variable"

    // Create a test project with some notes
    var project by remember {
        mutableStateOf(Project().apply {
            // Add some test notes to phrase 0
            phrases[0].steps[0].note = Note.fromString("C-4")
            phrases[0].steps[0].instrument = 3
            phrases[0].steps[4].note = Note.fromString("E-4")
            phrases[0].steps[4].instrument = 3
            phrases[0].steps[8].note = Note.fromString("G-4")
            phrases[0].steps[8].instrument = 3
            phrases[0].steps[12].note = Note.fromString("C-5")
            phrases[0].steps[12].instrument = 3
        })
    }

    // Navigation state - where we are in the app
    var currentScreen by remember { mutableStateOf(ScreenType.PHRASE) }
    var previousColumn by remember { mutableIntStateOf(getScreenColumn(ScreenType.PHRASE)) }

    // Cursor position - where the cursor is on screen
    var cursorRow by remember { mutableIntStateOf(0) }
    var cursorColumn by remember { mutableIntStateOf(1) }

    // Project screen specific state
    var projectCursorRow by remember { mutableIntStateOf(0) }
    var projectCursorColumn by remember { mutableIntStateOf(1) }
    var projectStatusMessage by remember { mutableStateOf("") }
    var projectStatusSuccess by remember { mutableStateOf(true) }

    // Instrument screen specific state
    var currentInstrument by remember { mutableIntStateOf(0) }
    var instrumentCursorRow by remember { mutableIntStateOf(0) }
    var instrumentCursorColumn by remember { mutableIntStateOf(1) }
    var instrumentStatusMessage by remember { mutableStateOf("") }
    var instrumentStatusSuccess by remember { mutableStateOf(true) }

    // Auto-dismiss status messages after 5 seconds
    // NOTE: All screen status messages use the same 5-second auto-dismiss behavior
    // to maintain consistency across the app (PROJECT, INSTRUMENT, etc.)
    LaunchedEffect(projectStatusMessage) {
        if (projectStatusMessage.isNotEmpty()) {
            kotlinx.coroutines.delay(5000)  // Wait 5 seconds
            projectStatusMessage = ""  // Clear message
        }
    }

    LaunchedEffect(instrumentStatusMessage) {
        if (instrumentStatusMessage.isNotEmpty()) {
            kotlinx.coroutines.delay(5000)  // Wait 5 seconds
            instrumentStatusMessage = ""  // Clear message
        }
    }

    // Playback and control state
    var isPlaying by remember { mutableStateOf(false) }

    // Which chain/phrase we're editing
    var currentChain by remember { mutableIntStateOf(0) }
    var currentPhrase by remember { mutableIntStateOf(0) }

    // Remember last edited values (for quick insertion)
    var lastEditedPhrase by remember { mutableIntStateOf(0) }
    var lastEditedChain by remember { mutableIntStateOf(0) }

    // Version counter to force recomposition when nested project data changes
    // Incrementing this tells Compose the project has changed even if the reference is the same
    var projectVersion by remember { mutableIntStateOf(0) }

    // File browser module and state
    val fileBrowserModule = remember { FileBrowserModule() }
    var fileBrowserState by remember {
        mutableStateOf(
            FileBrowserModule.State(
                currentDirectory = fileManager.getProjectsDirectory(),
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

    // Update lastEditedPhrase when cursor moves in chain screen
    // This runs every time cursorRow or currentChain changes
    lastEditedPhrase = project.chains[currentChain].phraseRefs[cursorRow]

    // Clean up audio engine when app closes
    // "DisposableEffect" runs code when the composable is removed from screen
    DisposableEffect(Unit) {
        onDispose {
            audioEngine.destroy()
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
    fun applyPhraseInputAction(
        action: InputAction,
        phraseIndex: Int,
        row: Int,
        column: Int
    ) {
        Log.d("PhraseInputAction", "phrase=$phraseIndex row=$row col=$column action=$action")
        val step = project.phrases[phraseIndex].steps[row]

        when (action) {
            is InputAction.SET_VALUE -> {
                when (column) {
                    1 -> {
                        // Note column: Convert MIDI value back to Note
                        step.note = Note.fromMidi(action.value)
                    }
                    2 -> {
                        // Volume column
                        step.volume = action.value
                    }
                    3 -> {
                        // Instrument column
                        step.instrument = action.value
                    }
                }
            }
            is InputAction.DELETE -> {
                when (column) {
                    1 -> {
                        // Clear note
                        step.note = Note.EMPTY
                    }
                }
            }
            is InputAction.INSERT_DEFAULT -> {
                // Insert default note (C-4)
                if (column == 1) {
                    step.note = Note.fromString("C-4")
                }
            }
            else -> { /* NONE or unhandled - do nothing */ }
        }

        // Trigger recomposition by incrementing version counter
        projectVersion++
        Log.d("PhraseInputAction", "projectVersion incremented to $projectVersion")
    }

    /**
     * Apply InputAction to chain step
     *
     * Handles value changes for chain editing (phrase refs and transpose)
     */
    fun applyChainInputAction(
        action: InputAction,
        chainIndex: Int,
        row: Int,
        column: Int
    ) {
        Log.d("ChainInputAction", "chain=$chainIndex row=$row col=$column action=$action")
        val chain = project.chains[chainIndex]

        when (action) {
            is InputAction.SET_VALUE -> {
                when (column) {
                    1 -> {
                        // Phrase reference column
                        chain.phraseRefs[row] = action.value
                    }
                    2 -> {
                        // Transpose column
                        chain.transposeValues[row] = action.value
                    }
                }
            }
            is InputAction.DELETE -> {
                when (column) {
                    1 -> {
                        // Clear phrase reference
                        chain.phraseRefs[row] = 0xFF  // Empty
                        chain.transposeValues[row] = 0x00  // Reset transpose to default
                    }
                }
            }
            is InputAction.INSERT_DEFAULT -> {
                if (column == 1) {
                    // Insert phrase 0 by default
                    chain.phraseRefs[row] = 0
                    chain.transposeValues[row] = 0x00  // Default transpose
                }
            }
            else -> { /* NONE or unhandled - do nothing */ }
        }

        // Trigger recomposition
        projectVersion++
        Log.d("ChainInputAction", "projectVersion incremented to $projectVersion")
    }

    /**
     * Apply InputAction to song track
     *
     * Handles value changes for song editing (chain references)
     */
    fun applySongInputAction(
        action: InputAction,
        trackIndex: Int,
        row: Int
    ) {
        Log.d("SongInputAction", "track=$trackIndex row=$row action=$action")
        val track = project.tracks[trackIndex]

        when (action) {
            is InputAction.SET_VALUE -> {
                // Ensure track is long enough
                while (track.chainRefs.size <= row) {
                    track.chainRefs.add(-1)
                }
                track.chainRefs[row] = action.value
            }
            is InputAction.DELETE -> {
                // Clear chain reference
                if (row < track.chainRefs.size) {
                    track.chainRefs[row] = -1
                }
            }
            is InputAction.INSERT_DEFAULT -> {
                // Insert chain 0 by default
                while (track.chainRefs.size <= row) {
                    track.chainRefs.add(-1)
                }
                track.chainRefs[row] = 0
            }
            else -> { /* NONE or unhandled - do nothing */ }
        }

        // Trigger recomposition
        projectVersion++
        Log.d("SongInputAction", "projectVersion incremented to $projectVersion")
    }

    /**
     * Apply InputAction to project settings
     *
     * Handles value changes for project screen (tempo, transpose, name)
     */
    fun applyProjectInputAction(
        action: InputAction,
        row: Int,
        column: Int
    ) {
        Log.d("ProjectInputAction", "row=$row col=$column action=$action")

        when (row) {
            0 -> {
                // TEMPO row
                when (action) {
                    is InputAction.SET_VALUE -> {
                        project.tempo = action.value.coerceIn(20, 999)
                    }
                    else -> { /* Other actions not applicable */ }
                }
            }
            1 -> {
                // TRANSPOSE row
                when (action) {
                    is InputAction.SET_VALUE -> {
                        project.transpose = action.value.coerceIn(0, 255)
                    }
                    else -> { /* Other actions not applicable */ }
                }
            }
            2 -> {
                // NAME row - per-character editing
                val charIndex = column - 1
                if (charIndex < 0 || charIndex >= 12) return

                when (action) {
                    is InputAction.SET_VALUE -> {
                        // Set character at position
                        val char = action.value.toChar()
                        val sb = StringBuilder(project.name.padEnd(12, ' '))
                        sb.setCharAt(charIndex, char)
                        project.name = sb.toString().trimEnd()  // Remove trailing spaces
                    }
                    is InputAction.DELETE -> {
                        // Delete character (replace with space)
                        if (charIndex < project.name.length) {
                            val sb = StringBuilder(project.name.padEnd(12, ' '))
                            sb.setCharAt(charIndex, ' ')
                            project.name = sb.toString().trimEnd()
                        }
                    }
                    else -> { /* Other actions not applicable */ }
                }
            }
            3 -> {
                // PROJECT row (LOAD/SAVE/NEW) - handled elsewhere
            }
        }

        // Trigger recomposition
        projectVersion++
        Log.d("ProjectInputAction", "projectVersion incremented to $projectVersion")
    }

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
     * Apply input action to instrument screen
     * Handles all instrument parameter editing
     */
    fun applyInstrumentInputAction(action: InputAction, row: Int, column: Int) {
        Log.d("InstrumentInputAction", "inst=$currentInstrument row=$row col=$column action=$action")
        val instrument = project.instruments[currentInstrument]

        when (row) {
            0 -> {
                // TYPE row - read-only for now (TODO: A+DPAD to change type)
            }
            1 -> {
                // LOAD row - handled as button action, not value editing
            }
            2 -> {
                // NAME row - read-only (shows loaded sample filename)
            }
            3 -> {
                // ROOT note
                when (action) {
                    is InputAction.SET_VALUE -> {
                        instrument.root = Note.fromMidi(action.value)
                        // Update base frequency in audio engine (combines ROOT + DETUNE)
                        audioEngine.updateInstrumentBaseFrequency(instrument)
                    }
                    is InputAction.DELETE -> {
                        instrument.root = Note.fromString("C-4")
                        // Update base frequency in audio engine (combines ROOT + DETUNE)
                        audioEngine.updateInstrumentBaseFrequency(instrument)
                    }
                    else -> {}
                }
            }
            4 -> {
                // DETUNE (00-FF hex)
                when (action) {
                    is InputAction.SET_VALUE -> {
                        instrument.detune = action.value.coerceIn(0, 255)
                        // Update base frequency in audio engine (combines ROOT + DETUNE)
                        audioEngine.updateInstrumentBaseFrequency(instrument)
                    }
                    else -> {}
                }
            }
            5 -> {
                // SPACER row - no editing
            }
            6 -> {
                // START (sample start point)
                when (action) {
                    is InputAction.SET_VALUE -> {
                        instrument.sampleStart = action.value.coerceIn(0, 255)
                        // Update playback parameters in audio engine
                        audioEngine.updateInstrumentPlaybackParams(instrument)
                    }
                    else -> {}
                }
            }
            7 -> {
                // END (sample end point)
                when (action) {
                    is InputAction.SET_VALUE -> {
                        instrument.sampleEnd = action.value.coerceIn(0, 255)
                        // Update playback parameters in audio engine
                        audioEngine.updateInstrumentPlaybackParams(instrument)
                    }
                    else -> {}
                }
            }
            8 -> {
                // REV (reverse: off/on)
                when (action) {
                    is InputAction.SET_VALUE -> {
                        instrument.reverse = action.value == 1
                        // Update playback parameters in audio engine
                        audioEngine.updateInstrumentPlaybackParams(instrument)
                    }
                    else -> {}
                }
            }
            9 -> {
                // LOOP mode (off/fwd/png)
                when (action) {
                    is InputAction.SET_VALUE -> {
                        val loopModes = listOf("off", "fwd", "png")
                        if (action.value in 0..2) {
                            instrument.loopMode = loopModes[action.value]
                        }
                        // Update playback parameters in audio engine
                        audioEngine.updateInstrumentPlaybackParams(instrument)
                    }
                    else -> {}
                }
            }
            10 -> {
                // LOOP ST (loop start point)
                when (action) {
                    is InputAction.SET_VALUE -> {
                        instrument.loopStart = action.value.coerceIn(0, 255)
                        // Update playback parameters in audio engine
                        audioEngine.updateInstrumentPlaybackParams(instrument)
                    }
                    else -> {}
                }
            }
        }

        // Trigger recomposition
        projectVersion++
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
     *   onAUp = { handleGenericInput { ctx -> genericInputHandler.handleAButton(ctx) } }
     *   onADown = { handleGenericInput { ctx -> genericInputHandler.handleBButton(ctx) } }
     *
     * @param handlerFunction Lambda that takes CursorContext and returns InputAction
     */
    fun handleGenericInput(handlerFunction: (CursorContext) -> InputAction) {
        when (currentScreen) {
            ScreenType.CHAIN -> {
                val chainState = ChainEditorState(
                    project.chains[currentChain],
                    cursorRow,
                    cursorColumn
                )
                val context = chainEditorModule.getCursorContext(chainState)
                val action = handlerFunction(context)
                applyChainInputAction(action, currentChain, cursorRow, cursorColumn)
            }
            ScreenType.PHRASE -> {
                val phraseState = PhraseEditorState(
                    project.phrases[currentPhrase],
                    cursorRow,
                    cursorColumn,
                    playbackRow = 0,
                    isPlaying = false
                )
                val context = phraseEditorModule.getCursorContext(phraseState)
                val action = handlerFunction(context)
                applyPhraseInputAction(action, currentPhrase, cursorRow, cursorColumn)
            }
            ScreenType.SONG -> {
                val songState = SongEditorState(
                    project,
                    cursorRow,
                    cursorTrack = cursorColumn
                )
                val context = songEditorModule.getCursorContext(songState)
                val action = handlerFunction(context)
                applySongInputAction(action, cursorColumn - 1, cursorRow)
            }
            ScreenType.PROJECT -> {
                val projectState = ProjectState(
                    project,
                    projectCursorRow,
                    projectCursorColumn,
                    projectStatusMessage,
                    projectStatusSuccess
                )
                val context = projectModule.getCursorContext(projectState)
                val action = handlerFunction(context)
                applyProjectInputAction(action, projectCursorRow, projectCursorColumn)
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
                    project.instruments[currentInstrument],
                    instrumentCursorRow,
                    instrumentCursorColumn,
                    instrumentStatusMessage,
                    instrumentStatusSuccess
                )
                val context = instrumentModule.getCursorContext(instrumentState)
                val action = handlerFunction(context)
                applyInstrumentInputAction(action, instrumentCursorRow, instrumentCursorColumn)
            }
            else -> { /* Other screens not yet implemented */ }
        }
    }

    // ═══════════════════════════════════════════════════════════════════════
    // BUTTON HANDLERS
    // ═══════════════════════════════════════════════════════════════════════

    // Create a ButtonHandlers object that contains all button press logic
    // Remove dependencies - handlers can access current values directly from scope
    // This prevents InputMapper from being recreated and losing modifier states
    val buttonHandlers = remember {
        ButtonHandlers(
            // ───────────────────────────────────────────────────────────────
            // D-PAD UP
            // ───────────────────────────────────────────────────────────────
            onDPadUp = {
                // Move cursor up (R+UP for screen navigation handled by InputMapper)
                when (currentScreen) {
                    ScreenType.PROJECT -> {
                        // Project screen has special cursor handling (rows 0-6)
                        projectCursorRow = if (projectCursorRow > 0) {
                            projectCursorRow - 1
                        } else {
                            6  // Wrap to bottom
                        }
                        projectCursorColumn = 1  // Reset to first value column
                    }
                    ScreenType.INSTRUMENT -> {
                        // Instrument screen has rows 0-10 (row 5 is spacer, skip it)
                        val oldRow = instrumentCursorRow
                        instrumentCursorRow = when {
                            instrumentCursorRow > 0 && instrumentCursorRow != 6 -> instrumentCursorRow - 1
                            instrumentCursorRow == 6 -> 4  // Skip spacer (row 5)
                            else -> 10  // Wrap to bottom
                        }
                        android.util.Log.d("InstrumentCursor", "UP: $oldRow → $instrumentCursorRow")
                        instrumentCursorColumn = 1  // Reset to first value column
                    }
                    ScreenType.FILE_BROWSER -> {
                        // File browser: move cursor up with wrap-around
                        if (fileBrowserState.items.isNotEmpty()) {
                            val newCursor = if (fileBrowserState.cursor > 0) {
                                fileBrowserState.cursor - 1
                            } else {
                                fileBrowserState.items.size - 1  // Wrap to last item
                            }

                            // Auto-scroll if needed (20 visible rows)
                            val newScroll = when {
                                newCursor < fileBrowserState.scroll -> newCursor
                                newCursor >= fileBrowserState.scroll + FileBrowserModule.VISIBLE_ROWS ->
                                    (newCursor - FileBrowserModule.VISIBLE_ROWS + 1).coerceAtLeast(0)
                                else -> fileBrowserState.scroll
                            }

                            fileBrowserState = fileBrowserState.copy(
                                cursor = newCursor,
                                scroll = newScroll
                            )
                        }
                    }
                    else -> {
                        // All other screens: simple cursor movement with wrapping (rows 0-15)
                        cursorRow = if (cursorRow > 0) cursorRow - 1 else 15
                    }
                }
            },

            // ───────────────────────────────────────────────────────────────
            // D-PAD DOWN
            // ───────────────────────────────────────────────────────────────
            onDPadDown = {
                // Move cursor down (R+DOWN for screen navigation handled by InputMapper)
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
                        // Instrument screen has rows 0-10 (row 5 is spacer, skip it)
                        val oldRow = instrumentCursorRow
                        instrumentCursorRow = when {
                            instrumentCursorRow < 10 && instrumentCursorRow != 4 -> instrumentCursorRow + 1
                            instrumentCursorRow == 4 -> 6  // Skip spacer (row 5)
                            else -> 0  // Wrap to top
                        }
                        android.util.Log.d("InstrumentCursor", "DOWN: $oldRow → $instrumentCursorRow")
                        instrumentCursorColumn = 1  // Reset column
                    }
                    ScreenType.FILE_BROWSER -> {
                        // File browser: move cursor down with wrap-around
                        if (fileBrowserState.items.isNotEmpty()) {
                            val newCursor = if (fileBrowserState.cursor < fileBrowserState.items.size - 1) {
                                fileBrowserState.cursor + 1
                            } else {
                                0  // Wrap to first item
                            }

                            // Auto-scroll if needed (20 visible rows)
                            val newScroll = when {
                                newCursor >= fileBrowserState.scroll + FileBrowserModule.VISIBLE_ROWS ->
                                    newCursor - FileBrowserModule.VISIBLE_ROWS + 1
                                newCursor < fileBrowserState.scroll -> newCursor
                                else -> fileBrowserState.scroll
                            }

                            fileBrowserState = fileBrowserState.copy(
                                cursor = newCursor,
                                scroll = newScroll
                            )
                        }
                    }
                    else -> {
                        // Most screens have 16 rows (0-15) with wrapping
                        cursorRow = if (cursorRow < 15) cursorRow + 1 else 0
                    }
                }
            },

            // ───────────────────────────────────────────────────────────────
            // D-PAD LEFT
            // ───────────────────────────────────────────────────────────────
            onDPadLeft = {
                // Move cursor left (R+LEFT for screen navigation handled by InputMapper)
                when (currentScreen) {
                    ScreenType.PROJECT -> {
                        // Project has different column limits per row
                        projectCursorColumn = handleProjectCursorLeft(
                            projectCursorRow,
                            projectCursorColumn
                        )
                    }
                    ScreenType.INSTRUMENT -> {
                        // Instrument: Row 2 (NAME) has columns 1-12, others have only column 1
                        if (instrumentCursorRow == 2) {
                            // NAME row: move between character positions (1-12)
                            if (instrumentCursorColumn > 1) {
                                instrumentCursorColumn--
                            }
                        } else {
                            // Other rows: only columns 0 and 1
                            if (instrumentCursorColumn > 0) {
                                instrumentCursorColumn--
                            }
                        }
                    }
                    ScreenType.FILE_BROWSER -> {
                        when (fileBrowserState.mode) {
                            FileBrowserModule.BrowserMode.RENAME, FileBrowserModule.BrowserMode.CREATE -> {
                                // In text editing mode: move character cursor left
                                if (fileBrowserState.renameCursor > 0) {
                                    fileBrowserState = fileBrowserState.copy(
                                        renameCursor = fileBrowserState.renameCursor - 1
                                    )
                                }
                            }
                            else -> {
                                // Normal/Delete mode: page up (20 items)
                                if (fileBrowserState.items.isNotEmpty()) {
                                    val newCursor = (fileBrowserState.cursor - FileBrowserModule.VISIBLE_ROWS).coerceAtLeast(0)

                                    // Auto-scroll to keep cursor visible
                                    val newScroll = when {
                                        newCursor < fileBrowserState.scroll -> newCursor
                                        newCursor >= fileBrowserState.scroll + FileBrowserModule.VISIBLE_ROWS ->
                                            (newCursor - FileBrowserModule.VISIBLE_ROWS + 1).coerceAtLeast(0)
                                        else -> fileBrowserState.scroll
                                    }

                                    fileBrowserState = fileBrowserState.copy(
                                        cursor = newCursor,
                                        scroll = newScroll
                                    )
                                }
                            }
                        }
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
            },

            // ───────────────────────────────────────────────────────────────
            // D-PAD RIGHT
            // ───────────────────────────────────────────────────────────────
            onDPadRight = {
                // Move cursor right (R+RIGHT for screen navigation handled by InputMapper)
                when (currentScreen) {
                    ScreenType.PROJECT -> {
                        // Project has different column limits per row
                        projectCursorColumn = handleProjectCursorRight(
                            projectCursorRow,
                            projectCursorColumn
                        )
                    }
                    ScreenType.INSTRUMENT -> {
                        // Instrument: Row 2 (NAME) has columns 1-12, others have only column 1
                        if (instrumentCursorRow == 2) {
                            // NAME row: move between character positions (max 12)
                            if (instrumentCursorColumn < 12) {
                                instrumentCursorColumn++
                            }
                        } else {
                            // Other rows: only columns 0 and 1
                            if (instrumentCursorColumn < 1) {
                                instrumentCursorColumn++
                            }
                        }
                    }
                    ScreenType.FILE_BROWSER -> {
                        when (fileBrowserState.mode) {
                            FileBrowserModule.BrowserMode.RENAME, FileBrowserModule.BrowserMode.CREATE -> {
                                // In text editing mode: move character cursor right (max 11 = 12 chars)
                                if (fileBrowserState.renameCursor < 11) {
                                    fileBrowserState = fileBrowserState.copy(
                                        renameCursor = fileBrowserState.renameCursor + 1
                                    )
                                }
                            }
                            else -> {
                                // Normal/Delete mode: page down (20 items)
                                if (fileBrowserState.items.isNotEmpty()) {
                                    val newCursor = (fileBrowserState.cursor + FileBrowserModule.VISIBLE_ROWS)
                                        .coerceAtMost(fileBrowserState.items.size - 1)

                                    // Auto-scroll to keep cursor visible
                                    val newScroll = when {
                                        newCursor >= fileBrowserState.scroll + FileBrowserModule.VISIBLE_ROWS ->
                                            newCursor - FileBrowserModule.VISIBLE_ROWS + 1
                                        newCursor < fileBrowserState.scroll -> newCursor
                                        else -> fileBrowserState.scroll
                                    }

                                    fileBrowserState = fileBrowserState.copy(
                                        cursor = newCursor,
                                        scroll = newScroll
                                    )
                                }
                            }
                        }
                    }
                    else -> {
                        // Get maximum column for this screen
                        val maxColumn = when (currentScreen) {
                            ScreenType.SONG -> 8     // 8 tracks
                            ScreenType.CHAIN -> 2    // PH + TSP columns
                            ScreenType.PHRASE -> 6   // Note + Vol + Inst + FX columns
                            else -> 0
                        }
                        // Move right if not at maximum
                        if (cursorColumn < maxColumn) cursorColumn++
                    }
                }
            },

            // ───────────────────────────────────────────────────────────────
            // BUTTON A - Primary action (insert/increment)
            // ───────────────────────────────────────────────────────────────
            onButtonA = {
                when (currentScreen) {
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
                                                val loaded = fileManager.loadProject(item.file)
                                                if (loaded != null) {
                                                    project = loaded
                                                    projectStatusMessage = "LOADED: ${item.file.nameWithoutExtension}"
                                                    projectStatusSuccess = true
                                                    projectVersion++
                                                    currentScreen = previousScreen
                                                } else {
                                                    fileBrowserState = fileBrowserState.copy(
                                                        statusMessage = "LOAD FAILED",
                                                        statusSuccess = false
                                                    )
                                                }
                                            }
                                            ScreenType.INSTRUMENT -> {
                                                // Load WAV sample file
                                                val success = audioEngine.loadSampleFromFile(
                                                    currentInstrument,
                                                    item.file.absolutePath
                                                )
                                                if (success) {
                                                    // Store file path in instrument and update sample ID
                                                    project.instruments[currentInstrument].sampleFilePath = item.file.absolutePath
                                                    project.instruments[currentInstrument].sampleId = currentInstrument

                                                    // Update base frequency based on ROOT + DETUNE
                                                    audioEngine.updateInstrumentBaseFrequency(project.instruments[currentInstrument])

                                                    // Update playback parameters (start/end/loop/reverse)
                                                    audioEngine.updateInstrumentPlaybackParams(project.instruments[currentInstrument])

                                                    instrumentStatusMessage = "LOADED ${item.file.nameWithoutExtension}"
                                                    instrumentStatusSuccess = true
                                                    projectVersion++
                                                    currentScreen = previousScreen
                                                } else {
                                                    fileBrowserState = fileBrowserState.copy(
                                                        statusMessage = "LOAD FAILED",
                                                        statusSuccess = false
                                                    )
                                                }
                                            }
                                            else -> {
                                                // Unknown previous screen - try loading as project
                                                val loaded = fileManager.loadProject(item.file)
                                                if (loaded != null) {
                                                    project = loaded
                                                    projectVersion++
                                                    currentScreen = previousScreen
                                                } else {
                                                    fileBrowserState = fileBrowserState.copy(
                                                        statusMessage = "LOAD FAILED",
                                                        statusSuccess = false
                                                    )
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
                                    val deleted = fileManager.deleteFileOrFolder(item.file)
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
                        Log.d("ProjectScreen", "A pressed: row=$projectCursorRow, col=$projectCursorColumn")
                        when (projectCursorRow) {
                            // ROW 3: PROJECT actions (LOAD/SAVE/NEW)
                            3 -> {
                                Log.d("ProjectScreen", "Row 3 action: column=$projectCursorColumn")
                                when (projectCursorColumn) {
                                    1 -> {  // LOAD - Show file browser
                                        Log.d("ProjectScreen", "LOAD button pressed - switching to FILE_BROWSER")
                                        previousScreen = currentScreen
                                        currentScreen = ScreenType.FILE_BROWSER
                                        // Reset file browser state with correct directory and extension
                                        fileBrowserState = fileBrowserState.copy(
                                            currentDirectory = fileManager.getProjectsDirectory(),
                                            cursor = 0,
                                            scroll = 0,
                                            mode = FileBrowserModule.BrowserMode.NORMAL,
                                            fileExtension = "ptp",  // Filter for project files
                                            statusMessage = ""
                                        )
                                        Log.d("ProjectScreen", "File browser opened for .ptp files")
                                    }
                                    2 -> {  // SAVE
                                        val success = fileManager.saveProject(project, project.name)
                                        if (success) {
                                            projectStatusMessage = "SAVED"
                                            projectStatusSuccess = true
                                        } else {
                                            projectStatusMessage = "SAVE FAILED"
                                            projectStatusSuccess = false
                                        }
                                    }
                                    3 -> {  // NEW
                                        project = Project()
                                        projectStatusMessage = "NEW PROJECT"
                                        projectStatusSuccess = true
                                    }
                                }
                            }
                            // Rows 4-6 (EXPORT, CLEAN, SYSTEM) - placeholder for now
                        }
                    }

                    // INSTRUMENT: Handle LOAD button
                    ScreenType.INSTRUMENT -> {
                        when (instrumentCursorRow) {
                            1 -> {  // ROW 1: LOAD button
                                if (instrumentCursorColumn == 1) {
                                    val samplesDir = fileManager.getSamplesDirectory()
                                    Log.d("InstrumentScreen", "LOAD button pressed - opening file browser for WAV files")
                                    Log.d("InstrumentScreen", "Samples directory: ${samplesDir.absolutePath}")
                                    Log.d("InstrumentScreen", "Directory exists: ${samplesDir.exists()}")
                                    Log.d("InstrumentScreen", "Directory can read: ${samplesDir.canRead()}")
                                    if (samplesDir.exists()) {
                                        val files = samplesDir.listFiles()
                                        Log.d("InstrumentScreen", "Files in directory: ${files?.size ?: 0}")
                                        files?.forEach { Log.d("InstrumentScreen", "  - ${it.name}") }
                                    }

                                    previousScreen = currentScreen
                                    currentScreen = ScreenType.FILE_BROWSER
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

                    else -> { /* Other screens not implemented yet */ }
                }
            },

            // ───────────────────────────────────────────────────────────────
            // BUTTON B - Secondary action (decrement)
            // ───────────────────────────────────────────────────────────────
            onButtonB = {
                when (currentScreen) {
                    // FILE BROWSER: Cancel operation or go back
                    ScreenType.FILE_BROWSER -> {
                        when (fileBrowserState.mode) {
                            FileBrowserModule.BrowserMode.NORMAL -> {
                                // Go back to previous screen
                                Log.d("FileBrowser", "Returning to $previousScreen")
                                currentScreen = previousScreen
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

                    // SONG SCREEN: Decrement chain reference
                    ScreenType.SONG -> {
                        val trackIndex = getTrackIndex(cursorColumn)
                        val track = project.tracks[trackIndex]

                        // Only edit if not empty
                        if (cursorRow < track.chainRefs.size &&
                            track.chainRefs[cursorRow] != -1) {
                            editSongChainRef(track, cursorRow, -1)
                            // Remember last edited value
                            lastEditedChain = track.chainRefs[cursorRow]
                        }
                    }

                    // CHAIN SCREEN: Use generic input system
                    ScreenType.CHAIN -> {
                        // Get cursor context from chain editor
                        val chainState = ChainEditorState(
                            project.chains[currentChain],
                            cursorRow,
                            cursorColumn
                        )
                        val context = chainEditorModule.getCursorContext(chainState)

                        // Handle input generically
                        val action = genericInputHandler.handleBButton(context)

                        // Apply the action
                        when (action) {
                            is InputAction.SET_VALUE -> {
                                // Update the value at cursor position
                                when (cursorColumn) {
                                    1 -> {
                                        // Phrase reference
                                        project.chains[currentChain].phraseRefs[cursorRow] = action.value
                                        lastEditedPhrase = action.value
                                    }
                                    2 -> {
                                        // Transpose value
                                        project.chains[currentChain].transposeValues[cursorRow] = action.value
                                    }
                                }
                            }
                            else -> { }
                        }
                    }

                    // PHRASE SCREEN: Now handled by generic input (A+direction combos)
                    ScreenType.PHRASE -> {
                        // All phrase value editing is now handled via generic input handler
                        // No need for manual cycling here
                    }

                    // PROJECT SCREEN: Decrement values
                    ScreenType.PROJECT -> {
                        when (projectCursorRow) {
                            // ROW 0: TEMPO (decrease by 1)
                            0 -> {
                                if (projectCursorColumn == 1) {
                                    project.tempo = (project.tempo - 1).coerceIn(1, 999)
                                }
                            }

                            // ROW 1: TRANSPOSE (decrease by 1)
                            1 -> {
                                if (projectCursorColumn == 1) {
                                    project.transpose = (project.transpose - 1).coerceIn(0, 255)
                                }
                            }

                            // ROW 2: NAME (cycle character backward Z→Y→X...)
                            2 -> {
                                if (projectCursorColumn >= 1) {
                                    val charIndex = projectCursorColumn - 1
                                    val allowedChars = ('A'..'Z') + ('0'..'9') + '_' + '-'

                                    val currentName = project.name.padEnd(12, '_')
                                    val currentChar = currentName.getOrNull(charIndex) ?: '_'
                                    val currentIndex = allowedChars.indexOf(currentChar)

                                    // Get previous char (wrap around)
                                    val prevChar = if (currentIndex == -1) {
                                        '_'
                                    } else {
                                        allowedChars[(currentIndex - 1 + allowedChars.size) % allowedChars.size]
                                    }

                                    val nameChars = currentName.toMutableList()
                                    nameChars[charIndex] = prevChar
                                    project.name = nameChars.joinToString("").trimEnd('_')
                                }
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
                when (currentScreen) {
                    // SONG SCREEN: Clear chain reference
                    ScreenType.SONG -> {
                        val trackIndex = getTrackIndex(cursorColumn)
                        clearSongChainRef(project.tracks[trackIndex], cursorRow)
                    }

                    // CHAIN SCREEN: Use generic input system (DELETE action)
                    ScreenType.CHAIN -> {
                        // Get cursor context from chain editor
                        val chainState = ChainEditorState(
                            project.chains[currentChain],
                            cursorRow,
                            cursorColumn
                        )
                        val context = chainEditorModule.getCursorContext(chainState)

                        // Handle SELECT as delete
                        val action = genericInputHandler.handleSelect(context)

                        // Apply the action
                        when (action) {
                            is InputAction.DELETE -> {
                                // Clear the value at cursor position
                                if (cursorColumn == 1) {
                                    clearChainSlot(project.chains[currentChain], cursorRow)
                                }
                            }
                            else -> { }
                        }
                    }

                    // FILE_BROWSER: SELECT button does nothing (combos handled separately)
                    ScreenType.FILE_BROWSER -> {
                        // Do nothing - SELECT combos (SELECT+A, SELECT+B, etc.) are handled in InputMapper
                    }

                    // OTHER SCREENS: Quick jump to main screen
                    else -> {
                        if (currentScreen !in MAIN_ROW_SCREENS) {
                            currentScreen = when (previousColumn) {
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
                when (currentScreen) {
                    // File browser: Preview selected WAV file
                    ScreenType.FILE_BROWSER -> {
                        if (previousScreen == ScreenType.INSTRUMENT && fileBrowserState.items.isNotEmpty()) {
                            val selectedItem = fileBrowserState.items[fileBrowserState.cursor]
                            val selectedFile = selectedItem.file
                            if (selectedFile.isFile && selectedFile.extension.lowercase() == "wav") {
                                audioEngine.previewSampleFile(selectedFile.absolutePath)
                            }
                        }
                    }

                    // Instrument screen: Preview instrument with all parameters
                    ScreenType.INSTRUMENT -> {
                        audioEngine.previewInstrument(project.instruments[currentInstrument])
                    }

                    // Other screens: Toggle playback
                    else -> {
                        isPlaying = !isPlaying
                    }
                }
            },

            // ───────────────────────────────────────────────────────────────
            // L BUTTON - Hold modifier (tracked by InputMapper)
            // ───────────────────────────────────────────────────────────────
            onL = {
                // L button alone
                // L is tracked as a hold modifier by InputMapper
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
                // A+UP: Small increment (uses GenericInputHandler)
                handleGenericInput { context -> genericInputHandler.handleAButton(context) }
            },

            onADown = {
                // A+DOWN: Small decrement (uses GenericInputHandler)
                handleGenericInput { context -> genericInputHandler.handleBButton(context) }
            },

            onALeft = {
                // A+LEFT: Large decrement (uses GenericInputHandler)
                handleGenericInput { context -> genericInputHandler.handleALeft(context) }
            },

            onARight = {
                // A+RIGHT: Large increment (uses GenericInputHandler)
                handleGenericInput { context -> genericInputHandler.handleARight(context) }
            },

            // ───────────────────────────────────────────────────────────────
            // A + B COMBINATION (Delete)
            // ───────────────────────────────────────────────────────────────
            onAB = {
                // A+B: Delete/clear value at cursor
                handleGenericInput { context -> genericInputHandler.handleABCombo(context) }
            },

            // ───────────────────────────────────────────────────────────────
            // R + DIRECTION COMBINATIONS (Screen navigation)
            // ───────────────────────────────────────────────────────────────
            onRUp = {
                // R+UP: Navigate to screen above in 5×5 grid (disabled in FILE_BROWSER)
                if (currentScreen != ScreenType.FILE_BROWSER) {
                    val (newScreen, newCol) = navigateUp(currentScreen, previousColumn)
                    if (newScreen != currentScreen) {
                        // Screen changed - reset cursor to default position
                        val (defaultRow, defaultCol) = getDefaultCursorPosition(newScreen)
                        cursorRow = defaultRow
                        cursorColumn = defaultCol
                    }
                    currentScreen = newScreen
                    previousColumn = newCol
                }
            },

            onRDown = {
                // R+DOWN: Navigate to screen below in 5×5 grid (disabled in FILE_BROWSER)
                if (currentScreen != ScreenType.FILE_BROWSER) {
                    val (newScreen, newCol) = navigateDown(currentScreen, previousColumn)
                    if (newScreen != currentScreen) {
                        // Screen changed - reset cursor to default position
                        val (defaultRow, defaultCol) = getDefaultCursorPosition(newScreen)
                        cursorRow = defaultRow
                        cursorColumn = defaultCol
                    }
                    currentScreen = newScreen
                    previousColumn = newCol
                }
            },

            onRLeft = {
                // R+LEFT: Navigate to screen on left in main row (disabled in FILE_BROWSER)
                if (currentScreen != ScreenType.FILE_BROWSER) {
                    val (newScreen, newCol) = navigateLeft(currentScreen, previousColumn)
                    if (newScreen != currentScreen) {
                        // Screen changed - reset cursor to default position
                        val (defaultRow, defaultCol) = getDefaultCursorPosition(newScreen)
                        cursorRow = defaultRow
                        cursorColumn = defaultCol
                    }
                    currentScreen = newScreen
                    previousColumn = newCol
                }
            },

            onRRight = {
                // R+RIGHT: Navigate to screen on right in main row (disabled in FILE_BROWSER)
                if (currentScreen != ScreenType.FILE_BROWSER) {
                    val (newScreen, newCol) = navigateRight(currentScreen, previousColumn)
                    if (newScreen != currentScreen) {
                        // Screen changed - reset cursor to default position
                        val (defaultRow, defaultCol) = getDefaultCursorPosition(newScreen)
                        cursorRow = defaultRow
                        cursorColumn = defaultCol
                    }
                    currentScreen = newScreen
                    previousColumn = newCol
                }
            },

            // ───────────────────────────────────────────────────────────────
            // L + DIRECTION COMBINATIONS (Context navigation)
            // ───────────────────────────────────────────────────────────────
            onLLeft = {
                // L+LEFT: Navigate to previous chain/phrase/instrument OR parent folder
                Log.d("Navigation", "L+LEFT: currentScreen=$currentScreen, currentChain=$currentChain, currentPhrase=$currentPhrase")
                when (currentScreen) {
                    ScreenType.CHAIN -> {
                        // Previous chain (wrap around)
                        currentChain = if (currentChain > 0) currentChain - 1 else 255
                        Log.d("Navigation", "  -> Changed to chain $currentChain")
                    }
                    ScreenType.PHRASE -> {
                        // Previous phrase (wrap around)
                        currentPhrase = if (currentPhrase > 0) currentPhrase - 1 else 255
                        Log.d("Navigation", "  -> Changed to phrase $currentPhrase")
                    }
                    ScreenType.INSTRUMENT -> {
                        // Previous instrument (wrap around)
                        currentInstrument = if (currentInstrument > 0) currentInstrument - 1 else 255
                        Log.d("Navigation", "  -> Changed to instrument $currentInstrument")
                    }
                    ScreenType.FILE_BROWSER -> {
                        // Navigate to parent folder
                        fileBrowserState = fileBrowserModule.navigateToParent(fileBrowserState)
                        Log.d("Navigation", "  -> Navigated to parent: ${fileBrowserState.currentDirectory.absolutePath}")
                    }
                    else -> { Log.d("Navigation", "  -> No action for screen $currentScreen") }
                }
            },

            onLRight = {
                // L+RIGHT: Navigate to next chain/phrase/instrument
                Log.d("Navigation", "L+RIGHT: currentScreen=$currentScreen, currentChain=$currentChain, currentPhrase=$currentPhrase")
                when (currentScreen) {
                    ScreenType.CHAIN -> {
                        // Next chain (wrap around)
                        currentChain = if (currentChain < 255) currentChain + 1 else 0
                        Log.d("Navigation", "  -> Changed to chain $currentChain")
                    }
                    ScreenType.PHRASE -> {
                        // Next phrase (wrap around)
                        currentPhrase = if (currentPhrase < 255) currentPhrase + 1 else 0
                        Log.d("Navigation", "  -> Changed to phrase $currentPhrase")
                    }
                    ScreenType.INSTRUMENT -> {
                        // Next instrument (wrap around)
                        currentInstrument = if (currentInstrument < 255) currentInstrument + 1 else 0
                        Log.d("Navigation", "  -> Changed to instrument $currentInstrument")
                    }
                    else -> { Log.d("Navigation", "  -> No action for screen $currentScreen") }
                }
            },

            // ─────────────────────────────────────────────────────────────────────
            // L+UP/DOWN: Sort mode cycling (file browser)
            // ─────────────────────────────────────────────────────────────────────
            onLUp = {
                if (currentScreen == ScreenType.FILE_BROWSER) {
                    val modes = FileSortMode.values()
                    val currentIndex = modes.indexOf(fileBrowserState.sortMode)
                    val nextMode = modes[(currentIndex + 1) % modes.size]
                    fileBrowserState = fileBrowserState.copy(
                        sortMode = nextMode
                    )
                }
            },

            onLDown = {
                if (currentScreen == ScreenType.FILE_BROWSER) {
                    val modes = FileSortMode.values()
                    val currentIndex = modes.indexOf(fileBrowserState.sortMode)
                    val prevMode = modes[(currentIndex - 1 + modes.size) % modes.size]
                    fileBrowserState = fileBrowserState.copy(
                        sortMode = prevMode
                    )
                }
            },

            // ─────────────────────────────────────────────────────────────────────
            // SELECT+A: Rename file/folder
            // ─────────────────────────────────────────────────────────────────────
            onSelectA = {
                if (currentScreen == ScreenType.FILE_BROWSER &&
                    fileBrowserState.mode == FileBrowserModule.BrowserMode.NORMAL) {
                    val item = fileBrowserState.items.getOrNull(fileBrowserState.cursor)
                    // Don't allow renaming parent directory (..)
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
            // SELECT+B: Delete file/folder
            // ─────────────────────────────────────────────────────────────────────
            onSelectB = {
                if (currentScreen == ScreenType.FILE_BROWSER &&
                    fileBrowserState.mode == FileBrowserModule.BrowserMode.NORMAL) {
                    val item = fileBrowserState.items.getOrNull(fileBrowserState.cursor)
                    // Don't allow deleting parent directory (..)
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
                if (currentScreen == ScreenType.FILE_BROWSER &&
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

    // Auto-focus when app starts so keyboard works immediately
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
                fileBrowserState = fileBrowserState
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
                fileBrowserState = fileBrowserState
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
            fileBrowserState = fileBrowserState
        )
    }
}

// ═══════════════════════════════════════════════════════════════════════════
// NAVIGATION HELPER FUNCTIONS
// ═══════════════════════════════════════════════════════════════════════════

/**
 * Get which column (0-4) a screen belongs to in the 5×5 grid
 *
 * The grid looks like this:
 *        Col 0   Col 1   Col 2   Col 3      Col 4
 * Row 0:         (none)  SCALE   INST_POOL  (none)
 * Row 1: PROJECT PROJECT GROOVE  MODS       PROJECT
 * Row 2: SONG    CHAIN   PHRASE  INSTRUMENT TABLE
 * Row 3: MIXER   MIXER   MIXER   MIXER      MIXER
 * Row 4: EFFECTS EFFECTS EFFECTS EFFECTS    EFFECTS
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
 * Get the main screen (row 2) for a given column
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
 * Navigate UP in the 5×5 grid
 * Returns: Pair(newScreen, newColumn)
 */
fun navigateUp(currentScreen: ScreenType, previousColumn: Int): Pair<ScreenType, Int> {
    // Figure out which column we're in
    val currentCol = if (getScreenColumn(currentScreen) == -1) {
        previousColumn  // We're in a shared room, use remembered column
    } else {
        getScreenColumn(currentScreen)
    }

    // Navigate up within this column
    return when (currentScreen) {
        // FROM ROW 4 (Effects) → UP TO ROW 3 (Mixer)
        ScreenType.EFFECTS -> Pair(ScreenType.MIXER, currentCol)

        // FROM ROW 3 (Mixer) → UP TO ROW 2 (Main)
        ScreenType.MIXER -> {
            val mainScreen = getMainScreenForColumn(currentCol)
            Pair(mainScreen, currentCol)
        }

        // FROM ROW 2 (Main) → UP TO ROW 1
        ScreenType.SONG, ScreenType.CHAIN, ScreenType.TABLE -> {
            Pair(ScreenType.PROJECT, currentCol)
        }
        ScreenType.PHRASE -> Pair(ScreenType.GROOVE, 2)
        ScreenType.INSTRUMENT -> Pair(ScreenType.MODS, 3)

        // FROM ROW 1 → UP TO ROW 0 (where it exists)
        ScreenType.GROOVE -> Pair(ScreenType.SCALE, 2)
        ScreenType.MODS -> Pair(ScreenType.INST_POOL, 3)
        ScreenType.PROJECT -> Pair(ScreenType.PROJECT, currentCol)

        // FROM ROW 0 → Already at top!
        ScreenType.SCALE, ScreenType.INST_POOL -> {
            Pair(currentScreen, currentCol)
        }

        // Popup screens don't participate in navigation
        ScreenType.FILE_BROWSER -> Pair(currentScreen, currentCol)
    }
}

/**
 * Navigate DOWN in the 5×5 grid
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
        ScreenType.INSTRUMENT, ScreenType.TABLE -> {
            Pair(ScreenType.MIXER, currentCol)
        }

        // FROM ROW 3 (Mixer) → DOWN TO ROW 4 (Effects)
        ScreenType.MIXER -> Pair(ScreenType.EFFECTS, currentCol)

        // FROM ROW 4 → Already at bottom!
        ScreenType.EFFECTS -> Pair(ScreenType.EFFECTS, currentCol)

        // Popup screens don't participate in navigation
        ScreenType.FILE_BROWSER -> Pair(currentScreen, currentCol)
    }
}

/**
 * Navigate LEFT - Only works on main row (S C P I T)
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
        ScreenType.SONG -> Pair(ScreenType.SONG, 0)  // Already leftmost
        else -> Pair(currentScreen, getScreenColumn(currentScreen))
    }
}

/**
 * Navigate RIGHT - Only works on main row (S C P I T)
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
        ScreenType.TABLE -> Pair(ScreenType.TABLE, 4)  // Already rightmost
        else -> Pair(currentScreen, getScreenColumn(currentScreen))
    }
}

/**
 * Get the minimum editable column for a screen type
 * Column 0 is often read-only (step numbers), so minimum is usually 1
 */
fun getMinEditableColumn(screenType: ScreenType): Int {
    return when (screenType) {
        // Phrase: Column 0 = step number (read-only), column 1 = NOTE (editable)
        ScreenType.PHRASE -> 1

        // Chain: Column 0 = step number (read-only), column 1 = PH (editable)
        ScreenType.CHAIN -> 1

        // Song: Column 0 = step number (read-only), column 1 = Track 1 (editable)
        // Note: Song uses cursorTrack (1-8), not cursorColumn (0-7)
        ScreenType.SONG -> 1

        // Project: Column 0 = row labels (read-only), column 1 = values (editable)
        ScreenType.PROJECT -> 1

        // Instrument: Column 0 might be label, column 1 is first editable
        ScreenType.INSTRUMENT -> 1

        // Table: Column 0 might be label, column 1 is first editable
        ScreenType.TABLE -> 1

        // Mixer/Effects: All tracks are editable starting from 0
        ScreenType.MIXER -> 0
        ScreenType.EFFECTS -> 0

        // Groove/Scale/Mods/InstPool: Assume column 1 is first editable
        ScreenType.GROOVE -> 1
        ScreenType.SCALE -> 1
        ScreenType.MODS -> 1
        ScreenType.INST_POOL -> 1

        // File browser: No column editing
        ScreenType.FILE_BROWSER -> 0
    }
}

/**
 * Get the minimum editable row for a screen type
 * Most screens start at row 0
 */
fun getMinEditableRow(screenType: ScreenType): Int {
    return when (screenType) {
        // All current screens start editing from row 0
        else -> 0
    }
}

/**
 * Get default cursor position for a screen
 * Returns Pair(row, column) representing the first available/editable cell
 *
 * This systematically finds the first editable position by using:
 * - getMinEditableRow() to skip any read-only header rows
 * - getMinEditableColumn() to skip any read-only label columns
 */
fun getDefaultCursorPosition(screenType: ScreenType): Pair<Int, Int> {
    val defaultRow = getMinEditableRow(screenType)
    val defaultColumn = getMinEditableColumn(screenType)
    return Pair(defaultRow, defaultColumn)
}

// ═══════════════════════════════════════════════════════════════════════════
// PROJECT SCREEN CURSOR HELPERS
// ═══════════════════════════════════════════════════════════════════════════

/**
 * Handle LEFT button for Project screen
 * Each row has different column limits
 */
fun handleProjectCursorLeft(cursorRow: Int, cursorColumn: Int): Int {
    val minColumn = getProjectMinColumn(cursorRow)
    if (cursorColumn > minColumn) {
        return cursorColumn - 1
    }
    return cursorColumn  // Already at leftmost
}

/**
 * Handle RIGHT button for Project screen
 */
fun handleProjectCursorRight(cursorRow: Int, cursorColumn: Int): Int {
    val maxColumn = getProjectMaxColumn(cursorRow)
    if (cursorColumn < maxColumn) {
        return cursorColumn + 1
    }
    return cursorColumn  // Already at rightmost
}

/**
 * Get MINIMUM cursor column for each Project row
 * Column 0 is always the label (not selectable)
 */
fun getProjectMinColumn(row: Int): Int {
    return 1  // Always 1 (column 0 is the label)
}

/**
 * Get MAXIMUM cursor column for each Project row
 */
fun getProjectMaxColumn(row: Int): Int {
    return when (row) {
        0 -> 1      // TEMPO: only 1 value column
        1 -> 1      // TRANSPOSE: only 1 value column
        2 -> 12     // NAME: 12 character positions
        3 -> 3      // PROJECT: LOAD, SAVE, NEW (3 options)
        4 -> 1      // EXPORT: only 1 value column
        5 -> 1      // CLEAN: only 1 value column
        6 -> 1      // SYSTEM: only 1 value column
        else -> 1
    }
}

// ═══════════════════════════════════════════════════════════════════════════
// DATA EDITING HELPER FUNCTIONS
// ═══════════════════════════════════════════════════════════════════════════

/**
 * Cycle note value (C-4, C#4, D-4, etc.)
 * @param direction +1 to go up, -1 to go down
 */
fun cycleNote(step: PhraseStep, direction: Int) {
    if (step.note == Note.EMPTY) {
        step.note = Note.fromMidi(60) // Start at C-4
    } else {
        val currentMidi = step.note.toMidi()
        val newMidi = (currentMidi + direction).coerceIn(0, 119)
        step.note = Note.fromMidi(newMidi)
    }
}

/**
 * Cycle volume value (00-FF, in steps of 16)
 */
fun cycleVolume(step: PhraseStep, direction: Int) {
    val newVolume = (step.volume + direction * 16).coerceIn(0, 255)
    step.volume = newVolume
}

/**
 * Cycle instrument (0-3 for now)
 */
fun cycleInstrument(step: PhraseStep, direction: Int) {
    step.instrument = ((step.instrument + direction + 128) % 12).coerceIn(0, 12)
}

// ───────────────────────────────────────────────────────────────────────────
// CHAIN EDITOR HELPER FUNCTIONS
// ───────────────────────────────────────────────────────────────────────────

/**
 * Insert phrase reference with last used value
 * Returns true if successfully inserted, false if slot already has value
 */
fun insertChainPhrase(chain: Chain, row: Int, lastPhraseValue: Int = 0): Boolean {
    if (chain.phraseRefs[row] == 0xFF) {
        chain.phraseRefs[row] = lastPhraseValue
        chain.transposeValues[row] = 0x00  // Default transpose
        return true  // Successfully inserted
    }
    return false  // Already has a value
}

/**
 * Edit phrase reference by delta
 * @param large If true, changes by 16 (0x10), else by 1
 */
fun editChainPhraseValue(chain: Chain, row: Int, direction: Int, large: Boolean = false) {
    val currentRef = chain.phraseRefs[row]
    if (currentRef == 0xFF) return  // Empty, can't edit

    val step = if (large) 16 else 1
    val newRef = (currentRef + direction * step + 256) % 255  // 0-254, wrap around, skip 255
    chain.phraseRefs[row] = newRef
}

/**
 * Edit transpose value
 * @param large If true, changes by 12 semitones (octave), else by 1
 */
fun editChainTransposeValue(chain: Chain, row: Int, direction: Int, large: Boolean = false) {
    val currentTranspose = chain.transposeValues[row]
    val step = if (large) 12 else 1
    val newTranspose = (currentTranspose + direction * step).coerceIn(0, 255)
    chain.transposeValues[row] = newTranspose
}

/**
 * Clear chain slot (set to empty)
 */
fun clearChainSlot(chain: Chain, row: Int) {
    chain.phraseRefs[row] = 0xFF
    chain.transposeValues[row] = 0x00  // Reset transpose to default
}

/**
 * Navigate to next/previous chain (B+UP/DOWN)
 */
fun navigateChain(current: Int, direction: Int): Int {
    return (current + direction + 256) % 256  // Wrap 0-255
}

// ───────────────────────────────────────────────────────────────────────────
// SONG EDITOR HELPER FUNCTIONS
// ───────────────────────────────────────────────────────────────────────────

/**
 * Insert chain reference in song track
 * Returns true if successfully inserted
 */
fun insertSongChainRef(track: Track, row: Int, lastChainValue: Int = 0): Boolean {
    // Expand track list if needed
    while (track.chainRefs.size <= row) {
        track.chainRefs.add(-1)  // Fill with empty slots
    }

    // If empty, insert
    if (row < track.chainRefs.size && track.chainRefs[row] == -1) {
        track.chainRefs[row] = lastChainValue
        return true
    }
    return false
}

/**
 * Edit chain reference in song track
 */
fun editSongChainRef(track: Track, row: Int, direction: Int) {
    if (row >= track.chainRefs.size) return

    val currentRef = track.chainRefs[row]
    if (currentRef == -1) return  // Empty, can't edit

    // Cycle 0-255
    val newRef = (currentRef + direction + 256) % 256
    track.chainRefs[row] = newRef
}

/**
 * Clear chain reference in song track (set to empty)
 */
fun clearSongChainRef(track: Track, row: Int) {
    if (row < track.chainRefs.size) {
        track.chainRefs[row] = -1
    }
}

/**
 * Get track index from cursor column (1-8 → 0-7)
 */
fun getTrackIndex(cursorColumn: Int): Int {
    return (cursorColumn - 1).coerceIn(0, 7)
}