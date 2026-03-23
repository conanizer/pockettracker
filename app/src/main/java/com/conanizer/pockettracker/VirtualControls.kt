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

package com.conanizer.pockettracker

import androidx.compose.foundation.background
import androidx.compose.foundation.gestures.detectTapGestures
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.Text
import androidx.compose.runtime.*
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.paint
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.painter.BitmapPainter
import androidx.compose.ui.input.pointer.pointerInput
import androidx.compose.ui.layout.ContentScale
import androidx.compose.ui.platform.LocalDensity
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.TextUnit
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import com.conanizer.pockettracker.core.ui.DeviceTheme
import kotlin.math.floor

/**
 * Callback fired on every virtual button press and release.
 * Provided via CompositionLocal so ScreenLayouts.kt needs no changes.
 *
 * (button, isPress) — isPress=true on touch-down, false on touch-up/cancel.
 *
 * Set this in MainActivity via CompositionLocalProvider before the layout
 * composables so all VirtualBtn/VirtualBtnThemed instances pick it up
 * automatically.
 */
val LocalButtonEventCallback =
    staticCompositionLocalOf<((button: VirtualButton, isPress: Boolean) -> Unit)?> { null }

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
    val buttonEvent = LocalButtonEventCallback.current

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
                        buttonEvent?.invoke(button, true)
                        inputMapper.onVirtualButton(button, ButtonAction.PRESSED)
                        tryAwaitRelease()
                        pressed = false
                        buttonEvent?.invoke(button, false)
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
// THEMED BUTTON — used exclusively by VirtualControlsPortrait2.
//
// Draws PNG image if available, else solid color fallback.
// Text positioned by explicit offsets (not centered) to match design spec.
// Text shifts down by pressedOffsetDp when button is held.
// ============================================================================
@Composable
private fun VirtualBtnThemed(
    inputMapper: InputMapper,
    button: VirtualButton,
    label: String,
    modifier: Modifier,
    theme: DeviceTheme,
    isWide: Boolean = false,
    baseFontSizeSp: TextUnit,
    textOffsetXDp: Float,
    textOffsetYDp: Float,
    pressedOffsetDp: Float,
) {
    var pressed by remember { mutableStateOf(false) }
    val buttonEvent = LocalButtonEventCallback.current

    val image = when {
        isWide &&  pressed  -> theme.buttonWidePressed
        isWide && !pressed  -> theme.buttonWideNormal
        !isWide &&  pressed -> theme.buttonSquarePressed
        else                -> theme.buttonSquareNormal
    }

    val topOffsetDp = textOffsetYDp + if (pressed) pressedOffsetDp else 0f

    Box(
        contentAlignment = Alignment.TopStart,
        modifier = modifier
            .then(
                if (image != null)
                    Modifier.paint(BitmapPainter(image), contentScale = ContentScale.FillBounds)
                else
                    Modifier.background(
                        if (pressed) theme.buttonPressedColor else theme.buttonNormalColor,
                        RoundedCornerShape(4.dp)
                    )
            )
            .pointerInput(button) {
                detectTapGestures(
                    onPress = {
                        pressed = true
                        buttonEvent?.invoke(button, true)
                        inputMapper.onVirtualButton(button, ButtonAction.PRESSED)
                        tryAwaitRelease()
                        pressed = false
                        buttonEvent?.invoke(button, false)
                        inputMapper.onVirtualButton(button, ButtonAction.RELEASED)
                    }
                )
            }
    ) {
        Text(
            text = label,
            fontFamily = theme.buttonFont,
            fontWeight = FontWeight.Normal,
            fontSize = baseFontSizeSp,
            color = theme.buttonLabelColor,
            modifier = Modifier.padding(start = textOffsetXDp.dp, top = topOffsetDp.dp)
        )
    }
}

