// ============================================================================
// VirtualControls.kt
//
// All virtual buttons route through InputMapper.onVirtualButton() so combos
// (L+A, A+DPAD, R+DPAD, B+DPAD, etc.) work identically to physical buttons.
//
// Each button uses pointerInput + detectTapGestures to fire PRESSED on
// touch-down and RELEASED on touch-up, enabling multi-touch hold combos.
//
// Pattern: 3.4w × 5.1h for each box
// ============================================================================

package com.example.pockettracker

import androidx.compose.foundation.background
import androidx.compose.foundation.gestures.detectTapGestures
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.Text
import androidx.compose.runtime.*
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.input.pointer.pointerInput
import androidx.compose.ui.platform.LocalDensity
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.TextUnit
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import kotlin.math.floor

private val BTN_NORMAL  = Color(0xFF3D5A80)
private val BTN_PRESSED = Color(0xFF98C1D9)

/**
 * A single virtual button that fires PRESSED/RELEASED through InputMapper.
 * This is the core primitive — all virtual controls are built from this.
 */
@Composable
private fun VirtualBtn(
    inputMapper: InputMapper,
    button: VirtualButton,
    label: String,
    modifier: Modifier,
    fontSize: TextUnit
) {
    var pressed by remember { mutableStateOf(false) }

    Box(
        contentAlignment = Alignment.Center,
        modifier = modifier
            .background(
                color = if (pressed) BTN_PRESSED else BTN_NORMAL,
                shape = RoundedCornerShape(4.dp)
            )
            .pointerInput(button) {
                detectTapGestures(
                    onPress = {
                        pressed = true
                        inputMapper.onVirtualButton(button, ButtonAction.PRESSED)
                        tryAwaitRelease()
                        pressed = false
                        inputMapper.onVirtualButton(button, ButtonAction.RELEASED)
                    }
                )
            }
    ) {
        Text(
            text = label,
            fontWeight = FontWeight.Bold,
            fontSize = fontSize,
            color = Color.White
        )
    }
}

// ============================================================================
// LEFT BUTTON BOX - Pattern: 3.4w × 5.1h
// Buttons: L, DPAD, SELECT
// ============================================================================
@Composable
fun VirtualControlsLeft(
    inputMapper: InputMapper,
    availableWidth: Int,
    availableHeight: Int
) {
    if (availableWidth <= 0 || availableHeight <= 0) return

    val PATTERN_WIDTH  = 3.4f
    val PATTERN_HEIGHT = 5.1f
    val density = LocalDensity.current.density

    val boxRatio    = availableWidth.toFloat() / availableHeight.toFloat()
    val patternRatio = PATTERN_WIDTH / PATTERN_HEIGHT

    val X = if (boxRatio < patternRatio) {
        floor(availableWidth / PATTERN_WIDTH)
    } else {
        floor(availableHeight / PATTERN_HEIGHT)
    }.toInt()

    val buttonSize       = X
    val lButtonWidth     = floor(X * 1.5f).toInt()
    val lButtonHeight    = floor(X * 0.7f).toInt()
    val selectWidth      = floor(X * 1.2f).toInt()
    val selectHeight     = floor(X * 0.6f).toInt()
    val smallSpacer      = floor(X * 0.2f).toInt()
    val largeSpacer      = floor(X * 2.0f).toInt()
    val mediumSpacerWidth = floor(X * 1.0f).toInt()

    val mainFontSize    = (X * 0.4f  / density).sp
    val triggerFontSize = (X * 0.35f / density).sp
    val smallFontSize   = (X * 0.25f / density).sp

    Column(
        modifier = Modifier
            .fillMaxSize()
            .background(Color(0xFF1a1a1a)),
        horizontalAlignment = Alignment.CenterHorizontally,
        verticalArrangement = Arrangement.Center
    ) {
        Spacer(modifier = Modifier.height((smallSpacer / density).dp))

        VirtualBtn(
            inputMapper = inputMapper,
            button = VirtualButton.L_SHIFT,
            label = "L",
            modifier = Modifier.size(width = (lButtonWidth / density).dp, height = (lButtonHeight / density).dp),
            fontSize = triggerFontSize
        )

        Spacer(modifier = Modifier.height((smallSpacer / density).dp))

        VirtualBtn(
            inputMapper = inputMapper,
            button = VirtualButton.DPAD_UP,
            label = "↑",
            modifier = Modifier.size((buttonSize / density).dp),
            fontSize = mainFontSize
        )

        Row(horizontalArrangement = Arrangement.Center) {
            Spacer(modifier = Modifier.width((smallSpacer / density).dp))
            VirtualBtn(
                inputMapper = inputMapper,
                button = VirtualButton.DPAD_LEFT,
                label = "←",
                modifier = Modifier.size((buttonSize / density).dp),
                fontSize = mainFontSize
            )
            Spacer(modifier = Modifier.width((mediumSpacerWidth / density).dp))
            VirtualBtn(
                inputMapper = inputMapper,
                button = VirtualButton.DPAD_RIGHT,
                label = "→",
                modifier = Modifier.size((buttonSize / density).dp),
                fontSize = mainFontSize
            )
            Spacer(modifier = Modifier.width((smallSpacer / density).dp))
        }

        VirtualBtn(
            inputMapper = inputMapper,
            button = VirtualButton.DPAD_DOWN,
            label = "↓",
            modifier = Modifier.size((buttonSize / density).dp),
            fontSize = mainFontSize
        )

        Spacer(modifier = Modifier.height((smallSpacer / density).dp))

        Row(horizontalArrangement = Arrangement.Center) {
            Spacer(modifier = Modifier.width((largeSpacer / density).dp))
            VirtualBtn(
                inputMapper = inputMapper,
                button = VirtualButton.SELECT,
                label = "SEL",
                modifier = Modifier.size(width = (selectWidth / density).dp, height = (selectHeight / density).dp),
                fontSize = smallFontSize
            )
            Spacer(modifier = Modifier.width((smallSpacer / density).dp))
        }

        Spacer(modifier = Modifier.height((smallSpacer / density).dp))
    }
}

