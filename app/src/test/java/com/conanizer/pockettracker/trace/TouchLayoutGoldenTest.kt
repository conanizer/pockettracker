package com.conanizer.pockettracker.trace

import com.conanizer.pockettracker.input.TouchLayoutMetrics
import org.junit.Assert.assertEquals
import org.junit.Test
import java.io.File
import kotlin.math.floor

/**
 * Convergence B2 — the TOUCH LAYOUT golden, the last thing recorded from Kotlin before the
 * convergence deletes it.
 *
 * Emits `testdata/units/touch-layout.txt`, every line computed by the REAL `TouchLayoutMetrics`
 * that `VirtualControls.kt`'s four composables call. Phase D's C++ `ui/touch_layout.*` re-parses
 * each line's inputs, recomputes the right-hand side, and byte-compares — the same contract as
 * p3-input/s3-units, so the touch layout stops being the one component ported on trust.
 *
 * ── ⚠️ WHAT THIS GOLDEN DOES **NOT** CLAIM, stated up front ─────────────────────────────────────
 *
 * It pins the SIZES, not the POSITIONS. `VirtualControls.kt` computes how big each button is and
 * how much space sits between them; WHERE each button lands is computed by Compose's measure/layout
 * pass from `Column`/`Row` + `Arrangement.Center`, and in Portrait2 from `Modifier.weight(1f)` —
 * whose own comment in that file says Compose distributes the width. Those positions exist only
 * once Android lays a real screen out, so no JVM test can record them and this one does not
 * pretend to.
 *
 * That split was chosen deliberately rather than settled for. A wrong ARRANGEMENT — buttons out of
 * order, off-centre, overlapping the backing image — is obvious the moment anyone looks at a phone.
 * A wrong PROPORTION at a screen size nobody in the room owns is invisible until a user reports it.
 * This golden covers the second, and the first is covered by having eyes. A port that matches every
 * line here can still stack the D-pad wrong; that is a known and accepted hole, not an oversight.
 *
 * ── The matrix ──────────────────────────────────────────────────────────────────────────────────
 *
 * Sizes bracket the real range (the composables are handed a CONTROL BOX, not the screen — see
 * `ScreenLayouts.kt`: half the device width per box in portrait, a side slice in landscape), and
 * three of them sit deliberately on the branch boundary. `patternUnit` forks on
 * `boxRatio < patternRatio` where patternRatio is 3.4/5.1, so (339,510) / (340,510) / (341,510)
 * take the width branch, the exact-equality branch and the height branch respectively — the one
 * fork in the whole file, pinned from both sides. A matrix of pretty round numbers would have
 * exercised one arm and called it covered.
 *
 * Densities are the real Android buckets plus the common off-bucket ones. Every value is exactly
 * representable in binary32, so the decimal form in the file round-trips bit-for-bit through
 * `strtof` and the C++ side needs no hex for the INPUT column.
 *
 * Like every golden here: missing file → generated; existing file → byte-compared (the drift
 * guard). To regenerate after an intentional change: delete testdata/units/touch-layout.txt,
 * rerun, commit.
 */
class TouchLayoutGoldenTest {

    companion object {
        private fun repoRoot(): File {
            var dir = File(System.getProperty("user.dir")!!).absoluteFile
            while (true) {
                if (File(dir, "settings.gradle.kts").exists() || File(dir, "settings.gradle").exists()) return dir
                dir = dir.parentFile ?: error("repo root not found from ${System.getProperty("user.dir")}")
            }
        }

        private val goldenFile: File get() = File(repoRoot(), "testdata/units/touch-layout.txt")

        /**
         * Control-box sizes in px. The composables guard `<= 0` and return before computing, so
         * nothing degenerate is swept — a line recorded for a size the app cannot reach would be
         * pinning behaviour no user can observe.
         */
        private val SIZES: List<Pair<Int, Int>> = listOf(
            // ── the branch boundary: patternRatio = 3.4/5.1. Both arms and the tie. ──
            339 to 510,   // boxRatio < patternRatio  → width branch
            340 to 510,   // boxRatio == patternRatio → height branch (the `<` is strict)
            341 to 510,   // boxRatio > patternRatio  → height branch

            // ── landscape side boxes (a slice of the screen, one per side) ──
            240 to 1080,
            300 to 1080,
            360 to 1440,
            432 to 1080,
            540 to 1200,

            // ── portrait half-boxes (deviceWidth/2 by the button-area height) ──
            360 to 600,
            360 to 800,
            540 to 700,
            540 to 900,
            720 to 1000,

            // ── Portrait2 full-width content areas; 135-grid multiples and non-multiples ──
            135 to 135,
            270 to 405,
            1080 to 1080,
            1080 to 2400,
            1440 to 3200,
            2800 to 1400,   // the resolution TouchLayoutMetrics.portrait2's comment names
            2400 to 1080,

            // ── small, and the Portrait2 height floor (buttonAreaH.coerceAtLeast(100)) ──
            100 to 100,
            200 to 100,
            720 to 100,
        )

        /** Real Android densities. Every one is exact in binary32, so decimal round-trips. */
        private val DENSITIES: List<Float> =
            listOf(0.75f, 1.0f, 1.5f, 2.0f, 2.625f, 2.75f, 3.0f, 3.5f, 4.0f)
    }

