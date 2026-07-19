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

package com.conanizer.pockettracker.input

import androidx.compose.foundation.background
import androidx.compose.foundation.gestures.detectTapGestures
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.Text
import androidx.compose.runtime.*
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clipToBounds
import androidx.compose.ui.draw.paint
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.painter.BitmapPainter
import androidx.compose.ui.input.pointer.pointerInput
import androidx.compose.ui.layout.ContentScale
import androidx.compose.ui.platform.LocalDensity
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.TextUnit
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import com.conanizer.pockettracker.ui.theme.DeviceTheme

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

// Shared press gesture for all virtual buttons: PRESSED on touch-down, RELEASED on touch-up.
// Each button gets its own pointer scope, so multi-touch hold combos (L+A, A+DPAD, ...) work
// exactly like physical buttons. Used by both VirtualBtn and VirtualBtnThemed.
private fun Modifier.virtualButtonPress(
    button: VirtualButton,
    inputMapper: InputMapper,
    buttonEvent: ((VirtualButton, Boolean) -> Unit)?,
    setPressed: (Boolean) -> Unit
): Modifier = pointerInput(button) {
    detectTapGestures(
        onPress = {
            setPressed(true)
            buttonEvent?.invoke(button, true)
            inputMapper.onVirtualButton(button, ButtonAction.PRESSED)
            tryAwaitRelease()
            setPressed(false)
            buttonEvent?.invoke(button, false)
            inputMapper.onVirtualButton(button, ButtonAction.RELEASED)
        }
    )
}

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
            .virtualButtonPress(button, inputMapper, buttonEvent) { pressed = it }
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

    val density = LocalDensity.current.density

    // The arithmetic lives in TouchLayoutMetrics so the convergence golden can
    // record the values this screen actually uses. See that file's header.
    val m = TouchLayoutMetrics.left(availableWidth, availableHeight, density)

    val buttonSize        = m.buttonSize
    val lButtonWidth      = m.lButtonWidth
    val lButtonHeight     = m.lButtonHeight
    val selectWidth       = m.selectWidth
    val selectHeight      = m.selectHeight
    val smallSpacer       = m.smallSpacer
    val largeSpacer       = m.largeSpacer
    val mediumSpacerWidth = m.mediumSpacerWidth

    val mainFontSize    = m.mainFontSp.sp
    val triggerFontSize = m.triggerFontSp.sp
    val smallFontSize   = m.smallFontSp.sp

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

    val density = LocalDensity.current.density

    val m = TouchLayoutMetrics.right(availableWidth, availableHeight, density)

    val buttonSize    = m.buttonSize
    val rButtonWidth  = m.rButtonWidth
    val rButtonHeight = m.rButtonHeight
    val startWidth    = m.startWidth
    val startHeight   = m.startHeight
    val smallSpacer   = m.smallSpacer
    val mediumSpacer  = m.mediumSpacer
    val largeSpacer   = m.largeSpacer
    val leftSpacer    = m.leftSpacer
    val rightSpacer   = m.rightSpacer

    val mainFontSize    = m.mainFontSp.sp
    val triggerFontSize = m.triggerFontSp.sp
    val smallFontSize   = m.smallFontSp.sp

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
    isDark: Boolean = false,   // A/B use the darker square PNG when the skin provides one
    baseFontSizeSp: TextUnit,
    textOffsetXDp: Float,
    textOffsetYDp: Float,
    pressedOffsetDp: Float,
) {
    var pressed by remember { mutableStateOf(false) }
    val buttonEvent = LocalButtonEventCallback.current

    val image = when {
        isWide &&  pressed -> theme.buttonWidePressed
        isWide && !pressed -> theme.buttonWideNormal
        isDark &&  pressed -> theme.buttonSquarePressedDark ?: theme.buttonSquarePressed
        isDark && !pressed -> theme.buttonSquareNormalDark  ?: theme.buttonSquareNormal
        pressed            -> theme.buttonSquarePressed
        else               -> theme.buttonSquareNormal
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
            .virtualButtonPress(button, inputMapper, buttonEvent) { pressed = it }
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

    // X is a Float so that 135X fills the available space exactly — see
    // TouchLayoutMetrics.portrait2 for why flooring would leave edge gaps.
    val m = TouchLayoutMetrics.portrait2(availableWidth, availableHeight, density)

    val cellDp    = m.cellDp.dp   // square button cell height (and width unit); rows use weight() for width
    val paddingDp = m.paddingDp.dp // outer padding

    val largeSp = m.largeSp.sp  // A, B, arrows
    val smallSp = m.smallSp.sp  // Sel, Start, L/R Shift

    val sqOffXDp   = m.sqOffXDp
    val wideOffXDp = m.wideOffXDp
    val offYDp     = m.offYDp
    val pressedDp  = m.pressedDp

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
            // Rows use fillMaxWidth() + weight(1f) per child so Compose distributes the available
            // width exactly — no dp-rounding accumulation that could let cells overflow the backing.

            // Row 1: [L Shift][R Shift]
            Row(modifier = Modifier.fillMaxWidth().height(cellDp)) {
                VirtualBtnThemed(inputMapper, VirtualButton.L_SHIFT, "L Shift",
                    Modifier.weight(1f).fillMaxHeight(), theme, isWide = true,
                    baseFontSizeSp = smallSp, textOffsetXDp = wideOffXDp,
                    textOffsetYDp = offYDp, pressedOffsetDp = pressedDp)
                VirtualBtnThemed(inputMapper, VirtualButton.R_SHIFT, "R Shift",
                    Modifier.weight(1f).fillMaxHeight(), theme, isWide = true,
                    baseFontSizeSp = smallSp, textOffsetXDp = wideOffXDp,
                    textOffsetYDp = offYDp, pressedOffsetDp = pressedDp)
            }

            // Row 2: [empty][↑][B][A]
            Row(modifier = Modifier.fillMaxWidth().height(cellDp)) {
                Spacer(Modifier.weight(1f))
                VirtualBtnThemed(inputMapper, VirtualButton.DPAD_UP, "↑",
                    Modifier.weight(1f).fillMaxHeight(), theme,
                    baseFontSizeSp = largeSp, textOffsetXDp = sqOffXDp,
                    textOffsetYDp = offYDp, pressedOffsetDp = pressedDp)
                VirtualBtnThemed(inputMapper, VirtualButton.B, "B",
                    Modifier.weight(1f).fillMaxHeight(), theme, isDark = true,
                    baseFontSizeSp = largeSp, textOffsetXDp = sqOffXDp,
                    textOffsetYDp = offYDp, pressedOffsetDp = pressedDp)
                VirtualBtnThemed(inputMapper, VirtualButton.A, "A",
                    Modifier.weight(1f).fillMaxHeight(), theme, isDark = true,
                    baseFontSizeSp = largeSp, textOffsetXDp = sqOffXDp,
                    textOffsetYDp = offYDp, pressedOffsetDp = pressedDp)
            }

            // Row 3: [←][↓][→][empty]
            Row(modifier = Modifier.fillMaxWidth().height(cellDp)) {
                VirtualBtnThemed(inputMapper, VirtualButton.DPAD_LEFT, "←",
                    Modifier.weight(1f).fillMaxHeight(), theme,
                    baseFontSizeSp = largeSp, textOffsetXDp = sqOffXDp,
                    textOffsetYDp = offYDp, pressedOffsetDp = pressedDp)
                VirtualBtnThemed(inputMapper, VirtualButton.DPAD_DOWN, "↓",
                    Modifier.weight(1f).fillMaxHeight(), theme,
                    baseFontSizeSp = largeSp, textOffsetXDp = sqOffXDp,
                    textOffsetYDp = offYDp, pressedOffsetDp = pressedDp)
                VirtualBtnThemed(inputMapper, VirtualButton.DPAD_RIGHT, "→",
                    Modifier.weight(1f).fillMaxHeight(), theme,
                    baseFontSizeSp = largeSp, textOffsetXDp = sqOffXDp,
                    textOffsetYDp = offYDp, pressedOffsetDp = pressedDp)
                Spacer(Modifier.weight(1f))
            }

            // Row 4: [empty][Sel][Start][empty]
            Row(modifier = Modifier.fillMaxWidth().height(cellDp)) {
                Spacer(Modifier.weight(1f))
                VirtualBtnThemed(inputMapper, VirtualButton.SELECT, "Sel",
                    Modifier.weight(1f).fillMaxHeight(), theme,
                    baseFontSizeSp = smallSp, textOffsetXDp = sqOffXDp,
                    textOffsetYDp = offYDp, pressedOffsetDp = pressedDp)
                VirtualBtnThemed(inputMapper, VirtualButton.START, "Start",
                    Modifier.weight(1f).fillMaxHeight(), theme,
                    baseFontSizeSp = smallSp, textOffsetXDp = sqOffXDp,
                    textOffsetYDp = offYDp, pressedOffsetDp = pressedDp)
                Spacer(Modifier.weight(1f))
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

    val density = LocalDensity.current.density

    // Each box gets half the width; the size computed here is handed down to
    // VirtualControlsLeft/Right as THEIR available size, so an error here moves
    // every button on the screen.
    val m = TouchLayoutMetrics.portrait(availableWidth, availableHeight)

    val boxWidth  = m.boxWidth
    val boxHeight = m.boxHeight

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
