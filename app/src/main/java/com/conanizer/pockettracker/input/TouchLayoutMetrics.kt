// ============================================================================
// TouchLayoutMetrics.kt
//
// The SIZE arithmetic behind the four touch layouts, lifted out of the
// composables in VirtualControls.kt so that it can be RECORDED.
//
// ── Why this file exists (convergence Phase B2) ─────────────────────────────
//
// Phase D rewrites VirtualControls.kt in C++. Phase E deletes the Kotlin, and
// after that no new golden can ever be recorded from it — so anything the port
// wants to be checked against has to be recorded NOW, while Kotlin still
// answers.
//
// The catch is that a touch layout is two different computations wearing one
// coat, and only one of them is in this file:
//
//   SIZES     — "each arrow is X px square, spacers are 0.2X" — plain
//               arithmetic over (availableWidth, availableHeight, density).
//               That is everything below, and a plain JVM test can read it.
//
//   POSITIONS — "the UP arrow lands at x=340, y=210" — produced by COMPOSE's
//               measure/layout pass from Column/Row + Arrangement.Center, and
//               in Portrait2 from Modifier.weight(1f). Not in this file, not
//               in any file; it only exists once Android lays the screen out.
//
// So the recorded golden pins the maths, not the arrangement. That split is
// deliberate and its rationale is worth keeping: a wrong ARRANGEMENT (buttons
// out of order, off-centre, overlapping) is obvious the moment you look at a
// phone, while a wrong PROPORTION only shows up on a screen size nobody in the
// room owns. The cheap recording covers the failure the eye cannot.
//
// ⚠️ The composables MUST call these functions rather than keeping a private
// copy of the sums. A golden that records a reimplementation records what its
// author believed, not what the app does — the distinction tools/ptdispatch's
// header makes at length. Every function here is the code that actually ran on
// the device, which is the only thing that makes the recording worth having.
//
// Types stop at the platform boundary on purpose: these return raw Int/Float
// px, sp and dp VALUES, and the composables wrap them in .dp / .sp. Compose's
// unit types are not needed to state the arithmetic, and keeping them out is
// what lets this be called from a unit test with no Android on the classpath.
// ============================================================================

package com.conanizer.pockettracker.input

import kotlin.math.floor

object TouchLayoutMetrics {

    // Pattern: 3.4w × 5.1h for each side box. The literals live here now; the
    // composables' own copies are gone.
    const val PATTERN_WIDTH = 3.4f
    const val PATTERN_HEIGHT = 5.1f

    /** Portrait2's grid is 135 units on its shorter axis. */
    const val GRID_UNITS = 135f

    /**
     * The base unit X for the 3.4×5.1 pattern: the largest whole pixel size at
     * which the pattern still fits inside (w, h). Shared by the LEFT box, the
     * RIGHT box, and the two-box PORTRAIT split — which is why it is one
     * function and not three copies.
     *
     * Copied VERBATIM from the composables, `.toInt()` placement included: the
     * two branches yield Float and the conversion applies to the whole
     * if-expression. Preserved rather than tidied, because tidying arithmetic
     * that is about to become a conformance reference is how a port acquires a
     * difference nobody chose.
     */
    fun patternUnit(availableWidth: Int, availableHeight: Int): Int {
        val boxRatio = availableWidth.toFloat() / availableHeight.toFloat()
        val patternRatio = PATTERN_WIDTH / PATTERN_HEIGHT
        return if (boxRatio < patternRatio) {
            floor(availableWidth / PATTERN_WIDTH)
        } else {
            floor(availableHeight / PATTERN_HEIGHT)
        }.toInt()
    }

    /** LEFT box: L, D-pad, SELECT. */
    data class Left(
        val x: Int,
        val buttonSize: Int,
        val lButtonWidth: Int,
        val lButtonHeight: Int,
        val selectWidth: Int,
        val selectHeight: Int,
        val smallSpacer: Int,
        val largeSpacer: Int,
        val mediumSpacerWidth: Int,
        val mainFontSp: Float,
        val triggerFontSp: Float,
        val smallFontSp: Float,
    )

    /** RIGHT box: R, A, B, START. */
    data class Right(
        val x: Int,
        val buttonSize: Int,
        val rButtonWidth: Int,
        val rButtonHeight: Int,
        val startWidth: Int,
        val startHeight: Int,
        val smallSpacer: Int,
        val mediumSpacer: Int,
        val largeSpacer: Int,
        val leftSpacer: Int,
        val rightSpacer: Int,
        val mainFontSp: Float,
        val triggerFontSp: Float,
        val smallFontSp: Float,
    )

