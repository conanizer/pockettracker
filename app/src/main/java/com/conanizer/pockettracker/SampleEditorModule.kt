package com.conanizer.pockettracker

import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.geometry.Size
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.drawscope.DrawScope
import com.conanizer.pockettracker.core.logic.InputAction

class SampleEditorModule : TrackerModule {
    override val width = 640
    override val height = 480

    // Waveform: 5px spacer above 3 header rows + 5px spacer below = Y=73, H=155
    private val WAVEFORM_Y = 73
    private val WAVEFORM_H = 155
    // Content rows start immediately after waveform (73+155 = 228)
    private fun contentY(row: Int) = (row - 8) * ROW_HEIGHT + 228  // rows 8-19

    // Row-background highlight for the full 640px width
    private fun DrawScope.rowBg(x: Int, y: Int, scale: Int) = drawRect(
        Color(0xFF333333),
        topLeft = Offset((x * scale).toFloat(), (y * scale).toFloat()),
        size    = Size((width * scale).toFloat(), (ROW_HEIGHT * scale).toFloat())
    )

    // ─── Main draw ───────────────────────────────────────────────────────────

    override fun DrawScope.draw(x: Int, y: Int, scale: Int, state: Any?) {
        val s = state as? SampleEditorState ?: return

        drawRect(Color(0xFF0a0a0a),
            topLeft = Offset((x * scale).toFloat(), (y * scale).toFloat()),
            size    = Size((width * scale).toFloat(), (height * scale).toFloat()))

        // 5px spacer above row 0; rows 0/1/2 at y+5/y+26/y+47; 5px spacer; waveform at y+73
        drawRow0Header(x, y + 5,              scale, s)
        drawRow1View  (x, y + 26,             scale, s)
        drawRow2Edit  (x, y + 47,             scale, s)
        drawWaveform  (x, y + WAVEFORM_Y,     scale, s)
        drawRow8Sel   (x, y + contentY(8),           scale, s)
        drawRow10Slice(x, y + contentY(10),          scale, s)
        drawRow11SliceDetail(x, y + contentY(11),    scale, s)
        drawOpsRow    (x, y + contentY(13), scale, OPS_ROW1, s.cursorRow == 13, s.cursorCol)
        drawOpsRow    (x, y + contentY(14), scale, OPS_ROW2, s.cursorRow == 14, s.cursorCol)
        drawRow16Fx   (x, y + contentY(16),          scale, s)
        drawRow18Name (x, y + contentY(18),          scale, s)
        drawRow19Save (x, y + contentY(19),          scale, s)

        if (s.showConfirmClose) drawConfirmDialog(x, y, scale)
    }

    // ─── Row draw helpers ─────────────────────────────────────────────────────

    private fun DrawScope.drawRow0Header(x: Int, y: Int, scale: Int, s: SampleEditorState) {
        val ty = y + TEXT_PADDING
        drawBitmapText("SAMPLE EDITOR", x + 10, ty, scale, Color.Cyan,     CHAR_SPACING, FONT_SCALE)
        drawBitmapText("${s.sampleRate}Hz",  x + 360, ty, scale, Color.Gray, CHAR_SPACING, FONT_SCALE)
        drawBitmapText(s.durationDisplay,    x + 492, ty, scale, Color.Gray, CHAR_SPACING, FONT_SCALE)
    }

    private fun DrawScope.drawRow1View(x: Int, y: Int, scale: Int, s: SampleEditorState) {
        val ty  = y + TEXT_PADDING
        val cur = s.cursorRow == 1
        if (cur) rowBg(x, y, scale)
        drawLabel3val(x, ty, scale, cur, s.cursorCol,
            "ZOOM",   "${1 shl s.zoomLevel}X",        0,
            "SOURCE", SOURCE_VALUES[s.sourceMode],     1,
            "RATE",   RATE_VALUES[s.rateMode],         2)
    }

    private fun DrawScope.drawRow2Edit(x: Int, y: Int, scale: Int, s: SampleEditorState) {
        val ty  = y + TEXT_PADDING
        val cur = s.cursorRow == 2
        if (cur) rowBg(x, y, scale)
        val pitchStr = if (s.pitchSemitones >= 0) "+${s.pitchSemitones}" else "${s.pitchSemitones}"
        drawLabel3val(x, ty, scale, cur, s.cursorCol,
            "PITCH",    pitchStr,                          0,
            "DURATION", DURATION_VALUES[s.durationIndex], 1,
            "SNAP",     if (s.snapEnabled) "ON" else "OFF", 2)
    }

