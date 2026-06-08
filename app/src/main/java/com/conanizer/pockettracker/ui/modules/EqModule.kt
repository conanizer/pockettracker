package com.conanizer.pockettracker.ui.modules

import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.geometry.Size
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.Path
import androidx.compose.ui.graphics.drawscope.DrawScope
import androidx.compose.ui.graphics.drawscope.Stroke
import com.conanizer.pockettracker.ui.theme.AppTheme
import com.conanizer.pockettracker.input.CursorCapabilities
import com.conanizer.pockettracker.input.CursorContext
import com.conanizer.pockettracker.input.CursorContextFactory
import com.conanizer.pockettracker.input.CursorValueType
import com.conanizer.pockettracker.ui.overlays.EQ_BAND_TYPE_NAMES
import com.conanizer.pockettracker.ui.overlays.EqCallerContext
import com.conanizer.pockettracker.ui.TrackerModule
import com.conanizer.pockettracker.core.data.EqBand
import com.conanizer.pockettracker.core.data.Project
import com.conanizer.pockettracker.core.logic.InputAction
import com.conanizer.pockettracker.ui.darken
import com.conanizer.pockettracker.ui.drawBitmapText
import com.conanizer.pockettracker.ui.toHex2
import kotlin.math.*

data class EqState(
    val project:       Project,
    val slotIndex:     Int,
    val cursorRow:     Int,
    val callerContext: EqCallerContext,
    val spectrumData:  FloatArray? = null,
    val appTheme: AppTheme = AppTheme.Companion.CLASSIC,
)

/**
 * EQ EDITOR SCREEN MODULE
 *
 * Layout:
 *   Y=0..VIS_H-1      — Visualization: spectrum bars (background) + EQ curve (foreground)
 *   Y=VIS_H..height-1 — Band editor: 3 columns (one per band), 4 param rows each
 *
 * Cursor: band = cursorRow/4 (column), param = cursorRow%4 (row within column)
 *   DPAD UP/DOWN    — move between params within the active band column
 *   DPAD LEFT/RIGHT — switch between band columns
 *   A + DPAD UP/DN  — +1 / −1
 *   A + DPAD LT/RT  — +16 / −16 (TYPE: ±1)
 *   B + DPAD LT/RT  — prev/next EQ preset slot
 *   SELECT          — close
 */
class EqModule : TrackerModule {
    override val width  = 495
    override val height = 392

    companion object {
        // ── Visualization area ────────────────────────────────────────────────
        const val VIS_H    = 220          // height of spectrum + curve display
        const val VIS_DB   = 15f          // ±dB range displayed

        // ── Editor area ────────────────────────────────────────────────────────
        const val HEADER_H   = 21
        const val ROW_H      = 21
        const val EDITOR_Y   = HEADER_H + ROW_H + VIS_H + 1  // header + 1-row spacer + viz + 1px gap
        const val FONT_SCALE   = 3
        const val CHAR_SPACING = 2

        // Editor column geometry: left label col + 3 equal band value cols
        const val LABEL_COL_W = 90
        const val BAND_COL_W  = 135  // (495 - LABEL_COL_W) / 3

        // cursor row constants
        const val MAX_CURSOR_ROW = 11

        val PARAM_LABELS = listOf("TYPE", "FREQ", "GAIN", "Q")
    }

    data class InputResult(val modified: Boolean, val eqBandChanged: Boolean = false)

    // ── Draw ──────────────────────────────────────────────────────────────────

    override fun DrawScope.draw(x: Int, y: Int, scale: Int, state: Any?) {
        val s = state as? EqState ?: return
        val t = s.appTheme

        // Background for whole module
        drawRect(
            color   = Color(t.background),
            topLeft = Offset((x * scale).toFloat(), (y * scale).toFloat()),
            size    = Size((width * scale).toFloat(), (height * scale).toFloat())
        )

        drawHeader(x, y, scale, s)
        drawVisualization(x, y, scale, s)
        drawEditor(x, y, scale, s)
    }

    // ── Header (very top, matches other screens) ──────────────────────────────

