package com.conanizer.pockettracker

import androidx.compose.foundation.Canvas
import androidx.compose.foundation.background
import androidx.compose.foundation.layout.*
import androidx.compose.runtime.*
import androidx.compose.ui.Modifier
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.geometry.Size
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.drawscope.DrawScope
import androidx.compose.ui.graphics.drawscope.clipRect
import androidx.compose.ui.graphics.drawscope.drawIntoCanvas
import androidx.compose.ui.graphics.drawscope.translate
import kotlin.math.min
import kotlinx.coroutines.delay
import com.conanizer.pockettracker.core.audio.AudioEngine
import com.conanizer.pockettracker.core.data.Note
import com.conanizer.pockettracker.core.data.Project
import com.conanizer.pockettracker.core.data.ScreenType
import com.conanizer.pockettracker.core.logic.EffectProcessor

/**
 * PIXEL-PERFECT TRACKER - MODULAR VERSION
 *
 * Screen: 640×480 (4:3 handheld)
 * Font: 15×15 pixels (5×5 bitmap × 3)
 * Rows: 21px each
 *
 * Modular architecture:
 * - OscilloscopeModule (620×70)
 * - PhraseEditorModule (620×392)
 * - NavigationMapModule (80×105) - NEW!
 * - Easy to add more modules!
 */

// Design constants
const val DESIGN_WIDTH_PX = 640

/**
 * CompositionLocal that carries the current LayoutMode down the composition tree.
 * Set via CompositionLocalProvider in PocketTrackerApp; read in PixelPerfectTracker
 * so it reaches drawLayout without threading through every intermediate composable.
 */
val LocalLayoutMode = compositionLocalOf { DeviceAdapter.LayoutMode.FULL }
const val DESIGN_HEIGHT_PX = 480
const val SCREEN_SPACER = 6      // Space between modules
const val SIDE_SPACER = 10       // Space on sides

