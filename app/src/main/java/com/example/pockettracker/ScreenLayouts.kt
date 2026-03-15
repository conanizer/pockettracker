// ============================================================================
// ScreenLayouts.kt
//
// PORTRAIT:   [SPACER] → [SCREEN scaled] → [BUTTONS remaining]
// LANDSCAPE:  [BUTTONS deviceHeight] [SCREEN] [BUTTONS deviceHeight]
// PORTRAIT2:  [SCREEN top] → [compact 4×4 button grid]
// ============================================================================

package com.example.pockettracker

import androidx.compose.foundation.background
import androidx.compose.foundation.focusable
import androidx.compose.foundation.layout.*
import androidx.compose.runtime.Composable
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.focus.FocusRequester
import androidx.compose.ui.focus.focusRequester
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.graphicsLayer
import androidx.compose.ui.unit.dp
import com.example.pockettracker.core.audio.AudioEngine
import com.example.pockettracker.core.data.Project
import com.example.pockettracker.core.data.ScreenType
import com.example.pockettracker.core.logic.PlaybackController

// ============================================================================
// TRACKER SCREEN PARAMS — all PixelPerfectTracker parameters in one bundle.
// Build once in PocketTrackerApp and pass to every layout function.
// ============================================================================
data class TrackerScreenParams(
    val currentScreen: ScreenType,
    val project: Project,
    val audioEngine: AudioEngine,
    val playbackController: PlaybackController,
    val cursorRow: Int,
    val cursorColumn: Int,
    val isPlaying: Boolean,
    val previousColumn: Int,
    val currentChain: Int,
    val currentPhrase: Int,
    val projectCursorRow: Int,
    val projectCursorColumn: Int,
    val projectStatusMessage: String,
    val projectStatusSuccess: Boolean,
    val projectVersion: Int,
    val currentInstrument: Int,
    val instrumentCursorRow: Int,
    val instrumentCursorColumn: Int,
    val instrumentStatusMessage: String,
    val instrumentStatusSuccess: Boolean,
    val fileBrowserState: FileBrowserModule.State? = null,
    val selectionInfo: String = "",
    val clipboardInfo: String = "",
    val selectionMode: Boolean = false,
    val isCellSelected: (Int, Int) -> Boolean = { _, _ -> false },
    val mixerCursorColumn: Int = 0,
    val trackPeaks: FloatArray = FloatArray(8),
    val masterPeaks: FloatArray = FloatArray(2),
    val currentTable: Int = 0,
    val tableCursorRow: Int = 0,
    val tableCursorColumn: Int = 1,
    val currentGroove: Int = 0,
    val grooveCursorRow: Int = 0,
    val modCursorRow: Int = 0,
    val modCursorPair: Int = 0,
    val modCursorSide: Int = 0,
    val isRendering: Boolean = false,
    val renderProgress: Float = 0f,
    val showResampleDialog: Boolean = false,
    val resampleDialogCursor: Int = 0,
    val showCleanDialog: Boolean = false,
    val cleanDialogTarget: String = "",  // "SEQ" or "INST"
    val cleanDialogCursor: Int = 0,      // 0 = YES, 1 = NO
    val songScrollPosition: Int = 0,
    val scalingMode: DeviceAdapter.ScalingMode = DeviceAdapter.ScalingMode.INTEGER
)

