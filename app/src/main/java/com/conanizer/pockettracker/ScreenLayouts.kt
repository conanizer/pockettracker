// ============================================================================
// ScreenLayouts.kt
//
// PORTRAIT:   [SPACER] → [SCREEN scaled] → [BUTTONS remaining]
// LANDSCAPE:  [BUTTONS deviceHeight] [SCREEN] [BUTTONS deviceHeight]
// PORTRAIT2:  [SCREEN top] → [compact 4×4 button grid]
// ============================================================================

package com.conanizer.pockettracker

import androidx.compose.foundation.background
import androidx.compose.foundation.focusable
import androidx.compose.foundation.layout.*
import androidx.compose.runtime.Composable
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.paint
import androidx.compose.ui.focus.FocusRequester
import androidx.compose.ui.focus.focusRequester
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.ImageBitmap
import androidx.compose.ui.graphics.TransformOrigin
import androidx.compose.ui.graphics.graphicsLayer
import androidx.compose.ui.graphics.painter.BitmapPainter
import androidx.compose.ui.layout.ContentScale
import androidx.compose.ui.platform.LocalDensity
import androidx.compose.ui.unit.dp
import com.conanizer.pockettracker.core.ui.DeviceTheme
import kotlin.math.floor
import com.conanizer.pockettracker.core.audio.AudioEngine
import com.conanizer.pockettracker.core.data.Project
import com.conanizer.pockettracker.core.data.ScreenType
import com.conanizer.pockettracker.core.logic.PlaybackController

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
    val showCleanDialog: Boolean = false,
    val cleanDialogTarget: String = "",  // "SEQ" or "INST"
    val cleanDialogCursor: Int = 0,      // 0 = YES, 1 = NO
    val songScrollPosition: Int = 0,
    val scalingMode: DeviceAdapter.ScalingMode = DeviceAdapter.ScalingMode.INTEGER,
    val buttonSoundEnabled: Boolean = false,
    val buttonSoundVolume: Int = 255,
    val buttonVibroEnabled: Boolean = false,
    val vibroPower: Int = 255,
    val qwertyKeyboardState: QwertyKeyboardState = QwertyKeyboardState(),
    val fxHelperState: FxHelperState = FxHelperState(),
    val settingsCursorRow: Int = 0,
    val settingsCursorColumn: Int = 1,
    val cursorRemember: Boolean = false
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
        showCleanDialog      = params.showCleanDialog,
        cleanDialogTarget    = params.cleanDialogTarget,
        cleanDialogCursor    = params.cleanDialogCursor,
        songScrollPosition   = params.songScrollPosition,
        scalingMode          = params.scalingMode,
        buttonSoundEnabled   = params.buttonSoundEnabled,
        buttonSoundVolume    = params.buttonSoundVolume,
        buttonVibroEnabled   = params.buttonVibroEnabled,
        vibroPower           = params.vibroPower,
        qwertyKeyboardState  = params.qwertyKeyboardState,
        fxHelperState        = params.fxHelperState,
        settingsCursorRow    = params.settingsCursorRow,
        settingsCursorColumn = params.settingsCursorColumn,
        cursorRemember       = params.cursorRemember
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
// PORTRAIT2 LAYOUT — Retro device skin (20:9 phones)
//
// Structure (all heights in X-units, X = deviceWidth / 135):
//   1. TOP PANEL     — ventilation grille, 39.75X tall
//   2. SCREEN BEZEL  — frame + tracker screen inside, 102.75X tall
//   3. BRANDING STRIP— logo + LEDs, 22.5X tall
//   4. BUTTON CLUSTER— themed 4×4 grid, 135X tall
//
// Total: 300X = deviceWidth × (300/135) = deviceWidth × 20/9  (perfect 20:9)
// ============================================================================
@Composable
fun PortraitLayout2WithVirtualButtons(
    layoutConfig: DeviceAdapter.LayoutConfig,
    scalingMode: DeviceAdapter.ScalingMode = DeviceAdapter.ScalingMode.INTEGER,
    params: TrackerScreenParams,
    inputMapper: InputMapper,
    focusRequester: FocusRequester,
    theme: DeviceTheme = DeviceTheme.DARK,
) {
    val density = LocalDensity.current.density
    val deviceW = layoutConfig.deviceWidth
    val deviceH = layoutConfig.deviceHeight

    // The device skin is 135X wide × 300X tall (20:9 ratio).
    // Derive X as the largest value that fits both screen dimensions:
    //
    //   Case A (≥20:9 ratio): X from width → full 300X skin fits in height.
    //     Top panel may have a few units leftover on extra-tall screens.
    //
    //   Case B (18:9–19.5:9): X from width → top panel shrinks/disappears to absorb
    //     the height deficit; bezel/branding/buttons remain full device width.
    //
    //   Case C (<260.25/135 ≈ 1.928 ratio, e.g. 16:9=1.778): height is the constraint.
    //     X is derived from height so bezel+branding+buttons fill the screen height.
    //     Skin width (135X) becomes narrower than the device — casingColor fills the sides.
    val xFromWidth = deviceW / 135f

    // Branding always spans full device width, so its height is always proportional
    // to device width (ratio 135:22.5 = 6:1). Using a scaled-down X here would make
    // the PNG stretch horizontally when the box is fillMaxWidth().
    val brandingH = (xFromWidth * 22.5f).toInt()

    val X: Float
    val topPanelH: Int

    when {
        xFromWidth * 300f <= deviceH -> {
            // Case A: full skin fits vertically at width-derived X
            X = xFromWidth
            topPanelH = (deviceH - X * 260.25f).toInt().coerceAtMost((X * 39.75f).toInt())
        }
        xFromWidth * 260.25f <= deviceH -> {
            // Case B: top panel shrinks (can reach 0) to absorb the height deficit
            X = xFromWidth
            topPanelH = (deviceH - X * 260.25f).toInt().coerceAtLeast(0)
        }
        else -> {
            // Case C: skin is height-constrained; derive X from remaining height after branding
            topPanelH = 0
            X = (deviceH - brandingH) / (102.75f + 135f)
        }
    }

    // Skin width in pixels. Equals deviceW in cases A/B; less than deviceW in case C.
    val contentW    = (X * 135f).toInt()
    val contentWDp  = (contentW / density).dp
    val bezelH      = (X * 102.75f).toInt()
    val buttonAreaH = (X * 135f).toInt()

    // When screenBezelThicknessX > 0 the border is expressed in X-units (proportional to skin
    // scale) so it exactly matches the scaled PNG border on every device.  Fall back to the
    // absolute dp value for solid-color bezels (DARK theme etc.).
    val bezelThickPx = if (theme.screenBezelThicknessX > 0f)
        X * theme.screenBezelThicknessX
    else
        theme.screenBezelThicknessDp * density
    val bezelThickDp = (bezelThickPx / density).dp

    // Applies PNG paint or solid color background depending on availability
    fun Modifier.themeBackground(image: ImageBitmap?, color: Color): Modifier =
        if (image != null)
            then(Modifier.paint(BitmapPainter(image), contentScale = ContentScale.FillBounds))
        else
            background(color)

    Column(
        modifier = Modifier
            .fillMaxSize()
            .background(Color(0xFFBF9971))   // fills sides (case C) and any bottom gap
            .focusRequester(focusRequester)
            .inputHandler(inputMapper)
            .focusable(),
        horizontalAlignment = Alignment.CenterHorizontally,
        verticalArrangement = Arrangement.Top
    ) {
        // 1. TOP PANEL — ventilation grille (absent in case C, shrunk in case B)
        if (topPanelH > 0) Box(
            modifier = Modifier
                .width(contentWDp)
                .height((topPanelH / density).dp)
                .themeBackground(theme.topPanelImage, theme.casingColor)
        )

        // 2. SCREEN BEZEL
        // innerW uses contentW (not deviceW) so the bezel border is proportional
        // to the skin width on all aspect ratios.
        val innerW = contentW - 2f * bezelThickPx
        val innerH = bezelH  - 2f * bezelThickPx
        val intScale = floor(minOf(innerW / DESIGN_WIDTH_PX, innerH / DESIGN_HEIGHT_PX))
            .toInt().coerceAtLeast(1)
        val fillFactor = minOf(innerW / (DESIGN_WIDTH_PX * intScale),
                               innerH / (DESIGN_HEIGHT_PX * intScale))

        Box(
            modifier = Modifier
                .width(contentWDp)
                .height((bezelH / density).dp)
                .themeBackground(theme.screenBezelImage, theme.screenBezelColor)
                .padding(bezelThickDp)
                .background(Color.Black),  // inner area black — hides any letterbox gap
            contentAlignment = Alignment.TopStart
        ) {
            if (scalingMode == DeviceAdapter.ScalingMode.INTEGER) {
                // INTEGER: give TrackerScreen the full inner area. PixelPerfectTracker picks
                // the integer scale itself and its black Canvas background hides the letterbox.
                Box(modifier = Modifier
                    .width((innerW / density).dp)
                    .height((innerH / density).dp)
                ) {
                    TrackerScreen(params)
                }
            } else {
                // NEAREST / BILINEAR: render at integer-scale size, then stretch by fillFactor
                // to fill the inner bezel area. Use TransformOrigin(0,0) + explicit pixel
                // translation so there is no centering-rounding gap on left/top edges.
                val scaledW = DESIGN_WIDTH_PX  * intScale * fillFactor  // ≈ innerW
                val scaledH = DESIGN_HEIGHT_PX * intScale * fillFactor  // ≈ innerH
                val transX  = (innerW - scaledW) / 2f
                val transY  = (innerH - scaledH) / 2f
                Box(
                    modifier = Modifier
                        .size(
                            width  = (DESIGN_WIDTH_PX  * intScale / density).dp,
                            height = (DESIGN_HEIGHT_PX * intScale / density).dp
                        )
                        .graphicsLayer {
                            scaleX = fillFactor
                            scaleY = fillFactor
                            transformOrigin = TransformOrigin(0f, 0f)
                            translationX = transX
                            translationY = transY
                        }
                ) {
                    TrackerScreen(params)
                }
            }
        }

        // 3. BRANDING STRIP — logo + LEDs (always full screen width)
        Box(
            modifier = Modifier
                .fillMaxWidth()
                .height((brandingH / density).dp)
                .themeBackground(theme.brandingPanelImage, theme.casingColor)
        )

        // 4. BUTTON CLUSTER
        Box(
            modifier = Modifier
                .width(contentWDp)
                .height((buttonAreaH / density).dp)
        ) {
            VirtualControlsPortrait2(
                inputMapper     = inputMapper,
                availableWidth  = contentW,
                availableHeight = buttonAreaH.coerceAtLeast(100),
                theme           = theme,
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
