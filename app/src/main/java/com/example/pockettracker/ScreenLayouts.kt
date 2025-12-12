// ============================================================================
// ScreenLayouts.kt - CORRECT VERSION (Finally!)
//
// PORTRAIT: Spacer (200px) → Screen → Buttons (remaining)
// LANDSCAPE: [Buttons deviceHeight] [Screen] [Buttons deviceHeight]
// ============================================================================

package com.example.pockettracker

import androidx.compose.foundation.background
import androidx.compose.foundation.focusable
import androidx.compose.foundation.layout.*
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.focus.FocusRequester
import androidx.compose.ui.focus.focusRequester
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.graphicsLayer
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp

// ============================================================================
// FULL SCREEN LAYOUT
// ============================================================================
@Composable
fun FullScreenLayout(
    layoutConfig: DeviceAdapter.LayoutConfig,
    currentScreen: ScreenType,
    project: Project,
    audioEngine: TrackerAudioEngine,
    cursorRow: Int,
    cursorColumn: Int,
    isPlaying: Boolean,
    previousColumn: Int,
    currentChain: Int,
    projectCursorRow: Int,
    projectCursorColumn: Int,
    projectStatusMessage: String,
    projectStatusSuccess: Boolean,
    inputMapper: InputMapper,
    focusRequester: FocusRequester
) {
    Box(
        modifier = Modifier
            .fillMaxSize()
            .background(Color.Black)
            .focusRequester(focusRequester)
            .inputHandler(inputMapper)
            .focusable(),
        contentAlignment = Alignment.Center
    ) {
        PixelPerfectTracker(
            currentScreen = currentScreen,
            project = project,
            audioEngine = audioEngine,
            cursorRow = cursorRow,
            cursorColumn = cursorColumn,
            isPlaying = isPlaying,
            previousColumn = previousColumn,
            currentChain = currentChain,
            projectCursorRow = projectCursorRow,
            projectCursorColumn = projectCursorColumn,
            projectStatusMessage = projectStatusMessage,
            projectStatusSuccess = projectStatusSuccess
        )
    }
}