    /** House format: floats as `0x` + the 8 hex digits of their binary32 bits. */
    private fun f32(v: Float): String = "0x%08X".format(v.toRawBits())

    /** Densities are exact in binary32; print them readably. */
    private fun dStr(d: Float): String = if (d == d.toInt().toFloat()) "${d.toInt()}" else "$d"

    private fun record(): List<String> {
        val out = mutableListOf<String>()

        out += "# Convergence B2 — touch layout SIZE goldens (left / right / portrait / portrait2)."
        out += "# Generated by TouchLayoutGoldenTest from the REAL TouchLayoutMetrics that"
        out += "# VirtualControls.kt's composables call. Phase D's C++ ui/touch_layout.* re-parses"
        out += "# each line's inputs, recomputes the RHS, and byte-compares."
        out += "#"
        out += "# format: <KIND> w=<px> h=<px> [d=<density>] => <name>=<value> ..."
        out += "#         ints decimal, floats 0x+8 hex of binary32 bits (house convention),"
        out += "#         densities decimal (all are exact in binary32, so they round-trip)."
        out += "#"
        out += "# ⚠️ SIZES ONLY — NOT POSITIONS. Where each button lands is computed by Compose"
        out += "# (Column/Row + Arrangement.Center; Portrait2 uses Modifier.weight(1f)), not by any"
        out += "# Kotlin this file can call. A port matching every line here can still stack the"
        out += "# D-pad in the wrong order. That hole is known and accepted: a wrong arrangement is"
        out += "# obvious on a phone, a wrong proportion on an unowned screen size is not."
        out += "#"
        out += "# PORTRAIT carries no density: boxWidth/boxHeight are px and density never enters."
        out += "# The (339|340|341, 510) triple straddles the patternRatio branch — 340/510 is the"
        out += "# exact tie, and `boxRatio < patternRatio` being strict sends it down the HEIGHT arm."

        // ── LEFT ────────────────────────────────────────────────────────────────────────────────
        out += ""
        out += "# LEFT box — L trigger, D-pad, SELECT."
        for ((w, h) in SIZES) for (d in DENSITIES) {
            val m = TouchLayoutMetrics.left(w, h, d)
            out += "LEFT w=$w h=$h d=${dStr(d)} => x=${m.x} btn=${m.buttonSize} " +
                "lw=${m.lButtonWidth} lh=${m.lButtonHeight} selw=${m.selectWidth} selh=${m.selectHeight} " +
                "small=${m.smallSpacer} large=${m.largeSpacer} med=${m.mediumSpacerWidth} " +
                "fmain=${f32(m.mainFontSp)} ftrig=${f32(m.triggerFontSp)} fsmall=${f32(m.smallFontSp)}"
        }

        // ── RIGHT ───────────────────────────────────────────────────────────────────────────────
        out += ""
        out += "# RIGHT box — R trigger, A, B, START."
        out += "# ⚠️ med=0.7X here where LEFT's is 1.0X, and RIGHT alone carries leftSpacer/rightSpacer."
        out += "# The two boxes are not mirror images; a port that shares one function between them"
        out += "# will match LEFT and drift on RIGHT."
        for ((w, h) in SIZES) for (d in DENSITIES) {
            val m = TouchLayoutMetrics.right(w, h, d)
            out += "RIGHT w=$w h=$h d=${dStr(d)} => x=${m.x} btn=${m.buttonSize} " +
                "rw=${m.rButtonWidth} rh=${m.rButtonHeight} startw=${m.startWidth} starth=${m.startHeight} " +
                "small=${m.smallSpacer} med=${m.mediumSpacer} large=${m.largeSpacer} " +
                "lsp=${m.leftSpacer} rsp=${m.rightSpacer} " +
                "fmain=${f32(m.mainFontSp)} ftrig=${f32(m.triggerFontSp)} fsmall=${f32(m.smallFontSp)}"
        }

        // ── PORTRAIT ────────────────────────────────────────────────────────────────────────────
        out += ""
        out += "# PORTRAIT — the two-box split. boxWidth/boxHeight become the available size handed"
        out += "# down to LEFT and RIGHT, so an error here moves every button on the screen."
        for ((w, h) in SIZES) {
            val m = TouchLayoutMetrics.portrait(w, h)
            out += "PORTRAIT w=$w h=$h => x=${m.x} boxw=${m.boxWidth} boxh=${m.boxHeight}"
        }

        // ── PORTRAIT2 ───────────────────────────────────────────────────────────────────────────
        out += ""
        out += "# PORTRAIT2 — the 135-unit grid. x is a FLOAT here (flooring would leave edge gaps"
        out += "# on any resolution that is not a multiple of 135), and is floored at 1.0."
        for ((w, h) in SIZES) for (d in DENSITIES) {
            val m = TouchLayoutMetrics.portrait2(w, h, d)
            out += "PORTRAIT2 w=$w h=$h d=${dStr(d)} => x=${f32(m.x)} cell=${f32(m.cellDp)} " +
                "pad=${f32(m.paddingDp)} large=${f32(m.largeSp)} small=${f32(m.smallSp)} " +
                "sqoff=${f32(m.sqOffXDp)} wideoff=${f32(m.wideOffXDp)} offy=${f32(m.offYDp)} " +
                "pressed=${f32(m.pressedDp)}"
        }

        return out
    }