@Composable
fun PixelPerfectTracker(
    currentScreen: ScreenType,
    project: Project,
    audioEngine: AudioEngine,
    playbackController: com.conanizer.pockettracker.core.logic.PlaybackController,
    cursorRow: Int,
    cursorColumn: Int,
    isPlaying: Boolean,
    previousColumn: Int,
    currentChain: Int,
    currentPhrase: Int,
    projectCursorRow: Int,
    projectCursorColumn: Int,
    projectStatusMessage: String,
    projectStatusSuccess: Boolean,
    projectVersion: Int,
    currentInstrument: Int,
    instrumentCursorRow: Int,
    instrumentCursorColumn: Int,
    instrumentStatusMessage: String,
    instrumentStatusSuccess: Boolean,
    fileBrowserState: FileBrowserModule.State? = null,
    // Copy/paste state
    selectionInfo: String = "",        // e.g., "SEL:CELL", "SEL:ROW", "SEL:ALL"
    clipboardInfo: String = "",        // e.g., "PHR:3x4", "CHN:1x8"
    selectionMode: Boolean = false,    // Whether selection mode is active
    isCellSelected: (Int, Int) -> Boolean = { _, _ -> false },  // Check if cell is selected
    // Mixer state
    mixerCursorColumn: Int = 0,        // 0-7 = tracks, 8 = master
    trackPeaks: FloatArray = FloatArray(8),
    masterPeaks: FloatArray = FloatArray(2),
    // Table state
    currentTable: Int = 0,
    tableCursorRow: Int = 0,
    tableCursorColumn: Int = 1,
    // Groove state
    currentGroove: Int = 0,
    grooveCursorRow: Int = 0,
    // Modulation state
    modCursorRow: Int = 0,
    modCursorPair: Int = 0,
    modCursorSide: Int = 0,
    // Render state (WAV export)
    isRendering: Boolean = false,
    renderProgress: Float = 0f,
    // Clean dialog state
    showCleanDialog: Boolean = false,
    cleanDialogTarget: String = "",  // "SEQ" or "INST"
    cleanDialogCursor: Int = 0,      // 0 = YES, 1 = NO
    // Song scroll position
    songScrollPosition: Int = 0,
    // Scaling mode (for project screen display)
    scalingMode: DeviceAdapter.ScalingMode = DeviceAdapter.ScalingMode.INTEGER,
    buttonSoundEnabled: Boolean = false,
    buttonSoundVolume: Int = 255,
    buttonVibroEnabled: Boolean = false,
    vibroPower: Int = 255,
    // QWERTY keyboard overlay state
    qwertyKeyboardState: QwertyKeyboardState = QwertyKeyboardState(),
    // FX helper overlay state
    fxHelperState: FxHelperState = FxHelperState(),
    // Settings screen cursor
    settingsCursorRow: Int = 0,
    settingsCursorColumn: Int = 1,
    // Cursor remember setting (for settings screen display)
    cursorRemember: Boolean = false
) {
    if (currentScreen == ScreenType.FILE_BROWSER) {
        android.util.Log.d("PixelPerfectTracker", "FILE_BROWSER screen, fileBrowserState=${if (fileBrowserState != null) "not null (${fileBrowserState.items.size} items)" else "NULL"}")
    }
    // Playback state
    var playbackRow by remember { mutableStateOf(0) }
    var playbackChainRow by remember { mutableStateOf(0) }
    var playbackPhraseStep by remember { mutableStateOf(0) }
    var playbackSongRow by remember { mutableStateOf(0) }
    var trackNotes by remember { mutableStateOf(List(8) { Note.EMPTY }) }

    // Oscilloscope refresh ticker (force continuous Canvas redraws for smooth waveform)
    var oscilloscopeTicker by remember { mutableStateOf(0L) }

    // Oscilloscope refresh loop (independent of playback position)
    LaunchedEffect(Unit) {
        while (true) {
            oscilloscopeTicker++
            delay(16L)  // ~60 FPS refresh rate
        }
    }

    // Playback position update loop
    // This is SIMPLIFIED: all scheduling logic moved to PlaybackController.updatePlaybackBuffer()
    // When project data changes during playback, reschedule immediately so edits are
    // heard on the next phrase loop rather than 2-3 phrases later.
    // LaunchedEffect(projectVersion) restarts every time the version changes, so we
    // always read fresh values of isPlaying and projectVersion.
    LaunchedEffect(projectVersion) {
        if (isPlaying) {
            playbackController.notifyDataChanged()
        }
    }

    // Playback position update loop
    // This is SIMPLIFIED: all scheduling logic moved to PlaybackController.updatePlaybackBuffer()
    LaunchedEffect(isPlaying, currentScreen) {
        if (isPlaying) {
            trackNotes = List(8) { Note.EMPTY }  // Clear stale notes at start of new playback
            while (isPlaying) {
                // Update lookahead buffer - PlaybackController handles all scheduling
                playbackController.updatePlaybackBuffer()

                // Get current playback position for UI updates
                val position = playbackController.getPlaybackPosition()
                playbackRow = position.row
                playbackChainRow = position.chainRow
                playbackPhraseStep = position.phraseStep
                playbackSongRow = position.songRow
                // Derive notes from actual playback position (not schedule-ahead trackStates)
                trackNotes = playbackController.getCurrentPlayingNotes()

                // Update UI at 60 Hz
                delay(16L)
            }
        } else {
            // isPlaying just became false — clear monitor immediately
            // (can't do this after the while loop because LaunchedEffect cancels the old
            // coroutine when its keys change, so code after the loop never executes)
            trackNotes = List(8) { Note.EMPTY }
        }
    }

    // NOTE: BoxWithConstraints intentionally replaced with a single Canvas.
    // BoxWithConstraints uses SubcomposeLayout internally, which can destroy+recreate
    // child composables (including Canvas) during remeasure, causing SEGV_ACCERR in
    // RenderThread. Using a single Canvas with DrawScope.size is both simpler and safe:
    // the Canvas node is never destroyed, scale is computed in the draw phase.
    //
    // key() also intentionally ABSENT — same reason (render node destruction race).
    // 60fps oscilloscope: oscilloscopeTicker read inside draw lambda → snapshot observer
    // re-invokes only the draw phase, no full recomposition, no node destruction.
    //
    // TrackerLayout created with remember{} (NOT inside the draw lambda).
    // The draw lambda re-runs 60fps due to oscilloscopeTicker; allocating 12 module
    // objects per frame causes GC pressure that can crash the RenderThread on Android 11
    // (Snapdragon GPU drivers can't safely be paused mid-frame by the JVM GC).
    val layoutMode = LocalLayoutMode.current
    val layout = remember { TrackerLayout() }
    Canvas(
        modifier = Modifier
            .fillMaxSize()
            .background(Color.Black)
    ) {
        // Compute integer scale from DrawScope.size (available in draw phase, no BoxWithConstraints needed).
        // Add 1px tolerance before dividing: dp↔px float conversion can lose up to 1px
        // (e.g. 1280px → 487.619dp → 1279.999px → 1279), which would drop scale by one level.
        val screenWidthPx = size.width.toInt()
        val screenHeightPx = size.height.toInt()
        val scaleX = (screenWidthPx + 1) / DESIGN_WIDTH_PX
        val scaleY = (screenHeightPx + 1) / DESIGN_HEIGHT_PX
        val scale = min(scaleX, scaleY).coerceAtLeast(1)
        val renderWidth = DESIGN_WIDTH_PX * scale
        val renderHeight = DESIGN_HEIGHT_PX * scale
        // Clamp to 0: if canvas is 1px narrower than renderWidth (due to rounding), avoid negative offset
        val offsetX = maxOf(0f, (screenWidthPx - renderWidth) / 2f)
        val offsetY = maxOf(0f, (screenHeightPx - renderHeight) / 2f)

        translate(offsetX, offsetY) {
            with(layout) {
                drawLayout(
                        scale = scale,
                        oscilloscopeTicker = oscilloscopeTicker,
                        currentScreen = currentScreen,
                        project = project,
                        cursorRow = cursorRow,
                        cursorColumn = cursorColumn,
                        isPlaying = isPlaying,
                        playbackRow = playbackRow,
                        playbackChainRow = playbackChainRow,
                        playbackSongRow = playbackSongRow,
                        audioEngine = audioEngine,
                        previousColumn = previousColumn,
                        currentChain = currentChain,
                        currentPhrase = currentPhrase,
                        projectCursorRow = projectCursorRow,
                        projectCursorColumn = projectCursorColumn,
                        projectStatusMessage = projectStatusMessage,
                        projectStatusSuccess = projectStatusSuccess,
                        projectVersion = projectVersion,
                        currentInstrument = currentInstrument,
                        instrumentCursorRow = instrumentCursorRow,
                        instrumentCursorColumn = instrumentCursorColumn,
                        instrumentStatusMessage = instrumentStatusMessage,
                        instrumentStatusSuccess = instrumentStatusSuccess,
                        fileBrowserState = fileBrowserState,
                        selectionInfo = selectionInfo,
                        clipboardInfo = clipboardInfo,
                        selectionMode = selectionMode,
                        isCellSelected = isCellSelected,
                        mixerCursorColumn = mixerCursorColumn,
                        trackPeaks = trackPeaks,
                        masterPeaks = masterPeaks,
                        currentTable = currentTable,
                        tableCursorRow = tableCursorRow,
                        tableCursorColumn = tableCursorColumn,
                        currentGroove = currentGroove,
                        grooveCursorRow = grooveCursorRow,
                        modCursorRow = modCursorRow,
                        modCursorPair = modCursorPair,
                        modCursorSide = modCursorSide,
                        isRendering = isRendering,
                        renderProgress = renderProgress,
                        showCleanDialog = showCleanDialog,
                        cleanDialogTarget = cleanDialogTarget,
                        cleanDialogCursor = cleanDialogCursor,
                        layoutMode = layoutMode,
                        songScrollPosition = songScrollPosition,
                        scalingMode = scalingMode,
                        buttonSoundEnabled = buttonSoundEnabled,
                        buttonSoundVolume = buttonSoundVolume,
                        buttonVibroEnabled = buttonVibroEnabled,
                        vibroPower = vibroPower,
                        qwertyKeyboardState = qwertyKeyboardState,
                        fxHelperState = fxHelperState,
                        settingsCursorRow = settingsCursorRow,
                        settingsCursorColumn = settingsCursorColumn,
                        cursorRemember = cursorRemember,
                        trackNotes = trackNotes
                    )
                }
            }
        }
}