// ============================================================================
// RIGHT BUTTON BOX - Pattern: 3.4w × 5.1h
// Buttons: R, A, B, START
// ============================================================================
@Composable
fun VirtualControlsRight(
    inputMapper: InputMapper,
    availableWidth: Int,
    availableHeight: Int
) {
    if (availableWidth <= 0 || availableHeight <= 0) return

    val PATTERN_WIDTH  = 3.4f
    val PATTERN_HEIGHT = 5.1f
    val density = LocalDensity.current.density

    val boxRatio     = availableWidth.toFloat() / availableHeight.toFloat()
    val patternRatio = PATTERN_WIDTH / PATTERN_HEIGHT

    val X = if (boxRatio < patternRatio) {
        floor(availableWidth / PATTERN_WIDTH)
    } else {
        floor(availableHeight / PATTERN_HEIGHT)
    }.toInt()

    val buttonSize   = X
    val rButtonWidth  = floor(X * 1.5f).toInt()
    val rButtonHeight = floor(X * 0.7f).toInt()
    val startWidth    = floor(X * 1.2f).toInt()
    val startHeight   = floor(X * 0.6f).toInt()
    val smallSpacer   = floor(X * 0.2f).toInt()
    val mediumSpacer  = floor(X * 0.7f).toInt()
    val largeSpacer   = floor(X * 2.0f).toInt()
    val leftSpacer    = floor(X * 1.7f).toInt()
    val rightSpacer   = floor(X * 0.7f).toInt()

    val mainFontSize    = (X * 0.4f  / density).sp
    val triggerFontSize = (X * 0.35f / density).sp
    val smallFontSize   = (X * 0.25f / density).sp

    Column(
        modifier = Modifier
            .fillMaxSize()
            .background(Color(0xFF1a1a1a)),
        horizontalAlignment = Alignment.CenterHorizontally,
        verticalArrangement = Arrangement.Center
    ) {
        Spacer(modifier = Modifier.height((smallSpacer / density).dp))

        VirtualBtn(
            inputMapper = inputMapper,
            button = VirtualButton.R_SHIFT,
            label = "R",
            modifier = Modifier.size(width = (rButtonWidth / density).dp, height = (rButtonHeight / density).dp),
            fontSize = triggerFontSize
        )

        Spacer(modifier = Modifier.height((mediumSpacer / density).dp))

        Row(horizontalArrangement = Arrangement.Center) {
            Spacer(modifier = Modifier.width((leftSpacer / density).dp))
            VirtualBtn(
                inputMapper = inputMapper,
                button = VirtualButton.A,
                label = "A",
                modifier = Modifier.size((buttonSize / density).dp),
                fontSize = mainFontSize
            )
            Spacer(modifier = Modifier.width((rightSpacer / density).dp))
        }

        Row(horizontalArrangement = Arrangement.Center) {
            Spacer(modifier = Modifier.width((rightSpacer / density).dp))
            VirtualBtn(
                inputMapper = inputMapper,
                button = VirtualButton.B,
                label = "B",
                modifier = Modifier.size((buttonSize / density).dp),
                fontSize = mainFontSize
            )
            Spacer(modifier = Modifier.width((leftSpacer / density).dp))
        }

        Spacer(modifier = Modifier.height((mediumSpacer / density).dp))

        Row(horizontalArrangement = Arrangement.Center) {
            Spacer(modifier = Modifier.width((smallSpacer / density).dp))
            VirtualBtn(
                inputMapper = inputMapper,
                button = VirtualButton.START,
                label = "STA",
                modifier = Modifier.size(width = (startWidth / density).dp, height = (startHeight / density).dp),
                fontSize = smallFontSize
            )
            Spacer(modifier = Modifier.width((largeSpacer / density).dp))
        }

        Spacer(modifier = Modifier.height((smallSpacer / density).dp))
    }
}