    /** Helper: draw three label/value pairs across one row. */
    private fun DrawScope.drawLabel3val(
        x: Int, ty: Int, scale: Int, isCurRow: Boolean, curCol: Int,
        l1: String, v1: String, c1: Int,
        l2: String, v2: String, c2: Int,
        l3: String, v3: String, c3: Int
    ) {
        fun labelColor(col: Int) = if (isCurRow && curCol == col) Color.Yellow else Color.Gray
        fun valueColor(col: Int) = if (isCurRow && curCol == col) Color.Yellow else Color.White
        drawBitmapText(l1, x + 10,  ty, scale, labelColor(c1), CHAR_SPACING, FONT_SCALE)
        drawBitmapText(v1, x + 110,  ty, scale, valueColor(c1), CHAR_SPACING, FONT_SCALE)
        drawBitmapText(l2, x + 180, ty, scale, labelColor(c2), CHAR_SPACING, FONT_SCALE)
        drawBitmapText(v2, x + 335, ty, scale, valueColor(c2), CHAR_SPACING, FONT_SCALE)
        drawBitmapText(l3, x + 445, ty, scale, labelColor(c3), CHAR_SPACING, FONT_SCALE)
        drawBitmapText(v3, x + 535, ty, scale, valueColor(c3), CHAR_SPACING, FONT_SCALE)
    }

