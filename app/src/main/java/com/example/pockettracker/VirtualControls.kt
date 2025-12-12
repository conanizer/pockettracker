// ============================================================================
// VirtualControls.kt - EXACT SPECIFICATION
// Pattern: 3.4w × 5.1h for each box
// ============================================================================

package com.example.pockettracker

import androidx.compose.foundation.background
import androidx.compose.foundation.layout.*
import androidx.compose.material3.Button
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.platform.LocalDensity
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import kotlin.math.floor

// ============================================================================
// LEFT BUTTON BOX - Pattern: 3.4w × 5.1h
// ============================================================================
@Composable
fun VirtualControlsLeft(
    onDPadUp: () -> Unit,
    onDPadDown: () -> Unit,
    onDPadLeft: () -> Unit,
    onDPadRight: () -> Unit,
    onL: () -> Unit,
    onSelect: () -> Unit,
    availableWidth: Int,
    availableHeight: Int
) {
    android.util.Log.d("VirtualControlsLeft", "🔴 FUNCTION CALLED! Width: $availableWidth, Height: $availableHeight")

    val PATTERN_WIDTH = 3.4f
    val PATTERN_HEIGHT = 5.1f

    val density = LocalDensity.current.density

    val boxRatio = availableWidth.toFloat() / availableHeight.toFloat()
    val patternRatio = PATTERN_WIDTH / PATTERN_HEIGHT

    val X = if (boxRatio < patternRatio) {
        floor(availableWidth / PATTERN_WIDTH)
    } else {
        floor(availableHeight / PATTERN_HEIGHT)
    }.toInt()

    android.util.Log.d("VirtualControlsLeft", "Box ratio: $boxRatio, Pattern ratio: $patternRatio")
    android.util.Log.d("VirtualControlsLeft", "X = $X pixels")
    android.util.Log.d("VirtualControlsLeft", "Density: $density")

    // All sizes in PIXELS, floored
    val buttonSize = X                              // 1.0X
    val lButtonWidth = floor(X * 1.5f).toInt()     // 1.5X
    val lButtonHeight = floor(X * 0.7f).toInt()    // 0.7X
    val selectWidth = floor(X * 1.2f).toInt()      // 1.2X
    val selectHeight = floor(X * 0.6f).toInt()     // 0.6X
    val smallSpacer = floor(X * 0.2f).toInt()      // 0.2X
    val largeSpacer = floor(X * 2.0f).toInt()      // 2.0X
    val mediumSpacerWidth = floor(X * 1.0f).toInt() // 1.0X (between left/right)

    android.util.Log.d("VirtualControlsLeft", "Button size: ${(buttonSize / density).dp}")
    android.util.Log.d("VirtualControlsLeft", "L button: ${(lButtonWidth / density).dp} × ${(lButtonHeight / density).dp}")

    // Font sizes
    val mainFontSize = (X * 0.4f / density).sp
    val triggerFontSize = (X * 0.35f / density).sp
    val smallFontSize = (X * 0.25f / density).sp

    android.util.Log.d("VirtualControlsLeft", "About to create Column...")

    Column(
        modifier = Modifier
            .fillMaxSize()
            .background(Color(0xFF1a1a1a)),  // ← Back to normal dark gray
        horizontalAlignment = Alignment.CenterHorizontally,
        verticalArrangement = Arrangement.Top
    ) {
        android.util.Log.d("VirtualControlsLeft", "✅ INSIDE Column! Creating buttons...")

        // spacer h0.2x
        Spacer(modifier = Modifier.height((smallSpacer / density).dp))

        // L button h0.7x w1.5x
        Button(
            onClick = onL,
            modifier = Modifier.size(
                width = (lButtonWidth / density).dp,
                height = (lButtonHeight / density).dp
            ),
            contentPadding = PaddingValues(0.dp)
        ) {
            Text("L", fontWeight = FontWeight.Bold, fontSize = triggerFontSize)
        }

        // spacer h0.2x
        Spacer(modifier = Modifier.height((smallSpacer / density).dp))

        // dpad up h1x w1x
        Button(
            onClick = onDPadUp,
            modifier = Modifier.size((buttonSize / density).dp),
            contentPadding = PaddingValues(0.dp)
        ) {
            Text("↑", fontWeight = FontWeight.Bold, fontSize = mainFontSize)
        }

        // spacer w0.2x, dpad left h1x w1x, spacer w1x, dpad right h1x w1x, spacer w0.2x
        Row(horizontalArrangement = Arrangement.Center) {
            Spacer(modifier = Modifier.width((smallSpacer / density).dp))
            Button(
                onClick = onDPadLeft,
                modifier = Modifier.size((buttonSize / density).dp),
                contentPadding = PaddingValues(0.dp)
            ) {
                Text("←", fontWeight = FontWeight.Bold, fontSize = mainFontSize)
            }
            Spacer(modifier = Modifier.width((mediumSpacerWidth / density).dp))
            Button(
                onClick = onDPadRight,
                modifier = Modifier.size((buttonSize / density).dp),
                contentPadding = PaddingValues(0.dp)
            ) {
                Text("→", fontWeight = FontWeight.Bold, fontSize = mainFontSize)
            }
            Spacer(modifier = Modifier.width((smallSpacer / density).dp))
        }

        // dpad down h1x w1x (no spacer before it!)
        Button(
            onClick = onDPadDown,
            modifier = Modifier.size((buttonSize / density).dp),
            contentPadding = PaddingValues(0.dp)
        ) {
            Text("↓", fontWeight = FontWeight.Bold, fontSize = mainFontSize)
        }

        // spacer h0.2x
        Spacer(modifier = Modifier.height((smallSpacer / density).dp))

        // spacer w2x, select h0.6x w1.2x, spacer w0.2x
        Row(horizontalArrangement = Arrangement.Center) {
            Spacer(modifier = Modifier.width((largeSpacer / density).dp))
            Button(
                onClick = onSelect,
                modifier = Modifier.size(
                    width = (selectWidth / density).dp,
                    height = (selectHeight / density).dp
                ),
                contentPadding = PaddingValues(0.dp)
            ) {
                Text("SEL", fontWeight = FontWeight.Bold, fontSize = smallFontSize)
            }
            Spacer(modifier = Modifier.width((smallSpacer / density).dp))
        }

        // spacer h0.2x
        Spacer(modifier = Modifier.height((smallSpacer / density).dp))
    }
}