// ============================================================================
// PORTRAIT2 MODE - Compact 4×4 button grid with 0.1X spacers between buttons
//
// Grid dimensions (with spacers):
//   Total width:  4×X + 3×0.1X = 4.3X  (4 cols + 3 inter-col spacers)
//   Total height: 1X spacer + 4×X + 3×0.1X = 5.3X
//
// Layout:
//   Spacer: 1X tall (between screen and buttons)
//   Row 1: [L 2.1X][sp 0.1X][R 2.1X]
//   sp 0.1X
//   Row 2: [empty X][sp 0.1X][UP X][sp 0.1X][B X][sp 0.1X][A X]
//   sp 0.1X
//   Row 3: [LEFT X][sp 0.1X][DOWN X][sp 0.1X][RIGHT X][sp 0.1X][empty X]
//   sp 0.1X
//   Row 4: [empty X][sp 0.1X][SEL X][sp 0.1X][START X][sp 0.1X][empty X]
// ============================================================================
@Composable
fun VirtualControlsPortrait2(
    inputMapper: InputMapper,
    availableWidth: Int,
    availableHeight: Int
) {
    if (availableWidth <= 0 || availableHeight <= 0) return

    val density = LocalDensity.current.density

    // 4.5X wide (2×0.1X outer + 4 buttons + 3×0.1X inner col spacers)
    // 5.2X tall  (0.8X spacer above + 4 buttons + 3×0.1X row spacers + 0.1X bottom)
    val xByWidth  = floor(availableWidth / 4.5f)
    val xByHeight = floor(availableHeight / 5.2f)
    val X = minOf(xByWidth, xByHeight).toInt().coerceAtLeast(1)

    val cellDp      = (X / density).dp
    val gapDp       = (X * 0.1f / density).dp   // 0.1X spacer between buttons and outer padding
    val lrWidthDp   = (X * 2.1f / density).dp   // L/R buttons: (4.3X buttons + 0.1X inner) / 2 = 2.15X ≈ 2.1X
    val spacerDp    = (X * 0.8f / density).dp   // 0.8X spacer between screen and buttons

    val mainFontSize    = (X * 0.4f / density).sp
    val triggerFontSize = (X * 0.35f / density).sp
    val smallFontSize   = (X * 0.25f / density).sp

    Column(
        modifier = Modifier
            .fillMaxSize()
            .background(Color(0xFF1a1a1a))
            .padding(horizontal = gapDp),  // 0.1X outer side padding
        horizontalAlignment = Alignment.CenterHorizontally,
        verticalArrangement = Arrangement.Center
    ) {
        // Spacer (0.8X) between screen and buttons
        Spacer(modifier = Modifier.height(spacerDp))

        // Row 1: [L (2.1X)][gap 0.1X][R (2.1X)]
        Row(horizontalArrangement = Arrangement.Center) {
            VirtualBtn(
                inputMapper = inputMapper,
                button = VirtualButton.L_SHIFT,
                label = "L",
                modifier = Modifier.size(width = lrWidthDp, height = cellDp),
                fontSize = triggerFontSize
            )
            Spacer(modifier = Modifier.width(gapDp))
            VirtualBtn(
                inputMapper = inputMapper,
                button = VirtualButton.R_SHIFT,
                label = "R",
                modifier = Modifier.size(width = lrWidthDp, height = cellDp),
                fontSize = triggerFontSize
            )
        }

        Spacer(modifier = Modifier.height(gapDp))

        // Row 2: [empty][gap][UP][gap][B][gap][A]
        Row(horizontalArrangement = Arrangement.Center) {
            Spacer(modifier = Modifier.size(cellDp))
            Spacer(modifier = Modifier.width(gapDp))
            VirtualBtn(
                inputMapper = inputMapper,
                button = VirtualButton.DPAD_UP,
                label = "↑",
                modifier = Modifier.size(cellDp),
                fontSize = mainFontSize
            )
            Spacer(modifier = Modifier.width(gapDp))
            VirtualBtn(
                inputMapper = inputMapper,
                button = VirtualButton.B,
                label = "B",
                modifier = Modifier.size(cellDp),
                fontSize = mainFontSize
            )
            Spacer(modifier = Modifier.width(gapDp))
            VirtualBtn(
                inputMapper = inputMapper,
                button = VirtualButton.A,
                label = "A",
                modifier = Modifier.size(cellDp),
                fontSize = mainFontSize
            )
        }

        Spacer(modifier = Modifier.height(gapDp))

        // Row 3: [LEFT][gap][DOWN][gap][RIGHT][gap][empty]
        Row(horizontalArrangement = Arrangement.Center) {
            VirtualBtn(
                inputMapper = inputMapper,
                button = VirtualButton.DPAD_LEFT,
                label = "←",
                modifier = Modifier.size(cellDp),
                fontSize = mainFontSize
            )
            Spacer(modifier = Modifier.width(gapDp))
            VirtualBtn(
                inputMapper = inputMapper,
                button = VirtualButton.DPAD_DOWN,
                label = "↓",
                modifier = Modifier.size(cellDp),
                fontSize = mainFontSize
            )
            Spacer(modifier = Modifier.width(gapDp))
            VirtualBtn(
                inputMapper = inputMapper,
                button = VirtualButton.DPAD_RIGHT,
                label = "→",
                modifier = Modifier.size(cellDp),
                fontSize = mainFontSize
            )
            Spacer(modifier = Modifier.width(gapDp))
            Spacer(modifier = Modifier.size(cellDp))
        }

        Spacer(modifier = Modifier.height(gapDp))

        // Row 4: [empty][gap][SEL][gap][START][gap][empty]
        Row(horizontalArrangement = Arrangement.Center) {
            Spacer(modifier = Modifier.size(cellDp))
            Spacer(modifier = Modifier.width(gapDp))
            VirtualBtn(
                inputMapper = inputMapper,
                button = VirtualButton.SELECT,
                label = "SEL",
                modifier = Modifier.size(cellDp),
                fontSize = smallFontSize
            )
            Spacer(modifier = Modifier.width(gapDp))
            VirtualBtn(
                inputMapper = inputMapper,
                button = VirtualButton.START,
                label = "STA",
                modifier = Modifier.size(cellDp),
                fontSize = smallFontSize
            )
            Spacer(modifier = Modifier.width(gapDp))
            Spacer(modifier = Modifier.size(cellDp))
        }

        // 0.1X bottom outer spacer
        Spacer(modifier = Modifier.height(gapDp))
    }
}