    /**
     * The two-box PORTRAIT split. Each box gets HALF the width, and the box
     * size it computes is then handed down to [left] / [right] as their own
     * available size — so an error here moves every button on the screen.
     */
    data class Portrait(
        val x: Int,
        val boxWidth: Int,
        val boxHeight: Int,
    )

    /** PORTRAIT2: the 135-unit grid, four rows of weight-distributed cells. */
    data class Portrait2(
        val x: Float,
        val cellDp: Float,
        val paddingDp: Float,
        val largeSp: Float,
        val smallSp: Float,
        val sqOffXDp: Float,
        val wideOffXDp: Float,
        val offYDp: Float,
        val pressedDp: Float,
    )

    fun left(availableWidth: Int, availableHeight: Int, density: Float): Left {
        val x = patternUnit(availableWidth, availableHeight)
        return Left(
            x = x,
            buttonSize = x,
            lButtonWidth = floor(x * 1.5f).toInt(),
            lButtonHeight = floor(x * 0.7f).toInt(),
            selectWidth = floor(x * 1.2f).toInt(),
            selectHeight = floor(x * 0.6f).toInt(),
            smallSpacer = floor(x * 0.2f).toInt(),
            largeSpacer = floor(x * 2.0f).toInt(),
            mediumSpacerWidth = floor(x * 1.0f).toInt(),
            mainFontSp = x * 0.4f / density,
            triggerFontSp = x * 0.35f / density,
            smallFontSp = x * 0.25f / density,
        )
    }

    fun right(availableWidth: Int, availableHeight: Int, density: Float): Right {
        val x = patternUnit(availableWidth, availableHeight)
        return Right(
            x = x,
            buttonSize = x,
            rButtonWidth = floor(x * 1.5f).toInt(),
            rButtonHeight = floor(x * 0.7f).toInt(),
            startWidth = floor(x * 1.2f).toInt(),
            startHeight = floor(x * 0.6f).toInt(),
            smallSpacer = floor(x * 0.2f).toInt(),
            // ⚠️ 0.7f here, where LEFT's mediumSpacerWidth is 1.0f. The two
            // boxes are NOT mirror images and never were; this is why they are
            // two functions rather than one with a side flag.
            mediumSpacer = floor(x * 0.7f).toInt(),
            largeSpacer = floor(x * 2.0f).toInt(),
            leftSpacer = floor(x * 1.7f).toInt(),
            rightSpacer = floor(x * 0.7f).toInt(),
            mainFontSp = x * 0.4f / density,
            triggerFontSp = x * 0.35f / density,
            smallFontSp = x * 0.25f / density,
        )
    }

    fun portrait(availableWidth: Int, availableHeight: Int): Portrait {
        val boxAvailableWidth = availableWidth / 2
        val boxAvailableHeight = availableHeight
        val x = patternUnit(boxAvailableWidth, boxAvailableHeight)
        return Portrait(
            x = x,
            boxWidth = floor(x * PATTERN_WIDTH).toInt(),
            boxHeight = floor(x * PATTERN_HEIGHT).toInt(),
        )
    }

    /**
     * ⚠️ X is a FLOAT here, unlike the 3.4×5.1 boxes, and the composable's own
     * comment says why: flooring would leave sub-unit gaps at the edges on any
     * resolution that is not a multiple of 135 (2800px → floor 20 → 135×20 =
     * 2700, a 100px gap). Keeping it fractional means 135X == availableWidth
     * always. A port that "tidies" this to an Int reintroduces the gap.
     */
    fun portrait2(availableWidth: Int, availableHeight: Int, density: Float): Portrait2 {
        val x = minOf(availableWidth / GRID_UNITS, availableHeight / GRID_UNITS).coerceAtLeast(1f)
        return Portrait2(
            x = x,
            cellDp = x * 33f / density,
            paddingDp = x * 1.5f / density,
            largeSp = x * 11f / density,
            smallSp = x * 7f / density,
            sqOffXDp = x * 7f / density,
            wideOffXDp = x * 8f / density,
            offYDp = x * 4f / density,
            pressedDp = x * 1f / density,
        )
    }
}