    private fun DrawScope.drawWaveform(x: Int, y: Int, scale: Int, s: SampleEditorState) {
        val bgCol = Color(0xFF111111)
        drawRect(bgCol,
            topLeft = Offset(((x + 10) * scale).toFloat(), (y * scale).toFloat()),
            size    = Size((620 * scale).toFloat(),          (WAVEFORM_H * scale).toFloat()))

        val midY = y + WAVEFORM_H / 2
        // Center line
        drawLine(Color(0xFF444444),
            start       = Offset(((x + 10) * scale).toFloat(), (midY * scale).toFloat()),
            end         = Offset(((x + 630) * scale).toFloat(), (midY * scale).toFloat()),
            strokeWidth = scale.toFloat())

        val viewLen  = (s.viewEnd - s.viewStart).coerceAtLeast(1L)
        val viewLenF = viewLen.toFloat()

        if (s.waveformData.size >= 2) {
            val bins  = s.waveformData.size / 2
            val wfW   = 620f
            val halfH = WAVEFORM_H / 2f
            for (i in 0 until bins) {
                val minVal = s.waveformData[i * 2]
                val maxVal = s.waveformData[i * 2 + 1]
                val cx  = x + 10 + (i * wfW / bins).toInt()
                val top = midY - (maxVal * halfH).toInt()
                val bot = midY - (minVal * halfH).toInt()
                // Bin frame position in full-sample coords
                val binFrame = s.viewStart + (i.toLong() * viewLen / bins)
                val inSel = binFrame in s.selectionStart until s.selectionEnd
                drawLine(if (inSel) Color.White else Color(0xFF666666),
                    start       = Offset((cx * scale).toFloat(), (top * scale).toFloat()),
                    end         = Offset((cx * scale).toFloat(), (bot * scale).toFloat()),
                    strokeWidth = scale.toFloat())
            }
        } else {
            drawBitmapText("WAVEFORM", x + 265, midY - 6, scale, Color(0xFF444444), CHAR_SPACING, FONT_SCALE)
        }

        // Selection S/E markers — clipped to visible window
        if (s.totalFrames > 0) {
            val mc = Color(0xFF00AAAA)
            val wfLeft  = x + 10
            val wfRight = x + 630
            val sXf = wfLeft + ((s.selectionStart - s.viewStart).toFloat() / viewLenF) * 620f
            val eXf = wfLeft + ((s.selectionEnd   - s.viewStart).toFloat() / viewLenF) * 620f
            val sX = sXf.toInt().coerceIn(wfLeft, wfRight)
            val eX = eXf.toInt().coerceIn(wfLeft, wfRight)
            if (sXf >= wfLeft && sXf <= wfRight) {
                drawLine(mc, Offset((sX * scale).toFloat(), (y * scale).toFloat()),
                             Offset((sX * scale).toFloat(), ((y + WAVEFORM_H) * scale).toFloat()), scale.toFloat())
                drawBitmapText("S", sX + 2, y + 3, scale, mc, CHAR_SPACING, FONT_SCALE)
            }
            if (eXf >= wfLeft && eXf <= wfRight) {
                drawLine(mc, Offset((eX * scale).toFloat(), (y * scale).toFloat()),
                             Offset((eX * scale).toFloat(), ((y + WAVEFORM_H) * scale).toFloat()), scale.toFloat())
                drawBitmapText("E", eX - 17, y + 3, scale, mc, CHAR_SPACING, FONT_SCALE)
            }
        }

        // Transient markers — drawn in TRANSIENT (0) and OFF (2) modes when markers are present.
        // In OFF mode, markers are read-only from the WAV cue chunk and shown for reference.
        if ((s.sliceMethod == 0 || s.sliceMethod == 2) && s.transientMarkers.isNotEmpty() && s.totalFrames > 0) {
            val wfLeft     = x + 10
            val wfRight    = x + 630
            val onSliceRow = s.cursorRow == 11

            // Highlight current slice region (start → end, not a spot near the marker)
            if (onSliceRow) {
                val (sliceStart, sliceEnd) = s.getSliceBounds(s.sliceIndex)
                val sXf = wfLeft + ((sliceStart - s.viewStart).toFloat() / viewLenF) * 620f
                val eXf = wfLeft + ((sliceEnd   - s.viewStart).toFloat() / viewLenF) * 620f
                val sX  = sXf.toInt().coerceIn(wfLeft, wfRight)
                val eX  = eXf.toInt().coerceIn(wfLeft, wfRight)
                if (eX > sX) {
                    drawRect(Color(0x22FFAA00),
                        topLeft = Offset((sX * scale).toFloat(), (y * scale).toFloat()),
                        size    = Size(((eX - sX) * scale).toFloat(), (WAVEFORM_H * scale).toFloat()))
                }
            }

            // Draw boundary lines at each transient position.
            // Marker idx is the right edge of slice idx and left edge of slice idx+1,
            // so active boundaries for sliceIndex are idx==sliceIndex-1 and idx==sliceIndex.
            s.transientMarkers.forEachIndexed { idx, markerFrame ->
                val mXf = wfLeft + ((markerFrame - s.viewStart).toFloat() / viewLenF) * 620f
                if (mXf >= wfLeft && mXf <= wfRight) {
                    val isActiveBound = onSliceRow && (idx == s.sliceIndex - 1 || idx == s.sliceIndex)
                    drawLine(if (isActiveBound) Color(0xFFFFAA00) else Color(0xFF664400),
                        start       = Offset((mXf * scale).toFloat(), (y * scale).toFloat()),
                        end         = Offset((mXf * scale).toFloat(), ((y + WAVEFORM_H) * scale).toFloat()),
                        strokeWidth = scale.toFloat())
                }
            }
        }

        // Slice division markers (DIVIDE mode)
        if (s.sliceMethod == 1 && s.sliceDivisions > 0 && s.totalFrames > 0) {
            val div     = s.sliceDivisions
            val wfLeft  = x + 10
            val wfRight = x + 630
            val onSliceRow = s.cursorRow == 11

            // Highlight current slice region
            if (onSliceRow) {
                val sliceStart = (s.sliceIndex.toLong() * s.totalFrames) / div
                val sliceEnd   = (((s.sliceIndex + 1).toLong() * s.totalFrames) / div).coerceAtMost(s.totalFrames.toLong())
                val sXf = wfLeft + ((sliceStart - s.viewStart).toFloat() / viewLenF) * 620f
                val eXf = wfLeft + ((sliceEnd   - s.viewStart).toFloat() / viewLenF) * 620f
                val sX  = sXf.toInt().coerceIn(wfLeft, wfRight)
                val eX  = eXf.toInt().coerceIn(wfLeft, wfRight)
                if (eX > sX) {
                    drawRect(Color(0x22FFAA00),
                        topLeft = Offset((sX * scale).toFloat(), (y * scale).toFloat()),
                        size    = Size(((eX - sX) * scale).toFloat(), (WAVEFORM_H * scale).toFloat()))
                }
            }

            // Draw boundary lines for each division
            for (i in 1 until div) {
                val markerFrame = (i.toLong() * s.totalFrames) / div
                val mXf = wfLeft + ((markerFrame - s.viewStart).toFloat() / viewLenF) * 620f
                if (mXf >= wfLeft && mXf <= wfRight) {
                    val isActiveBound = onSliceRow && (i == s.sliceIndex || i == s.sliceIndex + 1)
                    drawLine(if (isActiveBound) Color(0xFFFFAA00) else Color(0xFF664400),
                        start       = Offset((mXf * scale).toFloat(), (y * scale).toFloat()),
                        end         = Offset((mXf * scale).toFloat(), ((y + WAVEFORM_H) * scale).toFloat()),
                        strokeWidth = scale.toFloat())
                }
            }
        }

        // Real-time playback marker — position relative to view window
        if (s.playbackPosition >= 0f && s.totalFrames > 0) {
            val playFrame = s.playbackPosition * s.totalFrames
            val mXf = x + 10 + ((playFrame - s.viewStart.toFloat()) / viewLenF) * 620f
            if (mXf >= x + 10 && mXf <= x + 630) {
                drawLine(Color(0xFF00FF44),
                    start       = Offset((mXf * scale).toFloat(), (y * scale).toFloat()),
                    end         = Offset((mXf * scale).toFloat(), ((y + WAVEFORM_H) * scale).toFloat()),
                    strokeWidth = (scale * 2).toFloat())
            }
        }
    }