/**
 * LAYOUT MANAGER
 *
 * Positions and draws all modules
 * This is where you "build" your screen layout!
 */
class TrackerLayout {
    // ===================================
    // CREATE MODULE INSTANCES
    // ===================================
    private val oscilloscope = OscilloscopeModule(width = 620, height = 70)
    private val phraseEditor = PhraseEditorModule()
    private val navigationMap = NavigationMapModule()
    private val instrumentModule = InstrumentModule()
    private val mixerModule = MixerModule()
    private val chainEditor = ChainEditorModule()
    private val songEditor = SongEditorModule()
    private val projectModule = ProjectModule()
    private val fileBrowser = FileBrowserModule()
    private val tableModule = TableModule()
    private val grooveModule = GrooveModule()
    private val modulationModule = ModulationModule()
    private val settingsModule = SettingsModule()
    /**
     * Main layout drawing function
     * This arranges all modules on the 640×480 screen
     */
    fun DrawScope.drawLayout(
        scale: Int,
        oscilloscopeTicker: Long = 0L,  // Read in draw scope → Canvas redraws at 60fps for oscilloscope
        currentScreen: ScreenType,
        project: Project,
        cursorRow: Int,
        cursorColumn: Int,
        isPlaying: Boolean,
        playbackRow: Int,
        playbackChainRow: Int,
        playbackSongRow: Int,
        audioEngine: AudioEngine,
        previousColumn: Int,
        currentChain: Int,
        currentPhrase: Int = 0,
        projectCursorRow: Int = 0,
        projectCursorColumn: Int = 1,
        projectStatusMessage: String = "",
        projectStatusSuccess: Boolean = true,
        projectVersion: Int = 0,  // Version counter to force redraw on data changes
        currentInstrument: Int = 0,
        instrumentCursorRow: Int = 0,
        instrumentCursorColumn: Int = 1,
        instrumentStatusMessage: String = "",
        instrumentStatusSuccess: Boolean = true,
        fileBrowserState: FileBrowserModule.State? = null,  // File browser state
        // Copy/paste state
        selectionInfo: String = "",
        clipboardInfo: String = "",
        selectionMode: Boolean = false,
        isCellSelected: (Int, Int) -> Boolean = { _, _ -> false },
        // Mixer state
        mixerCursorColumn: Int = 0,
        trackPeaks: FloatArray = FloatArray(8),
        masterPeaks: FloatArray = FloatArray(2),
        // Table state
        currentTable: Int = 0,
        tableCursorRow: Int = 0,
        tableCursorColumn: Int = 1,
        // Groove state
        currentGroove: Int = 0,
        grooveCursorRow: Int = 0,
        // Modulation state
        modCursorRow: Int = 0,
        modCursorPair: Int = 0,
        modCursorSide: Int = 0,
        // Render state (WAV export)
        isRendering: Boolean = false,
        renderProgress: Float = 0f,
        // Clean dialog state
        showCleanDialog: Boolean = false,
        cleanDialogTarget: String = "",  // "SEQ" or "INST"
        cleanDialogCursor: Int = 0,      // 0 = YES, 1 = NO
        // Layout mode (from CompositionLocal, for display in project screen)
        layoutMode: DeviceAdapter.LayoutMode = DeviceAdapter.LayoutMode.FULL,
        // Song scroll position (viewport start row for 256-row song)
        songScrollPosition: Int = 0,
        // Scaling mode (for project screen display)
        scalingMode: DeviceAdapter.ScalingMode = DeviceAdapter.ScalingMode.INTEGER,
        buttonSoundEnabled: Boolean = false,
        buttonSoundVolume: Int = 255,
        buttonVibroEnabled: Boolean = false,
        vibroPower: Int = 255,
        // QWERTY keyboard overlay state
        qwertyKeyboardState: QwertyKeyboardState = QwertyKeyboardState(),
        // FX helper overlay state
        fxHelperState: FxHelperState = FxHelperState(),
        // Settings screen cursor
        settingsCursorRow: Int = 0,
        settingsCursorColumn: Int = 1,
        // Cursor remember setting (passed through to SettingsState for display)
        cursorRemember: Boolean = false,
        // Track note monitor
        trackNotes: List<Note> = List(8) { Note.EMPTY }
    ) {
        // ===================================
        // DRAW BACKGROUND
        // ===================================
        drawRect(
            color = Color(0xFF0a0a0a),  // Dark background
            topLeft = Offset.Zero,
            size = Size(
                (DESIGN_WIDTH_PX * scale).toFloat(),
                (DESIGN_HEIGHT_PX * scale).toFloat()
            )
        )

        // ===================================
        // LEFT SIDE: Main content modules (620px wide)
        // ===================================

        // Start position: 10px from left edge (centers 620px in 640px)
        val moduleX = SIDE_SPACER
        var currentY = SCREEN_SPACER  // Start 6px from top

        // Update waveform data from native audio engine (every frame)
        // When not playing, decay waveform to smoothly fade out oscilloscope
        audioEngine.updateWaveformWithDecay(isPlaying)

        // MODULE 1: OSCILLOSCOPE (waveform display)
        // Position: Top of screen
        // Size: 620×70
        with(oscilloscope) {
            draw(
                x = moduleX,
                y = currentY,
                scale = scale,
                state = audioEngine.waveformBuffer  // Pass audio waveform data
            )
        }

        // Draw clipboard/selection indicator on the right side of oscilloscope
        if (selectionInfo.isNotEmpty() || clipboardInfo.isNotEmpty()) {
            val indicatorY = currentY + 10  // 10px from top
            val indicatorX = moduleX + 620 - 150  // Right-aligned within module

            // Show selection info (green)
            if (selectionInfo.isNotEmpty()) {
                drawBitmapText(
                    text = selectionInfo,
                    x = indicatorX,
                    y = indicatorY,
                    scale = scale,
                    color = Color(0xFF00DD00),
                    spacing = 2,
                    fontScale = 3
                )
            }

            // Show clipboard info (cyan) below selection info
            if (clipboardInfo.isNotEmpty()) {
                val clipY = if (selectionInfo.isNotEmpty()) indicatorY + 21 else indicatorY
                drawBitmapText(
                    text = clipboardInfo,
                    x = indicatorX,
                    y = clipY,
                    scale = scale,
                    color = Color.Cyan,
                    spacing = 2,
                    fontScale = 3
                )
            }
        }

        // Move down for next module
        currentY += oscilloscope.height + SCREEN_SPACER  // 70 + 6 = 76px

        // MODULE 2: Switch between editors based on current screen.
        // FILE_BROWSER is full-screen and handled outside the clip region.
        // All other editors are clipped to the left of the right bar so that
        // row highlights / wide backgrounds don't bleed into the BPM display,
        // track note monitor, or navigation map.
        val editorClipRight = (DESIGN_WIDTH_PX - navigationMap.width - SIDE_SPACER - SCREEN_SPACER) * scale.toFloat()

        if (currentScreen == ScreenType.FILE_BROWSER) {
            if (fileBrowserState != null) {
                with(fileBrowser) {
                    draw(x = 0, y = 0, scale = scale, state = fileBrowserState)
                }
            } else {
                android.util.Log.e("FileBrowser", "fileBrowserState is NULL - cannot render!")
            }
        } else {
            clipRect(right = editorClipRight) {
                when (currentScreen) {
                    ScreenType.PROJECT -> {
                        with(projectModule) {
                            draw(
                                x = moduleX,
                                y = currentY,
                                scale = scale,
                                state = ProjectState(
                                    project = project,
                                    cursorRow = projectCursorRow,
                                    cursorColumn = projectCursorColumn,
                                    statusMessage = projectStatusMessage,
                                    isSuccess = projectStatusSuccess,
                                    isRendering = isRendering,
                                    renderProgress = renderProgress
                                )
                            )
                        }
                    }
                    // ===================================
                    // PHRASE SCREEN: Show phrase editor
                    // ===================================
                    ScreenType.PHRASE -> {
                        with(phraseEditor) {
                            draw(
                                x = moduleX,
                                y = currentY,
                                scale = scale,
                                state = PhraseEditorState(
                                    phrase = project.phrases[currentPhrase],
                                    cursorRow = cursorRow,
                                    cursorColumn = cursorColumn,
                                    playbackRow = playbackRow,
                                    isPlaying = isPlaying,
                                    selectionMode = selectionMode,
                                    isCellSelected = isCellSelected
                                )
                            )
                        }
                    }

                    // ===================================
                    // CHAIN SCREEN: Show chain editor
                    // ===================================
                    ScreenType.CHAIN -> {
                        with(chainEditor) {
                            draw(
                                x = moduleX,
                                y = currentY,
                                scale = scale,
                                state = ChainEditorState(
                                    chain = project.chains[currentChain],
                                    cursorRow = cursorRow,
                                    cursorColumn = cursorColumn,
                                    playbackRow = playbackChainRow,
                                    isPlaying = isPlaying,
                                    selectionMode = selectionMode,
                                    isCellSelected = isCellSelected
                                )
                            )
                        }
                    }

                    // ===================================
                    // SONG SCREEN: Show song editor
                    // ===================================
                    ScreenType.SONG -> {
                        with(songEditor) {
                            draw(
                                x = moduleX,
                                y = currentY,
                                scale = scale,
                                state = SongEditorState(
                                    project = project,
                                    cursorRow = cursorRow,
                                    cursorTrack = cursorColumn,  // Use cursorColumn as track selector
                                    isPlaying = isPlaying && currentScreen == ScreenType.SONG,
                                    playbackRow = playbackSongRow,
                                    selectionMode = selectionMode,
                                    isCellSelected = isCellSelected,
                                    scrollPosition = songScrollPosition
                                )
                            )
                        }
                    }

                    // ===================================
                    // INSTRUMENT SCREEN: Show instrument editor
                    // ===================================
                    ScreenType.INSTRUMENT -> {
                        with(instrumentModule) {
                            draw(
                                x = moduleX,
                                y = currentY,
                                scale = scale,
                                state = InstrumentState(
                                    instrument = project.instruments[currentInstrument],
                                    cursorRow = instrumentCursorRow,
                                    cursorColumn = instrumentCursorColumn,
                                    statusMessage = instrumentStatusMessage,
                                    isSuccess = instrumentStatusSuccess
                                )
                            )
                        }
                    }

                    // ===================================
                    // TABLE SCREEN: Show table editor
                    // ===================================
                    ScreenType.TABLE -> {
                        with(tableModule) {
                            // Find which track is playing with this table (if any)
                            var tablePlaybackRow: Int? = null
                            for (trackId in 0 until 8) {
                                if (audioEngine.getVoiceTableId(trackId) == currentTable) {
                                    val row = audioEngine.getVoiceTableRow(trackId)
                                    if (row >= 0) {
                                        tablePlaybackRow = row
                                        break
                                    }
                                }
                            }

                            draw(
                                x = moduleX,
                                y = currentY,
                                scale = scale,
                                state = TableState(
                                    table = project.tables[currentTable],
                                    cursorRow = tableCursorRow,
                                    cursorColumn = tableCursorColumn,
                                    playbackRow = tablePlaybackRow,
                                    ticRate = project.instruments.getOrNull(currentInstrument)?.tableTicRate ?: 0x06,
                                    selectionMode = selectionMode,
                                    isCellSelected = isCellSelected
                                )
                            )
                        }
                    }

                    // ===================================
                    // GROOVE SCREEN: Show groove pattern editor
                    // ===================================
                    ScreenType.GROOVE -> {
                        with(grooveModule) {
                            draw(
                                x = moduleX,
                                y = currentY,
                                scale = scale,
                                state = GrooveState(
                                    groove = project.grooves[currentGroove],
                                    cursorRow = grooveCursorRow,
                                    cursorColumn = 1
                                )
                            )
                        }
                    }

                    // ===================================
                    // MODS SCREEN: Show modulation editor
                    // ===================================
                    ScreenType.MODS -> {
                        with(modulationModule) {
                            draw(
                                x = moduleX,
                                y = currentY,
                                scale = scale,
                                state = ModulationState(
                                    instrument = project.instruments[currentInstrument],
                                    cursorRow = modCursorRow,
                                    cursorPair = modCursorPair,
                                    cursorSide = modCursorSide
                                )
                            )
                        }
                    }

                    // ===================================
                    // SETTINGS SCREEN: Show settings side menu
                    // ===================================
                    ScreenType.SETTINGS -> {
                        with(settingsModule) {
                            draw(
                                x = moduleX,
                                y = currentY,
                                scale = scale,
                                state = SettingsState(
                                    cursorRow = settingsCursorRow,
                                    cursorColumn = settingsCursorColumn,
                                    layoutMode = layoutMode,
                                    scalingMode = scalingMode,
                                    buttonSoundEnabled = buttonSoundEnabled,
                                    buttonSoundVolume = buttonSoundVolume,
                                    buttonVibroEnabled = buttonVibroEnabled,
                                    vibroPower = vibroPower,
                                    insertBefore = qwertyKeyboardState.insertBefore,
                                    cursorRemember = cursorRemember
                                )
                            )
                        }
                    }

                    // ===================================
                    // MIXER SCREEN: Show mixer with 8 tracks + master
                    // ===================================
                    ScreenType.MIXER -> {
                        with(mixerModule) {
                            draw(
                                x = moduleX,
                                y = currentY,
                                scale = scale,
                                state = MixerState(
                                    project = project,
                                    cursorColumn = mixerCursorColumn,
                                    trackPeaks = trackPeaks,
                                    masterPeaks = masterPeaks
                                )
                            )
                        }
                    }

                    // ===================================
                    // OTHER SCREENS: Show placeholder
                    // ===================================
                    else -> {
                        drawPlaceholderScreen(
                            x = moduleX,
                            y = currentY,
                            scale = scale,
                            screenType = currentScreen
                        )
                    }
                }
            } // end clipRect
        } // end FILE_BROWSER if/else

        // Left side total: 6 + 70 + 6 + 392 + 6 = 480px ✓

        // ===================================
        // RIGHT SIDE: Navigation map (80px wide)
        // ===================================
        // Note: Hidden when FILE_BROWSER is active to give full screen space

        if (currentScreen != ScreenType.FILE_BROWSER && currentScreen != ScreenType.SETTINGS) {
            val rightBarX = DESIGN_WIDTH_PX - navigationMap.width - SIDE_SPACER  // 515

            // BPM row aligns with column headers on all main editors:
            // editors lay out: header row (21px) + 14px spacer → column headers at y+35
            // Text within row is offset by TEXT_PADDING=3, so text at y+38 absolute = 82+35=117 row, 120 text
            val editorHeaderRowY = SCREEN_SPACER + oscilloscope.height + SCREEN_SPACER  // 82
            val bpmRowY = editorHeaderRowY + 21 + 14  // 117 — same as column header rows in editors
            val bpmTextY = bpmRowY + 3                // 120

            // ===================================
            // RIGHT BAR: BPM row
            // ===================================
            drawBitmapText("T>", rightBarX + 2, bpmTextY, scale, Color(0xFF666666), spacing = 2, fontScale = 3)
            drawBitmapText(project.tempo.toString(), rightBarX + 2 + 34, bpmTextY, scale, Color.White, spacing = 2, fontScale = 3)

            // ===================================
            // RIGHT BAR: Track Note Monitor
            // One spacer row (21px) below BPM, then 8 track rows
            // Format: "1  C-4" — track num in gray, note in white/dim
            // ===================================
            val trackRowsStartY = bpmRowY + 21 + 21  // skip BPM row + one spacer row = 159
            for (i in 0..7) {
                val textY = trackRowsStartY + (i * 21) + 3
                val note = trackNotes.getOrElse(i) { Note.EMPTY }
                val noteColor = if (note == Note.EMPTY) Color(0xFF333333) else Color.White
                // Track number + space + note: "1 " = 2 char-widths (34px) before note
                drawBitmapText((i + 1).toString(), rightBarX + 2, textY, scale, Color(0xFF666666), spacing = 2, fontScale = 3)
                drawBitmapText(note.toString(), rightBarX + 2 + 34, textY, scale, noteColor, spacing = 2, fontScale = 3)
            }

            // ===================================
            // NAVIGATION MAP
            // ===================================
            val navMapX = rightBarX
            val navMapY = DESIGN_HEIGHT_PX - navigationMap.height - SCREEN_SPACER

            with(navigationMap) {
                draw(
                    x = navMapX,
                    y = navMapY,
                    scale = scale,
                    state = NavigationMapState(
                        currentScreen = currentScreen,
                        sourceColumn = previousColumn
                    )
                )
            }
        }

        // ===================================
        // CLEAN CONFIRMATION DIALOG
        // Drawn on top of everything when active
        // ===================================
        if (showCleanDialog) {
            drawCleanDialog(scale, cleanDialogTarget, cleanDialogCursor)
        }

        // ===================================
        // QWERTY KEYBOARD OVERLAY
        // Drawn on top of everything when active
        // ===================================
        if (qwertyKeyboardState.isOpen) {
            drawQwertyKeyboard(qwertyKeyboardState, scale)
        }

        // ===================================
        // FX HELPER OVERLAY
        // Drawn on top of everything when active
        // ===================================
        if (fxHelperState.isOpen) {
            drawFxHelper(fxHelperState, scale)
        }

        // ===================================
        // LAYOUT COMPLETE!
        // ===================================
        // Left column: Oscilloscope + Phrase Editor (or placeholder)
        // Right corner: Navigation Map
    }