// ============================================================================
// PORTRAIT2 MODE — Retro device skin with themed button cluster.
//
// Layout (all dimensions in X-units, X = availableWidth / 135):
//   Outer box: 135X × 135X with button backing (PNG or color)
//   Inner column (1.5X padding): 132X wide
//   Row 1: [L Shift 66X][R Shift 66X]          — wide buttons
//   Row 2: [empty 33X][↑ 33X][B 33X][A 33X]    — square buttons
//   Row 3: [← 33X][↓ 33X][→ 33X][empty 33X]   — square buttons
//   Row 4: [empty 33X][Sel 33X][Start 33X][empty 33X]
// ============================================================================
@Composable
fun VirtualControlsPortrait2(
    inputMapper: InputMapper,
    availableWidth: Int,
    availableHeight: Int,
    theme: DeviceTheme = DeviceTheme.DARK,
) {
    if (availableWidth <= 0 || availableHeight <= 0) return

    val density = LocalDensity.current.density

    // X = base unit as a Float so that 135X fills the available space exactly.
    // Using floor() here would leave sub-unit gaps at the edges on resolutions
    // that are not a perfect multiple of 135 (e.g. 2800px → floor=20, 135×20=2700,
    // 100px gap). Keeping it as a Float means 135X == availableWidth always.
    val X = minOf(availableWidth / 135f, availableHeight / 135f).coerceAtLeast(1f)

    fun px(units: Float) = (X * units / density).dp

    val cellDp    = px(33f)   // square button: 33X × 33X
    val wideDp    = px(66f)   // wide button: 66X × 33X
    val paddingDp = px(1.5f)  // 12px outer padding

    val largeSp = (X * 11f / density).sp  // A, B, arrows
    val smallSp = (X *  7f / density).sp  // Sel, Start, L/R Shift

    val sqOffXDp   = X * 7f / density
    val wideOffXDp = X * 8f / density
    val offYDp     = X * 4f / density
    val pressedDp  = X * 1f / density

    // Cluster box fills the full available area. clipToBounds() ensures per-dp rounding of
    // individual button sizes never lets them visually overflow the backing image boundary.
    Box(
        modifier = Modifier
            .fillMaxSize()
            .clipToBounds()
            .then(
                if (theme.buttonBackingImage != null)
                    Modifier.paint(BitmapPainter(theme.buttonBackingImage), contentScale = ContentScale.FillBounds)
                else
                    Modifier.background(theme.buttonBackingColor, RoundedCornerShape(theme.buttonBackingCornerDp.dp))
            )
    ) {
        Column(
            modifier = Modifier
                .fillMaxSize()
                .padding(paddingDp),
            horizontalAlignment = Alignment.Start,
            verticalArrangement = Arrangement.Top
        ) {
            // Row 1: [L Shift][R Shift]
            Row {
                VirtualBtnThemed(inputMapper, VirtualButton.L_SHIFT, "L Shift",
                    Modifier.size(width = wideDp, height = cellDp), theme, isWide = true,
                    smallSp, wideOffXDp, offYDp, pressedDp)
                VirtualBtnThemed(inputMapper, VirtualButton.R_SHIFT, "R Shift",
                    Modifier.size(width = wideDp, height = cellDp), theme, isWide = true,
                    smallSp, wideOffXDp, offYDp, pressedDp)
            }

            // Row 2: [empty][↑][B][A]
            Row {
                Spacer(Modifier.size(cellDp))
                VirtualBtnThemed(inputMapper, VirtualButton.DPAD_UP, "↑",
                    Modifier.size(cellDp), theme,
                    baseFontSizeSp = largeSp, textOffsetXDp = sqOffXDp,
                    textOffsetYDp = offYDp, pressedOffsetDp = pressedDp)
                VirtualBtnThemed(inputMapper, VirtualButton.B, "B",
                    Modifier.size(cellDp), theme,
                    baseFontSizeSp = largeSp, textOffsetXDp = sqOffXDp,
                    textOffsetYDp = offYDp, pressedOffsetDp = pressedDp)
                VirtualBtnThemed(inputMapper, VirtualButton.A, "A",
                    Modifier.size(cellDp), theme,
                    baseFontSizeSp = largeSp, textOffsetXDp = sqOffXDp,
                    textOffsetYDp = offYDp, pressedOffsetDp = pressedDp)
            }

            // Row 3: [←][↓][→][empty]
            Row {
                VirtualBtnThemed(inputMapper, VirtualButton.DPAD_LEFT, "←",
                    Modifier.size(cellDp), theme,
                    baseFontSizeSp = largeSp, textOffsetXDp = sqOffXDp,
                    textOffsetYDp = offYDp, pressedOffsetDp = pressedDp)
                VirtualBtnThemed(inputMapper, VirtualButton.DPAD_DOWN, "↓",
                    Modifier.size(cellDp), theme,
                    baseFontSizeSp = largeSp, textOffsetXDp = sqOffXDp,
                    textOffsetYDp = offYDp, pressedOffsetDp = pressedDp)
                VirtualBtnThemed(inputMapper, VirtualButton.DPAD_RIGHT, "→",
                    Modifier.size(cellDp), theme,
                    baseFontSizeSp = largeSp, textOffsetXDp = sqOffXDp,
                    textOffsetYDp = offYDp, pressedOffsetDp = pressedDp)
                Spacer(Modifier.size(cellDp))
            }

            // Row 4: [empty][Sel][Start][empty]
            Row {
                Spacer(Modifier.size(cellDp))
                VirtualBtnThemed(inputMapper, VirtualButton.SELECT, "Sel",
                    Modifier.size(cellDp), theme,
                    baseFontSizeSp = smallSp, textOffsetXDp = sqOffXDp,
                    textOffsetYDp = offYDp, pressedOffsetDp = pressedDp)
                VirtualBtnThemed(inputMapper, VirtualButton.START, "Start",
                    Modifier.size(cellDp), theme,
                    baseFontSizeSp = smallSp, textOffsetXDp = sqOffXDp,
                    textOffsetYDp = offYDp, pressedOffsetDp = pressedDp)
                Spacer(Modifier.size(cellDp))
            }
        }
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