    private fun DrawScope.drawRow8Sel(x: Int, y: Int, scale: Int, s: SampleEditorState) {
        val ty  = y + TEXT_PADDING
        val cur = s.cursorRow == 8
        if (cur) rowBg(x, y, scale)
        drawBitmapText("SELECTION",               x + 10,  ty, scale, Color.Gray, CHAR_SPACING, FONT_SCALE)
        drawBitmapText(s.selectionStart.toHex8(), x + 180, ty, scale,
            if (cur && s.cursorCol == 0) Color.Yellow else Color.White, CHAR_SPACING, FONT_SCALE)
        drawBitmapText(s.selectionEnd.toHex8(),   x + 335, ty, scale,
            if (cur && s.cursorCol == 1) Color.Yellow else Color.White, CHAR_SPACING, FONT_SCALE)
        //drawBitmapText("LENGTH",                  x + 430, ty, scale, Color.Gray,  CHAR_SPACING, FONT_SCALE)
        //drawBitmapText((s.selectionEnd - s.selectionStart).toHex8(), x + 525, ty, scale, Color.Gray, CHAR_SPACING, FONT_SCALE)
    }

    private fun DrawScope.drawRow10Slice(x: Int, y: Int, scale: Int, s: SampleEditorState) {
        val ty  = y + TEXT_PADDING
        val cur = s.cursorRow == 10
        if (cur) rowBg(x, y, scale)
        drawBitmapText("SLICE", x + 10, ty, scale, Color.Gray, CHAR_SPACING, FONT_SCALE)
        drawBitmapText(SLICE_METHODS[s.sliceMethod], x + 175, ty, scale,
            if (cur && s.cursorCol == 0) Color.Yellow else Color.White, CHAR_SPACING, FONT_SCALE)
        if (s.sliceMethod != 2) {  // not OFF
            val (lbl, valStr) = when (s.sliceMethod) {
                0 -> "SENS" to s.sliceSensitivity.toHex2()
                else -> "BY"   to s.sliceDivisions.toHex2()
            }
            drawBitmapText(lbl,    x + 335, ty, scale, if (cur && s.cursorCol == 1) Color.Yellow else Color.Gray,  CHAR_SPACING, FONT_SCALE)
            drawBitmapText(valStr, x + 410, ty, scale, if (cur && s.cursorCol == 1) Color.Yellow else Color.White, CHAR_SPACING, FONT_SCALE)
        }
    }

    private fun DrawScope.drawRow11SliceDetail(x: Int, y: Int, scale: Int, s: SampleEditorState) {
        if (s.sliceMethod == 2) return
        val ty  = y + TEXT_PADDING
        val cur = s.cursorRow == 11
        if (cur) rowBg(x, y, scale)
        val idxColor = if (cur && s.cursorCol == 0) Color.Yellow else Color.White
        val posColor = if (cur && s.cursorCol == 1) Color.Yellow else Color.White
        if (s.sliceMethod == 0) {
            // TRANSIENT: N markers → N+1 slices; show "idx/total"
            val total = s.transientMarkers.size + 1
            drawBitmapText(s.sliceIndex.toHex2(), x + 90,  ty, scale, idxColor,    CHAR_SPACING, FONT_SCALE)
            drawBitmapText("/${total.toHex2()}",  x + 120, ty, scale, Color.Gray,  CHAR_SPACING, FONT_SCALE)
        } else {
            drawBitmapText(s.sliceIndex.toHex2(), x + 90,  ty, scale, idxColor,    CHAR_SPACING, FONT_SCALE)
        }
        drawBitmapText(s.effectiveSlicePosition.toHex8(), x + 175, ty, scale, posColor, CHAR_SPACING, FONT_SCALE)
    }

    private fun DrawScope.drawOpsRow(x: Int, y: Int, scale: Int, ops: List<String>, isCur: Boolean, curCol: Int) {
        val ty   = y + TEXT_PADDING
        if (isCur) rowBg(x, y, scale)
        val colW = width / ops.size   // ~106px per column
        ops.forEachIndexed { i, label ->
            val lx = x + i * colW + 15
            drawBitmapText(label, lx, ty, scale,
                if (isCur && curCol == i) Color.Yellow else Color.White, CHAR_SPACING, FONT_SCALE)
        }
    }