    /**
     * Draw the "CLEAN SEQ/INST?" confirmation dialog as a pixel-art overlay.
     * Centered on the 640×480 canvas.
     */
    private fun DrawScope.drawCleanDialog(scale: Int, target: String, cursor: Int) {
        val boxW = 200
        val boxH = 70
        val boxX = (DESIGN_WIDTH_PX - boxW) / 2
        val boxY = (DESIGN_HEIGHT_PX - boxH) / 2

        // Semi-transparent backdrop
        drawRect(
            color = Color(0xCC000000),
            topLeft = Offset.Zero,
            size = Size((DESIGN_WIDTH_PX * scale).toFloat(), (DESIGN_HEIGHT_PX * scale).toFloat())
        )

        // Dialog background
        drawRect(
            color = Color(0xFF1a1a1a),
            topLeft = Offset((boxX * scale).toFloat(), (boxY * scale).toFloat()),
            size = Size((boxW * scale).toFloat(), (boxH * scale).toFloat())
        )

        // Border
        drawRect(
            color = Color(0xFF00CCCC),
            topLeft = Offset((boxX * scale).toFloat(), (boxY * scale).toFloat()),
            size = Size((boxW * scale).toFloat(), (boxH * scale).toFloat()),
            style = androidx.compose.ui.graphics.drawscope.Stroke(width = scale.toFloat())
        )

        val textX = boxX + 10
        val fs = 3
        val cs = 2

        // Title: "CLEAN SEQ?" or "CLEAN INST?"
        drawBitmapText(
            text = "CLEAN $target?",
            x = textX,
            y = boxY + 6,
            scale = scale,
            color = Color.Cyan,
            spacing = cs,
            fontScale = fs
        )

        val yesPrefix = if (cursor == 0) ">" else " "
        drawBitmapText(
            text = "$yesPrefix YES",
            x = textX,
            y = boxY + 27,
            scale = scale,
            color = if (cursor == 0) Color.Yellow else Color.White,
            spacing = cs,
            fontScale = fs
        )

        val noPrefix = if (cursor == 1) ">" else " "
        drawBitmapText(
            text = "$noPrefix NO",
            x = textX,
            y = boxY + 48,
            scale = scale,
            color = if (cursor == 1) Color.Yellow else Color.White,
            spacing = cs,
            fontScale = fs
        )
    }

