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
    }

    // PEAKS mode state — persists between frames (module instance lives in TrackerLayout)
    private val peakValues = FloatArray(NUM_BARS)
    private val peakDecayCounters = IntArray(NUM_BARS)

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
            VisualizerType.SCOPE  -> drawScope(x, y, scale, waveformData, t)
            VisualizerType.BARS   -> drawBars(x, y, scale, waveformData, t, peakMode = false)
            VisualizerType.PEAKS  -> drawBars(x, y, scale, waveformData, t, peakMode = true)
            VisualizerType.MIRROR -> drawMirror(x, y, scale, waveformData, t)
            VisualizerType.FLAT   -> drawFlat(x, y, scale, t)
            VisualizerType.OCTA   -> drawOcta(x, y, scale, oscState?.trackWaveforms, oscState?.activeTrackMask ?: 0, t)
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

    private fun DrawScope.drawBars(
        x: Int, y: Int, scale: Int,
        waveformData: FloatArray,
        t: AppTheme,
        peakMode: Boolean
    ) {
        val centerY = y + height / 2f
        val maxAmp = (height / 2 - 2).toFloat()
        val samplesPerBar = waveformData.size.toFloat() / NUM_BARS

        for (i in 0 until NUM_BARS) {
            val barX = x + BAR_START_OFFSET + i * (BAR_W + BAR_GAP)

            // Average absolute amplitude over this bar's sample chunk
            val startSample = (i * samplesPerBar).toInt()
            val endSample = ((i + 1) * samplesPerBar).toInt().coerceAtMost(waveformData.size)
            var sum = 0f
            for (s in startSample until endSample) sum += kotlin.math.abs(waveformData[s])
            val barAmp = ((sum / (endSample - startSample).coerceAtLeast(1)) * WAVEFORM_GAIN).coerceIn(0f, 1f)

            val barHalfH = (barAmp * maxAmp).toInt().coerceAtLeast(1)

            drawRect(
                color = Color(t.vizWave),
                topLeft = Offset((barX * scale).toFloat(), ((centerY - barHalfH) * scale).toFloat()),
                size = Size((BAR_W * scale).toFloat(), (barHalfH * 2 * scale).toFloat())
            )

            if (peakMode) {
                if (barAmp > peakValues[i]) {
                    peakValues[i] = barAmp
                    peakDecayCounters[i] = 0
                } else {
                    peakDecayCounters[i]++
                    if (peakDecayCounters[i] > PEAK_HOLD_FRAMES) {
                        peakValues[i] = (peakValues[i] - 1f / maxAmp).coerceAtLeast(0f)
                    }
                }

                val peakHalfH = (peakValues[i] * maxAmp).toInt()
                if (peakHalfH > barHalfH) {
                    val peakColor = Color(t.vizWave.darken(0.55f))
                    // Top peak line
                    drawRect(
                        color = peakColor,
                        topLeft = Offset((barX * scale).toFloat(), ((centerY - peakHalfH - 1) * scale).toFloat()),
                        size = Size((BAR_W * scale).toFloat(), scale.toFloat())
                    )
                    // Bottom peak line (mirrored)
                    drawRect(
                        color = peakColor,
                        topLeft = Offset((barX * scale).toFloat(), ((centerY + peakHalfH) * scale).toFloat()),
                        size = Size((BAR_W * scale).toFloat(), scale.toFloat())
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
        val maxAmplitude = (height / 2) - 8

        drawLine(
            color = Color(t.vizCenterLine),
            start = Offset((x * scale).toFloat(), (centerY * scale).toFloat()),
            end = Offset(((x + width) * scale).toFloat(), (centerY * scale).toFloat()),
            strokeWidth = scale.toFloat()
        )

        if (trackWaveforms == null) return

        val activeTracks = (0 until 8).filter { (activeTrackMask shr it) and 1 == 1 }
        val count = activeTracks.size.coerceAtLeast(1)
        val trackWidth = width / count

        activeTracks.forEachIndexed { idx, trackId ->
            val startX = x + idx * trackWidth
            val waveData = trackWaveforms[trackId]
            val path = Path()
            for (i in 0 until trackWidth) {
                val waveIdx = (i.toLong() * waveData.size / trackWidth).toInt().coerceIn(0, waveData.size - 1)
                val sample = (waveData[waveIdx] * WAVEFORM_GAIN).coerceIn(-1f, 1f)
                val px = ((startX + i) * scale).toFloat()
                val py = ((centerY + sample * maxAmplitude) * scale).toFloat()
                if (i == 0) path.moveTo(px, py) else path.lineTo(px, py)
            }
            drawPath(path = path, color = Color(t.vizWave), style = Stroke(width = scale.toFloat()))

            // Divider line between tracks (skip after last track)
            if (idx < activeTracks.size - 1) {
                val divX = startX + trackWidth
                drawLine(
                    color = Color(t.vizCenterLine),
                    start = Offset((divX * scale).toFloat(), (y * scale).toFloat()),
                    end = Offset((divX * scale).toFloat(), ((y + height) * scale).toFloat()),
                    strokeWidth = scale.toFloat()
                )
            }
        }
    }
}

data class OscilloscopeState(
    val waveformBuffer: FloatArray,
    val appTheme: AppTheme = AppTheme.CLASSIC,
    val trackWaveforms: Array<FloatArray>? = null,
    val activeTrackMask: Int = 0
) {
    override fun equals(other: Any?): Boolean {
        if (this === other) return true
        if (other !is OscilloscopeState) return false
        return waveformBuffer.contentEquals(other.waveformBuffer) &&
               appTheme == other.appTheme &&
               activeTrackMask == other.activeTrackMask &&
               (trackWaveforms === other.trackWaveforms ||
                (trackWaveforms != null && other.trackWaveforms != null &&
                 trackWaveforms.indices.all { trackWaveforms[it].contentEquals(other.trackWaveforms[it]) }))
    }
    override fun hashCode(): Int = 31 * waveformBuffer.contentHashCode() + appTheme.hashCode()
}