    private fun DrawScope.drawRow16Fx(x: Int, y: Int, scale: Int, s: SampleEditorState) {
        val ty  = y + TEXT_PADDING
        val cur = s.cursorRow == 16
        if (cur) rowBg(x, y, scale)
        drawBitmapText("EFFECT",               x + 10,  ty, scale, Color.Gray, CHAR_SPACING, FONT_SCALE)
        drawBitmapText(FX_TYPES[s.fxType],     x + 180, ty, scale,
            if (cur && s.cursorCol == 0) Color.Yellow else Color.White, CHAR_SPACING, FONT_SCALE)
        val valStr = if (s.fxType == 4) SYNC_TYPES[s.syncType] else s.fxValue.toHex2()
        drawBitmapText(valStr,                 x + 290, ty, scale,
            if (cur && s.cursorCol == 1) Color.Yellow else Color.White, CHAR_SPACING, FONT_SCALE)
        drawBitmapText("APPLY",                x + 440, ty, scale,
            if (cur && s.cursorCol == 2) Color.Yellow else Color.White, CHAR_SPACING, FONT_SCALE)
    }

    private fun DrawScope.drawRow18Name(x: Int, y: Int, scale: Int, s: SampleEditorState) {
        val ty  = y + TEXT_PADDING
        val cur = s.cursorRow == 18
        if (cur) rowBg(x, y, scale)
        drawBitmapText("NAME",        x + 10,  ty, scale, if (cur) Color.Yellow else Color.Gray,  CHAR_SPACING, FONT_SCALE)
        drawBitmapText(s.sampleName,  x + 120, ty, scale, if (cur) Color.Yellow else Color.White, CHAR_SPACING, FONT_SCALE)
    }

    private fun DrawScope.drawRow19Save(x: Int, y: Int, scale: Int, s: SampleEditorState) {
        val ty  = y + TEXT_PADDING
        val cur = s.cursorRow == 19
        if (cur) rowBg(x, y, scale)
        drawBitmapText("LOAD",      x + 120, ty, scale, if (cur && s.cursorCol == 0) Color.Yellow else Color.White, CHAR_SPACING, FONT_SCALE)
        drawBitmapText("SAVE",      x + 230, ty, scale, if (cur && s.cursorCol == 1) Color.Yellow else Color.White, CHAR_SPACING, FONT_SCALE)
        drawBitmapText("OVERWRITE", x + 335, ty, scale, if (cur && s.cursorCol == 2) Color.Yellow else Color.White, CHAR_SPACING, FONT_SCALE)
        if (s.sliceMethod != 2) {
            drawBitmapText("CHOP", x + 510, ty, scale, if (cur && s.cursorCol == 3) Color.Yellow else Color.White, CHAR_SPACING, FONT_SCALE)
        }
    }

    private fun DrawScope.drawConfirmDialog(x: Int, y: Int, scale: Int) {
        drawRect(Color(0xAA000000),
            topLeft = Offset((x * scale).toFloat(), (y * scale).toFloat()),
            size    = Size((width * scale).toFloat(), (height * scale).toFloat()))
        val dX = x + 160; val dY = y + 200; val dW = 320; val dH = 80
        drawRect(Color(0xFF222222),
            topLeft = Offset((dX * scale).toFloat(), (dY * scale).toFloat()),
            size    = Size((dW * scale).toFloat(),   (dH * scale).toFloat()))
        drawRect(Color(0xFF555555),
            topLeft = Offset((dX * scale).toFloat(), (dY * scale).toFloat()),
            size    = Size((dW * scale).toFloat(),   (2 * scale).toFloat()))
        drawBitmapText("ARE YOU SURE?", dX + 75, dY + 15, scale, Color.White,  CHAR_SPACING, FONT_SCALE)
        drawBitmapText("A=YES  B=NO",   dX + 85, dY + 45, scale, Color.Yellow, CHAR_SPACING, FONT_SCALE)
    }

    // ─── Cursor context ───────────────────────────────────────────────────────

