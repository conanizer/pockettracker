package com.conanizer.pockettracker.ui

import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.drawscope.DrawScope
import com.conanizer.pockettracker.core.logic.EffectProcessor
import com.conanizer.pockettracker.core.data.Chain
import com.conanizer.pockettracker.core.data.PhraseStep
import com.conanizer.pockettracker.core.data.Track
import com.conanizer.pockettracker.ui.theme.AppTheme

// ═══════════════════════════════════════════════════════════════════════════
// SHARED RENDERING CONSTANTS
// ═══════════════════════════════════════════════════════════════════════════

const val FONT_SCALE = 3      // 5×5 bitmap scaled 3× = 15×15px
const val CHAR_SPACING = 2    // 2px between characters
const val ROW_HEIGHT = 21     // Each row is 21px tall (FONT_SCALE*5 + CHAR_SPACING*2 + 4)
const val TEXT_PADDING = 3    // 3px padding above/below text

// ═══════════════════════════════════════════════════════════════════════════
// HEX FORMATTING
// ═══════════════════════════════════════════════════════════════════════════

// Precomputed hex strings. toHex2() is called for every visible hex cell every frame (~100+ cells
// at 60 fps); allocating 3 short-lived strings per call (toString/uppercase/padStart) was tens of
// thousands of garbage strings/sec — the documented GC-pressure vector that has crashed the
// RenderThread on Snapdragon drivers (see PixelPerfectRenderer). Caching makes both allocation-free.
private val HEX2_CACHE: Array<String> = Array(256) { it.toString(16).uppercase().padStart(2, '0') }
private val HEX1_CACHE: Array<String> = Array(16) { it.toString(16).uppercase() }

/** Format as 2-digit uppercase hex (e.g. 255 → "FF"). Masks to lower 8 bits. */
fun Int.toHex2(): String = HEX2_CACHE[this and 0xFF]

/** Format as 1-digit uppercase hex (e.g. 15 → "F"). Masks to lower 4 bits. */
fun Int.toHex1(): String = HEX1_CACHE[this and 0x0F]

/** Format as 8-digit uppercase hex (e.g. 65536L → "00010000"). */
fun Long.toHex8(): String = this.toString(16).uppercase().padStart(8, '0')

/** Multiply RGB channels by [factor] (0..1 = darker, 1+ = brighter). Alpha is preserved. */
fun Long.darken(factor: Float): Long {
    val a = (this shr 24) and 0xFFL
    val r = (((this shr 16) and 0xFFL) * factor).toLong().coerceIn(0, 255)
    val g = (((this shr 8) and 0xFFL) * factor).toLong().coerceIn(0, 255)
    val b = ((this and 0xFFL) * factor).toLong().coerceIn(0, 255)
    return (a shl 24) or (r shl 16) or (g shl 8) or b
}

// ═══════════════════════════════════════════════════════════════════════════
// ROW BACKGROUND COLOR
// ═══════════════════════════════════════════════════════════════════════════

/**
 * Standard row background color for all editor screens (Phrase, Chain, Song, Table).
 *
 * Priority: playing > selected > cursor > every-4th-row accent > default
 */

fun rowBgColor(
    index: Int,
    cursorRow: Int,
    playbackRow: Int,
    isPlaying: Boolean,
    isSelected: Boolean,
    theme: AppTheme
): Color = when {
    isPlaying && index == playbackRow -> Color(theme.rowPlayback)
    isSelected                        -> Color(theme.rowSelection)
    index == cursorRow                -> Color(theme.rowCursor)
    index % 4 == 0                    -> Color(theme.rowEvery4th)
    else                              -> Color(theme.background)
}

fun clearEffect(step: PhraseStep, fxSlot: Int) = step.setFx(fxSlot, 0x00, 0x00)

/**
 * Get effect type 3-letter name for display. Thin UI alias for [EffectProcessor.effectName] — the
 * code↔name map lives in core (keyed off the FX_* constants) so it can't drift from the effect codes.
 */
fun getEffectTypeName(effectType: Int): String = EffectProcessor.effectName(effectType)



fun clearChainSlot(chain: Chain, row: Int) {
    chain.phraseRefs[row] = -1  // -1 = empty
    chain.transposeValues[row] = 0x00  // Reset transpose to default
}

fun clearSongChainRef(track: Track, row: Int) {
    if (row < track.chainRefs.size) {
        track.chainRefs[row] = -1
    }
}

/**
 * Get track index from cursor column (1-8 → 0-7)
 */
fun getTrackIndex(cursorColumn: Int): Int {
    return (cursorColumn - 1).coerceIn(0, 7)
}

// ═══════════════════════════════════════════════════════════════════════════
// GRID CELL  (shared by the row editors: phrase, chain, song, table)
// ═══════════════════════════════════════════════════════════════════════════

/**
 * One value cell in an editor grid row, with the standard colour priority:
 * cursor > selection > empty > [valueColor]. Collapses the per-cell `when` blocks
 * that every editor used to inline.
 *
 * @param valueColor the cell's "normal" colour (varies per column: values, params, FX names).
 */
fun DrawScope.drawCell(
    text: String, x: Int, textY: Int, scale: Int,
    isCursor: Boolean, isSelected: Boolean, isEmpty: Boolean,
    valueColor: Long, t: AppTheme
) {
    val color = when {
        isCursor   -> Color(t.textCursor)
        isSelected -> Color(t.vizWave)
        isEmpty    -> Color(t.textEmpty)
        else       -> Color(valueColor)
    }
    drawBitmapText(text, x, textY, scale, color, CHAR_SPACING, FONT_SCALE)
}

// ═══════════════════════════════════════════════════════════════════════════
// EQ CELL  (shared by instrument, instrument pool, mixer, effects, sample editor)
// ═══════════════════════════════════════════════════════════════════════════

/**
 * Draws an EQ slot value ("--" when unassigned, hex when set) plus a trailing ">" affordance that
 * signals the cell opens the EQ editor. Used by every EQ cell so they all look identical.
 *  - value: textEmpty when "--", textValue when set, textCursor when focused.
 *  - ">":   textCursor when focused, else textValue (never dims with the value) — drawn right after
 *           the 2-char value.
 *
 * @param valueX    left x of the value; the ">" follows 2 chars later.
 * @param isCursor  true when the cursor is on this EQ cell (highlights value + arrow).
 * @param showArrow false hides the ">" (the instrument pool hides it on its non-selected rows).
 */
fun DrawScope.drawEqCell(
    valueX: Int, textY: Int, scale: Int,
    eqSlot: Int, isCursor: Boolean, t: AppTheme, showArrow: Boolean = true
) {
    val eqStr = if (eqSlot < 0) "--" else eqSlot.toHex2()
    val valueColor = when {
        isCursor   -> Color(t.textCursor)
        eqSlot < 0 -> Color(t.textEmpty)
        else       -> Color(t.textValue)
    }
    drawBitmapText(eqStr, valueX, textY, scale, valueColor, CHAR_SPACING, FONT_SCALE)
    if (showArrow) {
        drawBitmapText(">", valueX + 2 * (5 * FONT_SCALE + CHAR_SPACING), textY, scale,
            if (isCursor) Color(t.textCursor) else Color(t.textValue), CHAR_SPACING, FONT_SCALE)
    }
}

