package com.conanizer.pockettracker

import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.geometry.Size
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.drawscope.DrawScope

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
        const val MAX_ROW = 16  // last row index (color row 16)

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
            ColorRow("MTR LOW",    { it.meterLow },      { t, v -> t.copy(meterLow      = v) }),
            ColorRow("MTR MID",    { it.meterMid },      { t, v -> t.copy(meterMid      = v) }),
            ColorRow("MTR HIGH",   { it.meterHigh },     { t, v -> t.copy(meterHigh     = v) })
        )
    }

    private val FONT_SCALE = 3
    private val CHAR_SPACING = 2
    private val ROW_HEIGHT = 21
    private val TEXT_PADDING = 3

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

        var rowY = y + TEXT_PADDING
        drawBitmapText(
            text = "THEME EDIT", x = nameColumnX, y = rowY, scale = scale,
            color = Color(theme.textTitle), spacing = CHAR_SPACING, fontScale = FONT_SCALE
        )
        rowY += ROW_HEIGHT + 14

        // ── ROW 0: THEME (builtin cycle + SAVE/LOAD) ──────────────────
        val isCursorTheme = es.cursorRow == 0
        if (isCursorTheme) {
            drawRect(
                color = Color(theme.rowCursor),
                topLeft = Offset((x * scale).toFloat(), (rowY * scale).toFloat()),
                size = Size((width * scale).toFloat(), (ROW_HEIGHT * scale).toFloat())
            )
        }
        drawBitmapText(
            text = "THEME", x = nameColumnX, y = rowY + TEXT_PADDING, scale = scale,
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
            text = theme.name, x = themeNameX, y = rowY + TEXT_PADDING, scale = scale,
            color = nameColor, spacing = CHAR_SPACING, fontScale = FONT_SCALE
        )
        // SAVE button (position ch=1)
        val saveColor = when {
            isCursorTheme && es.cursorChannel == 1 -> Color(theme.textCursor)
            isCursorTheme -> Color(theme.textValue)
            else -> Color(theme.textParam)
        }
        drawBitmapText(
            text = "SAVE", x = saveLabelX, y = rowY + TEXT_PADDING, scale = scale,
            color = saveColor, spacing = CHAR_SPACING, fontScale = FONT_SCALE
        )
        // LOAD button (position ch=2)
        val loadColor = when {
            isCursorTheme && es.cursorChannel == 2 -> Color(theme.textCursor)
            isCursorTheme -> Color(theme.textValue)
            else -> Color(theme.textParam)
        }
        drawBitmapText(
            text = "LOAD", x = loadLabelX, y = rowY + TEXT_PADDING, scale = scale,
            color = loadColor, spacing = CHAR_SPACING, fontScale = FONT_SCALE
        )
        rowY += ROW_HEIGHT

        // ── ROWS 1-16: Color parameters ────────────────────────────────
        for (i in COLOR_ROWS.indices) {
            val colorRow = COLOR_ROWS[i]
            val colorValue = colorRow.get(theme)
            val isCursor = es.cursorRow == i + 1  // cursorRow 1 = COLOR_ROWS[0], etc.

            if (isCursor) {
                drawRect(
                    color = Color(theme.rowCursor),
                    topLeft = Offset((x * scale).toFloat(), (rowY * scale).toFloat()),
                    size = Size((width * scale).toFloat(), (ROW_HEIGHT * scale).toFloat())
                )
            }

            drawBitmapText(
                text = colorRow.label, x = nameColumnX, y = rowY + TEXT_PADDING, scale = scale,
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

            drawBitmapText(r.toHex2(), rColumnX, rowY + TEXT_PADDING, scale, rColor, CHAR_SPACING, FONT_SCALE)
            drawBitmapText(g.toHex2(), gColumnX, rowY + TEXT_PADDING, scale, gColor, CHAR_SPACING, FONT_SCALE)
            drawBitmapText(b.toHex2(), bColumnX, rowY + TEXT_PADDING, scale, bColor, CHAR_SPACING, FONT_SCALE)

            drawRect(
                color = Color(colorValue),
                topLeft = Offset((swatchX * scale).toFloat(), (rowY * scale).toFloat()),
                size = Size((swatchW * scale).toFloat(), (ROW_HEIGHT * scale).toFloat())
            )

            rowY += ROW_HEIGHT
        }
    }
}