// ============================================================================
// RIGHT BUTTON BOX - Pattern: 3.4w × 5.1h
// ============================================================================
@Composable
fun VirtualControlsRight(
    onButtonA: () -> Unit,
    onButtonB: () -> Unit,
    onR: () -> Unit,
    onStart: () -> Unit,
    availableWidth: Int,
    availableHeight: Int
) {
    android.util.Log.d("VirtualControlsRight", "🟢 FUNCTION CALLED! Width: $availableWidth, Height: $availableHeight")

    val PATTERN_WIDTH = 3.4f
    val PATTERN_HEIGHT = 5.1f

    val density = LocalDensity.current.density

    val boxRatio = availableWidth.toFloat() / availableHeight.toFloat()
    val patternRatio = PATTERN_WIDTH / PATTERN_HEIGHT

    val X = if (boxRatio < patternRatio) {
        floor(availableWidth / PATTERN_WIDTH)
    } else {
        floor(availableHeight / PATTERN_HEIGHT)
    }.toInt()

    // All sizes in PIXELS, floored
    val buttonSize = X
    val rButtonWidth = floor(X * 1.5f).toInt()
    val rButtonHeight = floor(X * 0.7f).toInt()
    val startWidth = floor(X * 1.2f).toInt()
    val startHeight = floor(X * 0.6f).toInt()
    val smallSpacer = floor(X * 0.2f).toInt()       // 0.2X
    val mediumSpacer = floor(X * 0.7f).toInt()      // 0.7X
    val largeSpacer = floor(X * 2.0f).toInt()       // 2.0X
    val leftSpacer = floor(X * 1.7f).toInt()        // 1.7X
    val rightSpacer = floor(X * 0.7f).toInt()       // 0.7X

    val mainFontSize = (X * 0.4f / density).sp
    val triggerFontSize = (X * 0.35f / density).sp
    val smallFontSize = (X * 0.25f / density).sp

    Column(
        modifier = Modifier
            .fillMaxSize()
            .background(Color(0xFF1a1a1a)),
        horizontalAlignment = Alignment.CenterHorizontally,
        verticalArrangement = Arrangement.Top
    ) {
        // spacer h0.2x
        Spacer(modifier = Modifier.height((smallSpacer / density).dp))

        // R button h0.7x w1.5x
        Button(
            onClick = onR,
            modifier = Modifier.size(
                width = (rButtonWidth / density).dp,
                height = (rButtonHeight / density).dp
            ),
            contentPadding = PaddingValues(0.dp)
        ) {
            Text("R", fontWeight = FontWeight.Bold, fontSize = triggerFontSize)
        }

        // spacer h0.7x
        Spacer(modifier = Modifier.height((mediumSpacer / density).dp))

        // spacer w1.7x, A button h1x w1x, spacer w0.7x
        Row(horizontalArrangement = Arrangement.Center) {
            Spacer(modifier = Modifier.width((leftSpacer / density).dp))
            Button(
                onClick = onButtonA,
                modifier = Modifier.size((buttonSize / density).dp),
                contentPadding = PaddingValues(0.dp)
            ) {
                Text("A", fontWeight = FontWeight.Bold, fontSize = mainFontSize)
            }
            Spacer(modifier = Modifier.width((rightSpacer / density).dp))
        }

        // spacer w0.7x, B button h1x w1x, spacer w1.7x (no spacer between A and B rows!)
        Row(horizontalArrangement = Arrangement.Center) {
            Spacer(modifier = Modifier.width((rightSpacer / density).dp))
            Button(
                onClick = onButtonB,
                modifier = Modifier.size((buttonSize / density).dp),
                contentPadding = PaddingValues(0.dp)
            ) {
                Text("B", fontWeight = FontWeight.Bold, fontSize = mainFontSize)
            }
            Spacer(modifier = Modifier.width((leftSpacer / density).dp))
        }

        // spacer h0.7x
        Spacer(modifier = Modifier.height((mediumSpacer / density).dp))

        // spacer w0.2x, start h0.6x w1.2x, spacer w2x
        Row(horizontalArrangement = Arrangement.Center) {
            Spacer(modifier = Modifier.width((smallSpacer / density).dp))
            Button(
                onClick = onStart,
                modifier = Modifier.size(
                    width = (startWidth / density).dp,
                    height = (startHeight / density).dp
                ),
                contentPadding = PaddingValues(0.dp)
            ) {
                Text("STA", fontWeight = FontWeight.Bold, fontSize = smallFontSize)
            }
            Spacer(modifier = Modifier.width((largeSpacer / density).dp))
        }

        // spacer h0.2x
        Spacer(modifier = Modifier.height((smallSpacer / density).dp))
    }
}