/** Thin wrapper — forwards every field of [params] to [PixelPerfectTracker]. */
@Composable
private fun TrackerScreen(params: TrackerScreenParams, modifier: Modifier = Modifier) {
    PixelPerfectTracker(
        currentScreen        = params.currentScreen,
        project              = params.project,
        audioEngine          = params.audioEngine,
        playbackController   = params.playbackController,
        cursorRow            = params.cursorRow,
        cursorColumn         = params.cursorColumn,
        isPlaying            = params.isPlaying,
        previousColumn       = params.previousColumn,
        currentChain         = params.currentChain,
        currentPhrase        = params.currentPhrase,
        projectCursorRow     = params.projectCursorRow,
        projectCursorColumn  = params.projectCursorColumn,
        projectStatusMessage = params.projectStatusMessage,
        projectStatusSuccess = params.projectStatusSuccess,
        projectVersion       = params.projectVersion,
        currentInstrument    = params.currentInstrument,
        instrumentCursorRow  = params.instrumentCursorRow,
        instrumentCursorColumn = params.instrumentCursorColumn,
        instrumentStatusMessage = params.instrumentStatusMessage,
        instrumentStatusSuccess = params.instrumentStatusSuccess,
        fileBrowserState     = params.fileBrowserState,
        selectionInfo        = params.selectionInfo,
        clipboardInfo        = params.clipboardInfo,
        selectionMode        = params.selectionMode,
        isCellSelected       = params.isCellSelected,
        mixerCursorColumn    = params.mixerCursorColumn,
        trackPeaks           = params.trackPeaks,
        masterPeaks          = params.masterPeaks,
        currentTable         = params.currentTable,
        tableCursorRow       = params.tableCursorRow,
        tableCursorColumn    = params.tableCursorColumn,
        currentGroove        = params.currentGroove,
        grooveCursorRow      = params.grooveCursorRow,
        modCursorRow         = params.modCursorRow,
        modCursorPair        = params.modCursorPair,
        modCursorSide        = params.modCursorSide,
        isRendering          = params.isRendering,
        renderProgress       = params.renderProgress,
        showResampleDialog   = params.showResampleDialog,
        resampleDialogCursor = params.resampleDialogCursor,
        showCleanDialog      = params.showCleanDialog,
        cleanDialogTarget    = params.cleanDialogTarget,
        cleanDialogCursor    = params.cleanDialogCursor,
        songScrollPosition   = params.songScrollPosition,
        scalingMode          = params.scalingMode
    )
}

// ============================================================================
// FULL SCREEN LAYOUT
// ============================================================================
@Composable
fun FullScreenLayout(
    layoutConfig: DeviceAdapter.LayoutConfig,
    scalingMode: DeviceAdapter.ScalingMode = DeviceAdapter.ScalingMode.INTEGER,
    params: TrackerScreenParams,
    inputMapper: InputMapper,
    focusRequester: FocusRequester
) {
    val density = androidx.compose.ui.platform.LocalDensity.current.density

    Box(
        modifier = Modifier
            .fillMaxSize()
            .background(Color.Black)
            .focusRequester(focusRequester)
            .inputHandler(inputMapper)
            .focusable(),
        contentAlignment = Alignment.Center
    ) {
        if (scalingMode == DeviceAdapter.ScalingMode.INTEGER) {
            // INTEGER: PixelPerfectTracker auto-calculates integer scale from available space
            TrackerScreen(params)
        } else {
            // BILINEAR / NEAREST: render at integer scale then apply fill factor via graphicsLayer
            val intScale = layoutConfig.screenScale
            val fillFactor = minOf(
                layoutConfig.deviceWidth.toFloat() / (DESIGN_WIDTH_PX * intScale),
                layoutConfig.deviceHeight.toFloat() / (DESIGN_HEIGHT_PX * intScale)
            )
            Box(
                modifier = Modifier
                    .size(
                        width  = (DESIGN_WIDTH_PX  * intScale / density).dp,
                        height = (DESIGN_HEIGHT_PX * intScale / density).dp
                    )
                    .graphicsLayer { scaleX = fillFactor; scaleY = fillFactor }
            ) {
                TrackerScreen(params)
            }
        }
    }
}

