package com.conanizer.pockettracker.ui.modules

import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.geometry.Size
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.drawscope.DrawScope
import com.conanizer.pockettracker.ui.theme.AppTheme
import com.conanizer.pockettracker.input.CursorContext
import com.conanizer.pockettracker.input.CursorContextFactory
import com.conanizer.pockettracker.ui.TrackerModule
import com.conanizer.pockettracker.core.data.Project
import com.conanizer.pockettracker.core.logic.InputAction
import com.conanizer.pockettracker.ui.drawBitmapText
import com.conanizer.pockettracker.ui.drawEqCell
import com.conanizer.pockettracker.ui.toHex2
import kotlin.math.log10

/**
 * MIXER SCREEN MODULE (620×392)
 *
 * Layout:
 *
 * TRACK SECTION (upper ~half):
 *   8 stereo track columns + master stereo column (same width as tracks)
 *   Track meters: TRACK_METER_H=155px  Master meter: MASTER_METER_H=200px (original)
 *   Track volumes below track meters (row 0)
 *   Master values below master meter at original y position
 *
 * SEND SECTION (lower ~half):
 *   REV meter (left) + DEL meter (right), side by side.
 *   Each: "REV"/"DEL" header ABOVE the meter, wet value BELOW it.
 *
 * Cursor model (mixerMasterRow, cursorColumn):
 *   row 0: cols 0-7 = track volumes, col 8 = master MIX volume
 *   row 1: col 0 = REV wet, col 1 = DEL wet, col 8 = master EQ slot
 *   row 2: col 8 = master OTT/DUST depth
 *   row 3: col 8 = master LIM pre-gain
 * Navigation: DOWN from a track → REV; REV →(R)→ DEL →(R)→ EQ;
 *   EQ/OTT/DST/LIM →(L)→ DEL; UP from REV/DEL → first track.
 */
class MixerModule : TrackerModule {
    override val width  = 620
    override val height = 392

    private val FONT_SCALE   = 3
    private val CHAR_SPACING = 2
    private val ROW_HEIGHT   = 21
    private val TEXT_PADDING = 3

    private val METER_SPACING = 53
    private val FIRST_METER_X = 10

    // Track section
    private val TRACK_METER_TOP = 24
    private val TRACK_METER_H   = 155
    private val TRACK_VOL_Y     = 182    // TRACK_METER_TOP + TRACK_METER_H + 3

    // Master meter — keeps original height; starts at same top as tracks
    private val MASTER_METER_H  = 200

    // Send section — REV/DEL headers ABOVE the meters, wet values BELOW them.
    private val SEND_METER_TOP  = 234    // = MROW0_Y: meters start at the REV+MIX value row
    private val SEND_METER_H    = 112    // 234 + 112 + 4 = 350 → value row sits just below
    private val SEND_HEADER_Y   = 216    // SEND_METER_TOP - 18: header labels above the meters
    private val SEND_VALUE_Y    = 350    // just below the send meters: REV/DEL wet values

    // All stereo bars (tracks AND master) use the same slim dimensions
    private val BAR_W   = 20
    private val BAR_SEP = 1

    // Master column start X
    private val MASTER_X = FIRST_METER_X + 8 * METER_SPACING  // = 434

    // Master value rows — kept at original position below master meter (y ≈ 260)
    private val MROW0_Y = TRACK_METER_TOP + MASTER_METER_H + 10  // = 234... actually 260
    private val MROW1_Y = MROW0_Y + ROW_HEIGHT
    private val MROW2_Y = MROW1_Y + ROW_HEIGHT
    private val MROW3_Y = MROW2_Y + ROW_HEIGHT

    // Master value text positions
    private val MSTR_LABEL_X = MASTER_X - 65
    private val MSTR_VALUE_X = MASTER_X + 5

    // LED-style segment dimensions — larger than the visualizer's 2px so they aren't tiny
    private val SEG_H    = 4
    private val SEG_GAP  = 1
    private val SEG_STEP = SEG_H + SEG_GAP  // 5px per LED cell

    private val PEAK_HOLD_FRAMES = 45

    // Color zone thresholds as fraction of meter height from bottom.
    // Meter range: -42 dB to +6 dB = 48 dB total.
    //   -12 dB → 30/48 of range from bottom (LOW / MID boundary)
    //     0 dB → 42/48 of range from bottom (MID / HIGH boundary)
    private val LOW_TO_MID_FRAC  = 30f / 48f
    private val MID_TO_HIGH_FRAC = 42f / 48f