// ============================================================================
// PORTRAIT MODE - Pattern: 6.8w × 5.1h (two 3.4w boxes side by side)
// ============================================================================
@Composable
fun VirtualControls(
    inputMapper: InputMapper,
    availableHeight: Int = 0,
    availableWidth: Int = 0
) {
    if (availableWidth <= 0 || availableHeight <= 0) return

    val SINGLE_BOX_PATTERN_WIDTH  = 3.4f
    val SINGLE_BOX_PATTERN_HEIGHT = 5.1f
    val density = LocalDensity.current.density

    val boxAvailableWidth  = availableWidth / 2
    val boxAvailableHeight = availableHeight

    val boxRatio     = boxAvailableWidth.toFloat() / boxAvailableHeight.toFloat()
    val patternRatio = SINGLE_BOX_PATTERN_WIDTH / SINGLE_BOX_PATTERN_HEIGHT

    val X = if (boxRatio < patternRatio) {
        floor(boxAvailableWidth / SINGLE_BOX_PATTERN_WIDTH)
    } else {
        floor(boxAvailableHeight / SINGLE_BOX_PATTERN_HEIGHT)
    }.toInt()

    val boxWidth  = floor(X * SINGLE_BOX_PATTERN_WIDTH).toInt()
    val boxHeight = floor(X * SINGLE_BOX_PATTERN_HEIGHT).toInt()

    val boxWidthDp  = (boxWidth  / density).dp
    val boxHeightDp = (boxHeight / density).dp

    Row(
        modifier = Modifier
            .fillMaxSize()
            .background(Color(0xFF1a1a1a)),
        horizontalArrangement = Arrangement.Center,
        verticalAlignment = Alignment.CenterVertically
    ) {
        Box(modifier = Modifier.size(width = boxWidthDp, height = boxHeightDp)) {
            VirtualControlsLeft(
                inputMapper = inputMapper,
                availableWidth = boxWidth,
                availableHeight = boxHeight
            )
        }
        Box(modifier = Modifier.size(width = boxWidthDp, height = boxHeightDp)) {
            VirtualControlsRight(
                inputMapper = inputMapper,
                availableWidth = boxWidth,
                availableHeight = boxHeight
            )
        }
    }
}
