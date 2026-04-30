package com.conanizer.pockettracker

import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.geometry.Size
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.drawscope.DrawScope
import com.conanizer.pockettracker.core.data.Project

/**
 * MIXER SCREEN MODULE
 *
 * Displays 8 track columns + master column with volume faders and peak meters.
 *
 * Layout:
 * - 8 track columns (01-08) with vertical peak meters
 * - 1 master column (MSTR) with stereo peak meter
 * - Volume values (00-FF) shown below each meter
 * - Cursor moves between columns (0-8)
 *
 * Size: 620×392 pixels (same as other editor screens)
 */
class MixerModule : TrackerModule {
    // ===================================
    // MODULE DIMENSIONS
    // ===================================
    override val width = 620
    override val height = 392

    // ===================================
    // FONT & LAYOUT CONSTANTS
    // ===================================
    private val FONT_SCALE = 3      // 5×5 bitmap scaled 3× = 15×15 pixels
    private val CHAR_SPACING = 2    // 2px between characters
    private val ROW_HEIGHT = 21     // Each row is 21px tall
    private val TEXT_PADDING = 3    // 3px padding above text

    // Meter layout
    private val METER_WIDTH = 30           // Width of each track meter
    private val METER_HEIGHT = 200         // Height of meters
    private val MASTER_METER_WIDTH = 48    // Master meter is wider (stereo)
    private val METER_SPACING = 53         // Space between meter starts (reduced by 2px from original 55)
    private val FIRST_METER_X = 10         // X position of first meter

    // Colors
    private val COLOR_METER_BG = Color(0xFF1a1a1a)      // Dark background
    private val COLOR_METER_GREEN = Color(0xFF00cc00)   // Normal level
    private val COLOR_METER_YELLOW = Color(0xFFcccc00)  // Hot level (75%+)
    private val COLOR_METER_RED = Color(0xFFcc0000)     // Clipping (95%+)
    private val COLOR_METER_BORDER = Color(0xFF444444)  // Border color