    fun getCursorContext(s: SampleEditorState): CursorContext {
        if (s.showConfirmClose) return CursorContextFactory.none()
        return when (s.cursorRow) {
            0 -> CursorContextFactory.readOnly()
            1 -> when (s.cursorCol) {
                0 -> CursorContextFactory.hexByte(s.zoomLevel, 0, 4)
                1 -> CursorContextFactory.toggleTernary(SOURCE_VALUES[s.sourceMode],   SOURCE_VALUES)
                2 -> CursorContextFactory.toggleTernary(RATE_VALUES[s.rateMode],       RATE_VALUES)
                else -> CursorContextFactory.none()
            }
            2 -> when (s.cursorCol) {
                0 -> CursorContextFactory.hexByte(s.pitchSemitones + 24, 0, 48)
                1 -> CursorContextFactory.toggleTernary(DURATION_VALUES[s.durationIndex], DURATION_VALUES)
                2 -> CursorContextFactory.toggleBinary(s.snapEnabled)
                else -> CursorContextFactory.none()
            }
            in 3..8 -> CursorContextFactory.none()  // waveform / selection: handled directly in MainActivity
            10 -> when (s.cursorCol) {
                0 -> CursorContextFactory.toggleTernary(SLICE_METHODS[s.sliceMethod], SLICE_METHODS)
                1 -> when (s.sliceMethod) {
                    0    -> CursorContextFactory.hexByte(s.sliceSensitivity, 0, 255)
                    1    -> CursorContextFactory.hexByte(s.sliceDivisions,   0, 255)
                    else -> CursorContextFactory.none()
                }
                else -> CursorContextFactory.none()
            }
            11 -> when {
                s.sliceMethod == 0 && s.cursorCol == 0 ->
                    CursorContextFactory.hexByte(s.sliceIndex, 0, s.transientMarkers.size)
                s.sliceMethod == 1 && s.cursorCol == 0 ->
                    CursorContextFactory.hexByte(s.sliceIndex, 0, (s.sliceDivisions - 1).coerceAtLeast(0))
                else -> CursorContextFactory.none()
            }
            13, 14 -> CursorContextFactory.none()  // action rows: handled in MainActivity
            16 -> when (s.cursorCol) {
                0 -> CursorContextFactory.toggleTernary(FX_TYPES[s.fxType], FX_TYPES)
                1 -> when (s.fxType) {
                    3    -> CursorContextFactory.hexByte(s.fxValue, 0, 127)  // EQ: slot 0-127
                    4    -> CursorContextFactory.toggleTernary(SYNC_TYPES[s.syncType], SYNC_TYPES)
                    else -> CursorContextFactory.hexByte(s.fxValue, 0, 255)
                }
                2 -> CursorContextFactory.none()  // APPLY: action
                else -> CursorContextFactory.none()
            }
            18, 19 -> CursorContextFactory.none()  // name / save: handled in MainActivity
            else   -> CursorContextFactory.none()
        }
    }

    // ─── Input handler ────────────────────────────────────────────────────────

    fun handleInput(s: SampleEditorState, action: InputAction): InputResult {
        when (s.cursorRow) {
            1 -> when (s.cursorCol) {
                0 -> if (action is InputAction.SET_VALUE) return InputResult(zoomLevel  = action.value.coerceIn(0, 4))
                1 -> if (action is InputAction.SET_VALUE) return InputResult(sourceMode = action.value)
                2 -> if (action is InputAction.SET_VALUE) return InputResult(rateMode   = action.value)
            }
            2 -> when (s.cursorCol) {
                0 -> if (action is InputAction.SET_VALUE) return InputResult(pitchSemitones = action.value - 24)
                1 -> if (action is InputAction.SET_VALUE) return InputResult(durationIndex  = action.value)
                2 -> if (action is InputAction.SET_VALUE) return InputResult(snapEnabled    = action.value == 1)
            }
            10 -> when (s.cursorCol) {
                0 -> if (action is InputAction.SET_VALUE) return InputResult(
                    sliceMethod = action.value,
                    // Switching to TRANSIENT clears existing markers so detection runs fresh.
                    // Switching to DIVIDE or OFF preserves markers (they become read-only display).
                    transientMarkers = if (action.value == 0) intArrayOf() else null
                )
                1 -> if (action is InputAction.SET_VALUE) return when (s.sliceMethod) {
                    0    -> InputResult(sliceSensitivity = action.value, transientMarkers = intArrayOf())
                    else -> InputResult(sliceDivisions   = action.value)
                }
            }
            11 -> if (action is InputAction.SET_VALUE && s.cursorCol == 0 && s.sliceMethod != 2) {
                val newIdx = action.value
                val (sliceStart, sliceEnd) = s.getSliceBounds(newIdx)
                return InputResult(sliceIndex = newIdx, selectionStart = sliceStart, selectionEnd = sliceEnd)
            }
            16 -> when (s.cursorCol) {
                0 -> if (action is InputAction.SET_VALUE) return InputResult(fxType  = action.value)
                1 -> if (action is InputAction.SET_VALUE) return if (s.fxType == 4)
                         InputResult(syncType = action.value)
                     else InputResult(fxValue = action.value)
            }
        }
        return InputResult()
    }

