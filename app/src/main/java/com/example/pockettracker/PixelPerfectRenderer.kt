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
import androidx.compose.ui.graphics.drawscope.translate
import androidx.compose.ui.platform.LocalDensity
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
    fileBrowserState: FileBrowserModule.State? = null
) {
    android.util.Log.d("PixelPerfectTracker", "==== PixelPerfectTracker called ====")
    android.util.Log.d("PixelPerfectTracker", "Screen: $currentScreen")
    android.util.Log.d("PixelPerfectTracker", "Instrument cursor: row=$instrumentCursorRow, col=$instrumentCursorColumn")

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

    val density = LocalDensity.current

    // Main container with letterboxing
    BoxWithConstraints(
        modifier = Modifier
            .fillMaxSize()
            .background(Color.Black)
    ) {
        // Calculate scale factor
        val screenWidthPx = with(density) { maxWidth.toPx() }.toInt()
        val screenHeightPx = with(density) { maxHeight.toPx() }.toInt()

        val scaleX = screenWidthPx / DESIGN_WIDTH_PX
        val scaleY = screenHeightPx / DESIGN_HEIGHT_PX
        val scale = min(scaleX, scaleY).coerceAtLeast(1)

        val renderWidth = DESIGN_WIDTH_PX * scale
        val renderHeight = DESIGN_HEIGHT_PX * scale

        // Center the content
        val offsetX = (screenWidthPx - renderWidth) / 2f
        val offsetY = (screenHeightPx - renderHeight) / 2f

        // Main canvas - use key() to force redraw when state changes
        // Key on: projectVersion, screen type, cursor positions, and oscilloscope ticker (for smooth waveform)
        key(projectVersion, currentScreen, cursorRow, cursorColumn, projectCursorRow, projectCursorColumn, instrumentCursorRow, instrumentCursorColumn, oscilloscopeTicker) {
            Canvas(modifier = Modifier.fillMaxSize()) {
                translate(offsetX, offsetY) {
                    // Use layout manager to draw modules
                    val layout = TrackerLayout()
                    with(layout) {
                        drawLayout(
                            scale = scale,
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
                            fileBrowserState = fileBrowserState
                        )
                    }
                }
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
    private val chainEditor = ChainEditorModule()
    private val songEditor = SongEditorModule()
    private val projectModule = ProjectModule()
    private val fileBrowser = FileBrowserModule()
    /**
     * Main layout drawing function
     * This arranges all modules on the 640×480 screen
     */
    fun DrawScope.drawLayout(
        scale: Int,
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
        fileBrowserState: FileBrowserModule.State? = null  // File browser state
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
        // NOTE: Adjust update rate here if needed (e.g., skip frames for slower update)
        audioEngine.updateWaveform()

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
        // Move down for next module
        currentY += oscilloscope.height + SCREEN_SPACER  // 70 + 6 = 76px

        // MODULE 2: PHRASE EDITOR (main content)
        // Position: Below oscilloscope
        // Size: 620×392
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
                    isPlaying = isPlaying
                )
            )
        }

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
                            cursorRow = projectCursorRow,  // Pass cursor state
                            cursorColumn = projectCursorColumn,
                            statusMessage = projectStatusMessage,
                            isSuccess = projectStatusSuccess
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
                            isPlaying = isPlaying
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
                            isPlaying = isPlaying
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
                            playbackRow = playbackSongRow
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

        if (currentScreen != ScreenType.FILE_BROWSER) {
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
        // LAYOUT COMPLETE!
        // ===================================
        // Left column: Oscilloscope + Phrase Editor (or placeholder)
        // Right corner: Navigation Map
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

fun DrawScope.drawBitmapChar(
    char: Char,
    x: Int,
    y: Int,
    scale: Int,
    color: Color,
    fontScale: Int = 1
) {
    // Try the character as-is first, then fall back to uppercase for letters
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

    // Draw each pixel of the 5×5 bitmap
    for (row in 0..4) {
        val rowData = charData[row].toInt()
        for (col in 0..4) {
            val isSet = (rowData and (1 shl (4 - col))) != 0
            if (isSet) {
                drawRect(
                    color = color,
                    topLeft = Offset(
                        ((x + col * fontScale) * scale).toFloat(),
                        ((y + row * fontScale) * scale).toFloat()
                    ),
                    size = Size(
                        (scale * fontScale).toFloat(),
                        (scale * fontScale).toFloat()
                    )
                )
            }
        }
    }
}

// Font data is now imported from BitmapFont5x5.kt (internal val FONT_5X5)