    /**
     * Main draw function
     */
    override fun DrawScope.draw(x: Int, y: Int, scale: Int, state: Any?) {
        val mixerState = state as? MixerState ?: return

        // ===================================
        // STEP 1: Draw background
        // ===================================
        drawRect(
            color = Color(0xFF0a0a0a),
            topLeft = Offset((x * scale).toFloat(), (y * scale).toFloat()),
            size = Size((width * scale).toFloat(), (height * scale).toFloat())
        )

        // ===================================
        // STEP 2: Draw header "MIXER"
        // ===================================
        val rowY = y + TEXT_PADDING

        drawBitmapText(
            text = "MIXER",
            x = x + 10,
            y = rowY,
            scale = scale,
            color = Color.Cyan,
            spacing = CHAR_SPACING,
            fontScale = FONT_SCALE
        )


        // ===================================
        // STEP 3: Draw track columns (01-08)
        // ===================================
        val meterY = y + 50  // Start meters below header

        // Track columns are only active on the volume row; other rows edit master-only params
        val onVolumeRow = mixerState.mixerMasterRow == 0

        for (trackIndex in 0 until 8) {
            val meterX = x + FIRST_METER_X + (trackIndex * METER_SPACING)
            val isSelected = onVolumeRow && mixerState.cursorColumn == trackIndex
            val track = mixerState.project.tracks[trackIndex]
            val peakLevel = if (trackIndex < mixerState.trackPeaks.size) mixerState.trackPeaks[trackIndex] else 0f

            // Draw track label (0-7, single digit, centered on meter)
            val labelText = (trackIndex + 1).toString()
            drawBitmapText(
                text = labelText,
                x = meterX + (METER_WIDTH - 5 * FONT_SCALE) / 2,
                y = meterY - 25,
                scale = scale,
                color = if (isSelected) Color.Yellow else Color.White,
                spacing = CHAR_SPACING,
                fontScale = FONT_SCALE
            )

            // Draw meter
            drawMeter(
                x = meterX,
                y = meterY,
                meterWidth = METER_WIDTH,
                meterHeight = METER_HEIGHT,
                level = peakLevel,
                scale = scale,
                isSelected = isSelected,
                isMuted = track.mute
            )

            // Draw volume value below meter
            val volHex = track.volume.toString(16).padStart(2, '0').uppercase()
            drawBitmapText(
                text = volHex,
                x = meterX + 2,
                y = meterY + METER_HEIGHT + 10,
                scale = scale,
                color = if (isSelected) Color.Yellow else Color.White,
                spacing = CHAR_SPACING,
                fontScale = FONT_SCALE
            )

            // Draw mute indicator if muted
            if (track.mute) {
                drawBitmapText(
                    text = "M",
                    x = meterX + 6,
                    y = meterY + METER_HEIGHT + 30,
                    scale = scale,
                    color = Color.Red,
                    spacing = CHAR_SPACING,
                    fontScale = FONT_SCALE
                )
            }
        }

        // ===================================
        // STEP 4: Draw master column
        // ===================================
        val masterX = x + FIRST_METER_X + (8 * METER_SPACING)
        val isSelected = onVolumeRow && mixerState.cursorColumn == 8

        // Draw master label centered over the stereo meter pair.
        // Right bar ends at masterX + (METER_WIDTH/2+8) + (METER_WIDTH/2+4) = masterX + METER_WIDTH + 12
        // Pair center from masterX = (METER_WIDTH + 12) / 2 = 21
        // "MSTR" label width = 4 chars * 15px + 3 gaps * 2px = 66px → start at masterX + 21 - 33 = masterX - 12
        val mstrPairCenter = (METER_WIDTH + 12) / 2  // = 21
        val mstrLabelWidth = 4 * 5 * FONT_SCALE + 3 * CHAR_SPACING  // = 66px
        drawBitmapText(
            text = "MSTR",
            x = masterX + mstrPairCenter - mstrLabelWidth / 2,
            y = meterY - 25,
            scale = scale,
            color = if (isSelected) Color.Yellow else Color.White,
            spacing = CHAR_SPACING,
            fontScale = FONT_SCALE
        )

        // Draw stereo master meter (two bars side by side)
        val masterPeakL = if (mixerState.masterPeaks.size >= 1) mixerState.masterPeaks[0] else 0f
        val masterPeakR = if (mixerState.masterPeaks.size >= 2) mixerState.masterPeaks[1] else 0f

        // Left channel
        drawMeter(
            x = masterX,
            y = meterY,
            meterWidth = METER_WIDTH / 2 + 4,
            meterHeight = METER_HEIGHT,
            level = masterPeakL,
            scale = scale,
            isSelected = isSelected,
            isMuted = false
        )

        // Right channel
        drawMeter(
            x = masterX + METER_WIDTH / 2 + 8,
            y = meterY,
            meterWidth = METER_WIDTH / 2 + 4,
            meterHeight = METER_HEIGHT,
            level = masterPeakR,
            scale = scale,
            isSelected = isSelected,
            isMuted = false
        )

        // Draw master volume value (highlighted when sub-row 0 is selected)
        val masterVolSelected = isSelected && mixerState.mixerMasterRow == 0
        val masterVolHex = mixerState.project.masterVolume.toString(16).padStart(2, '0').uppercase()
        drawBitmapText(
            text = masterVolHex,
            x = masterX + 5,
            y = meterY + METER_HEIGHT + 10,
            scale = scale,
            color = if (masterVolSelected) Color.Yellow else Color.White,
            spacing = CHAR_SPACING,
            fontScale = FONT_SCALE
        )

        // Draw MASTER FX selector row (mixerMasterRow == 1)
        val masterFxSelected = mixerState.mixerMasterRow == 1
        val fxLabelWidth = (3 + 1) * (5 * FONT_SCALE + CHAR_SPACING) // "BUS " = 4 chars × 17px
        val fxValueText = if (mixerState.project.masterBusFx == 0) "OTT" else "DUST"
        drawBitmapText(
            text = "BUS",
            x = masterX - 68,
            y = meterY + METER_HEIGHT + 31,
            scale = scale,
            color = Color.Gray,
            spacing = CHAR_SPACING,
            fontScale = FONT_SCALE
        )
        drawBitmapText(
            text = fxValueText,
            x = masterX - 63 + fxLabelWidth,
            y = meterY + METER_HEIGHT + 31,
            scale = scale,
            color = if (masterFxSelected) Color.Yellow else Color.White,
            spacing = CHAR_SPACING,
            fontScale = FONT_SCALE
        )

        // Draw effect depth row (mixerMasterRow == 2) — label matches active bus effect
        val depthSelected = mixerState.mixerMasterRow == 2
        val depthLabelText = if (mixerState.project.masterBusFx == 0) "OTT" else "DUST"
        val depthHex = (if (mixerState.project.masterBusFx == 0) mixerState.project.ottDepth
                        else mixerState.project.dustDepth).toString(16).padStart(2, '0').uppercase()
        val depthLabelWidth = (3 + 1) * (5 * FONT_SCALE + CHAR_SPACING)
        drawBitmapText(
            text = depthLabelText,
            x = masterX - 68,
            y = meterY + METER_HEIGHT + 52,
            scale = scale,
            color = Color.Gray,
            spacing = CHAR_SPACING,
            fontScale = FONT_SCALE
        )
        drawBitmapText(
            text = depthHex,
            x = masterX - 63 + depthLabelWidth,
            y = meterY + METER_HEIGHT + 52,
            scale = scale,
            color = if (depthSelected) Color.Yellow else Color.White,
            spacing = CHAR_SPACING,
            fontScale = FONT_SCALE
        )
    }