    // Peak hold state per channel (heights in unscaled pixels).
    // Indices: 0..15 = track L/R (i*2, i*2+1), 16..17 = master L/R,
    //          18..19 = reverb L/R, 20..21 = delay L/R.
    private val peakHoldPx   = FloatArray(22)
    private val peakCounters = IntArray(22)

    override fun DrawScope.draw(x: Int, y: Int, scale: Int, state: Any?) {
        val s = state as? MixerState ?: return
        val t = s.appTheme

        // Background
        drawRect(
            color   = Color(t.background),
            topLeft = Offset((x * scale).toFloat(), (y * scale).toFloat()),
            size    = Size((width * scale).toFloat(), (height * scale).toFloat())
        )

        drawBitmapText("MIXER", x + 10, y + TEXT_PADDING, scale,
            Color(t.textTitle), CHAR_SPACING, FONT_SCALE)

        // ── Track stereo meters + volumes (active only at row 0) ───────────
        for (i in 0..7) {
            val mX    = x + FIRST_METER_X + i * METER_SPACING
            val isSel = s.mixerMasterRow == 0 && s.cursorColumn == i
            val peakL = s.trackPeaks.getOrElse(i * 2) { 0f }
            val peakR = s.trackPeaks.getOrElse(i * 2 + 1) { 0f }

            drawStereoMeter(mX, y + TRACK_METER_TOP, TRACK_METER_H,
                peakL, peakR, scale, isSel, s.project.tracks[i].mute, t,
                i * 2, i * 2 + 1)

            drawBitmapText(s.project.tracks[i].volume.toHex2(),
                mX + 5, y + TRACK_VOL_Y, scale,
                if (isSel) Color(t.textCursor) else Color(t.textValue), CHAR_SPACING, FONT_SCALE)
        }

        // ── Master stereo meter (same width as tracks, original height) ────
        val masterSel  = s.cursorColumn == 8
        drawStereoMeter(
            x + MASTER_X, y + TRACK_METER_TOP, MASTER_METER_H,
            s.masterPeaks.getOrElse(0) { 0f },
            s.masterPeaks.getOrElse(1) { 0f },
            scale, masterSel, false, t, 16, 17)

        // ── Send return meters: REV left, DEL right (both on send row 1) ───
        val revSendSel = s.mixerMasterRow == 1 && s.cursorColumn == 0
        val delSendSel = s.mixerMasterRow == 1 && s.cursorColumn == 1
        drawStereoMeter(
            x + FIRST_METER_X, y + SEND_METER_TOP, SEND_METER_H,
            s.reverbPeaks.getOrElse(0) { 0f }, s.reverbPeaks.getOrElse(1) { 0f },
            scale, revSendSel, false, t, 18, 19)
        drawStereoMeter(
            x + FIRST_METER_X + METER_SPACING, y + SEND_METER_TOP, SEND_METER_H,
            s.delayPeaks.getOrElse(0) { 0f }, s.delayPeaks.getOrElse(1) { 0f },
            scale, delSendSel, false, t, 20, 21)

        val charW = 5 * FONT_SCALE + CHAR_SPACING
        val revCX = x + FIRST_METER_X + (BAR_W + BAR_SEP + BAR_W) / 2
        val delCX = x + FIRST_METER_X + METER_SPACING + (BAR_W + BAR_SEP + BAR_W) / 2

        // Header labels ABOVE the meters
        drawBitmapText("REV", revCX - (3 * charW) / 2, y + SEND_HEADER_Y,
            scale, Color(t.textParam), CHAR_SPACING, FONT_SCALE)
        drawBitmapText("DEL", delCX - (3 * charW) / 2, y + SEND_HEADER_Y,
            scale, Color(t.textParam), CHAR_SPACING, FONT_SCALE)

        // Wet values BELOW the meters (highlighted when selected)
        drawBitmapText(s.project.reverbWet.toHex2(), revCX - (2 * charW) / 2, y + SEND_VALUE_Y,
            scale, if (revSendSel) Color(t.textCursor) else Color(t.textValue), CHAR_SPACING, FONT_SCALE)
        drawBitmapText(s.project.delayWet.toHex2(), delCX - (2 * charW) / 2, y + SEND_VALUE_Y,
            scale, if (delSendSel) Color(t.textCursor) else Color(t.textValue), CHAR_SPACING, FONT_SCALE)

        // ── Master value rows (at original position, aligned with send section) ──
        val eqSlot     = s.project.masterEqSlot
        val isOtt      = s.project.masterBusFx == 0
        val depthLabel = if (isOtt) "OTT" else "DST"
        val depthText  = (if (isOtt) s.project.ottDepth else s.project.dustDepth).toHex2()

        val mixSel   = masterSel && s.mixerMasterRow == 0
        val eqSel    = masterSel && s.mixerMasterRow == 1
        val depthSel = masterSel && s.mixerMasterRow == 2
        val limSel   = masterSel && s.mixerMasterRow == 3

        // Row 0: MIX vol editable (right)
        drawBitmapText("MIX", x + MSTR_LABEL_X, y + MROW0_Y,
            scale, Color(t.textParam), CHAR_SPACING, FONT_SCALE)
        drawBitmapText(s.project.masterVolume.toHex2(), x + MSTR_VALUE_X, y + MROW0_Y,
            scale, if (mixSel) Color(t.textCursor) else Color(t.textValue), CHAR_SPACING, FONT_SCALE)

        // Row 1: EQ slot editable (right) — value + ">" opens the EQ editor
        drawBitmapText("EQ", x + MSTR_LABEL_X, y + MROW1_Y,
            scale, Color(t.textParam), CHAR_SPACING, FONT_SCALE)
        drawEqCell(x + MSTR_VALUE_X, y + MROW1_Y, scale, eqSlot, eqSel, t)

        // Row 2: OTT/DUST depth editable (right only)
        drawBitmapText(depthLabel, x + MSTR_LABEL_X, y + MROW2_Y,
            scale, Color(t.textParam), CHAR_SPACING, FONT_SCALE)
        drawBitmapText(depthText, x + MSTR_VALUE_X, y + MROW2_Y,
            scale, if (depthSel) Color(t.textCursor) else Color(t.textValue), CHAR_SPACING, FONT_SCALE)

        // Row 3: LIM pre-gain editable (right only)
        drawBitmapText("LIM", x + MSTR_LABEL_X, y + MROW3_Y,
            scale, Color(t.textParam), CHAR_SPACING, FONT_SCALE)
        drawBitmapText(s.project.limiterPreGain.toHex2(), x + MSTR_VALUE_X, y + MROW3_Y,
            scale, if (limSel) Color(t.textCursor) else Color(t.textValue), CHAR_SPACING, FONT_SCALE)
    }

