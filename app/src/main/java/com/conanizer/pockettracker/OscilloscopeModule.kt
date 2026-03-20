package com.conanizer.pockettracker

import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.geometry.Size
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.Path
import androidx.compose.ui.graphics.drawscope.DrawScope
import androidx.compose.ui.graphics.drawscope.Stroke

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

    companion object {
        // OSCILLOSCOPE GAIN: Adjust this to make waveform taller/shorter
        // 1.0 = normal, 2.0 = double height, 4.0 = quad height, etc.
        // Higher values make quiet audio more visible but may clip loud audio
        const val WAVEFORM_GAIN = 3.0f
    }

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

        // Draw waveform as a single path — one draw call instead of 619.
        // 619 individual drawLine calls caused RenderThread stack overflow on Android 11
        // (Adreno GPU driver consumes more native stack per draw command than Android 13).
        val maxAmplitude = (height / 2) - 8  // 8px margin top/bottom

        val path = Path()
        for (i in 0 until width) {
            val sample = (waveformData[i % waveformData.size] * WAVEFORM_GAIN).coerceIn(-1f, 1f)
            val px = ((x + i) * scale).toFloat()
            val py = ((centerY + sample * maxAmplitude) * scale).toFloat()
            if (i == 0) path.moveTo(px, py) else path.lineTo(px, py)
        }
        drawPath(
            path = path,
            color = Color(0xFF00ff00),  // Bright green
            style = Stroke(width = scale.toFloat())
        )
    }
}