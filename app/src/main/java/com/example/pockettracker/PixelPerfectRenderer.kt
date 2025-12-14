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
    audioEngine: TrackerAudioEngine,
    cursorRow: Int,
    cursorColumn: Int,
    isPlaying: Boolean,
    previousColumn: Int,
    currentChain: Int,
    currentPhrase: Int,
    projectCursorRow: Int,
    projectCursorColumn: Int,
    projectStatusMessage: String,
    projectStatusSuccess: Boolean
) {
    // Playback state
    var playbackRow by remember { mutableStateOf(0) }

    // Playback loop
    LaunchedEffect(isPlaying) {
        if (isPlaying) {
            while (isPlaying) {
                val step = project.phrases[0].steps[playbackRow]
                if (!step.isEmpty()) {
                    audioEngine.playNote(step.note, step.instrument, 0, step.volume / 255f)
                }
                playbackRow = (playbackRow + 1) % 16
                delay(60000L / 128 / 4)  // 128 BPM, 16th notes
            }
        } else {
            playbackRow = 0
            audioEngine.stopAll()
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

        // Main canvas
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
                        audioEngine = audioEngine,
                        previousColumn = previousColumn,
                        currentChain = currentChain,
                        currentPhrase = currentPhrase,
                        projectCursorRow = projectCursorRow,
                        projectCursorColumn = projectCursorColumn,
                        projectStatusMessage = projectStatusMessage,
                        projectStatusSuccess = projectStatusSuccess
                    )
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
    private val chainEditor = ChainEditorModule()
    private val songEditor = SongEditorModule()
    private val projectModule = ProjectModule()
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
        audioEngine: TrackerAudioEngine,
        previousColumn: Int,
        currentChain: Int,
        currentPhrase: Int = 0,
        projectCursorRow: Int = 0,
        projectCursorColumn: Int = 1,
        projectStatusMessage: String = "",
        projectStatusSuccess: Boolean = true
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
                            chain = project.chains[currentChain],  // Show first chain for now
                            cursorRow = cursorRow,
                            cursorColumn = cursorColumn
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
                            cursorTrack = cursorColumn  // Use cursorColumn as track selector
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

        // Left side total: 6 + 70 + 6 + 392 + 6 = 480px ✓

        // ===================================
        // RIGHT SIDE: Navigation map (80px wide)
        // ===================================

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
    val charData = FONT_5X5[char.uppercaseChar()]

    if (charData == null) {
        // Missing character - draw outline square
        drawRect(
            color = color,
            topLeft = Offset((x * scale).toFloat(), (y * scale).toFloat()),
            size = androidx.compose.ui.geometry.Size(
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
                    size = androidx.compose.ui.geometry.Size(
                        (scale * fontScale).toFloat(),
                        (scale * fontScale).toFloat()
                    )
                )
            }
        }
    }
}

// Font data
private val FONT_5X5 = mapOf(
    '0' to byteArrayOf(0b01110, 0b10001, 0b10001, 0b10001, 0b01110),
    '1' to byteArrayOf(0b00100, 0b01100, 0b00100, 0b00100, 0b01110),
    '2' to byteArrayOf(0b01110, 0b10001, 0b00010, 0b00100, 0b11111),
    '3' to byteArrayOf(0b11111, 0b00010, 0b00110, 0b00001, 0b11110),
    '4' to byteArrayOf(0b10001, 0b10001, 0b11111, 0b00001, 0b00001),
    '5' to byteArrayOf(0b11111, 0b10000, 0b11110, 0b00001, 0b11110),
    '6' to byteArrayOf(0b01110, 0b10000, 0b11110, 0b10001, 0b01110),
    '7' to byteArrayOf(0b11111, 0b00001, 0b00010, 0b00100, 0b01000),
    '8' to byteArrayOf(0b01110, 0b10001, 0b01110, 0b10001, 0b01110),
    '9' to byteArrayOf(0b01110, 0b10001, 0b01111, 0b00001, 0b01110),
    'A' to byteArrayOf(0b01110, 0b10001, 0b11111, 0b10001, 0b10001),
    'B' to byteArrayOf(0b11110, 0b10001, 0b11110, 0b10001, 0b11110),
    'C' to byteArrayOf(0b01110, 0b10001, 0b10000, 0b10001, 0b01110),
    'D' to byteArrayOf(0b11110, 0b10001, 0b10001, 0b10001, 0b11110),
    'E' to byteArrayOf(0b11111, 0b10000, 0b11110, 0b10000, 0b11111),
    'F' to byteArrayOf(0b11111, 0b10000, 0b11110, 0b10000, 0b10000),
    'G' to byteArrayOf(0b01110, 0b10000, 0b10011, 0b10001, 0b01110),
    'H' to byteArrayOf(0b10001, 0b10001, 0b11111, 0b10001, 0b10001),
    'I' to byteArrayOf(0b01110, 0b00100, 0b00100, 0b00100, 0b01110),
    'J' to byteArrayOf(0b00111, 0b00001, 0b00001, 0b10001, 0b01110),
    'K' to byteArrayOf(0b10001, 0b10010, 0b11100, 0b10010, 0b10001),
    'L' to byteArrayOf(0b10000, 0b10000, 0b10000, 0b10000, 0b11111),
    'M' to byteArrayOf(0b10001, 0b11011, 0b10101, 0b10001, 0b10001),
    'N' to byteArrayOf(0b10001, 0b11001, 0b10101, 0b10011, 0b10001),
    'O' to byteArrayOf(0b01110, 0b10001, 0b10001, 0b10001, 0b01110),
    'P' to byteArrayOf(0b11110, 0b10001, 0b11110, 0b10000, 0b10000),
    'Q' to byteArrayOf(0b01110, 0b10001, 0b10001, 0b10010, 0b01101),
    'R' to byteArrayOf(0b11110, 0b10001, 0b11110, 0b10010, 0b10001),
    'S' to byteArrayOf(0b01111, 0b10000, 0b01110, 0b00001, 0b11110),
    'T' to byteArrayOf(0b11111, 0b00100, 0b00100, 0b00100, 0b00100),
    'U' to byteArrayOf(0b10001, 0b10001, 0b10001, 0b10001, 0b01110),
    'V' to byteArrayOf(0b10001, 0b10001, 0b10001, 0b01010, 0b00100),
    'W' to byteArrayOf(0b10001, 0b10001, 0b10101, 0b11011, 0b10001),
    'X' to byteArrayOf(0b10001, 0b01010, 0b00100, 0b01010, 0b10001),
    'Y' to byteArrayOf(0b10001, 0b01010, 0b00100, 0b00100, 0b00100),
    'Z' to byteArrayOf(0b11111, 0b00010, 0b00100, 0b01000, 0b11111),
    '-' to byteArrayOf(0b00000, 0b00000, 0b11111, 0b00000, 0b00000),
    '#' to byteArrayOf(0b01010, 0b11111, 0b01010, 0b11111, 0b01010),
    '.' to byteArrayOf(0b00000, 0b00000, 0b00000, 0b00000, 0b00100),
    ':' to byteArrayOf(0b00000, 0b00100, 0b00000, 0b00100, 0b00000),
    '/' to byteArrayOf(0b00001, 0b00010, 0b00100, 0b01000, 0b10000),
    ' ' to byteArrayOf(0b00000, 0b00000, 0b00000, 0b00000, 0b00000),
)