    /**
     * Draws a stereo pair (L bar | separator | R bar) with segmented LED-style bars,
     * fixed zone colors, and a floating peak-hold marker per channel.
     */
    private fun DrawScope.drawStereoMeter(
        x: Int, y: Int, h: Int,
        levelL: Float, levelR: Float,
        scale: Int,
        isSelected: Boolean,
        isMuted: Boolean,
        t: AppTheme,
        peakIdxL: Int,
        peakIdxR: Int
    ) {
        val borderColor = if (isSelected) Color(t.textCursor) else Color(t.meterBorder)
        val rX = x + BAR_W + BAR_SEP

        // Outer border (covers L + 1px separator strip + R)
        drawRect(borderColor,
            topLeft = Offset(((x - 1) * scale).toFloat(), ((y - 1) * scale).toFloat()),
            size    = Size(((BAR_W + BAR_SEP + BAR_W + 2) * scale).toFloat(), ((h + 2) * scale).toFloat()))

        // L background
        drawRect(Color(t.meterBackground),
            topLeft = Offset((x * scale).toFloat(), (y * scale).toFloat()),
            size    = Size((BAR_W * scale).toFloat(), (h * scale).toFloat()))
        // R background
        drawRect(Color(t.meterBackground),
            topLeft = Offset((rX * scale).toFloat(), (y * scale).toFloat()),
            size    = Size((BAR_W * scale).toFloat(), (h * scale).toFloat()))

        val lhPx = levelToHeightPx(levelL, h)
        val rhPx = levelToHeightPx(levelR, h)

        updatePeak(peakIdxL, lhPx, isMuted)
        updatePeak(peakIdxR, rhPx, isMuted)

        if (!isMuted) {
            drawSegmentedBar(x,  y, h, lhPx, scale, t)
            drawSegmentedBar(rX, y, h, rhPx, scale, t)
        }

        // Peak markers drawn after bars so they appear on top of the background
        drawPeakMarker(x,  y, h, peakIdxL, scale, t)
        drawPeakMarker(rX, y, h, peakIdxR, scale, t)
    }

