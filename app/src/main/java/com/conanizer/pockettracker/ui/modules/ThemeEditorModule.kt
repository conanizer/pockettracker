package com.conanizer.pockettracker.ui.modules

import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.geometry.Size
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.drawscope.DrawScope
import com.conanizer.pockettracker.ui.theme.AppTheme
import com.conanizer.pockettracker.ui.TrackerModule
import com.conanizer.pockettracker.ui.CHAR_SPACING
import com.conanizer.pockettracker.ui.FONT_SCALE
import com.conanizer.pockettracker.ui.ROW_HEIGHT
import com.conanizer.pockettracker.ui.TEXT_PADDING
import com.conanizer.pockettracker.ui.drawBitmapText
import com.conanizer.pockettracker.ui.toHex2

data class ThemeEditorState(
    val isOpen: Boolean = false,
    val cursorRow: Int = 0,       // 0 = THEME row (top), 1-16 = color rows
    val cursorChannel: Int = 0    // On THEME row: 0=cycle, 1=SAVE, 2=LOAD; On color rows: 0=R, 1=G, 2=B
)

data class ThemeEditorDrawState(
    val theme: AppTheme,
    val editorState: ThemeEditorState
)

class ThemeEditorModule : TrackerModule {
    override val width = 510
    override val height = 392

    companion object {
        const val MAX_ROW = 17  // last row index (color row 17; list is scrollable)

        data class ColorRow(
            val label: String,
            val get: (AppTheme) -> Long,
            val set: (AppTheme, Long) -> AppTheme
        )

        val COLOR_ROWS = listOf(
            ColorRow("BACKGROUND", { it.background },    { t, v -> t.copy(background    = v) }),
            ColorRow("ROW 4TH",    { it.rowEvery4th },   { t, v -> t.copy(rowEvery4th   = v) }),
            ColorRow("ROW CURSOR", { it.rowCursor },     { t, v -> t.copy(rowCursor     = v) }),
            ColorRow("ROW PLAY",   { it.rowPlayback },   { t, v -> t.copy(rowPlayback   = v) }),
            ColorRow("ROW SELECT", { it.rowSelection },  { t, v -> t.copy(rowSelection  = v) }),
            ColorRow("TXT TITLE",  { it.textTitle },     { t, v -> t.copy(textTitle     = v) }),
            ColorRow("TXT PARAM",  { it.textParam },     { t, v -> t.copy(textParam     = v) }),
            ColorRow("TXT VALUE",  { it.textValue },     { t, v -> t.copy(textValue     = v) }),
            ColorRow("TXT CURSOR", { it.textCursor },    { t, v -> t.copy(textCursor    = v) }),
            ColorRow("TXT EMPTY",  { it.textEmpty },     { t, v -> t.copy(textEmpty     = v) }),
            ColorRow("VIZ BG",     { it.vizBackground }, { t, v -> t.copy(vizBackground = v) }),
            ColorRow("VIZ LINE",   { it.vizCenterLine }, { t, v -> t.copy(vizCenterLine = v) }),
            ColorRow("VIZ WAVE",   { it.vizWave },       { t, v -> t.copy(vizWave       = v) }),
            ColorRow("MTR BG",     { it.meterBackground },{ t, v -> t.copy(meterBackground = v) }),
            ColorRow("MTR LOW",    { it.meterLow },      { t, v -> t.copy(meterLow      = v) }),
            ColorRow("MTR MID",    { it.meterMid },      { t, v -> t.copy(meterMid      = v) }),
            ColorRow("MTR HIGH",   { it.meterHigh },     { t, v -> t.copy(meterHigh     = v) })
        )

        // ── The two edits, as PURE functions ──────────────────────────────────────────────────────
        //
        // These lived in AppInputDispatcher as `adjustThemeColor` / `cycleNextBuiltinTheme` /
        // `cyclePrevBuiltinTheme`, reading `themeEditorState.cursorRow` and writing `appTheme` straight
        // out of the dispatcher's Compose state. The arithmetic is unchanged — it has been MOVED here
        // and given its inputs as parameters, and the dispatcher now calls it.
        //
        // ⚠️ WHY THE MOVE: the C++ port byte-compares this behaviour against the Kotlin original
        // (`tools/ptinput`), and it can only do that if the Kotlin is REACHABLE from a JVM unit test.
        // Buried in the dispatcher it was not — that class is entangled with ~60 `mutableStateOf` refs
        // and cannot be constructed off-device. The alternative was to let the test re-implement the
        // arithmetic and compare the port against THAT, which measures nothing: a fixture that
        // re-derives the thing it is measuring cannot catch the thing it is measuring. Same reason
        // EqModule's frequency formatter was made `internal` in S8.

        /**
         * Nudge one channel of the colour on `row` (1..17; row 0 is the THEME header and is rejected).
         * `delta` is ±0x01 from A+UP/A+DOWN, ±0x10 from A+RIGHT/A+LEFT. Channels CLAMP at 0 and 255.
         *
         * The alpha is FORCED to 0xFF on every write — a theme colour is opaque by construction, and
         * there is no row in this editor that could reach an alpha channel.
         */
        fun adjustColor(theme: AppTheme, row: Int, channel: Int, delta: Int): AppTheme {
            if (row < 1 || row > COLOR_ROWS.size) return theme
            val colorRow = COLOR_ROWS[row - 1]
            val current = colorRow.get(theme)
            val r = ((current shr 16) and 0xFFL).toInt()
            val g = ((current shr 8)  and 0xFFL).toInt()
            val b = ( current         and 0xFFL).toInt()
            val newColor = 0xFF000000L or when (channel) {
                0 -> ((r + delta).coerceIn(0, 255).toLong() shl 16) or (g.toLong() shl 8) or b.toLong()
                1 -> (r.toLong() shl 16) or ((g + delta).coerceIn(0, 255).toLong() shl 8) or b.toLong()
                2 -> (r.toLong() shl 16) or (g.toLong() shl 8) or (b + delta).coerceIn(0, 255).toLong()
                else -> (r.toLong() shl 16) or (g.toLong() shl 8) or b.toLong()
            }
            return colorRow.set(theme, newColor)
        }

        /**
         * Step the built-in palette: `delta` < 0 is PREV (A+UP), >= 0 is NEXT (A+DOWN).
         *
         * ⚠️ It REPLACES the whole theme, so any colours dialled on the current one are lost — that is
         * what SAVE is for. And the two directions are not exact inverses: a theme whose name is not a
         * built-in (anything loaded from a `.ptt`) has index −1, so NEXT lands on CLASSIC and PREV on
         * MONO. Both enter the ring at an end; neither returns you to where you were, because the theme
         * left the ring the moment it stopped being a built-in.
         *
         * `visualizerType` rides across the swap — the palette belongs to the theme, the visualizer to
         * the user.
         */
        fun cycleBuiltin(theme: AppTheme, delta: Int): AppTheme {
            val builtins = AppTheme.BUILTINS
            val idx = builtins.indexOfFirst { it.name == theme.name }
            val target = if (delta >= 0) {
                if (idx >= 0) (idx + 1) % builtins.size else 0
            } else {
                if (idx > 0) idx - 1 else builtins.size - 1
            }
            return builtins[target].copy(visualizerType = theme.visualizerType)
        }
    }


