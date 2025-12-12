package com.example.pockettracker

import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.geometry.Size
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.drawscope.DrawScope

/**
 * OSCILLOSCOPE MODULE
 *
 * Displays real-time audio waveform
 *
 * Default size: 620×70 pixels
 * State type: FloatArray (waveform samples, -1.0 to 1.0)
 */
class OscilloscopeModule(
    override val width: Int = 620,
    override val height: Int = 70
) : TrackerModule {

    override fun DrawScope.draw(x: Int, y: Int, scale: Int, state: Any?) {
        // Cast state to waveform data
        val waveformData = state as? FloatArray ?: FloatArray(width)

        // Module background
        drawRect(
            color = Color(0xFF0a0a0a),
            topLeft = Offset((x * scale).toFloat(), (y * scale).toFloat()),
            size = Size((width * scale).toFloat(), (height * scale).toFloat())
        )

        // Center reference line
        val centerY = y + height / 2
        drawLine(
            color = Color(0xFF333333),
            start = Offset((x * scale).toFloat(), (centerY * scale).toFloat()),
            end = Offset(((x + width) * scale).toFloat(), (centerY * scale).toFloat()),
            strokeWidth = scale.toFloat()
        )

        // Draw waveform as connected line segments
        val maxAmplitude = (height / 2) - 8  // 8px margin top/bottom

        for (i in 0 until width - 1) {
            // Get two consecutive samples
            val sample1 = waveformData[i % waveformData.size].coerceIn(-1f, 1f)
            val sample2 = waveformData[(i + 1) % waveformData.size].coerceIn(-1f, 1f)

            // Convert to screen coordinates
            val y1 = centerY + (sample1 * maxAmplitude).toInt()
            val y2 = centerY + (sample2 * maxAmplitude).toInt()

            // Draw line between points (classic oscilloscope look)
            drawLine(
                color = Color(0xFF00ff00),  // Bright green
                start = Offset(((x + i) * scale).toFloat(), (y1 * scale).toFloat()),
                end = Offset(((x + i + 1) * scale).toFloat(), (y2 * scale).toFloat()),
                strokeWidth = scale.toFloat()
            )
        }
    }
}