    private fun DrawScope.drawHeader(x: Int, y: Int, scale: Int, s: EqState) {
        val charW = 5 * FONT_SCALE + CHAR_SPACING
        val hY    = y + 3
        val t     = s.appTheme
        drawBitmapText("EQ ${s.slotIndex.toHex2()}", x + 10, hY, scale, Color(t.textTitle), CHAR_SPACING, FONT_SCALE)
        val callerLabel = when (val ctx = s.callerContext) {
            is EqCallerContext.MasterEq       -> "MASTER"
            is EqCallerContext.ReverbInputEq  -> "REV IN"
            is EqCallerContext.DelayInputEq   -> "DLY IN"
            is EqCallerContext.InstrumentEq   -> "INST ${ctx.instrId.toHex2()}"
            is EqCallerContext.SampleEditorFx -> "SAMPLE"
        }
        drawBitmapText(callerLabel, x + 10 + 8 * charW, hY, scale, Color(t.textParam), CHAR_SPACING, FONT_SCALE)
    }

    // ── Visualization ─────────────────────────────────────────────────────────

    private fun DrawScope.drawVisualization(x: Int, y: Int, scale: Int, s: EqState) {
        val vx      = x
        val vy      = y + HEADER_H + ROW_H
        val bottomY = ((vy + VIS_H) * scale).toFloat()
        val t       = s.appTheme

        // Background
        drawRect(
            color   = Color(t.vizBackground),
            topLeft = Offset((vx * scale).toFloat(), (vy * scale).toFloat()),
            size    = Size((width * scale).toFloat(), (VIS_H * scale).toFloat())
        )

        // Pre-compute spectrum curve Y positions (log-mapped bins from C++ map 1:1 to pixels).
        // Done before grid lines so fill can be drawn underneath them.
        val curveY: FloatArray? = s.spectrumData?.takeIf { it.size >= 2 }?.let { spectrum ->
            val n = spectrum.size
            FloatArray(width) { xi ->
                val bin0 = (xi.toFloat() / (width - 1) * (n - 1)).toInt().coerceIn(0, n - 1)
                val mag  = maxOf(spectrum[bin0], spectrum[(bin0 + 1).coerceAtMost(n - 1)])
                ((vy + VIS_H - (mag * VIS_H).toInt().coerceIn(0, VIS_H)) * scale).toFloat()
            }
        }

        // Spectrum fill — drawn before grid lines so grid lines render on top
        curveY?.let { cy ->
            val fillPath = Path().apply {
                moveTo((vx * scale).toFloat(), cy[0])
                for (xi in 1 until width) lineTo(((vx + xi) * scale).toFloat(), cy[xi])
                lineTo(((vx + width - 1) * scale).toFloat(), bottomY)
                lineTo((vx * scale).toFloat(), bottomY)
                close()
            }
            drawPath(fillPath, Color(t.textParam.darken(0.27f)))
        }

        // dB grid lines (horizontal) at -12, -6, 0, +6, +12
        val dbLevels = listOf(-12, -6, 0, 6, 12)
        for (db in dbLevels) {
            val lineY = vy + dbToPixel(db.toFloat())
            val gridColor = if (db == 0) Color(t.vizCenterLine) else Color(t.rowEvery4th)
            drawLine(
                color       = gridColor,
                start       = Offset((vx * scale).toFloat(), (lineY * scale).toFloat()),
                end         = Offset(((vx + width) * scale).toFloat(), (lineY * scale).toFloat()),
                strokeWidth = (scale * if (db == 0) 2 else 1).toFloat()
            )
        }

        // Frequency grid lines (vertical) + labels
        val freqMarkers = listOf(20f to "20", 100f to "100", 200f to "200", 500f to "500",
            1000f to "1K", 2000f to "2K", 5000f to "5K", 10000f to "10K", 20000f to "20K")
        for ((freq, label) in freqMarkers) {
            val fx = vx + freqToPixel(freq)
            drawLine(
                color       = Color(t.rowEvery4th),
                start       = Offset((fx * scale).toFloat(), (vy * scale).toFloat()),
                end         = Offset((fx * scale).toFloat(), ((vy + VIS_H) * scale).toFloat()),
                strokeWidth = scale.toFloat()
            )
            drawBitmapText(label, fx + 2, vy + 3, scale, Color(t.vizCenterLine), CHAR_SPACING, 2)
        }

        // Spectrum curve stroke — drawn after grid lines so it sits above them
        curveY?.let { cy ->
            val curvePath = Path().apply {
                moveTo((vx * scale).toFloat(), cy[0])
                for (xi in 1 until width) lineTo(((vx + xi) * scale).toFloat(), cy[xi])
            }
            drawPath(curvePath, Color(t.textParam), style = Stroke(width = scale.toFloat()))
        }

        // EQ band response curve (yellow, always on top of spectrum)
        val preset = s.project.eqPresets.getOrNull(s.slotIndex)
        if (preset != null) {
            val path = Path()
            var started = false
            for (xi in 0 until width) {
                val normX = xi.toFloat() / (width - 1)
                val freq  = 20f * (1000f).pow(normX)
                val db   = computeCombinedGainDb(preset.bands, freq)
                val py   = vy + dbToPixel(db)
                val px   = vx + xi
                if (!started) { path.moveTo((px * scale).toFloat(), (py * scale).toFloat()); started = true }
                else          { path.lineTo((px * scale).toFloat(), (py * scale).toFloat()) }
            }
            drawPath(path, Color(t.textCursor), style = Stroke(width = (scale * 2).toFloat()))
        }

        // Separator line
        val sepY = vy + VIS_H
        drawLine(
            color       = Color(t.vizCenterLine),
            start       = Offset((vx * scale).toFloat(), (sepY * scale).toFloat()),
            end         = Offset(((vx + width) * scale).toFloat(), (sepY * scale).toFloat()),
            strokeWidth = scale.toFloat()
        )
    }