    @Test
    fun goldensMatchOrAreGenerated() {
        val lines = record()
        val f = goldenFile
        if (!f.exists()) {
            f.parentFile!!.mkdirs()
            f.writeText(lines.joinToString("\n", postfix = "\n"))
            println("GENERATED ${f.path} (${lines.size} lines)")
            return
        }
        val expected = f.readText().trimEnd('\n').split("\n")
        val actual = lines
        for (i in 0 until minOf(expected.size, actual.size)) {
            assertEquals("touch-layout.txt drift at line ${i + 1}", expected[i], actual[i])
        }
        assertEquals("touch-layout.txt line count", expected.size, actual.size)
    }

    /**
     * ⚠️ THE REFACTOR GUARD, and the reason it is a test rather than a careful reading.
     *
     * `TouchLayoutMetrics` did not exist until the B2 session; its contents were lifted out of the
     * four composables so a JVM test could reach them. A transcription error during that lift would
     * be invisible — the golden would faithfully record the WRONG arithmetic, the C++ port would
     * match it byte-for-byte, and both would disagree with the app that shipped.
     *
     * So this transcribes the pre-refactor expressions INDEPENDENTLY, straight from the composables
     * as they read before the lift, and asserts the extracted functions agree across the whole
     * matrix. It is checking the move, not the maths.
     */
    @Test
    fun extractionMatchesPreRefactorFormulas() {
        val patternWidth = 3.4f
        val patternHeight = 5.1f

        fun originalX(w: Int, h: Int): Int {
            val boxRatio = w.toFloat() / h.toFloat()
            val patternRatio = patternWidth / patternHeight
            return if (boxRatio < patternRatio) {
                floor(w / patternWidth)
            } else {
                floor(h / patternHeight)
            }.toInt()
        }

        for ((w, h) in SIZES) {
            for (d in DENSITIES) {
                val x = originalX(w, h)

                val l = TouchLayoutMetrics.left(w, h, d)
                assertEquals("left x @ $w×$h", x, l.x)
                assertEquals("left buttonSize @ $w×$h", x, l.buttonSize)
                assertEquals("left lButtonWidth @ $w×$h", floor(x * 1.5f).toInt(), l.lButtonWidth)
                assertEquals("left lButtonHeight @ $w×$h", floor(x * 0.7f).toInt(), l.lButtonHeight)
                assertEquals("left selectWidth @ $w×$h", floor(x * 1.2f).toInt(), l.selectWidth)
                assertEquals("left selectHeight @ $w×$h", floor(x * 0.6f).toInt(), l.selectHeight)
                assertEquals("left smallSpacer @ $w×$h", floor(x * 0.2f).toInt(), l.smallSpacer)
                assertEquals("left largeSpacer @ $w×$h", floor(x * 2.0f).toInt(), l.largeSpacer)
                assertEquals("left mediumSpacerWidth @ $w×$h", floor(x * 1.0f).toInt(), l.mediumSpacerWidth)
                assertEquals("left mainFont @ $w×$h d=$d", x * 0.4f / d, l.mainFontSp, 0.0f)
                assertEquals("left triggerFont @ $w×$h d=$d", x * 0.35f / d, l.triggerFontSp, 0.0f)
                assertEquals("left smallFont @ $w×$h d=$d", x * 0.25f / d, l.smallFontSp, 0.0f)

                val r = TouchLayoutMetrics.right(w, h, d)
                assertEquals("right x @ $w×$h", x, r.x)
                assertEquals("right buttonSize @ $w×$h", x, r.buttonSize)
                assertEquals("right rButtonWidth @ $w×$h", floor(x * 1.5f).toInt(), r.rButtonWidth)
                assertEquals("right rButtonHeight @ $w×$h", floor(x * 0.7f).toInt(), r.rButtonHeight)
                assertEquals("right startWidth @ $w×$h", floor(x * 1.2f).toInt(), r.startWidth)
                assertEquals("right startHeight @ $w×$h", floor(x * 0.6f).toInt(), r.startHeight)
                assertEquals("right smallSpacer @ $w×$h", floor(x * 0.2f).toInt(), r.smallSpacer)
                assertEquals("right mediumSpacer @ $w×$h", floor(x * 0.7f).toInt(), r.mediumSpacer)
                assertEquals("right largeSpacer @ $w×$h", floor(x * 2.0f).toInt(), r.largeSpacer)
                assertEquals("right leftSpacer @ $w×$h", floor(x * 1.7f).toInt(), r.leftSpacer)
                assertEquals("right rightSpacer @ $w×$h", floor(x * 0.7f).toInt(), r.rightSpacer)
                assertEquals("right mainFont @ $w×$h d=$d", x * 0.4f / d, r.mainFontSp, 0.0f)
                assertEquals("right triggerFont @ $w×$h d=$d", x * 0.35f / d, r.triggerFontSp, 0.0f)
                assertEquals("right smallFont @ $w×$h d=$d", x * 0.25f / d, r.smallFontSp, 0.0f)

                val p2 = TouchLayoutMetrics.portrait2(w, h, d)
                val px2 = minOf(w / 135f, h / 135f).coerceAtLeast(1f)
                assertEquals("p2 x @ $w×$h", px2, p2.x, 0.0f)
                assertEquals("p2 cell @ $w×$h d=$d", px2 * 33f / d, p2.cellDp, 0.0f)
                assertEquals("p2 pad @ $w×$h d=$d", px2 * 1.5f / d, p2.paddingDp, 0.0f)
                assertEquals("p2 largeSp @ $w×$h d=$d", px2 * 11f / d, p2.largeSp, 0.0f)
                assertEquals("p2 smallSp @ $w×$h d=$d", px2 * 7f / d, p2.smallSp, 0.0f)
                assertEquals("p2 sqOffX @ $w×$h d=$d", px2 * 7f / d, p2.sqOffXDp, 0.0f)
                assertEquals("p2 wideOffX @ $w×$h d=$d", px2 * 8f / d, p2.wideOffXDp, 0.0f)
                assertEquals("p2 offY @ $w×$h d=$d", px2 * 4f / d, p2.offYDp, 0.0f)
                assertEquals("p2 pressed @ $w×$h d=$d", px2 * 1f / d, p2.pressedDp, 0.0f)
            }

            // PORTRAIT takes no density.
            val p = TouchLayoutMetrics.portrait(w, h)
            val bx = originalX(w / 2, h)
            assertEquals("portrait x @ $w×$h", bx, p.x)
            assertEquals("portrait boxWidth @ $w×$h", floor(bx * patternWidth).toInt(), p.boxWidth)
            assertEquals("portrait boxHeight @ $w×$h", floor(bx * patternHeight).toInt(), p.boxHeight)
        }
    }
}