// ============================================================================
// PORTRAIT MODE - Pattern: 6.8w × 5.1h
// ============================================================================
@Composable
fun VirtualControls(
    onDPadUp: () -> Unit,
    onDPadDown: () -> Unit,
    onDPadLeft: () -> Unit,
    onDPadRight: () -> Unit,
    onButtonA: () -> Unit,
    onButtonB: () -> Unit,
    onSelect: () -> Unit,
    onStart: () -> Unit,
    onL: () -> Unit,
    onR: () -> Unit,
    availableHeight: Int = 0,
    availableWidth: Int = 0
) {
    val SINGLE_BOX_PATTERN_WIDTH = 3.4f
    val SINGLE_BOX_PATTERN_HEIGHT = 5.1f

    val density = LocalDensity.current.density

    // Split width for two boxes side-by-side
    val boxAvailableWidth = availableWidth / 2
    val boxAvailableHeight = availableHeight

    val boxRatio = boxAvailableWidth.toFloat() / boxAvailableHeight.toFloat()
    val patternRatio = SINGLE_BOX_PATTERN_WIDTH / SINGLE_BOX_PATTERN_HEIGHT

    val X = if (boxRatio < patternRatio) {
        floor(boxAvailableWidth / SINGLE_BOX_PATTERN_WIDTH)
    } else {
        floor(boxAvailableHeight / SINGLE_BOX_PATTERN_HEIGHT)
    }.toInt()

    val boxWidth = floor(X * SINGLE_BOX_PATTERN_WIDTH).toInt()
    val boxHeight = floor(X * SINGLE_BOX_PATTERN_HEIGHT).toInt()

    val boxWidthDp = (boxWidth / density).dp
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
                onDPadUp = onDPadUp,
                onDPadDown = onDPadDown,
                onDPadLeft = onDPadLeft,
                onDPadRight = onDPadRight,
                onL = onL,
                onSelect = onSelect,
                availableWidth = boxWidth,
                availableHeight = boxHeight
            )
        }

        Box(modifier = Modifier.size(width = boxWidthDp, height = boxHeightDp)) {
            VirtualControlsRight(
                onButtonA = onButtonA,
                onButtonB = onButtonB,
                onR = onR,
                onStart = onStart,
                availableWidth = boxWidth,
                availableHeight = boxHeight
            )
        }
    }
}