    // ── Editor (bottom section) ───────────────────────────────────────────────

    private fun DrawScope.drawEditor(x: Int, y: Int, scale: Int, s: EqState) {
        val edY      = y + EDITOR_Y
        val curBand  = s.cursorRow / 4
        val curParam = s.cursorRow % 4
        val preset   = s.project.eqPresets.getOrNull(s.slotIndex)
        val t        = s.appTheme

        val bandX = intArrayOf(
            x + LABEL_COL_W,
            x + LABEL_COL_W + BAND_COL_W,
            x + LABEL_COL_W + 2 * BAND_COL_W
        )

        // Header row: active band = textTitle, inactive = textEmpty
        for (bi in 0..2) {
            val hdrCol = if (bi == curBand) Color(t.textTitle) else Color(t.textEmpty)
            drawBitmapText("BAND ${bi + 1}", bandX[bi] + 6, edY + 3, scale, hdrCol, CHAR_SPACING, FONT_SCALE)
        }

        // 4 param rows
        for (pi in 0..3) {
            val rowY     = edY + ROW_H + pi * ROW_H
            val isParSel = pi == curParam

            // Full-row background when cursor is on this row
            if (isParSel) {
                drawRect(
                    color   = Color(t.rowCursor),
                    topLeft = Offset((x * scale).toFloat(), (rowY * scale).toFloat()),
                    size    = Size((width * scale).toFloat(), (ROW_H * scale).toFloat())
                )
            }

            // Label: textCursor on cursor row, textEmpty otherwise
            val lblCol = if (isParSel) Color(t.textCursor) else Color(t.textEmpty)
            drawBitmapText(PARAM_LABELS[pi], x + 6, rowY + 3, scale, lblCol, CHAR_SPACING, FONT_SCALE)

            // Values: textCursor on cursor cell, textValue for active band, textEmpty for inactive
            for (bi in 0..2) {
                val isCursor  = bi == curBand && isParSel
                val band: EqBand? = preset?.bands?.getOrNull(bi)

                val valueCol = when {
                    isCursor       -> Color(t.textCursor)
                    bi == curBand  -> Color(t.textValue)
                    else           -> Color(t.textEmpty)
                }

                val valText = when {
                    band == null -> "--"
                    pi == 0      -> EQ_BAND_TYPE_NAMES.getOrElse(band.type) { "???" }
                    pi == 1      -> formatFreqHz(20f * 1000f.pow(band.freq / 255f))
                    pi == 2      -> formatGainDb((band.gain - 128f) / 128f * 12f)
                    else         -> band.q.toHex2()
                }
                drawBitmapText(valText, bandX[bi] + 6, rowY + 3, scale, valueCol, CHAR_SPACING, FONT_SCALE)
            }
        }
    }