    // ============================================================================
    // QWERTY KEYBOARD OVERLAY
    // ============================================================================

    /**
     * Draw the QWERTY keyboard overlay.
     *
     * Layout (box = 470×195px, centered at 320,240):
     *   y=  5  Header label (fieldLabel, grey, centered) — fontScale=4 (20px chars)
     *   y= 35  Text input row — dynamic centering, cursor=26px high
     *   y= 71  Key row 0  (Q-P  or  1-0)   — cellH=26px, gap=4px
     *   y=101  Key row 1  (A-L  or  !-_)
     *   y=131  Key row 2  (Z-M  or  <>-])
     *   y=161  Space bar
     *   y=195  → boxH = 195
     *
     * Box: 470×195, top-left at (85, 142)
     */
    private fun DrawScope.drawQwertyKeyboard(state: QwertyKeyboardState, scale: Int) {
        val fs = 4          // font scale for keys (20×20px chars)
        val cs = 3          // char spacing (slightly wider than before)
        val charW = 5 * fs + cs  // 23px per char slot

        // ── Box geometry ──────────────────────────────────────────────────────
        val boxW = 470
        val boxH = 195
        val boxX = (DESIGN_WIDTH_PX - boxW) / 2    // 85
        val boxY = (DESIGN_HEIGHT_PX - boxH) / 2   // 142

        // Inner usable area (5px padding each side)
        val innerX = boxX + 5
        val innerW = boxW - 10   // 460px = 20 chars × 23px

        // ── Semi-transparent backdrop ─────────────────────────────────────────
        drawRect(
            color = Color(0xCC000000),
            topLeft = Offset.Zero,
            size = Size((DESIGN_WIDTH_PX * scale).toFloat(), (DESIGN_HEIGHT_PX * scale).toFloat())
        )

        // ── Dialog box ────────────────────────────────────────────────────────
        drawRect(
            color = Color(0xFF1a1a1a),
            topLeft = Offset((boxX * scale).toFloat(), (boxY * scale).toFloat()),
            size = Size((boxW * scale).toFloat(), (boxH * scale).toFloat())
        )
        drawRect(
            color = Color(0xFF00CCCC),
            topLeft = Offset((boxX * scale).toFloat(), (boxY * scale).toFloat()),
            size = Size((boxW * scale).toFloat(), (boxH * scale).toFloat()),
            style = androidx.compose.ui.graphics.drawscope.Stroke(width = scale.toFloat())
        )

        // ── Context-sensitive header (centered, grey) ─────────────────────────
        val labelW = state.fieldLabel.length * charW
        val labelX = boxX + (boxW - labelW) / 2
        drawBitmapText(
            text = state.fieldLabel,
            x = labelX,
            y = boxY + 5 + 3,  // +3 for text baseline padding
            scale = scale,
            color = Color(0xFF888888),
            spacing = cs,
            fontScale = fs
        )

        // ── Text input row (dynamically centered on current text) ─────────────
        val textRowY = boxY + 35
        val cellH = 26     // unified cell height for both text cursor and key cells
        val boxCenterX = boxX + boxW / 2  // 85 + 235 = 320

        // Center text around the box's horizontal midpoint.
        // When text is empty, cursor sits at centre; as chars are added the group stays centred.
        val textLen = state.text.length
        val textDisplayW = textLen * charW
        val textStartX = (boxCenterX - textDisplayW / 2).coerceAtLeast(innerX)

        // Draw each text character and the cursor highlight
        // Iterate through current text length + 1 to cover the end-of-text cursor position
        for (i in 0..textLen) {
            val charPixelX = textStartX + i * charW
            val isCursor = (i == state.textCursor)

            // Cursor highlight box: full cellH tall, charWidth wide, aligned with character
            if (isCursor) {
                drawRect(
                    color = Color(0xFF444400),
                    topLeft = Offset((charPixelX * scale).toFloat(), (textRowY * scale).toFloat()),
                    size = Size(((5 * fs) * scale).toFloat(), (cellH * scale).toFloat())
                )
            }

            // Draw actual character (skip the end-of-text phantom position)
            if (i < textLen) {
                drawBitmapText(
                    text = state.text[i].toString(),
                    x = charPixelX,
                    y = textRowY + 3,  // baseline padding inside cell
                    scale = scale,
                    color = if (isCursor) Color.Yellow else Color.White,
                    spacing = cs,
                    fontScale = fs
                )
            }
        }

        // ── Key rows ──────────────────────────────────────────────────────────
        val rows = qwertyRowsForLayout(state.layout)
        val cellW = 46     // key cell width (innerW=460 / 10 keys = 46px)
        val rowBaseY = boxY + 71
        val rowGap = 4     // gap between rows

        for (rowIdx in rows.indices) {
            val row = rows[rowIdx]
            val rowY = rowBaseY + rowIdx * (cellH + rowGap)

            if (rowIdx == 3) {
                // Space bar: single wide key, centered
                val spaceW = 7 * cellW   // 322px wide
                val spaceX = boxX + (boxW - spaceW) / 2
                val isSpaceCursor = (state.keyCursorRow == rowIdx)

                drawRect(
                    color = if (isSpaceCursor) Color(0xFF444400) else Color(0xFF2a2a2a),
                    topLeft = Offset((spaceX * scale).toFloat(), (rowY * scale).toFloat()),
                    size = Size((spaceW * scale).toFloat(), (cellH * scale).toFloat())
                )
                if (isSpaceCursor) {
                    drawRect(
                        color = Color.Yellow,
                        topLeft = Offset((spaceX * scale).toFloat(), (rowY * scale).toFloat()),
                        size = Size((spaceW * scale).toFloat(), (cellH * scale).toFloat()),
                        style = androidx.compose.ui.graphics.drawscope.Stroke(width = scale.toFloat())
                    )
                }
                drawBitmapText(
                    text = "SPACE",
                    x = spaceX + (spaceW - 5 * charW) / 2,
                    y = rowY + 3,
                    scale = scale,
                    color = if (isSpaceCursor) Color.Yellow else Color(0xFF888888),
                    spacing = cs,
                    fontScale = fs
                )
            } else {
                // Normal key row: offset to center shorter rows
                val rowTotalW = row.size * cellW
                val rowOffsetX = (innerW - rowTotalW) / 2
                val rowStartX = innerX + rowOffsetX

                for (colIdx in row.indices) {
                    val keyChar = row[colIdx]
                    val cellX = rowStartX + colIdx * cellW
                    val isCursor = (state.keyCursorRow == rowIdx && state.keyCursorCol == colIdx)

                    // Key background
                    drawRect(
                        color = if (isCursor) Color(0xFF444400) else Color(0xFF2a2a2a),
                        topLeft = Offset((cellX * scale).toFloat(), (rowY * scale).toFloat()),
                        size = Size(((cellW - 1) * scale).toFloat(), (cellH * scale).toFloat())
                    )

                    // Cursor border
                    if (isCursor) {
                        drawRect(
                            color = Color.Yellow,
                            topLeft = Offset((cellX * scale).toFloat(), (rowY * scale).toFloat()),
                            size = Size(((cellW - 1) * scale).toFloat(), (cellH * scale).toFloat()),
                            style = androidx.compose.ui.graphics.drawscope.Stroke(width = scale.toFloat())
                        )
                    }

                    // Key character (centered in cell)
                    val charX = cellX + (cellW - 1 - 5 * fs) / 2
                    drawBitmapText(
                        text = keyChar.toString(),
                        x = charX,
                        y = rowY + 3,
                        scale = scale,
                        color = if (isCursor) Color.Yellow else Color.White,
                        spacing = cs,
                        fontScale = fs
                    )
                }
            }
        }
    }