    /**
     * Draw a single peak meter with true dBFS scaling
     *
     * Meter scale (true dBFS):
     * - Top of meter = +6dB (amplitude 2.0) - severe clipping
     * - 0dB (amplitude 1.0) = ~86% up the meter - clipping threshold
     * - -6dB (amplitude 0.5) = ~71% up
     * - -12dB (amplitude 0.25) = ~57% up
     * - -24dB (amplitude 0.063) = ~29% up
     * - Bottom = -42dB
     *
     * Color zones (true dBFS):
     * - Red:    >= 0dB (amplitude >= 1.0) - CLIPPING!
     * - Yellow: >= -6dB (amplitude >= 0.5) - hot but safe
     * - Green:  < -6dB - normal operating range
     */
    private fun DrawScope.drawMeter(
        x: Int,
        y: Int,
        meterWidth: Int,
        meterHeight: Int,
        level: Float,
        scale: Int,
        isSelected: Boolean,
        isMuted: Boolean
    ) {
        val scaledX = (x * scale).toFloat()
        val scaledY = (y * scale).toFloat()
        val scaledWidth = (meterWidth * scale).toFloat()
        val scaledHeight = (meterHeight * scale).toFloat()

        // Draw border (yellow if selected)
        drawRect(
            color = if (isSelected) Color.Yellow else COLOR_METER_BORDER,
            topLeft = Offset(scaledX - scale, scaledY - scale),
            size = Size(scaledWidth + 2 * scale, scaledHeight + 2 * scale)
        )

        // Draw background
        drawRect(
            color = COLOR_METER_BG,
            topLeft = Offset(scaledX, scaledY),
            size = Size(scaledWidth, scaledHeight)
        )

        // Don't draw level if muted
        if (isMuted) {
            return
        }

        // Convert amplitude to dB (true dBFS)
        // dB range: -42dB (bottom) to +6dB (top) = 48dB range
        // 0dB (amplitude 1.0) is at position 42/48 = 0.875 (87.5% up)
        val minDb = -42f
        val maxDb = 6f
        val dbRange = maxDb - minDb  // 48dB

        // Avoid log of zero - use a floor level
        val safeLevel = level.coerceAtLeast(0.00001f)
        val db = 20f * kotlin.math.log10(safeLevel)
        val clampedDb = db.coerceIn(minDb, maxDb)

        // Map dB to 0-1 meter position
        val meterPosition = (clampedDb - minDb) / dbRange

        // Calculate level bar height (from bottom up)
        val levelHeight = scaledHeight * meterPosition
        val levelY = scaledY + scaledHeight - levelHeight

        // Color based on true dBFS thresholds
        // >= 0dB (amplitude >= 1.0) = RED (clipping!)
        // >= -6dB (amplitude >= 0.5) = YELLOW (hot)
        // < -6dB = GREEN (safe)
        val levelColor = when {
            db >= 0f -> COLOR_METER_RED       // >= 0dB: CLIPPING!
            db >= -6f -> COLOR_METER_YELLOW   // >= -6dB: Hot
            else -> COLOR_METER_GREEN         // < -6dB: Normal
        }

        // Draw level bar
        if (levelHeight > 0) {
            drawRect(
                color = levelColor,
                topLeft = Offset(scaledX, levelY),
                size = Size(scaledWidth, levelHeight)
            )
        }

        // Draw gradient segments for visual appeal (draw lines at segment boundaries)
        val segmentCount = 16
        val segmentHeight = scaledHeight / segmentCount
        for (i in 1 until segmentCount) {
            val segY = scaledY + i * segmentHeight
            drawRect(
                color = COLOR_METER_BG,
                topLeft = Offset(scaledX, segY),
                size = Size(scaledWidth, scale.toFloat())
            )
        }
    }