    // ── Input ─────────────────────────────────────────────────────────────────

    fun getCursorContext(state: EqState): CursorContext {
        val preset = state.project.eqPresets.getOrNull(state.slotIndex)
            ?: return CursorContextFactory.none()
        val band = preset.bands.getOrNull(state.cursorBand)
            ?: return CursorContextFactory.none()
        return when (state.cursorParam) {
            0 -> CursorContext(
                valueType = CursorValueType.HEX_BYTE,
                capabilities = CursorCapabilities(
                    canIncrement = true,
                    canDecrement = true,
                    canIncrementFast = true,
                    canDecrementFast = true
                ),
                currentValue = band.type,
                minValue = 0, maxValue = EQ_BAND_TYPE_NAMES.size - 1,
                smallStep = 1, largeStep = 1
            )
            1 -> CursorContextFactory.hexByte(band.freq, min = 0, max = 255)
            2 -> CursorContextFactory.hexByte(band.gain, min = 0, max = 255)
            3 -> CursorContextFactory.hexByte(band.q,    min = 0, max = 255)
            else -> CursorContextFactory.none()
        }
    }

    fun handleInput(state: EqState, action: InputAction, onProjectModified: () -> Unit): InputResult {
        if (action !is InputAction.SET_VALUE) return InputResult(modified = false)
        val preset = state.project.eqPresets.getOrNull(state.slotIndex) ?: return InputResult(modified = false)
        val band   = preset.bands.getOrNull(state.cursorBand)           ?: return InputResult(modified = false)
        when (state.cursorParam) {
            0 -> band.type = action.value.coerceIn(0, EQ_BAND_TYPE_NAMES.size - 1)
            1 -> band.freq = action.value.coerceIn(0, 255)
            2 -> band.gain = action.value.coerceIn(0, 255)
            3 -> band.q    = action.value.coerceIn(0, 255)
        }
        onProjectModified()
        return InputResult(modified = true, eqBandChanged = true)
    }

    // ── EQ curve math ─────────────────────────────────────────────────────────

    /** Sum gain in dB from all active bands at the given frequency. */
    private fun computeCombinedGainDb(bands: Array<EqBand>, freq: Float, sampleRate: Float = 44100f): Float {
        var totalDb = 0f
        for (band in bands) {
            if (band.type == 0) continue
            totalDb += bandGainDb(band, freq, sampleRate)
        }
        return totalDb.coerceIn(-VIS_DB, VIS_DB)
    }