    // ============================================================================
    // FX HELPER OVERLAY
    // ============================================================================

    /**
     * Draw the FX helper overlay.
     *
     * Layout (box = 580×222px, centered on 640×480 canvas):
     *   Description area: up to 4 lines × ROW_HEIGHT (84px)
     *   8px spacer
     *   "EFFECT" header (21px), centered
     *   8px spacer
     *   4 rows × 5 cols effect grid (84px)
     *
     * Box: 580×222, top-left at (30, 129)
     */
    private fun DrawScope.drawFxHelper(state: FxHelperState, scale: Int) {
        val fs = 3          // font scale (15px chars)
        val cs = 2          // char spacing
        val charW = 5 * fs + cs  // 17px per char slot
        val rowH = 21       // row height

        // ── Box geometry ─────────────────────────────────────────────────────
        val boxW = 580
        val boxH = 222
        val boxX = (DESIGN_WIDTH_PX - boxW) / 2    // 30
        val boxY = (DESIGN_HEIGHT_PX - boxH) / 2   // 129
        val innerX = boxX + 10

        // ── Semi-transparent backdrop ─────────────────────────────────────────
        drawRect(
            color = Color(0xCC000000),
            topLeft = Offset.Zero,
            size = Size((DESIGN_WIDTH_PX * scale).toFloat(), (DESIGN_HEIGHT_PX * scale).toFloat())
        )

        // ── Dialog box ────────────────────────────────────────────────────────
        drawRect(
            color = Color(0xFF1a1a1a),
            topLeft = Offset((boxX * scale).toFloat(), (boxY * scale).toFloat()),
            size = Size((boxW * scale).toFloat(), (boxH * scale).toFloat())
        )
        drawRect(
            color = Color(0xFF00CCCC),
            topLeft = Offset((boxX * scale).toFloat(), (boxY * scale).toFloat()),
            size = Size((boxW * scale).toFloat(), (boxH * scale).toFloat()),
            style = androidx.compose.ui.graphics.drawscope.Stroke(width = scale.toFloat())
        )

        // ── Description lines (up to 4) ───────────────────────────────────────
        val descLines = state.descriptionLines()
        var textY = boxY + 8
        for (i in 0 until 4) {
            val line = descLines.getOrNull(i) ?: break
            drawBitmapText(
                text = line,
                x = innerX,
                y = textY + 3,
                scale = scale,
                color = Color(0xFFCCCCCC),
                spacing = cs,
                fontScale = fs
            )
            textY += rowH
        }

        // ── "EFFECT" header (centered, cyan) ──────────────────────────────────
        val headerY = boxY + 8 + 4 * rowH + 8   // 8 + 84 + 8 = 100px from top
        val headerText = "EFFECT"
        val headerW = headerText.length * charW
        val headerX = boxX + (boxW - headerW) / 2
        drawBitmapText(
            text = headerText,
            x = headerX,
            y = headerY + 3,
            scale = scale,
            color = Color.Cyan,
            spacing = cs,
            fontScale = fs
        )

        // ── Effect grid (5 cols × 4 rows) ─────────────────────────────────────
        val gridY = headerY + rowH + 8           // after header + spacer
        val cellW = 96
        val gridCols = 5
        val gridW = gridCols * cellW             // 480px
        val gridX = boxX + (boxW - gridW) / 2   // center in box

        val effectTypes = EffectProcessor.EFFECT_TYPES
        for (i in effectTypes.indices) {
            val row = i / gridCols
            val col = i % gridCols
            val cellX = gridX + col * cellW
            val cellY = gridY + row * rowH

            val isCursor = (state.cursorRow == row && state.cursorCol == col)
            val effectName = getEffectTypeName(effectTypes[i])

            // Cursor highlight background
            if (isCursor) {
                drawRect(
                    color = Color(0xFF444400),
                    topLeft = Offset((cellX * scale).toFloat(), (cellY * scale).toFloat()),
                    size = Size((cellW * scale).toFloat(), (rowH * scale).toFloat())
                )
                drawRect(
                    color = Color.Yellow,
                    topLeft = Offset((cellX * scale).toFloat(), (cellY * scale).toFloat()),
                    size = Size((cellW * scale).toFloat(), (rowH * scale).toFloat()),
                    style = androidx.compose.ui.graphics.drawscope.Stroke(width = scale.toFloat())
                )
            }

            // Effect name centered in cell
            val nameW = 3 * charW
            val nameX = cellX + (cellW - nameW) / 2
            drawBitmapText(
                text = effectName,
                x = nameX,
                y = cellY + 3,
                scale = scale,
                color = when {
                    isCursor -> Color.Yellow
                    effectTypes[i] == EffectProcessor.FX_NONE -> Color(0xFF666666)
                    else -> Color(0xFFaaaaaa)
                },
                spacing = cs,
                fontScale = fs
            )
        }
    }