    override fun DrawScope.draw(x: Int, y: Int, scale: Int, state: Any?) {
        val s = state as? ThemeEditorDrawState ?: return
        val theme = s.theme
        val es = s.editorState

        drawRect(
            color = Color(theme.background),
            topLeft = Offset((x * scale).toFloat(), (y * scale).toFloat()),
            size = Size((width * scale).toFloat(), (height * scale).toFloat())
        )

        val nameColumnX = x + 10
        val rColumnX    = x + 230
        val gColumnX    = x + 267
        val bColumnX    = x + 304
        val swatchX     = x + 350
        val swatchW     = width - 350 - 10

        // Theme name / SAVE / LOAD column positions
        val themeNameX  = x + 165
        val saveLabelX  = x + 310
        val loadLabelX  = x + 390

        val titleY = y + TEXT_PADDING
        drawBitmapText(
            text = "THEME EDIT", x = nameColumnX, y = titleY, scale = scale,
            color = Color(theme.textTitle), spacing = CHAR_SPACING, fontScale = FONT_SCALE
        )

        // The color list is taller than the panel, so the rows below the title scroll to
        // keep the cursor in view — same idea as the song screen / file browser.
        val rowAreaTopY = titleY + ROW_HEIGHT + 14
        val totalRows = 1 + COLOR_ROWS.size          // row 0 = THEME, rows 1..N = colors
        val maxVisibleRows = ((height - (rowAreaTopY - y)) / ROW_HEIGHT).coerceAtLeast(1)
        val maxScroll = (totalRows - maxVisibleRows).coerceAtLeast(0)
        val scrollOffset =
            (if (es.cursorRow >= maxVisibleRows) es.cursorRow - maxVisibleRows + 1 else 0)
                .coerceIn(0, maxScroll)
        fun isRowVisible(logical: Int) =
            logical in scrollOffset until (scrollOffset + maxVisibleRows)
        fun rowTop(logical: Int) = rowAreaTopY + (logical - scrollOffset) * ROW_HEIGHT

        // ── ROW 0: THEME (builtin cycle + SAVE/LOAD) ──────────────────
        if (isRowVisible(0)) {
            val themeRowY = rowTop(0)
            val isCursorTheme = es.cursorRow == 0
            if (isCursorTheme) {
                drawRect(
                    color = Color(theme.rowCursor),
                    topLeft = Offset((x * scale).toFloat(), (themeRowY * scale).toFloat()),
                    size = Size((width * scale).toFloat(), (ROW_HEIGHT * scale).toFloat())
                )
            }
            drawBitmapText(
                text = "THEME", x = nameColumnX, y = themeRowY + TEXT_PADDING, scale = scale,
                color = if (isCursorTheme) Color(theme.textCursor) else Color(theme.textParam),
                spacing = CHAR_SPACING, fontScale = FONT_SCALE
            )
            // Theme name (position ch=0)
            val nameColor = when {
                isCursorTheme && es.cursorChannel == 0 -> Color(theme.textCursor)
                isCursorTheme -> Color(theme.textValue)
                else -> Color(theme.textParam)
            }
            drawBitmapText(
                text = theme.name, x = themeNameX, y = themeRowY + TEXT_PADDING, scale = scale,
                color = nameColor, spacing = CHAR_SPACING, fontScale = FONT_SCALE
            )
            // SAVE button (position ch=1)
            val saveColor = when {
                isCursorTheme && es.cursorChannel == 1 -> Color(theme.textCursor)
                isCursorTheme -> Color(theme.textValue)
                else -> Color(theme.textParam)
            }
            drawBitmapText(
                text = "SAVE", x = saveLabelX, y = themeRowY + TEXT_PADDING, scale = scale,
                color = saveColor, spacing = CHAR_SPACING, fontScale = FONT_SCALE
            )
            // LOAD button (position ch=2)
            val loadColor = when {
                isCursorTheme && es.cursorChannel == 2 -> Color(theme.textCursor)
                isCursorTheme -> Color(theme.textValue)
                else -> Color(theme.textParam)
            }
            drawBitmapText(
                text = "LOAD", x = loadLabelX, y = themeRowY + TEXT_PADDING, scale = scale,
                color = loadColor, spacing = CHAR_SPACING, fontScale = FONT_SCALE
            )
        }

        // ── ROWS 1..N: Color parameters ────────────────────────────────
        for (i in COLOR_ROWS.indices) {
            val logical = i + 1                       // cursorRow 1 = COLOR_ROWS[0], etc.
            if (!isRowVisible(logical)) continue
            val colorRow = COLOR_ROWS[i]
            val colorValue = colorRow.get(theme)
            val isCursor = es.cursorRow == logical
            val cRowY = rowTop(logical)

            if (isCursor) {
                drawRect(
                    color = Color(theme.rowCursor),
                    topLeft = Offset((x * scale).toFloat(), (cRowY * scale).toFloat()),
                    size = Size((width * scale).toFloat(), (ROW_HEIGHT * scale).toFloat())
                )
            }

            drawBitmapText(
                text = colorRow.label, x = nameColumnX, y = cRowY + TEXT_PADDING, scale = scale,
                color = if (isCursor) Color(theme.textCursor) else Color(theme.textParam),
                spacing = CHAR_SPACING, fontScale = FONT_SCALE
            )

            val r = ((colorValue shr 16) and 0xFFL).toInt()
            val g = ((colorValue shr 8)  and 0xFFL).toInt()
            val b = ( colorValue         and 0xFFL).toInt()

            val rColor = when {
                isCursor && es.cursorChannel == 0 -> Color(theme.textCursor)
                isCursor -> Color(theme.textValue)
                else -> Color(theme.textParam)
            }
            val gColor = when {
                isCursor && es.cursorChannel == 1 -> Color(theme.textCursor)
                isCursor -> Color(theme.textValue)
                else -> Color(theme.textParam)
            }
            val bColor = when {
                isCursor && es.cursorChannel == 2 -> Color(theme.textCursor)
                isCursor -> Color(theme.textValue)
                else -> Color(theme.textParam)
            }

            drawBitmapText(r.toHex2(), rColumnX, cRowY + TEXT_PADDING, scale, rColor, CHAR_SPACING, FONT_SCALE)
            drawBitmapText(g.toHex2(), gColumnX, cRowY + TEXT_PADDING, scale, gColor, CHAR_SPACING, FONT_SCALE)
            drawBitmapText(b.toHex2(), bColumnX, cRowY + TEXT_PADDING, scale, bColor, CHAR_SPACING, FONT_SCALE)

            drawRect(
                color = Color(colorValue),
                topLeft = Offset((swatchX * scale).toFloat(), (cRowY * scale).toFloat()),
                size = Size((swatchW * scale).toFloat(), (ROW_HEIGHT * scale).toFloat())
            )
        }
    }
}
