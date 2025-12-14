package com.example.pockettracker

import android.os.Bundle
import android.util.Log
import androidx.activity.ComponentActivity
import androidx.activity.compose.setContent
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
    // INPUT ACTION HELPER
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
                        chain.transposeValues[row] = 0x80  // Reset transpose
                    }
                }
            }
            is InputAction.INSERT_DEFAULT -> {
                if (column == 1) {
                    // Insert phrase 0 by default
                    chain.phraseRefs[row] = 0
                    chain.transposeValues[row] = 0x80  // No transpose
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
        val track = project.song.tracks[trackIndex]

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
                        // Project screen has special cursor handling
                        if (projectCursorRow > 0) {
                            projectCursorRow--
                            projectCursorColumn = 1  // Reset to first value column
                        }
                    }
                    else -> {
                        // All other screens: simple cursor movement
                        if (cursorRow > 0) cursorRow--
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
                        // Project has 7 rows (0-6)
                        if (projectCursorRow < 6) {
                            projectCursorRow++
                            projectCursorColumn = 1  // Reset column
                        }
                    }
                    else -> {
                        // Most screens have 16 rows (0-15)
                        if (cursorRow < 15) cursorRow++
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
                    // SONG SCREEN: Insert or increment chain reference
                    ScreenType.SONG -> {
                        // Figure out which track (0-7) based on cursor column (1-8)
                        val trackIndex = getTrackIndex(cursorColumn)
                        val track = project.tracks[trackIndex]

                        // Check if this cell is empty
                        val isEmpty = cursorRow >= track.chainRefs.size ||
                                track.chainRefs[cursorRow] == -1

                        if (isEmpty) {
                            // Empty: Insert last edited chain (or 0)
                            insertSongChainRef(track, cursorRow, lastEditedChain)
                        } else {
                            // Has value: Increase by 1
                            editSongChainRef(track, cursorRow, 1)
                            // Remember this value for next insertion
                            lastEditedChain = track.chainRefs[cursorRow]
                        }
                    }

                    // PHRASE SCREEN: Edit note/volume/instrument
                    ScreenType.PHRASE -> {
                        when (cursorColumn) {
                            1 -> cycleNote(project.phrases[currentPhrase].steps[cursorRow], 1)
                            2 -> cycleVolume(project.phrases[currentPhrase].steps[cursorRow], 1)
                            3 -> cycleInstrument(project.phrases[currentPhrase].steps[cursorRow], 1)
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
                        val action = genericInputHandler.handleAButton(context)

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
                            is InputAction.INSERT_DEFAULT -> {
                                // Insert last edited phrase
                                insertChainPhrase(
                                    project.chains[currentChain],
                                    cursorRow,
                                    lastEditedPhrase
                                )
                            }
                            else -> { }
                        }
                    }

                    // PROJECT SCREEN: Edit project parameters
                    ScreenType.PROJECT -> {
                        when (projectCursorRow) {
                            // ROW 0: TEMPO (increase by 1)
                            0 -> {
                                if (projectCursorColumn == 1) {
                                    project.tempo = (project.tempo + 1).coerceIn(1, 999)
                                }
                            }

                            // ROW 1: TRANSPOSE (increase by 1)
                            1 -> {
                                if (projectCursorColumn == 1) {
                                    project.transpose = (project.transpose + 1).coerceIn(0, 255)
                                }
                            }

                            // ROW 2: NAME (cycle character forward A→B→C...)
                            2 -> {
                                if (projectCursorColumn >= 1) {
                                    // Get which character position (1-12)
                                    val charIndex = projectCursorColumn - 1

                                    // Allowed characters: A-Z, 0-9, _, -
                                    val allowedChars = ('A'..'Z') + ('0'..'9') + '_' + '-'

                                    // Get current name, pad to 12 chars with underscores
                                    val currentName = project.name.padEnd(12, '_')
                                    val currentChar = currentName.getOrNull(charIndex) ?: '_'

                                    // Find current char in allowed list
                                    val currentIndex = allowedChars.indexOf(currentChar)

                                    // Get next char (wrap around to 'A' after '-')
                                    val nextChar = if (currentIndex == -1) {
                                        'A'  // Default to A if not found
                                    } else {
                                        allowedChars[(currentIndex + 1) % allowedChars.size]
                                    }

                                    // Update name
                                    val nameChars = currentName.toMutableList()
                                    nameChars[charIndex] = nextChar
                                    project.name = nameChars.joinToString("").trimEnd('_')
                                }
                            }

                            // ROW 3: PROJECT actions (LOAD/SAVE/NEW)
                            3 -> {
                                when (projectCursorColumn) {
                                    1 -> {  // LOAD
                                        val loaded = fileManager.loadProject("UNTITLED")
                                        if (loaded != null) {
                                            project = loaded
                                            projectStatusMessage = "LOADED"
                                            projectStatusSuccess = true
                                        } else {
                                            projectStatusMessage = "LOAD FAILED"
                                            projectStatusSuccess = false
                                        }
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

                    else -> { /* Other screens not implemented yet */ }
                }
            },

            // ───────────────────────────────────────────────────────────────
            // BUTTON B - Secondary action (decrement)
            // ───────────────────────────────────────────────────────────────
            onButtonB = {
                when (currentScreen) {
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

                    // PHRASE SCREEN: Decrement values
                    ScreenType.PHRASE -> {
                        when (cursorColumn) {
                            1 -> cycleNote(project.phrases[currentPhrase].steps[cursorRow], -1)
                            2 -> cycleVolume(project.phrases[currentPhrase].steps[cursorRow], -1)
                            3 -> cycleInstrument(project.phrases[currentPhrase].steps[cursorRow], -1)
                        }
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
            // START BUTTON - Toggle playback
            // ───────────────────────────────────────────────────────────────
            onStart = {
                isPlaying = !isPlaying
            },

            // ───────────────────────────────────────────────────────────────
            // L BUTTON - Hold modifier (tracked by InputMapper)
            // ───────────────────────────────────────────────────────────────
            onL = {
                // L is tracked as a hold modifier by InputMapper
                // Combinations like L+A, L+direction are handled in InputMapper
            },

            // ───────────────────────────────────────────────────────────────
            // R BUTTON - Hold modifier (tracked by InputMapper)
            // ───────────────────────────────────────────────────────────────
            onR = {
                // R is tracked as a hold modifier by InputMapper
                // Combinations like R+arrows for screen navigation handled in InputMapper
            },

            // ───────────────────────────────────────────────────────────────
            // A + DIRECTION COMBINATIONS (M8-style value editing)
            // ───────────────────────────────────────────────────────────────
            onAUp = {
                // A+UP: Small increment (uses GenericInputHandler)
                when (currentScreen) {
                    ScreenType.CHAIN -> {
                        val chainState = ChainEditorState(
                            project.chains[currentChain],
                            cursorRow,
                            cursorColumn
                        )
                        val context = chainEditorModule.getCursorContext(chainState)
                        val action = genericInputHandler.handleAButton(context)
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
                        val action = genericInputHandler.handleAButton(context)
                        applyPhraseInputAction(action, currentPhrase, cursorRow, cursorColumn)
                    }
                    ScreenType.SONG -> {
                        val songState = SongEditorState(
                            project,
                            cursorRow,
                            cursorTrack = cursorColumn
                        )
                        val context = songEditorModule.getCursorContext(songState)
                        val action = genericInputHandler.handleAButton(context)
                        applySongInputAction(action, cursorColumn - 1, cursorRow)
                    }
                    else -> { /* Other screens not yet implemented */ }
                }
            },

            onADown = {
                // A+DOWN: Small decrement (uses GenericInputHandler)
                when (currentScreen) {
                    ScreenType.CHAIN -> {
                        val chainState = ChainEditorState(
                            project.chains[currentChain],
                            cursorRow,
                            cursorColumn
                        )
                        val context = chainEditorModule.getCursorContext(chainState)
                        val action = genericInputHandler.handleBButton(context)
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
                        val action = genericInputHandler.handleBButton(context)
                        applyPhraseInputAction(action, currentPhrase, cursorRow, cursorColumn)
                    }
                    ScreenType.SONG -> {
                        val songState = SongEditorState(
                            project,
                            cursorRow,
                            cursorTrack = cursorColumn
                        )
                        val context = songEditorModule.getCursorContext(songState)
                        val action = genericInputHandler.handleBButton(context)
                        applySongInputAction(action, cursorColumn - 1, cursorRow)
                    }
                    else -> { /* Other screens not yet implemented */ }
                }
            },

            onALeft = {
                // A+LEFT: Large decrement (uses GenericInputHandler)
                when (currentScreen) {
                    ScreenType.CHAIN -> {
                        val chainState = ChainEditorState(
                            project.chains[currentChain],
                            cursorRow,
                            cursorColumn
                        )
                        val context = chainEditorModule.getCursorContext(chainState)
                        val action = genericInputHandler.handleALeft(context)
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
                        val action = genericInputHandler.handleALeft(context)
                        applyPhraseInputAction(action, currentPhrase, cursorRow, cursorColumn)
                    }
                    ScreenType.SONG -> {
                        val songState = SongEditorState(
                            project,
                            cursorRow,
                            cursorTrack = cursorColumn
                        )
                        val context = songEditorModule.getCursorContext(songState)
                        val action = genericInputHandler.handleALeft(context)
                        applySongInputAction(action, cursorColumn - 1, cursorRow)
                    }
                    else -> { /* Other screens not yet implemented */ }
                }
            },

            onARight = {
                // A+RIGHT: Large increment (uses GenericInputHandler)
                when (currentScreen) {
                    ScreenType.CHAIN -> {
                        val chainState = ChainEditorState(
                            project.chains[currentChain],
                            cursorRow,
                            cursorColumn
                        )
                        val context = chainEditorModule.getCursorContext(chainState)
                        val action = genericInputHandler.handleARight(context)
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
                        val action = genericInputHandler.handleARight(context)
                        applyPhraseInputAction(action, currentPhrase, cursorRow, cursorColumn)
                    }
                    ScreenType.SONG -> {
                        val songState = SongEditorState(
                            project,
                            cursorRow,
                            cursorTrack = cursorColumn
                        )
                        val context = songEditorModule.getCursorContext(songState)
                        val action = genericInputHandler.handleARight(context)
                        applySongInputAction(action, cursorColumn - 1, cursorRow)
                    }
                    else -> { /* Other screens not yet implemented */ }
                }
            },

            // ───────────────────────────────────────────────────────────────
            // R + DIRECTION COMBINATIONS (Screen navigation)
            // ───────────────────────────────────────────────────────────────
            onRUp = {
                // R+UP: Navigate to screen above in 5×5 grid
                val (newScreen, newCol) = navigateUp(currentScreen, previousColumn)
                currentScreen = newScreen
                previousColumn = newCol
            },

            onRDown = {
                // R+DOWN: Navigate to screen below in 5×5 grid
                val (newScreen, newCol) = navigateDown(currentScreen, previousColumn)
                currentScreen = newScreen
                previousColumn = newCol
            },

            onRLeft = {
                // R+LEFT: Navigate to screen on left in main row
                val (newScreen, newCol) = navigateLeft(currentScreen, previousColumn)
                currentScreen = newScreen
                previousColumn = newCol
            },

            onRRight = {
                // R+RIGHT: Navigate to screen on right in main row
                val (newScreen, newCol) = navigateRight(currentScreen, previousColumn)
                currentScreen = newScreen
                previousColumn = newCol
            },

            // ───────────────────────────────────────────────────────────────
            // L + DIRECTION COMBINATIONS (Context navigation)
            // ───────────────────────────────────────────────────────────────
            onLLeft = {
                // L+LEFT: Navigate to previous chain/phrase/instrument
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
                    else -> { Log.d("Navigation", "  -> No action for screen $currentScreen") }
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
        focusRequester.requestFocus()
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
                projectVersion = projectVersion
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
                projectVersion = projectVersion
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
            projectVersion = projectVersion
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
    step.instrument = ((step.instrument + direction + 128) % 4).coerceIn(0, 3)
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
        chain.transposeValues[row] = 0x80  // Default: no transpose
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
    chain.transposeValues[row] = 0x80  // Reset transpose too
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