// ============================================================================
// PORTRAIT LAYOUT
// Structure: [SPACER 0px] → [SCREEN scaled] → [BUTTONS remaining]
// ============================================================================
@Composable
fun PortraitLayoutWithVirtualButtons(
    layoutConfig: DeviceAdapter.LayoutConfig,
    scalingMode: DeviceAdapter.ScalingMode = DeviceAdapter.ScalingMode.INTEGER,
    params: TrackerScreenParams,
    inputMapper: InputMapper,
    focusRequester: FocusRequester
) {
    val spacerHeight = DeviceAdapter.PORTRAIT_SPACER_HEIGHT
    val density = androidx.compose.ui.platform.LocalDensity.current.density
    val spacerHeightDp = (spacerHeight / density).dp

    // For non-INTEGER scaling, compute a float scale that fills the available screen slot.
    // The button area size is determined by the portrait pattern dimensions.
    val buttonX = kotlin.math.floor(layoutConfig.deviceWidth / DeviceAdapter.PORTRAIT_PATTERN_WIDTH)
    val buttonAreaHeight = kotlin.math.floor(buttonX * DeviceAdapter.PORTRAIT_PATTERN_HEIGHT).toInt()
    val availScreenHeight = (layoutConfig.deviceHeight - spacerHeight - buttonAreaHeight).coerceAtLeast(1)
    val floatScale = minOf(layoutConfig.deviceWidth / 640f, availScreenHeight / 480f)

    val effectiveScale = if (scalingMode == DeviceAdapter.ScalingMode.INTEGER) {
        layoutConfig.screenScale.toFloat()
    } else {
        floatScale
    }
    val effectiveScreenHeight = (480 * effectiveScale).toInt()
    val availableButtonHeight = (layoutConfig.deviceHeight - spacerHeight - effectiveScreenHeight)
        .coerceAtLeast(100)

    Column(
        modifier = Modifier
            .fillMaxSize()
            .background(Color.Black)
            .focusRequester(focusRequester)
            .inputHandler(inputMapper)
            .focusable(),
        horizontalAlignment = Alignment.CenterHorizontally,
        verticalArrangement = Arrangement.Top
    ) {
        // 1. SPACER at top
        Spacer(modifier = Modifier.height(spacerHeightDp))

        // 2. SCREEN (scaled)
        Box(
            modifier = Modifier
                .fillMaxWidth()
                .height((effectiveScreenHeight / density).dp),
            contentAlignment = Alignment.TopCenter
        ) {
            Box(
                modifier = Modifier
                    .size(width = 640.dp, height = 480.dp)
                    .graphicsLayer {
                        scaleX = effectiveScale
                        scaleY = effectiveScale
                    }
            ) {
                TrackerScreen(params)
            }
        }

        // 3. BUTTONS (remaining space)
        Box(
            modifier = Modifier
                .fillMaxWidth()
                .height((availableButtonHeight / density).dp)
        ) {
            VirtualControls(
                inputMapper = inputMapper,
                availableWidth = layoutConfig.deviceWidth,
                availableHeight = availableButtonHeight
            )
        }
    }
}

// ============================================================================
// LANDSCAPE LAYOUT
// Structure: [BUTTONS deviceHeight] [SCREEN] [BUTTONS deviceHeight]
// ============================================================================
@Composable
fun LandscapeLayoutWithVirtualButtons(
    layoutConfig: DeviceAdapter.LayoutConfig,
    scalingMode: DeviceAdapter.ScalingMode = DeviceAdapter.ScalingMode.INTEGER,
    params: TrackerScreenParams,
    inputMapper: InputMapper,
    focusRequester: FocusRequester
) {
    val availableButtonWidth = (layoutConfig.deviceWidth - layoutConfig.scaledScreenWidth) / 2
    val availableButtonHeight = layoutConfig.deviceHeight

    val density = androidx.compose.ui.platform.LocalDensity.current.density
    val buttonWidthDp = maxOf(0f, availableButtonWidth / density).dp

    android.util.Log.d("LandscapeLayout", "=== LANDSCAPE LAYOUT ===")
    android.util.Log.d("LandscapeLayout", "Device: ${layoutConfig.deviceWidth}×${layoutConfig.deviceHeight}")
    android.util.Log.d("LandscapeLayout", "Screen: ${layoutConfig.scaledScreenWidth}×${layoutConfig.scaledScreenHeight}")
    android.util.Log.d("LandscapeLayout", "Button width: $availableButtonWidth px = $buttonWidthDp")
    android.util.Log.d("LandscapeLayout", "Density: $density")
    android.util.Log.d("LandscapeLayout", "====================")

    Box(
        modifier = Modifier
            .fillMaxSize()
            .background(Color.Black)
            .focusRequester(focusRequester)
            .inputHandler(inputMapper)
            .focusable()
    ) {
        // Screen in centre — padding reserves button space on each side
        Box(
            modifier = Modifier
                .fillMaxSize()
                .padding(start = buttonWidthDp, end = buttonWidthDp),
            contentAlignment = Alignment.Center
        ) {
            TrackerScreen(params)
        }

        // LEFT BUTTONS
        Box(
            modifier = Modifier
                .width(buttonWidthDp)
                .fillMaxHeight()
                .align(Alignment.CenterStart)
                .background(Color(0xFF1a1a1a))
        ) {
            VirtualControlsLeft(
                inputMapper = inputMapper,
                availableWidth = availableButtonWidth,
                availableHeight = availableButtonHeight
            )
        }

        // RIGHT BUTTONS
        Box(
            modifier = Modifier
                .width(buttonWidthDp)
                .fillMaxHeight()
                .align(Alignment.CenterEnd)
                .background(Color(0xFF1a1a1a))
        ) {
            VirtualControlsRight(
                inputMapper = inputMapper,
                availableWidth = availableButtonWidth,
                availableHeight = availableButtonHeight
            )
        }
    }
}