    private fun DrawScope.drawSegmentedBar(
        x: Int, y: Int, h: Int,
        barHpx: Int,
        scale: Int,
        t: AppTheme
    ) {
        val barBottom = y + h
        var dy = 0
        while (dy + SEG_H <= barHpx) {
            val segTop = barBottom - dy - SEG_H
            drawRect(
                color   = segmentColor(dy, h, t),
                topLeft = Offset((x * scale).toFloat(), (segTop * scale).toFloat()),
                size    = Size((BAR_W * scale).toFloat(), (SEG_H * scale).toFloat())
            )
            dy += SEG_STEP
        }
    }

    private fun updatePeak(idx: Int, levelPx: Int, isMuted: Boolean) {
        val px = if (isMuted) 0 else levelPx
        if (px > peakHoldPx[idx]) {
            peakHoldPx[idx] = px.toFloat()
            peakCounters[idx] = 0
        } else {
            peakCounters[idx]++
            if (peakCounters[idx] > PEAK_HOLD_FRAMES) {
                peakHoldPx[idx] = (peakHoldPx[idx] - SEG_STEP).coerceAtLeast(0f)
            }
        }
    }

    private fun DrawScope.drawPeakMarker(
        x: Int, y: Int, h: Int,
        peakIdx: Int,
        scale: Int,
        t: AppTheme
    ) {
        val peakPx = peakHoldPx[peakIdx]
        if (peakPx <= 0f) return

        // Snap to nearest segment boundary so peak always aligns with LED grid
        val peakDy = (peakPx.toInt() / SEG_STEP) * SEG_STEP
        val peakTop = y + h - peakDy - SEG_H
        if (peakTop < y) return  // clamp: never draw above meter bounds

        drawRect(
            color   = segmentColor(peakDy, h, t),
            topLeft = Offset((x * scale).toFloat(), (peakTop * scale).toFloat()),
            size    = Size((BAR_W * scale).toFloat(), (SEG_H * scale).toFloat())
        )
    }

    // Color based on segment position in the meter, not on current signal level.
    // Zones are fixed: low (green) → mid (yellow, from -12 dB) → high (red, from 0 dB).
    private fun segmentColor(dyFromBottom: Int, totalH: Int, t: AppTheme): Color {
        val frac = dyFromBottom.toFloat() / totalH
        return when {
            frac >= MID_TO_HIGH_FRAC -> Color(t.meterHigh)
            frac >= LOW_TO_MID_FRAC  -> Color(t.meterMid)
            else                      -> Color(t.meterLow)
        }
    }

    private fun levelToHeightPx(level: Float, meterH: Int): Int {
        val db  = 20f * log10(level.coerceAtLeast(0.00001f))
        val pos = (db.coerceIn(-42f, 6f) + 42f) / 48f
        return (meterH * pos).toInt()
    }

    fun getCursorContext(state: MixerState): CursorContext {
        // Row 0: track volumes (cols 0-7) or master MIX (col 8)
        if (state.mixerMasterRow == 0) {
            return if (state.cursorColumn < 8)
                CursorContextFactory.hexByte(state.project.tracks[state.cursorColumn].volume, 0, 255)
            else
                CursorContextFactory.hexByte(state.project.masterVolume, 0, 255)
        }
        // Send row (row 1): REV (col 0) / DEL (col 1) — side by side under their meters
        if (state.mixerMasterRow == 1 && state.cursorColumn == 0)
            return CursorContextFactory.hexByte(state.project.reverbWet, 0, 255)
        if (state.mixerMasterRow == 1 && state.cursorColumn == 1)
            return CursorContextFactory.hexByte(state.project.delayWet, 0, 255)
        // Col 8 (master): EQ, OTT/DUST, or LIM
        if (state.cursorColumn == 8) return when (state.mixerMasterRow) {
            1 -> CursorContextFactory.hexByte(
                currentValue = if (state.project.masterEqSlot < 0) -1 else state.project.masterEqSlot,
                min = 0, max = 127, emptyValue = -1, canDelete = true, canInsert = true
            )
            2 -> {
                val depth = if (state.project.masterBusFx == 0) state.project.ottDepth else state.project.dustDepth
                CursorContextFactory.hexByte(depth, 0, 255)
            }
            3 -> CursorContextFactory.hexByte(state.project.limiterPreGain, 0, 255)
            else -> CursorContextFactory.none()
        }
        return CursorContextFactory.none()
    }