// ============================================================================
// PORTRAIT LAYOUT
// Structure: [SPACER 200px] → [SCREEN scaled] → [BUTTONS remaining]
// ============================================================================
@Composable
fun PortraitLayoutWithVirtualButtons(
    layoutConfig: DeviceAdapter.LayoutConfig,
    currentScreen: ScreenType,
    project: Project,
    audioEngine: TrackerAudioEngine,
    cursorRow: Int,
    cursorColumn: Int,
    isPlaying: Boolean,
    previousColumn: Int,
    currentChain: Int,
    projectCursorRow: Int,
    projectCursorColumn: Int,
    projectStatusMessage: String,
    projectStatusSuccess: Boolean,
    buttonHandlers: ButtonHandlers,
    inputMapper: InputMapper,
    focusRequester: FocusRequester
) {
    // FIXED SPACER HEIGHT
    val spacerHeight = 200

    // Get density for PX → DP conversion
    val density = androidx.compose.ui.platform.LocalDensity.current.density
    val spacerHeightDp = (spacerHeight / density).dp  // ← Convert pixels to DP!

    // Calculate available space for buttons
    // Formula: deviceHeight - spacerHeight - scaledScreenHeight
    val availableButtonHeight = layoutConfig.deviceHeight - spacerHeight - layoutConfig.scaledScreenHeight
    val availableButtonWidth = layoutConfig.deviceWidth  // Full width

    Column(
        modifier = Modifier
            .fillMaxSize()
            .background(Color.Black)
            .focusRequester(focusRequester)
            .inputHandler(inputMapper)
            .focusable(),
        horizontalAlignment = Alignment.CenterHorizontally,
        verticalArrangement = Arrangement.Top  // ← Add this to align to top!
    ) {
        // 1. SPACER at top (200px converted to DP)
        Spacer(modifier = Modifier.height(spacerHeightDp))

        // 2. SCREEN (scaled) - aligned to top
        Box(
            modifier = Modifier
                .fillMaxWidth()
                .height((layoutConfig.scaledScreenHeight / density).dp),  // ← Convert to DP!
            contentAlignment = Alignment.TopCenter
        ) {
            // Apply scaling to base 640×480 screen
            Box(
                modifier = Modifier
                    .size(width = 640.dp, height = 480.dp)
                    .graphicsLayer {
                        val scale = layoutConfig.screenScale.toFloat()
                        scaleX = scale
                        scaleY = scale
                    }
            ) {
                PixelPerfectTracker(
                    currentScreen = currentScreen,
                    project = project,
                    audioEngine = audioEngine,
                    cursorRow = cursorRow,
                    cursorColumn = cursorColumn,
                    isPlaying = isPlaying,
                    previousColumn = previousColumn,
                    currentChain = currentChain,
                    projectCursorRow = projectCursorRow,
                    projectCursorColumn = projectCursorColumn,
                    projectStatusMessage = projectStatusMessage,
                    projectStatusSuccess = projectStatusSuccess
                )
            }
        }

        // 3. BUTTONS (remaining space)
        Box(
            modifier = Modifier
                .fillMaxWidth()
                .height((availableButtonHeight / density).dp)  // ← Convert to DP!
        ) {
            VirtualControls(
                onDPadUp = buttonHandlers.onDPadUp,
                onDPadDown = buttonHandlers.onDPadDown,
                onDPadLeft = buttonHandlers.onDPadLeft,
                onDPadRight = buttonHandlers.onDPadRight,
                onButtonA = buttonHandlers.onButtonA,
                onButtonB = buttonHandlers.onButtonB,
                onSelect = buttonHandlers.onSelect,
                onStart = buttonHandlers.onStart,
                onL = buttonHandlers.onL,
                onR = buttonHandlers.onR,
                // Pass the CALCULATED available space
                availableWidth = availableButtonWidth,
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
    currentScreen: ScreenType,
    project: Project,
    audioEngine: TrackerAudioEngine,
    cursorRow: Int,
    cursorColumn: Int,
    isPlaying: Boolean,
    previousColumn: Int,
    currentChain: Int,
    projectCursorRow: Int,
    projectCursorColumn: Int,
    projectStatusMessage: String,
    projectStatusSuccess: Boolean,
    buttonHandlers: ButtonHandlers,
    inputMapper: InputMapper,
    focusRequester: FocusRequester
) {
    // Calculate available space for each button panel
    // Formula: (deviceWidth - scaledScreenWidth) / 2
    val availableButtonWidth = (layoutConfig.deviceWidth - layoutConfig.scaledScreenWidth) / 2
    val availableButtonHeight = layoutConfig.deviceHeight  // FULL device height!

    // Get density for PX → DP conversion
    val density = androidx.compose.ui.platform.LocalDensity.current.density
    val buttonWidthDp = (availableButtonWidth / density).dp
    val screenWidthDp = ((layoutConfig.scaledScreenWidth - 3) / density).dp  // ← Subtract 3px to avoid exact fit!

    // DEBUG LOGGING
    android.util.Log.d("LandscapeLayout", "=== LANDSCAPE LAYOUT ===")
    android.util.Log.d("LandscapeLayout", "Device: ${layoutConfig.deviceWidth}×${layoutConfig.deviceHeight}")
    android.util.Log.d("LandscapeLayout", "Screen: ${layoutConfig.scaledScreenWidth}×${layoutConfig.scaledScreenHeight}")
    android.util.Log.d("LandscapeLayout", "Button width: $availableButtonWidth px = $buttonWidthDp")
    android.util.Log.d("LandscapeLayout", "Screen width: ${layoutConfig.scaledScreenWidth} px = $screenWidthDp")
    android.util.Log.d("LandscapeLayout", "Total: ${buttonWidthDp.value} + ${screenWidthDp.value} + ${buttonWidthDp.value} = ${buttonWidthDp.value * 2 + screenWidthDp.value} dp")
    android.util.Log.d("LandscapeLayout", "Density: $density")
    android.util.Log.d("LandscapeLayout", "====================")

    // WORKAROUND: Use overlapping layout with negative offset
    Box(
        modifier = Modifier
            .fillMaxSize()
            .background(Color.Black)
            .focusRequester(focusRequester)
            .inputHandler(inputMapper)
            .focusable()
    ) {
        // Put screen in center FIRST
        Box(
            modifier = Modifier
                .fillMaxSize()
                .padding(start = buttonWidthDp, end = buttonWidthDp),  // Leave room for buttons
            contentAlignment = Alignment.Center
        ) {
            android.util.Log.d("LandscapeLayout", "CENTER BOX composing: ${screenWidthDp}")
            Box(
                modifier = Modifier
                    .size(width = 640.dp, height = 480.dp)
                    .graphicsLayer {
                        val scale = layoutConfig.screenScale.toFloat()
                        scaleX = scale
                        scaleY = scale
                    }
            ) {
                PixelPerfectTracker(
                    currentScreen = currentScreen,
                    project = project,
                    audioEngine = audioEngine,
                    cursorRow = cursorRow,
                    cursorColumn = cursorColumn,
                    isPlaying = isPlaying,
                    previousColumn = previousColumn,
                    currentChain = currentChain,
                    projectCursorRow = projectCursorRow,
                    projectCursorColumn = projectCursorColumn,
                    projectStatusMessage = projectStatusMessage,
                    projectStatusSuccess = projectStatusSuccess
                )
            }
        }

        // LEFT BUTTONS - Overlay on left using Alignment
        Box(
            modifier = Modifier
                .width(buttonWidthDp)
                .fillMaxHeight()
                .align(Alignment.CenterStart)  // ← Align to left!
                .background(Color(0xFF1a1a1a))
        ) {
            android.util.Log.d("LandscapeLayout", "LEFT BOX composing: ${buttonWidthDp}")
            VirtualControlsLeft(
                onDPadUp = buttonHandlers.onDPadUp,
                onDPadDown = buttonHandlers.onDPadDown,
                onDPadLeft = buttonHandlers.onDPadLeft,
                onDPadRight = buttonHandlers.onDPadRight,
                onL = buttonHandlers.onL,
                onSelect = buttonHandlers.onSelect,
                availableWidth = availableButtonWidth,
                availableHeight = availableButtonHeight
            )
        }

        // RIGHT BUTTONS - Overlay on right using Alignment
        Box(
            modifier = Modifier
                .width(buttonWidthDp)
                .fillMaxHeight()
                .align(Alignment.CenterEnd)  // ← Align to right!
                .background(Color(0xFF1a1a1a))
        ) {
            android.util.Log.d("LandscapeLayout", "RIGHT BOX composing: ${buttonWidthDp}")
            VirtualControlsRight(
                onButtonA = buttonHandlers.onButtonA,
                onButtonB = buttonHandlers.onButtonB,
                onR = buttonHandlers.onR,
                onStart = buttonHandlers.onStart,
                // Pass the CALCULATED available space
                availableWidth = availableButtonWidth,
                availableHeight = availableButtonHeight  // FULL device height!
            )
        }
    }
}

// ============================================================================
// CALCULATION EXAMPLES
// ============================================================================
//
// PORTRAIT (1080×2337, scale 1x):
// - Spacer: 200px
// - Screen: 640×480 (scaled to 1x = 480px tall)
// - Available for buttons: 2337 - 200 - 480 = 1657px tall × 1080px wide
// - Box ratio: 1080/1657 = 0.652
// - Pattern ratio: 6.8/5.1 = 1.333
// - 0.652 < 1.333 → WIDTH limits
// - X = 1080 / 6.8 = 158.82 → 158 (floor)
// - Button box: 158 × 6.8 = 1074.4px wide, 158 × 5.1 = 805.8px tall
// - Fits in 1657×1080? YES ✓
//
// LANDSCAPE (2400×1017, scale 2x):
// - Screen: 1280×960 (scaled to 2x)
// - Available for each button panel: (2400 - 1280) / 2 = 560px wide × 1017px tall
// - Box ratio: 560/1017 = 0.551
// - Pattern ratio: 3.4/5.1 = 0.667
// - 0.551 < 0.667 → WIDTH limits
// - X = 560 / 3.4 = 164.70 → 164 (floor)
// - Button box: 164 × 3.4 = 557.6px wide, 164 × 5.1 = 836.4px tall
// - Fits in 560×1017? YES ✓
//
// ============================================================================