    /**
     * Draw a placeholder screen for screens that don't have modules yet
     * This shows the screen name and a "Coming Soon" message
     */
    private fun DrawScope.drawPlaceholderScreen(
        x: Int,
        y: Int,
        scale: Int,
        screenType: ScreenType
    ) {
        // Draw background (same size as phrase editor: 620×392)
        drawRect(
            color = Color(0xFF0a0a0a),
            topLeft = Offset((x * scale).toFloat(), (y * scale).toFloat()),
            size = Size((620 * scale).toFloat(), (392 * scale).toFloat())
        )

        // Draw screen title at top (15px font + 3px padding = starts at y+3)
        val titleY = y + 3
        drawBitmapText(
            text = screenType.label.uppercase(),
            x = x + 20,  // 20px from left edge
            y = titleY,
            scale = scale,
            color = Color.Cyan,
            spacing = 2,
            fontScale = 3  // 15×15 font
        )

        // Draw "COMING SOON" message in center
        // Calculate center position
        val messageY = y + 180  // Roughly centered vertically in 392px
        val message = "COMING SOON"

        // Calculate message width to center it
        // Each char is 5×3=15px wide, plus 2px spacing between chars
        val messageWidth = (message.length * 15) + ((message.length - 1) * 2)
        val messageX = x + (620 - messageWidth) / 2

        drawBitmapText(
            text = message,
            x = messageX,
            y = messageY,
            scale = scale,
            color = Color(0xFF666666),  // Dark gray
            spacing = 2,
            fontScale = 3
        )
    }
}