    private fun bandGainDb(band: EqBand, freq: Float, sampleRate: Float): Float {
        val fc    = 20f * (1000f).pow(band.freq / 255f)        // log 20-20kHz
        val gainDb = (band.gain - 128f) / 128f * 12f           // -12..+12 dB
        val q     = 0.1f * (100f).pow(band.q / 255f)           // log 0.1-10

        // LOWCUT/HICUT use DaisySP SVF (not biquad) — match SVF transfer function exactly
        if (band.type == 2 || band.type == 5) return svfGainDb(band.type, fc, q, freq, sampleRate)

        val w0    = 2f * PI.toFloat() * fc   / sampleRate
        val w     = 2f * PI.toFloat() * freq / sampleRate
        val cosW0 = cos(w0); val sinW0 = sin(w0)
        val alpha = sinW0 / (2f * q)
        val cosW  = cos(w);  val cos2W = cos(2f * w)
        val sinW  = sin(w);  val sin2W = sin(2f * w)

        val A = 10f.pow(gainDb / 40f)

        val b0: Float; val b1: Float; val b2: Float
        val a0: Float; val a1: Float; val a2: Float
        when (band.type) {
            1 -> { // LO SHELF
                val sqA = sqrt(A)
                b0 = A * ((A+1) - (A-1)*cosW0 + 2*sqA*alpha)
                b1 = 2*A * ((A-1) - (A+1)*cosW0)
                b2 = A * ((A+1) - (A-1)*cosW0 - 2*sqA*alpha)
                a0 = (A+1) + (A-1)*cosW0 + 2*sqA*alpha
                a1 = -2f * ((A-1) + (A+1)*cosW0)
                a2 = (A+1) + (A-1)*cosW0 - 2*sqA*alpha
            }
            3 -> { // BELL (peaking)
                b0=1+alpha*A; b1= -2*cosW0; b2=1-alpha*A
                a0=1+alpha/A; a1= -2*cosW0; a2=1-alpha/A
            }
            4 -> { // HISHELF
                val sqA = sqrt(A)
                b0 = A * ((A+1) + (A-1)*cosW0 + 2*sqA*alpha)
                b1 = -2*A * ((A-1) + (A+1)*cosW0)
                b2 = A * ((A+1) + (A-1)*cosW0 - 2*sqA*alpha)
                a0 = (A+1) - (A-1)*cosW0 + 2*sqA*alpha
                a1 = 2f * ((A-1) - (A+1)*cosW0)
                a2 = (A+1) - (A-1)*cosW0 - 2*sqA*alpha
            }
            else -> return 0f
        }

        // Evaluate H(e^jω): use complex numerator/denominator
        val numRe = b0 + b1*cosW + b2*cos2W
        val numIm = -(b1*sinW + b2*sin2W)
        val denRe = a0 + a1*cosW + a2*cos2W
        val denIm = -(a1*sinW + a2*sin2W)

        val magSq = (numRe*numRe + numIm*numIm) / (denRe*denRe + denIm*denIm + 1e-30f)
        return 10f * log10(magSq + 1e-30f)
    }