    /**
     * Get cursor context for mixer screen
     */
    fun getCursorContext(state: MixerState): CursorContext {
        return when (state.mixerMasterRow) {
            1 -> CursorContext(   // MASTER FX: cycle OTT(0) / DUST(1)
                valueType = CursorValueType.HEX_BYTE,
                capabilities = CursorCapabilities(
                    canIncrement = true, canDecrement = true,
                    canIncrementFast = false, canDecrementFast = false
                ),
                currentValue = state.project.masterBusFx,
                minValue = 0, maxValue = 1, smallStep = 1, largeStep = 1, emptyValue = -1
            )
            2 -> {  // Effect depth (OTT or DUST depending on selection)
                val currentDepth = if (state.project.masterBusFx == 0) state.project.ottDepth
                                   else state.project.dustDepth
                CursorContextFactory.hexByte(currentValue = currentDepth, min = 0, max = 255)
            }
            else -> if (state.cursorColumn < 8)
                CursorContextFactory.hexByte(currentValue = state.project.tracks[state.cursorColumn].volume, min = 0, max = 255)
            else
                CursorContextFactory.hexByte(currentValue = state.project.masterVolume, min = 0, max = 255)
        }
    }

    /**
     * Handle input action for mixer screen
     */
    fun handleInput(
        state: MixerState,
        action: com.conanizer.pockettracker.core.logic.InputAction,
        onProjectModified: () -> Unit
    ): InputResult {
        when (action) {
            is com.conanizer.pockettracker.core.logic.InputAction.SET_VALUE -> {
                when (state.mixerMasterRow) {
                    1 -> {  // MASTER FX selector
                        state.project.masterBusFx = action.value.coerceIn(0, 1)
                        onProjectModified()
                        return InputResult(modified = true, masterFxChanged = true)
                    }
                    2 -> {  // Effect depth (routes to ottDepth or dustDepth)
                        val isOtt = state.project.masterBusFx == 0
                        if (isOtt) state.project.ottDepth = action.value.coerceIn(0, 255)
                        else       state.project.dustDepth = action.value.coerceIn(0, 255)
                        onProjectModified()
                        return InputResult(modified = true, ottDepthChanged = isOtt, dustDepthChanged = !isOtt)
                    }
                    else -> {  // Volume row
                        when {
                            state.cursorColumn < 8 -> state.project.tracks[state.cursorColumn].volume = action.value.coerceIn(0, 255)
                            else -> state.project.masterVolume = action.value.coerceIn(0, 255)
                        }
                        onProjectModified()
                        return InputResult(modified = true)
                    }
                }
            }
            // Mute toggle handled separately via LGPT-style controls (R+B)
            else -> {}
        }
        return InputResult(modified = false)
    }

    data class InputResult(
        val modified: Boolean,
        val ottDepthChanged: Boolean = false,
        val dustDepthChanged: Boolean = false,
        val masterFxChanged: Boolean = false
    )
}

/**
 * STATE DATA FOR MIXER SCREEN
 *
 * @param project The current project (contains track volumes and master volume)
 * @param cursorColumn Which column (0-7 = tracks, 8 = master)
 * @param trackPeaks Array of 8 floats for track peak levels (0.0-1.0)
 * @param masterPeaks Array of 2 floats for master L/R peak levels (0.0-1.0)
 */
data class MixerState(
    val project: Project,
    val cursorColumn: Int = 0,
    val mixerMasterRow: Int = 0,
    val trackPeaks: FloatArray = FloatArray(8),
    val masterPeaks: FloatArray = FloatArray(2)
) {
    override fun equals(other: Any?): Boolean {
        if (this === other) return true
        if (javaClass != other?.javaClass) return false
        other as MixerState
        if (project != other.project) return false
        if (cursorColumn != other.cursorColumn) return false
        if (mixerMasterRow != other.mixerMasterRow) return false
        if (!trackPeaks.contentEquals(other.trackPeaks)) return false
        if (!masterPeaks.contentEquals(other.masterPeaks)) return false
        return true
    }

    override fun hashCode(): Int {
        var result = project.hashCode()
        result = 31 * result + cursorColumn
        result = 31 * result + mixerMasterRow
        result = 31 * result + trackPeaks.contentHashCode()
        result = 31 * result + masterPeaks.contentHashCode()
        return result
    }
}
