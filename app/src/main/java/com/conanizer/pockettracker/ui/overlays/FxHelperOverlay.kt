package com.conanizer.pockettracker.ui.overlays

import com.conanizer.pockettracker.core.logic.EffectProcessor

/**
 * FX HELPER OVERLAY
 *
 * A modal grid-picker for effect types, shown when the user holds A and presses
 * UP or DOWN while the cursor is on an FX type column in the PHRASE or TABLE screen.
 *
 * Controls (while open):
 *   A + DPAD UP/DOWN     — navigate rows in the 5×4 effect grid
 *   A + DPAD LEFT/RIGHT  — navigate columns in the 5×4 effect grid
 *   Release A            — confirm selected effect and close
 */

// ============================================================================
// EFFECT DESCRIPTIONS
// ============================================================================

/**
 * Description lines for each effect, indexed to match EffectProcessor.EFFECT_TYPES.
 *
 * Format per entry (2–4 lines):
 *   [0] = "SHORT_NAME: brief description"
 *   [1] = "what value (or 1st nibble) does"
 *   [2] = "what 2nd nibble does" (optional)
 *   [3] = "table-specific behavior" (optional)
 *
 * Max line length: ~31 chars to fit within the 580px overlay at font scale 3.
 */
val EFFECT_DESCRIPTIONS: List<List<String>> = listOf(
    /* 00  ---  NONE         */ listOf("---: No effect",                  "Empty FX slot"),
    /* 01  ARC  ARP CONFIG   */ listOf("ARC: Arpeggio config",            "x=mode(0=UP 1=DN 2=PP 3=RND)", "y=speed in ticks"),
    /* 02  CHA  CHANCE       */ listOf("CHA: Probability gate",           "x=prob(0=never F=always 8=50%)", "y=target(0=note 1-3=FX slot)"),
    /* 03  DEL  DELAY        */ listOf("DEL: Delay note trigger",         "xx=ticks before note fires"),
    /* 04  GRV  GROOVE       */ listOf("GRV: Groove assign",              "xx=groove ID (00=disable)"),
    /* 05  HOP  JUMP         */ listOf("HOP: Phrase/table jump",          "y=target row (FF=stop track)", "table: x=repeat count"),
    /* 06  TIC  TICK RATE    */ listOf("TIC: Table tick rate",            "01-FB=ticks per row", "FC-FF=special modes"),
    /* 07  ARP  ARPEGGIO     */ listOf("ARP: Arpeggio",                   "x=+semitones 1st note", "y=+semitones 2nd note", "configure speed with ARC"),
    /* 08  KIL  KILL         */ listOf("KIL: Kill voice",                 "00: stops sample immediately"),
    /* 09  OFF  OFFSET       */ listOf("OFF: Sample offset",              "xx=start point (00-FF)"),
    /* 10  RND  RANDOMIZE    */ listOf("RND: Randomize FX",               "randomizes previous FX column", "x=min nibble  y=max nibble"),
    /* 11  RNL  RAND LEFT    */ listOf("RNL: Randomize left FX",          "same as RND but targets", "FX column to the left"),
    /* 12  RPT  REPEAT       */ listOf("RPT: Retrigger",                  "RX0: retrig every x ticks", "RXY(Y!=0): retrig y+vol ramp x"),
    /* 13  TBL  TABLE        */ listOf("TBL: Table override",             "xx=table ID for this note"),
    /* 14  THO  TABLE HOP    */ listOf("THO: Table hop",                  "xx=target row in current table"),
    /* 15  VOL  VOLUME       */ listOf("VOL: Volume",                     "xx=volume (00=silent FF=max)"),
    /* 16  PSL  PITCH SLIDE  */ listOf("PSL: Pitch slide",                "xx=duration(01=fast FF=slow)", "slides pitch from previous note"),
    /* 17  PBN  PITCH BEND   */ listOf("PBN: Pitch bend",                 "01-7F=bend up  80-FF=bend down", "00=stop bending"),
    /* 18  PVB  VIBRATO      */ listOf("PVB: Vibrato",                    "x=speed(0-F Hz)", "y=depth(0-F in 1/8 semitone)"),
    /* 19  PVX  EXT VIBRATO  */ listOf("PVX: Extreme vibrato",            "4x depth and 2x speed", "same format as PVB"),
    /* 20  PIT  PITCH OFFSET */ listOf("PIT: Pitch semitone offset",      "00-7F=+0..+127 semitones", "80-FF=-128..-1 semitones", "does not affect slice index"),
    /* 21  SLI  SLICE INDEX  */ listOf("SLI: Slice index override",       "xx=slice index (00-FF)", "works even when SLICE=OFF")
)

// ============================================================================
// STATE
// ============================================================================

private const val FX_GRID_COLS = 6
private const val FX_GRID_ROWS = 4   // 22 effects total → 6 × 4 (last 2 cells empty)

/**
 * All state needed to display and operate the FX helper overlay.
 *
 * @param isOpen      Whether the overlay is visible
 * @param cursorRow   Row in the 6×4 effect grid (0–3)
 * @param cursorCol   Column in the 6×4 effect grid (0–5)
 */
data class FxHelperState(
    val isOpen: Boolean = false,
    val cursorRow: Int = 0,
    val cursorCol: Int = 0
) {
    /** Linear index into EffectProcessor.EFFECT_TYPES for the highlighted effect. */
    val cursorIndex: Int get() = cursorRow * FX_GRID_COLS + cursorCol
}

// ============================================================================
// NAVIGATION
// ============================================================================

fun FxHelperState.fxMoveCursorUp(): FxHelperState =
    copy(cursorRow = (cursorRow - 1 + FX_GRID_ROWS) % FX_GRID_ROWS)

fun FxHelperState.fxMoveCursorDown(): FxHelperState =
    copy(cursorRow = (cursorRow + 1) % FX_GRID_ROWS)

fun FxHelperState.fxMoveCursorLeft(): FxHelperState =
    copy(cursorCol = if (cursorCol == 0) FX_GRID_COLS - 1 else cursorCol - 1)

fun FxHelperState.fxMoveCursorRight(): FxHelperState =
    copy(cursorCol = (cursorCol + 1) % FX_GRID_COLS)

/** Effect type code (from EffectProcessor) for the currently highlighted cell. */
fun FxHelperState.selectedEffectCode(): Int =
    EffectProcessor.EFFECT_TYPES.getOrElse(cursorIndex) { EffectProcessor.FX_NONE }

/** Description lines for the currently highlighted effect. */
fun FxHelperState.descriptionLines(): List<String> =
    EFFECT_DESCRIPTIONS.getOrElse(cursorIndex) { listOf("---", "No effect") }