    /**
     * Z-domain transfer function of the DaisySP double-pass Chamberlin SVF.
     *
     * DaisySP Process() runs two sequential passes on the same input and averages:
     *   out_low  = 0.5*low_pass1  + 0.5*low_pass2
     *   out_high = 0.5*high_pass1 + 0.5*high_pass2
     *
     * Combined state update (drive=0):
     *   L[n] = β·L[n-1] + α·B[n-1] + f²·x[n],  β=1−f²
     *   B[n] = −α·L[n-1] + γ·B[n-1] + α·x[n],  γ=k²−f²
     * where k=1−fd−f², α=f(1+k).
     *
     * Steady-state at z=e^{jω}: B̂=α(z−1)/D, L̂=(αB̂+f²)/(z−β),
     *   D=(z−γ)(z−β)+α².
     *
     * pass1: low1 = L[n-1]+f·B[n-1],  high1 = x − L[n-1] − (d+f)·B[n-1]
     * pass2: low2 = L[n],              high2 = k·x − k·L[n-1] − μ·B[n-1], μ=f+(d+f)k
     *
     * Averaged transfer functions:
     *   H_LP = 0.5·(1+e^{−jω})·L̂ + 0.5·f·e^{−jω}·B̂
     *   H_HP = 0.5·(1+k) − 0.5·e^{−jω}·((1+k)·L̂ + (d+f+μ)·B̂)
     */
    private fun svfGainDb(type: Int, fc: Float, q: Float, vizFreq: Float, sampleRate: Float): Float {
        val fcC = fc.coerceAtMost(sampleRate * 0.45f)
        val f = 2.0 * sin(PI * minOf(0.25, fcC.toDouble() / (sampleRate * 2.0)))
        val res = (1.0 - 1.0 / (2.0 * q)).coerceIn(0.0, 0.99)
        val d = minOf(2.0 * (1.0 - res.pow(0.25)), minOf(2.0, 2.0 / f - f * 0.5))

        val k = 1.0 - f * d - f * f
        val alpha = f * (1.0 + k)
        val beta  = 1.0 - f * f
        val gamma = k * k - f * f
        val mu    = f + (d + f) * k

        val w    = 2.0 * PI * vizFreq / sampleRate
        val cosW = cos(w); val sinW = sin(w)

        // D = z² − (β+γ)z + (βγ+α²) evaluated at z = e^{jω}
        val dRe = cos(2.0 * w) - (beta + gamma) * cosW + (beta * gamma + alpha * alpha)
        val dIm = sin(2.0 * w) - (beta + gamma) * sinW
        val dMSq = dRe * dRe + dIm * dIm + 1e-30

        // B̂ = α(z−1)/D
        val bNumRe = alpha * (cosW - 1.0);  val bNumIm = alpha * sinW
        val bRe = (bNumRe * dRe + bNumIm * dIm) / dMSq
        val bIm = (bNumIm * dRe - bNumRe * dIm) / dMSq

        // L̂ = (αB̂ + f²) / (z−β)
        val lNumRe = alpha * bRe + f * f;  val lNumIm = alpha * bIm
        val zbRe = cosW - beta;            val zbIm = sinW
        val zbMSq = zbRe * zbRe + zbIm * zbIm + 1e-30
        val lRe = (lNumRe * zbRe + lNumIm * zbIm) / zbMSq
        val lIm = (lNumIm * zbRe - lNumRe * zbIm) / zbMSq

        // e^{−jω} terms reused by both branches
        val emjwL_Re = cosW * lRe + sinW * lIm   // Re(e^{-jω}·L̂)
        val emjwL_Im = cosW * lIm - sinW * lRe   // Im(e^{-jω}·L̂)
        val emjwB_Re = cosW * bRe + sinW * bIm   // Re(e^{-jω}·B̂)
        val emjwB_Im = cosW * bIm - sinW * bRe   // Im(e^{-jω}·B̂)

        val magSq: Double = if (type == 5) { // LP (HICUT)
            // H_LP = 0.5·(1+e^{−jω})·L̂ + 0.5·f·e^{−jω}·B̂
            val hRe = 0.5 * (lRe + emjwL_Re + f * emjwB_Re)
            val hIm = 0.5 * (lIm + emjwL_Im + f * emjwB_Im)
            hRe * hRe + hIm * hIm
        } else { // HP (LOWCUT)
            // H_HP = 0.5·(1+k) − 0.5·e^{−jω}·((1+k)·L̂ + (d+f+μ)·B̂)
            val onePlusK = 1.0 + k
            val dfm = d + f + mu
            val hRe = 0.5 * onePlusK - 0.5 * (onePlusK * emjwL_Re + dfm * emjwB_Re)
            val hIm =                - 0.5 * (onePlusK * emjwL_Im + dfm * emjwB_Im)
            hRe * hRe + hIm * hIm
        }

        return (10.0 * log10(magSq + 1e-30)).toFloat().coerceIn(-VIS_DB, VIS_DB)
    }

    // ── Helpers ───────────────────────────────────────────────────────────────

    /** Convert dB to y-pixel offset within the VIS_H area (0 dB = center). */
    private fun dbToPixel(db: Float): Int {
        val center = VIS_H / 2
        return (center - db * (VIS_H / 2f) / VIS_DB).toInt().coerceIn(0, VIS_H - 1)
    }

    /** Convert frequency (Hz) to x-pixel within the visualization width. */
    private fun freqToPixel(freq: Float): Int {
        val t = ln(freq / 20f) / ln(1000f)  // log base-1000 of (freq/20)
        return (t * (width - 1)).toInt().coerceIn(0, width - 1)
    }

    private fun formatFreqHz(hz: Float): String {
        val rounded = hz.toInt()
        return when {
            rounded < 1000  -> "${rounded}Hz"
            rounded < 10000 -> "${"%.1f".format(hz / 1000f)}kHz"
            else            -> "${(hz / 1000f + 0.5f).toInt()}kHz"
        }
    }

    private fun formatGainDb(db: Float): String {
        val sign = if (db >= 0f) "+" else ""
        return "${sign}%.1f".format(db)
    }
}

private val EqState.cursorBand:  Int get() = cursorRow / 4
private val EqState.cursorParam: Int get() = cursorRow % 4