// ============================================================================
// PORTRAIT2 LAYOUT
// Structure: [SCREEN top, centered] → [compact 4×4 button grid]
// ============================================================================
@Composable
fun PortraitLayout2WithVirtualButtons(
    layoutConfig: DeviceAdapter.LayoutConfig,
    scalingMode: DeviceAdapter.ScalingMode = DeviceAdapter.ScalingMode.INTEGER,
    params: TrackerScreenParams,
    inputMapper: InputMapper,
    focusRequester: FocusRequester
) {
    val density = androidx.compose.ui.platform.LocalDensity.current.density

    // Button grid height (includes 1X spacer above buttons)
    val buttonAreaHeight = layoutConfig.virtualButtonsHeight

    val remainingHeight = (layoutConfig.deviceHeight - buttonAreaHeight).coerceAtLeast(1)
    val floatScale = minOf(layoutConfig.deviceWidth / 640f, remainingHeight / 480f)
    val effectiveScale = if (scalingMode == DeviceAdapter.ScalingMode.INTEGER) {
        layoutConfig.screenScale.toFloat()
    } else {
        floatScale
    }
    val effectiveScreenHeight = (480 * effectiveScale).toInt()

    // Center [screen + buttons] block vertically on the device
    val totalContentHeight = effectiveScreenHeight + buttonAreaHeight
    val topPadding = ((layoutConfig.deviceHeight - totalContentHeight) / 2).coerceAtLeast(0)

    Column(
        modifier = Modifier
            .fillMaxSize()
            .background(Color.Black)
            .focusRequester(focusRequester)
            .inputHandler(inputMapper)
            .focusable(),
        horizontalAlignment = Alignment.CenterHorizontally,
        verticalArrangement = Arrangement.Top
    ) {
        // 0. TOP SPACER to center content vertically
        Spacer(modifier = Modifier.height((topPadding / density).dp))

        // 1. SCREEN (scaled, centred horizontally)
        Box(
            modifier = Modifier
                .fillMaxWidth()
                .height((effectiveScreenHeight / density).dp),
            contentAlignment = Alignment.TopCenter
        ) {
            Box(
                modifier = Modifier
                    .size(width = 640.dp, height = 480.dp)
                    .graphicsLayer {
                        scaleX = effectiveScale
                        scaleY = effectiveScale
                    }
            ) {
                TrackerScreen(params)
            }
        }

        // 2. BUTTON GRID (compact portrait2 layout, includes 1X spacer at top)
        Box(
            modifier = Modifier
                .fillMaxWidth()
                .height((buttonAreaHeight / density).dp)
        ) {
            VirtualControlsPortrait2(
                inputMapper = inputMapper,
                availableWidth = layoutConfig.deviceWidth,
                availableHeight = buttonAreaHeight.coerceAtLeast(100)
            )
        }
    }
}

// ============================================================================
// CALCULATION EXAMPLES
// ============================================================================
//
// PORTRAIT (1080×2337, scale 1x):
// - Spacer: 0px
// - Screen: 640×480 (scaled to 1x = 480px tall)
// - Available for buttons: 2337 - 0 - 480 = 1857px tall × 1080px wide
// - Box ratio: 1080/1857 = 0.582
// - Pattern ratio: 6.8/5.1 = 1.333
// - 0.582 < 1.333 → WIDTH limits
// - X = 1080 / 6.8 = 158.82 → 158 (floor)
// - Button box: 158 × 6.8 = 1074.4px wide, 158 × 5.1 = 805.8px tall  ✓
//
// LANDSCAPE (2400×1017, scale 2x):
// - Screen: 1280×960 (scaled to 2x)
// - Available for each button panel: (2400 - 1280) / 2 = 560px wide × 1017px tall
// - Box ratio: 560/1017 = 0.551
// - Pattern ratio: 3.4/5.1 = 0.667
// - 0.551 < 0.667 → WIDTH limits
// - X = 560 / 3.4 = 164.70 → 164 (floor)
// - Button box: 164 × 3.4 = 557.6px wide, 164 × 5.1 = 836.4px tall  ✓
//
// ============================================================================