    data class InputResult(
        val zoomLevel:        Int?      = null,
        val sourceMode:       Int?      = null,
        val rateMode:         Int?      = null,
        val pitchSemitones:   Int?      = null,
        val durationIndex:    Int?      = null,
        val snapEnabled:      Boolean?  = null,
        val sliceMethod:      Int?      = null,
        val sliceSensitivity: Int?      = null,
        val sliceDivisions:   Int?      = null,
        val sliceIndex:       Int?      = null,
        val transientMarkers: IntArray? = null,
        val fxType:           Int?      = null,
        val fxValue:          Int?      = null,
        val syncType:         Int?      = null,
        val selectionStart:   Long?     = null,
        val selectionEnd:     Long?     = null
    ) {
        val modified = listOf(zoomLevel, sourceMode, rateMode, pitchSemitones, durationIndex,
            snapEnabled, sliceMethod, sliceSensitivity, sliceDivisions, sliceIndex, transientMarkers,
            fxType, fxValue, syncType, selectionStart, selectionEnd
        ).any { it != null }
    }

    companion object {
        val SOURCE_VALUES   = listOf("LEFT", "RIGHT", "STEREO")
        val RATE_VALUES     = listOf("HIGH", "NORM", "LOFI")
        val DURATION_VALUES = listOf("4 BAR", "2 BAR", "1 BAR", "1/2", "1/4", "1/8", "1/16", "1/32")
        val FX_TYPES        = listOf("OTT", "DUST", "DRIVE", "EQ", "SYNC")
        val SYNC_TYPES      = listOf("RPITCH")
        val SLICE_METHODS   = listOf("TRANSIENT", "DIVIDE", "OFF")
        val OPS_ROW1 = listOf("CROP", "COPY", "CUT", "DUPL", "PASTE", "DEL")
        val OPS_ROW2 = listOf("NORM", "FADE+", "FADE-", "SLNC", "REV", "UNDO")

        /** Next navigable row above (skips waveform rows 3-7, spacers 9, 12, 15, 17). */
        fun rowAbove(row: Int, sliceMethod: Int = -1) = when (row) {
            0  -> 0
            1  -> 19  // wrap to last row
            8  -> 2;  10 -> 8
            13 -> if (sliceMethod == 2) 10 else 11
            16 -> 14; 18 -> 16
            else -> row - 1
        }
        /** Next navigable row below (skips waveform rows 3-7, spacers 9, 12, 15, 17). */
        fun rowBelow(row: Int, sliceMethod: Int = -1) = when (row) {
            2  -> 8;  8 -> 10
            10 -> if (sliceMethod == 2) 13 else 11
            11 -> 13; 14 -> 16; 16 -> 18
            19 -> 1   // wrap to first row
            else -> row + 1
        }
        /** Maximum cursor column for a given row. */
        fun maxColForRow(row: Int, sliceMethod: Int = 2) = when (row) {
            1, 2     -> 2
            in 3..8  -> 1
            10       -> if (sliceMethod == 2) 0 else 1
            11       -> 1
            13       -> 5
            14       -> 5
            16       -> 2
            18       -> 0
            19       -> if (sliceMethod == 2) 2 else 3
            else     -> 0
        }
    }
}

// ─── State data class ─────────────────────────────────────────────────────────

