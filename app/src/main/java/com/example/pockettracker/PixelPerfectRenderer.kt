package com.example.pockettracker

import androidx.compose.foundation.Canvas
import androidx.compose.foundation.background
import androidx.compose.foundation.layout.*
import androidx.compose.runtime.*
import androidx.compose.ui.Modifier
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.geometry.Size
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.drawscope.DrawScope
import androidx.compose.ui.graphics.drawscope.drawIntoCanvas
import androidx.compose.ui.graphics.drawscope.translate
import kotlin.math.min
import kotlinx.coroutines.delay
import com.example.pockettracker.core.audio.AudioEngine
import com.example.pockettracker.core.data.Project
import com.example.pockettracker.core.data.ScreenType

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
    playbackController: com.example.pockettracker.core.logic.PlaybackController,
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
    // Settings screen cursor
    settingsCursorRow: Int = 0,
    settingsCursorColumn: Int = 1
) {
    if (currentScreen == ScreenType.FILE_BROWSER) {
        android.util.Log.d("PixelPerfectTracker", "FILE_BROWSER screen, fileBrowserState=${if (fileBrowserState != null) "not null (${fileBrowserState.items.size} items)" else "NULL"}")
    }
    // Playback state
    var playbackRow by remember { mutableStateOf(0) }
    var playbackChainRow by remember { mutableStateOf(0) }
    var playbackPhraseStep by remember { mutableStateOf(0) }
    var playbackSongRow by remember { mutableStateOf(0) }

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
    LaunchedEffect(isPlaying, currentScreen) {
        if (isPlaying) {
            while (isPlaying) {
                // Update lookahead buffer - PlaybackController handles all scheduling
                playbackController.updatePlaybackBuffer()

                // Get current playback position for UI updates
                val position = playbackController.getPlaybackPosition()
                playbackRow = position.row
                playbackChainRow = position.chainRow
                playbackPhraseStep = position.phraseStep
                playbackSongRow = position.songRow

                // Update UI at 60 Hz
                delay(16L)
            }
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
                        settingsCursorRow = settingsCursorRow,
                        settingsCursorColumn = settingsCursorColumn
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
        // Settings screen cursor
        settingsCursorRow: Int = 0,
        settingsCursorColumn: Int = 1
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

        // MODULE 2: Switch between editors based on current screen
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
                            insertBefore = qwertyKeyboardState.insertBefore
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
            // FILE BROWSER: Full screen file selection
            // ===================================
            ScreenType.FILE_BROWSER -> {
                // Debug log removed - was spamming logcat on every frame (60+ fps)
                if (fileBrowserState != null) {
                    with(fileBrowser) {
                        draw(
                            x = 0,  // Full screen: start at 0, 0
                            y = 0,
                            scale = scale,
                            state = fileBrowserState
                        )
                    }
                } else {
                    android.util.Log.e("FileBrowser", "fileBrowserState is NULL - cannot render!")
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

        // Left side total: 6 + 70 + 6 + 392 + 6 = 480px ✓

        // ===================================
        // RIGHT SIDE: Navigation map (80px wide)
        // ===================================
        // Note: Hidden when FILE_BROWSER is active to give full screen space

        if (currentScreen != ScreenType.FILE_BROWSER && currentScreen != ScreenType.SETTINGS) {
            // Calculate position for bottom-right corner
            // X position: 640 (screen width) - 80 (module width) - 10 (right margin) = 550px
            val navMapX = DESIGN_WIDTH_PX - navigationMap.width - SIDE_SPACER

            // Y position: 480 (screen height) - 105 (module height) - 6 (bottom margin) = 369px
            val navMapY = DESIGN_HEIGHT_PX - navigationMap.height - SCREEN_SPACER

            // MODULE 3: NAVIGATION MAP (shows current position in screen hierarchy)
            // Position: Bottom-right corner
            // Size: 80×105
            with(navigationMap) {
                draw(
                    x = navMapX,
                    y = navMapY,
                    scale = scale,
                    state = NavigationMapState(
                        currentScreen = currentScreen,
                        sourceColumn = previousColumn  // ✨ Use the passed value
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
     * Layout (box = 450×172px, centered at 320,240):
     *   y=  5  Header label (fieldLabel, grey, centered) — fontScale=4 (20px chars)
     *   y= 30  Text input row — 20 chars × 22px wide, centered
     *   y= 60  Key row 0  (Q-P  or  1-0)   — cellH=26px
     *   y= 90  Key row 1  (A-L  or  !-_)
     *   y=120  Key row 2  (Z-M  or  <>-])
     *   y=150  Space bar
     *   y=176  → boxH = 176
     *
     * Box: 450×176, top-left at (95, 152), bottom-right at (545, 328)
     */
    private fun DrawScope.drawQwertyKeyboard(state: QwertyKeyboardState, scale: Int) {
        val fs = 4          // font scale for keys (20×20px chars)
        val cs = 2          // char spacing
        val charW = 5 * fs + cs  // 22px per char slot

        // ── Box geometry ──────────────────────────────────────────────────────
        // Layout (top-to-bottom, coords relative to boxY):
        //   +5   header label row      (h=20 chars + 5 padding = 25)
        //   +30  text input row        (h=26)
        //   +60  key row 0 Q-P / 1-0   (cellH=26, gap=4 → row spacing=30)
        //   +90  key row 1 A-L / !-_
        //   +120 key row 2 Z-M / <>-]
        //   +150 space bar             (h=26)
        //   +176 → boxH = 176
        val boxW = 450
        val boxH = 176
        val boxX = (DESIGN_WIDTH_PX - boxW) / 2    // 95
        val boxY = (DESIGN_HEIGHT_PX - boxH) / 2   // 152

        // Inner usable area (5px padding each side)
        val innerX = boxX + 5
        val innerW = boxW - 10   // 440px = 20 chars × 22px

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

        // ── Text input row (centered in inner area) ───────────────────────────
        val textRowY = boxY + 30

        // Draw text characters with cursor highlight.
        // textCursor can be 0..text.length (inclusive).
        // No dash placeholder for empty slots — only cursor highlight is shown.
        for (i in 0 until state.maxLength) {
            val charPixelX = innerX + i * charW
            val isCursor = (i == state.textCursor)
            val hasChar = i < state.text.length

            // Cursor highlight box
            if (isCursor) {
                drawRect(
                    color = Color(0xFF444400),
                    topLeft = Offset((charPixelX * scale).toFloat(), (textRowY * scale).toFloat()),
                    size = Size(((5 * fs) * scale).toFloat(), (20 * scale).toFloat())
                )
            }

            // Only draw the character if there's actual text here
            if (hasChar) {
                drawBitmapText(
                    text = state.text[i].toString(),
                    x = charPixelX,
                    y = textRowY + 3,
                    scale = scale,
                    color = if (isCursor) Color.Yellow else Color.White,
                    spacing = cs,
                    fontScale = fs
                )
            }
        }

        // ── Key rows ──────────────────────────────────────────────────────────
        val rows = qwertyRowsForLayout(state.layout)
        val cellW = 44     // key cell width (innerW=440 / 10 keys = 44px)
        val cellH = 26     // key cell height (20px char + 6px padding)
        val rowBaseY = boxY + 60
        val rowGap = 4     // gap between rows

        for (rowIdx in rows.indices) {
            val row = rows[rowIdx]
            val rowY = rowBaseY + rowIdx * (cellH + rowGap)

            if (rowIdx == 3) {
                // Space bar: single wide key, centered
                val spaceW = 7 * cellW   // 308px wide
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