    fun handleInput(
        state: MixerState,
        action: InputAction,
        onProjectModified: () -> Unit
    ): InputResult {
        if (state.cursorColumn == 8 && state.mixerMasterRow == 1) {
            when (action) {
                is InputAction.SET_VALUE -> {
                    state.project.masterEqSlot = action.value.coerceIn(0, 127)
                    onProjectModified()
                    return InputResult(modified = true, masterEqChanged = true)
                }
                InputAction.DELETE -> {
                    state.project.masterEqSlot = -1
                    onProjectModified()
                    return InputResult(modified = true, masterEqChanged = true)
                }
                InputAction.INSERT_DEFAULT -> {
                    state.project.masterEqSlot = 0
                    onProjectModified()
                    return InputResult(modified = true, masterEqChanged = true)
                }
                else -> {}
            }
        }

        when (action) {
            is InputAction.SET_VALUE -> {
                when {
                    // Send return volumes (row 1): REV col 0, DEL col 1
                    state.mixerMasterRow == 1 && state.cursorColumn == 0 -> {
                        state.project.reverbWet = action.value.coerceIn(0, 255)
                        onProjectModified()
                        return InputResult(modified = true, reverbWetChanged = true)
                    }
                    state.mixerMasterRow == 1 && state.cursorColumn == 1 -> {
                        state.project.delayWet = action.value.coerceIn(0, 255)
                        onProjectModified()
                        return InputResult(modified = true, delayWetChanged = true)
                    }
                    // Row 0: track volumes
                    state.mixerMasterRow == 0 && state.cursorColumn < 8 -> {
                        state.project.tracks[state.cursorColumn].volume = action.value.coerceIn(0, 255)
                        onProjectModified()
                        return InputResult(modified = true)
                    }
                    // Row 0, col 8: master volume
                    state.mixerMasterRow == 0 -> {
                        state.project.masterVolume = action.value.coerceIn(0, 255)
                        onProjectModified()
                        return InputResult(modified = true)
                    }
                    // Master col (col 8): OTT/DUST
                    state.mixerMasterRow == 2 -> {
                        val isOtt = state.project.masterBusFx == 0
                        if (isOtt) state.project.ottDepth  = action.value.coerceIn(0, 255)
                        else       state.project.dustDepth = action.value.coerceIn(0, 255)
                        onProjectModified()
                        return InputResult(modified = true, ottDepthChanged = isOtt, dustDepthChanged = !isOtt)
                    }
                    // Master col (col 8): LIM pre-gain
                    state.mixerMasterRow == 3 -> {
                        state.project.limiterPreGain = action.value.coerceIn(0, 255)
                        onProjectModified()
                        return InputResult(modified = true, limiterPreGainChanged = true)
                    }
                }
            }
            else -> {}
        }
        return InputResult(modified = false)
    }

    data class InputResult(
        val modified: Boolean,
        val ottDepthChanged: Boolean       = false,
        val dustDepthChanged: Boolean      = false,
        val masterEqChanged: Boolean       = false,
        val reverbWetChanged: Boolean      = false,
        val delayWetChanged: Boolean       = false,
        val limiterPreGainChanged: Boolean = false
    )
}

data class MixerState(
    val project:       Project,
    val cursorColumn:  Int        = 0,
    val mixerMasterRow: Int       = 0,
    val trackPeaks:    FloatArray = FloatArray(16),
    val masterPeaks:   FloatArray = FloatArray(2),
    val reverbPeaks:   FloatArray = FloatArray(2),
    val delayPeaks:    FloatArray = FloatArray(2),
    val appTheme: AppTheme = AppTheme.Companion.CLASSIC
) {
    override fun equals(other: Any?): Boolean {
        if (this === other) return true
        if (javaClass != other?.javaClass) return false
        other as MixerState
        return project        == other.project
            && cursorColumn   == other.cursorColumn
            && mixerMasterRow == other.mixerMasterRow
            && trackPeaks.contentEquals(other.trackPeaks)
            && masterPeaks.contentEquals(other.masterPeaks)
            && reverbPeaks.contentEquals(other.reverbPeaks)
            && delayPeaks.contentEquals(other.delayPeaks)
            && appTheme       == other.appTheme
    }

    override fun hashCode(): Int {
        var result = project.hashCode()
        result = 31 * result + cursorColumn
        result = 31 * result + mixerMasterRow
        result = 31 * result + trackPeaks.contentHashCode()
        result = 31 * result + masterPeaks.contentHashCode()
        result = 31 * result + reverbPeaks.contentHashCode()
        result = 31 * result + delayPeaks.contentHashCode()
        result = 31 * result + appTheme.hashCode()
        return result
    }
}