/**
 * BITMAP FONT RENDERING FUNCTIONS
 *
 * These are used by modules to draw text
 */

fun DrawScope.drawBitmapText(
    text: String,
    x: Int,
    y: Int,
    scale: Int,
    color: Color,
    spacing: Int = 2,
    fontScale: Int = 1
) {
    var currentX = x
    for (char in text) {
        drawBitmapChar(char, currentX, y, scale, color, fontScale)
        currentX += (5 * fontScale) + spacing
    }
}

/**
 * Pixel-perfect paint for bitmap font rendering.
 * isAntiAlias = false prevents sub-pixel blending between adjacent pixels.
 */
private val _bitmapPaint = androidx.compose.ui.graphics.Paint().apply {
    isAntiAlias = false
}

fun DrawScope.drawBitmapChar(
    char: Char,
    x: Int,
    y: Int,
    scale: Int,
    color: Color,
    fontScale: Int = 1
) {
    val charData = FONT_5X5[char] ?: FONT_5X5[char.uppercaseChar()]

    if (charData == null) {
        // Missing character - draw outline square
        drawRect(
            color = color,
            topLeft = Offset((x * scale).toFloat(), (y * scale).toFloat()),
            size = Size(
                (5 * fontScale * scale).toFloat(),
                (5 * fontScale * scale).toFloat()
            ),
            style = androidx.compose.ui.graphics.drawscope.Stroke(width = (scale * fontScale).toFloat())
        )
        return
    }

    drawIntoCanvas { canvas ->
        val paint = _bitmapPaint
        paint.color = color
        // Each row: merge consecutive set pixels into a single rect to eliminate internal edges
        for (row in 0..4) {
            val rowData = charData[row].toInt()
            val rowY = ((y + row * fontScale) * scale).toFloat()
            val rowBottom = rowY + (fontScale * scale).toFloat()
            var col = 0
            while (col <= 4) {
                if ((rowData and (1 shl (4 - col))) != 0) {
                    val runStart = col
                    while (col <= 4 && (rowData and (1 shl (4 - col))) != 0) col++
                    val left  = ((x + runStart * fontScale) * scale).toFloat()
                    val right = left + ((col - runStart) * fontScale * scale).toFloat()
                    canvas.drawRect(left, rowY, right, rowBottom, paint)
                } else {
                    col++
                }
            }
        }
    }
}

// Font data is now imported from BitmapFont5x5.kt (internal val FONT_5X5)