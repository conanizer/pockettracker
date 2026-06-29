package com.conanizer.pockettracker.ui.modules

import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.geometry.Size
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.drawscope.DrawScope
import com.conanizer.pockettracker.ui.theme.AppTheme
import com.conanizer.pockettracker.ui.TrackerModule
import com.conanizer.pockettracker.ui.theme.VisualizerType
import com.conanizer.pockettracker.ui.darken

/**
 * VISUALIZER MODULE
 *
 * Top bar visualizer (620×70). Dispatches to one of six render modes based on
 * appTheme.visualizerType (SCOPE / FLAT / OCTA / OCTA_FULL / SPECTRUM / SPECTRUM_PEAKS).
 * SCOPE and OCTA share the same ProTracker-style pixel-dot wave (drawWaveDots).
 *
 * Default size: 620×70 pixels
 */
class OscilloscopeModule(
    override val width: Int = 620,
    override val height: Int = 70
) : TrackerModule {

    companion object {
        const val WAVEFORM_GAIN = 3.0f

        const val NUM_BARS = 40
        const val BAR_W = 14
        const val BAR_GAP = 1
        // Total bar area = 40*14 + 39 = 599px; 10px margin each side within 620
        const val BAR_START_OFFSET = 10

        const val PEAK_HOLD_FRAMES = 30

        // SPECTRUM: LED-style segment height and gap (in pixels)
        const val SEGMENT_H = 2
        const val SEG_GAP   = 1
        const val SEG_STEP  = SEGMENT_H + SEG_GAP  // 3px per LED cell

        // SPECTRUM smoothing: instant attack, exponential decay (~333ms fall at 60fps)
        const val BAR_DECAY = 0.90f

        // OCTA: gap between track scopes (in pixels)
        const val OCTA_TRACK_GAP = 10
    }

    // SPECTRUM_PEAKS peak-hold state — persists between frames (module instance lives in TrackerLayout)
    private val peakValues = FloatArray(NUM_BARS)
    private val peakDecayCounters = IntArray(NUM_BARS)
    // Smoothed bar amplitudes for SPECTRUM / SPECTRUM_PEAKS (instant attack, slow fall)
    private val barSmoothed = FloatArray(NUM_BARS)

    override fun DrawScope.draw(x: Int, y: Int, scale: Int, state: Any?) {
        val oscState = state as? OscilloscopeState
        val waveformData = oscState?.waveformBuffer ?: (state as? FloatArray) ?: FloatArray(width)
        val t = oscState?.appTheme ?: AppTheme.Companion.CLASSIC

        // ProTracker look: OCTA modes fill the whole strip with `background` so the inter-scope gaps
        // read as background, then drawOcta paints each scope's own `vizBackground` panel. Every other
        // mode fills the strip with `vizBackground` as before.
        val isOcta = t.visualizerType == VisualizerType.OCTA ||
                     t.visualizerType == VisualizerType.OCTA_FULL
        drawRect(
            color = Color(if (isOcta) t.background else t.vizBackground),
            topLeft = Offset((x * scale).toFloat(), (y * scale).toFloat()),
            size = Size((width * scale).toFloat(), (height * scale).toFloat())
        )

        when (t.visualizerType) {
            VisualizerType.SCOPE          -> drawScope(x, y, scale, waveformData, t)
            VisualizerType.FLAT           -> drawFlat(x, y, scale, t)
            VisualizerType.OCTA,
            VisualizerType.OCTA_FULL      -> drawOcta(x, y, scale, oscState?.trackWaveforms, oscState?.activeTrackMask ?: 0, t)
            VisualizerType.SPECTRUM       -> drawBarAmps(x, y, scale, oscState?.spectrumData ?: FloatArray(NUM_BARS), t, peakMode = false)
            VisualizerType.SPECTRUM_PEAKS -> drawBarAmps(x, y, scale, oscState?.spectrumData ?: FloatArray(NUM_BARS), t, peakMode = true)
        }
    }

    private fun DrawScope.drawScope(x: Int, y: Int, scale: Int, waveformData: FloatArray, t: AppTheme) {
        val centerY = y + height / 2
        val maxAmplitude = (height / 2) - 4
        drawLine(
            color = Color(t.vizCenterLine),
            start = Offset((x * scale).toFloat(), (centerY * scale).toFloat()),
            end = Offset(((x + width) * scale).toFloat(), (centerY * scale).toFloat()),
            strokeWidth = scale.toFloat()
        )
        // ProTracker-style pixel dots — same look as each OCTA lane, but spanning the full width.
        drawWaveDots(x, width, waveformData, centerY, maxAmplitude, Color(t.vizWave), scale)
    }

    /**
     * Draw one waveform as integer-quantized pixel dots (one drawRect per x). Shared by SCOPE
     * (full width) and OCTA (per-lane). One drawRect per x (the same approach OCTA uses) avoids both
     * a continuous Path stroke and the 619 individual drawLine calls that overflow the RenderThread
     * native stack on Android 11 (Adreno driver), while keeping SCOPE visually consistent with OCTA.
     */
    private fun DrawScope.drawWaveDots(
        scopeX: Int, scopeWidth: Int, waveData: FloatArray,
        centerY: Int, maxAmplitude: Int, waveColor: Color, scale: Int
    ) {
        for (i in 0 until scopeWidth) {
            val waveIdx = (i.toLong() * waveData.size / scopeWidth).toInt().coerceIn(0, waveData.size - 1)
            val sample = (waveData[waveIdx] * WAVEFORM_GAIN).coerceIn(-1f, 1f)
            val dotY = centerY + (sample * maxAmplitude).toInt()
            drawRect(
                color = waveColor,
                topLeft = Offset(((scopeX + i) * scale).toFloat(), (dotY * scale).toFloat()),
                size = Size(scale.toFloat(), scale.toFloat())
            )
        }
    }

    private fun DrawScope.drawBarAmps(
        x: Int, y: Int, scale: Int,
        rawAmps: FloatArray,
        t: AppTheme,
        peakMode: Boolean
    ) {
        val barBottom = y + height - 2           // 2px margin at bottom
        val maxAmp = (height - 4).toFloat()      // full usable height (2px top + 2px bottom margin)

        for (i in 0 until NUM_BARS) {
            val barX = x + BAR_START_OFFSET + i * (BAR_W + BAR_GAP)
            val raw = if (i < rawAmps.size) rawAmps[i] else 0f

            // Instant attack, exponential decay
            if (raw > barSmoothed[i]) barSmoothed[i] = raw
            else barSmoothed[i] = (barSmoothed[i] * BAR_DECAY).coerceAtLeast(0f)

            val barH = (barSmoothed[i] * maxAmp).toInt()

            // Draw LED-style segments from bottom up
            val barColor = Color(t.vizWave)
            var dy = 0
            while (dy + SEGMENT_H <= barH) {
                val segTop = barBottom - dy - SEGMENT_H + 1
                drawRect(
                    color = barColor,
                    topLeft = Offset((barX * scale).toFloat(), (segTop * scale).toFloat()),
                    size = Size((BAR_W * scale).toFloat(), (SEGMENT_H * scale).toFloat())
                )
                dy += SEG_STEP
            }

            if (peakMode) {
                if (barSmoothed[i] > peakValues[i]) {
                    peakValues[i] = barSmoothed[i]
                    peakDecayCounters[i] = 0
                } else {
                    peakDecayCounters[i]++
                    if (peakDecayCounters[i] > PEAK_HOLD_FRAMES) {
                        peakValues[i] = (peakValues[i] - SEG_STEP.toFloat() / maxAmp).coerceAtLeast(0f)
                    }
                }

                val peakH = (peakValues[i] * maxAmp).toInt()
                // Snap peak to nearest segment boundary
                val peakDy = (peakH / SEG_STEP) * SEG_STEP
                if (peakDy > barH) {
                    val peakTop = barBottom - peakDy - SEGMENT_H + 1
                    drawRect(
                        color = Color(t.vizWave.darken(0.55f)),
                        topLeft = Offset((barX * scale).toFloat(), (peakTop * scale).toFloat()),
                        size = Size((BAR_W * scale).toFloat(), (SEGMENT_H * scale).toFloat())
                    )
                }
            }
        }
    }

    private fun DrawScope.drawFlat(x: Int, y: Int, scale: Int, t: AppTheme) {
        drawLine(
            color = Color(t.vizCenterLine),
            start = Offset((x * scale).toFloat(), ((y + height - 1) * scale).toFloat()),
            end = Offset(((x + width) * scale).toFloat(), ((y + height - 1) * scale).toFloat()),
            strokeWidth = scale.toFloat()
        )
    }

    private fun DrawScope.drawOcta(
        x: Int, y: Int, scale: Int,
        trackWaveforms: Array<FloatArray>?,
        activeTrackMask: Int,
        t: AppTheme
    ) {
        val centerY = y + height / 2
        val maxAmplitude = (height / 2) - 4
        val waveColor = Color(t.vizWave)
        val centerColor = Color(t.vizCenterLine)
        val panelColor = Color(t.vizBackground)

        if (trackWaveforms == null || activeTrackMask == 0) {
            // Nothing scheduled — one full-width panel + center line (the strip itself is `background`).
            drawRect(
                color = panelColor,
                topLeft = Offset((x * scale).toFloat(), (y * scale).toFloat()),
                size = Size((width * scale).toFloat(), (height * scale).toFloat())
            )
            drawLine(
                color = centerColor,
                start = Offset((x * scale).toFloat(), (centerY * scale).toFloat()),
                end = Offset(((x + width) * scale).toFloat(), (centerY * scale).toFloat()),
                strokeWidth = scale.toFloat()
            )
            return
        }

        // 0-7 = song tracks, 8 = preview lane (only set when stopped — see PixelPerfectRenderer).
        // OCTA_FULL forces mask 0xFF (all 8 tracks) regardless of what's scheduled.
        val activeTracks = (0 until 9).filter { (activeTrackMask shr it) and 1 == 1 }
        val count = activeTracks.size
        // Each track scope gets an equal share of the width, minus gaps between them
        val totalGap = OCTA_TRACK_GAP * (count - 1)
        val trackW = (width - totalGap) / count

        activeTracks.forEachIndexed { idx, trackId ->
            val scopeX = x + idx * (trackW + OCTA_TRACK_GAP)
            val waveData = trackWaveforms[trackId]

            // ProTracker panel for this scope (the gaps between panels stay `background`)
            drawRect(
                color = panelColor,
                topLeft = Offset((scopeX * scale).toFloat(), (y * scale).toFloat()),
                size = Size((trackW * scale).toFloat(), (height * scale).toFloat())
            )

            // Center line for this scope
            drawLine(
                color = centerColor,
                start = Offset((scopeX * scale).toFloat(), (centerY * scale).toFloat()),
                end = Offset(((scopeX + trackW) * scale).toFloat(), (centerY * scale).toFloat()),
                strokeWidth = scale.toFloat()
            )

            drawWaveDots(scopeX, trackW, waveData, centerY, maxAmplitude, waveColor, scale)
        }
    }
}

data class OscilloscopeState(
    val waveformBuffer: FloatArray,
    val appTheme: AppTheme = AppTheme.Companion.CLASSIC,
    val trackWaveforms: Array<FloatArray>? = null,
    val activeTrackMask: Int = 0,
    val spectrumData: FloatArray? = null  // 40 log-spaced bins (0-1) for SPECTRUM modes
) {
    override fun equals(other: Any?): Boolean {
        if (this === other) return true
        if (other !is OscilloscopeState) return false
        return waveformBuffer.contentEquals(other.waveformBuffer) &&
               appTheme == other.appTheme &&
               activeTrackMask == other.activeTrackMask &&
               (trackWaveforms === other.trackWaveforms ||
                (trackWaveforms != null && other.trackWaveforms != null &&
                 trackWaveforms.indices.all { trackWaveforms[it].contentEquals(other.trackWaveforms[it]) })) &&
               (spectrumData === other.spectrumData ||
                (spectrumData != null && other.spectrumData != null &&
                 spectrumData.contentEquals(other.spectrumData)))
    }
    override fun hashCode(): Int = 31 * waveformBuffer.contentHashCode() + appTheme.hashCode()
}