data class SampleEditorState(
    val sampleId:    Int,
    val instrumentId: Int,
    val cursorRow: Int = 1,
    val cursorCol: Int = 0,
    // Sample info
    val sampleName:     String  = "",
    val sampleFilePath: String? = null,
    val sampleRate:     Int     = 44100,
    val totalFrames:    Int     = 0,
    // Waveform display data (min/max per column, populated by JNI)
    val waveformData: FloatArray = floatArrayOf(),
    // Row 1: View controls
    val zoomLevel:  Int = 0,   // 0=1×, 1=2×, 2=4×, 3=8×, 4=16×
    val sourceMode: Int = 0,   // 0=LEFT, 1=RIGHT, 2=STEREO
    val rateMode:   Int = 0,   // 0=HIGH, 1=NORM, 2=LOFI
    // Row 2: Edit controls
    val pitchSemitones: Int     = 0,
    val durationIndex:  Int     = 2,   // default "1 BAR"
    val snapEnabled:    Boolean = true,
    // Selection (32-bit frame positions)
    val selectionStart: Long = 0L,
    val selectionEnd:   Long = 0L,
    // Slicing
    val sliceMethod:      Int      = 2,    // 0=TRANSIENT, 1=DIVIDE, 2=OFF
    val sliceSensitivity: Int      = 64,
    val sliceDivisions:   Int      = 8,
    val sliceIndex:       Int      = 0,
    val slicePosition:    Long     = 0L,
    val transientMarkers: IntArray = intArrayOf(),
    // FX row
    val fxType:  Int = 0,   // 0=OTT, 1=DUST, 2=DRIVE, 3=EQ, 4=SYNC
    val fxValue: Int = 0,
    val syncType: Int = 0,  // when fxType==SYNC: 0=RPITCH
    // State flags
    val isModified:       Boolean = false,
    val showConfirmClose: Boolean = false,
    // Real-time playback position (0.0-1.0 fraction, or -1 if not playing)
    val playbackPosition: Float = -1f
) {
    /** Returns (sliceStart, sliceEnd) frame pair for the given slice index in the current mode. */
    fun getSliceBounds(idx: Int): Pair<Long, Long> = when (sliceMethod) {
        0 -> {
            val start = if (idx == 0) 0L else transientMarkers.getOrElse(idx - 1) { 0 }.toLong()
            val end   = transientMarkers.getOrElse(idx) { totalFrames }.toLong()
            Pair(start, end)
        }
        1 -> {
            val div   = sliceDivisions.coerceAtLeast(1)
            val start = (idx.toLong() * totalFrames) / div
            val end   = (((idx + 1).toLong() * totalFrames) / div).coerceAtMost(totalFrames.toLong())
            Pair(start, end)
        }
        else -> Pair(0L, totalFrames.toLong())
    }

    val effectiveSlicePosition: Long get() = getSliceBounds(sliceIndex).first

    // Zoom view window: follows playback during playback, otherwise anchors to the active marker
    private val viewCenter: Long get() {
        if (playbackPosition >= 0f && totalFrames > 0)
            return (playbackPosition * totalFrames).toLong()
        return when {
            cursorRow == 8  && cursorCol == 0 -> selectionStart
            cursorRow == 8  && cursorCol == 1 -> selectionEnd
            cursorRow == 11 && sliceMethod != 2 -> effectiveSlicePosition
            else -> selectionStart
        }
    }
    val viewStart: Long get() {
        if (zoomLevel == 0 || totalFrames <= 0) return 0L
        val visibleFrames = (totalFrames.toLong() ushr zoomLevel).coerceAtLeast(1L)
        val start = viewCenter - visibleFrames / 2L
        return start.coerceIn(0L, (totalFrames.toLong() - visibleFrames).coerceAtLeast(0L))
    }
    val viewEnd: Long get() {
        if (zoomLevel == 0 || totalFrames <= 0) return totalFrames.toLong()
        val visibleFrames = (totalFrames.toLong() ushr zoomLevel).coerceAtLeast(1L)
        return (viewStart + visibleFrames).coerceAtMost(totalFrames.toLong())
    }

    val durationDisplay: String get() {
        if (sampleRate <= 0 || totalFrames <= 0) return "--:--.--"
        val ratio          = Math.pow(2.0, pitchSemitones / 12.0)
        val effectiveFrames = (totalFrames / ratio).toLong().coerceAtLeast(1L)
        val ms    = (effectiveFrames * 1000L) / sampleRate
        val mins  = ms / 60000
        val secs  = (ms % 60000) / 1000
        val centis = (ms % 1000) / 10
        return "%02d:%02d.%02d".format(mins, secs, centis)
    }

    fun applyResult(r: SampleEditorModule.InputResult) = copy(
        zoomLevel        = r.zoomLevel           ?: zoomLevel,
        sourceMode       = r.sourceMode          ?: sourceMode,
        rateMode         = r.rateMode            ?: rateMode,
        pitchSemitones   = r.pitchSemitones      ?: pitchSemitones,
        durationIndex    = r.durationIndex       ?: durationIndex,
        snapEnabled      = r.snapEnabled         ?: snapEnabled,
        sliceMethod      = r.sliceMethod         ?: sliceMethod,
        sliceSensitivity = r.sliceSensitivity    ?: sliceSensitivity,
        sliceDivisions   = r.sliceDivisions      ?: sliceDivisions,
        sliceIndex       = r.sliceIndex          ?: sliceIndex,
        transientMarkers = r.transientMarkers    ?: transientMarkers,
        fxType           = r.fxType              ?: fxType,
        fxValue          = r.fxValue             ?: fxValue,
        syncType         = r.syncType            ?: syncType,
        selectionStart   = r.selectionStart      ?: selectionStart,
        selectionEnd     = r.selectionEnd        ?: selectionEnd
    )
}
