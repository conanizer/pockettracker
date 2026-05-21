package com.conanizer.pockettracker

import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.geometry.Size
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.Path
import androidx.compose.ui.graphics.drawscope.DrawScope
import androidx.compose.ui.graphics.drawscope.Stroke

/**
 * VISUALIZER MODULE
 *
 * Top bar visualizer (620×70). Dispatches to one of five render modes based on
 * appTheme.visualizerType. All modes receive the same FloatArray waveform data.
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

        // BARS/PEAKS: LED-style segment height and gap (in pixels)
        const val SEGMENT_H = 2
        const val SEG_GAP   = 1
        const val SEG_STEP  = SEGMENT_H + SEG_GAP  // 3px per LED cell

        // BARS/PEAKS smoothing: instant attack, exponential decay (~333ms fall at 60fps)
        const val BAR_DECAY = 0.90f

        // OCTA: gap between track scopes (in pixels)
        const val OCTA_TRACK_GAP = 3
    }

    // PEAKS mode state — persists between frames (module instance lives in TrackerLayout)
    private val peakValues = FloatArray(NUM_BARS)
    private val peakDecayCounters = IntArray(NUM_BARS)
    // Smoothed bar amplitudes for BARS/PEAKS/SPECTRUM/SPECTRUM_PEAKS (instant attack, slow fall)
    private val barSmoothed = FloatArray(NUM_BARS)

    override fun DrawScope.draw(x: Int, y: Int, scale: Int, state: Any?) {
        val oscState = state as? OscilloscopeState
        val waveformData = oscState?.waveformBuffer ?: (state as? FloatArray) ?: FloatArray(width)
        val t = oscState?.appTheme ?: AppTheme.CLASSIC

        drawRect(
            color = Color(t.vizBackground),
            topLeft = Offset((x * scale).toFloat(), (y * scale).toFloat()),
            size = Size((width * scale).toFloat(), (height * scale).toFloat())
        )

        when (t.visualizerType) {
            VisualizerType.SCOPE          -> drawScope(x, y, scale, waveformData, t)
            VisualizerType.BARS           -> drawBarAmps(x, y, scale, waveformToBarAmps(waveformData), t, peakMode = false)
            VisualizerType.PEAKS          -> drawBarAmps(x, y, scale, waveformToBarAmps(waveformData), t, peakMode = true)
            VisualizerType.MIRROR         -> drawMirror(x, y, scale, waveformData, t)
            VisualizerType.FLAT           -> drawFlat(x, y, scale, t)
            VisualizerType.OCTA           -> drawOcta(x, y, scale, oscState?.trackWaveforms, oscState?.activeTrackMask ?: 0, t)
            VisualizerType.SPECTRUM       -> drawBarAmps(x, y, scale, oscState?.spectrumData ?: FloatArray(NUM_BARS), t, peakMode = false)
            VisualizerType.SPECTRUM_PEAKS -> drawBarAmps(x, y, scale, oscState?.spectrumData ?: FloatArray(NUM_BARS), t, peakMode = true)
        }
    }

    private fun DrawScope.drawScope(x: Int, y: Int, scale: Int, waveformData: FloatArray, t: AppTheme) {
        val centerY = y + height / 2
        drawLine(
            color = Color(t.vizCenterLine),
            start = Offset((x * scale).toFloat(), (centerY * scale).toFloat()),
            end = Offset(((x + width) * scale).toFloat(), (centerY * scale).toFloat()),
            strokeWidth = scale.toFloat()
        )

        // Single path — 619 individual drawLine calls caused RenderThread stack overflow
        // on Android 11 (Adreno GPU driver consumes more native stack per draw command).
        val maxAmplitude = (height / 2) - 8
        val path = Path()
        for (i in 0 until width) {
            val sample = (waveformData[i % waveformData.size] * WAVEFORM_GAIN).coerceIn(-1f, 1f)
            val px = ((x + i) * scale).toFloat()
            val py = ((centerY + sample * maxAmplitude) * scale).toFloat()
            if (i == 0) path.moveTo(px, py) else path.lineTo(px, py)
        }
        drawPath(path = path, color = Color(t.vizWave), style = Stroke(width = scale.toFloat()))
    }

    private fun waveformToBarAmps(waveformData: FloatArray): FloatArray {
        val samplesPerBar = waveformData.size.toFloat() / NUM_BARS
        return FloatArray(NUM_BARS) { i ->
            val startSample = (i * samplesPerBar).toInt()
            val endSample = ((i + 1) * samplesPerBar).toInt().coerceAtMost(waveformData.size)
            var sum = 0f
            for (s in startSample until endSample) sum += kotlin.math.abs(waveformData[s])
            ((sum / (endSample - startSample).coerceAtLeast(1)) * WAVEFORM_GAIN).coerceIn(0f, 1f)
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

    private fun DrawScope.drawMirror(x: Int, y: Int, scale: Int, waveformData: FloatArray, t: AppTheme) {
        val centerY = y + height / 2
        val maxAmplitude = (height / 2) - 4

        drawLine(
            color = Color(t.vizCenterLine),
            start = Offset((x * scale).toFloat(), (centerY * scale).toFloat()),
            end = Offset(((x + width) * scale).toFloat(), (centerY * scale).toFloat()),
            strokeWidth = scale.toFloat()
        )

        val topPath = Path()
        val bottomPath = Path()
        for (i in 0 until width) {
            val sample = (waveformData[i % waveformData.size] * WAVEFORM_GAIN).coerceIn(-1f, 1f)
            val px = ((x + i) * scale).toFloat()
            val offset = sample * maxAmplitude
            val topPy    = ((centerY - offset) * scale).toFloat()
            val bottomPy = ((centerY + offset) * scale).toFloat()
            if (i == 0) {
                topPath.moveTo(px, topPy)
                bottomPath.moveTo(px, bottomPy)
            } else {
                topPath.lineTo(px, topPy)
                bottomPath.lineTo(px, bottomPy)
            }
        }
        val stroke = Stroke(width = scale.toFloat())
        drawPath(path = topPath,    color = Color(t.vizWave), style = stroke)
        drawPath(path = bottomPath, color = Color(t.vizWave), style = stroke)
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

        if (trackWaveforms == null || activeTrackMask == 0) {
            // Nothing scheduled — draw center line only
            drawLine(
                color = Color(t.vizCenterLine),
                start = Offset((x * scale).toFloat(), (centerY * scale).toFloat()),
                end = Offset(((x + width) * scale).toFloat(), (centerY * scale).toFloat()),
                strokeWidth = scale.toFloat()
            )
            return
        }

        val activeTracks = (0 until 8).filter { (activeTrackMask shr it) and 1 == 1 }
        val count = activeTracks.size
        // Each track scope gets an equal share of the width, minus gaps between them
        val totalGap = OCTA_TRACK_GAP * (count - 1)
        val trackW = (width - totalGap) / count
        val waveColor = Color(t.vizWave)
        val centerColor = Color(t.vizCenterLine)

        activeTracks.forEachIndexed { idx, trackId ->
            val scopeX = x + idx * (trackW + OCTA_TRACK_GAP)
            val waveData = trackWaveforms[trackId]

            // Center line for this scope
            drawLine(
                color = centerColor,
                start = Offset((scopeX * scale).toFloat(), (centerY * scale).toFloat()),
                end = Offset(((scopeX + trackW) * scale).toFloat(), (centerY * scale).toFloat()),
                strokeWidth = scale.toFloat()
            )

            // ProTracker-style: individual pixel dots, no interpolation between samples
            for (i in 0 until trackW) {
                val waveIdx = (i.toLong() * waveData.size / trackW).toInt().coerceIn(0, waveData.size - 1)
                val sample = (waveData[waveIdx] * WAVEFORM_GAIN).coerceIn(-1f, 1f)
                val dotY = centerY + (sample * maxAmplitude).toInt()
                drawRect(
                    color = waveColor,
                    topLeft = Offset(((scopeX + i) * scale).toFloat(), (dotY * scale).toFloat()),
                    size = Size(scale.toFloat(), scale.toFloat())
                )
            }
        }
    }
}

data class OscilloscopeState(
    val waveformBuffer: FloatArray,
    val appTheme: AppTheme = AppTheme.CLASSIC,
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
