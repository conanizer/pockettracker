package com.conanizer.pockettracker.ui.overlays

import com.conanizer.pockettracker.core.logic.EffectProcessor

/**
 * FX HELPER OVERLAY
 *
 * A modal grid-picker for effect types, shown when the user holds A and presses
 * UP or DOWN while the cursor is on an FX type column in the PHRASE or TABLE screen.
 *
 * Controls (while open):
 *   A + DPAD UP/DOWN     — navigate rows in the 6×5 effect grid
 *   A + DPAD LEFT/RIGHT  — navigate columns in the 6×5 effect grid
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
    /* 03  LAT  LATENCY      */ listOf("LAT: Latency (delay trigger)",    "xx=ticks before note fires"),
    /* 04  GRV  GROOVE       */ listOf("GRV: Groove assign",              "xx=groove ID (00=disable)"),
    /* 05  HOP  JUMP         */ listOf("HOP: Phrase/table jump",          "y=target row (FF=stop track)", "table: x=repeat count"),
    /* 06  TIC  TICK RATE    */ listOf("TIC: Table tick rate",            "01-FB=ticks per row", "FC-FF=special modes"),
    /* 07  ARP  ARPEGGIO     */ listOf("ARP: Arpeggio",                   "x=+semitones 1st note", "y=+semitones 2nd note", "configure speed with ARC"),
    /* 08  KIL  KILL         */ listOf("KIL: Kill voice",                 "xx=ticks of latency before stop", "00=immediate, 0C=next step"),
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
    /* 21  SLI  SLICE INDEX  */ listOf("SLI: Slice index override",       "xx=slice index (00-FF)", "works even when SLICE=OFF"),
    /* 22  PAN  PAN          */ listOf("PAN: Per-note pan",               "00=left 80=center FF=right", "this note only; next reverts"),
    /* 23  BCK  DIRECTION    */ listOf("BCK: Playback direction",         "00=reverse 01=forward", "sampler; toggle live to scratch"),
    /* 24  REV  REVERB SEND  */ listOf("REV: Per-note reverb send",       "xx=send amount (00-FF)", "this note only"),
    /* 25  DEL  DELAY SEND   */ listOf("DEL: Per-note delay send",        "xx=send amount (00-FF)", "this note only"),
    /* 26  EQN  EQ (NOTE)    */ listOf("EQN: Per-note EQ slot",           "xx=EQ preset slot (00-7F)", "this note only"),
    /* 27  EQM  EQ (MIXER)   */ listOf("EQM: Master/mixer EQ slot",       "xx=EQ preset slot (00-7F)", "holds till next EQM", "resets to mixer EQ on stop")
)

// ============================================================================
// STATE
// ============================================================================

const val FX_GRID_COLS = 6
const val FX_GRID_ROWS = 5   // 28 effects → 4 full rows (24) + a centered last row of 4

// The first FX_GRID_ROWS-1 rows are full (6 each); the remaining effects sit on the last row,
// horizontally centered, with the leftover edge cells left empty and UNREACHABLE by the cursor.
// Centering is derived from the real effect count so the layout adapts if effects are added later.
val FX_FULL_CELLS        = (FX_GRID_ROWS - 1) * FX_GRID_COLS                                   // 24
val FX_LAST_ROW_COUNT    = (EffectProcessor.EFFECT_TYPES.size - FX_FULL_CELLS).coerceIn(0, FX_GRID_COLS)  // 4
val FX_LAST_ROW_FIRST_COL = (FX_GRID_COLS - FX_LAST_ROW_COUNT) / 2                              // 1
val FX_LAST_ROW_LAST_COL  = FX_LAST_ROW_FIRST_COL + FX_LAST_ROW_COUNT - 1                       // 4
private val FX_LAST_ROW    = FX_GRID_ROWS - 1                                                   // 4

/** Map a linear effect index to its (row, col) in the grid (last row is centered). */
fun fxIndexToCell(index: Int): Pair<Int, Int> =
    if (index < FX_FULL_CELLS) (index / FX_GRID_COLS) to (index % FX_GRID_COLS)
    else FX_LAST_ROW to (FX_LAST_ROW_FIRST_COL + (index - FX_FULL_CELLS))

/** Clamp a column to the reachable range for [row] (last row excludes the empty edge cells). */
private fun clampColForRow(row: Int, col: Int): Int =
    if (row == FX_LAST_ROW) col.coerceIn(FX_LAST_ROW_FIRST_COL, FX_LAST_ROW_LAST_COL) else col

/**
 * All state needed to display and operate the FX helper overlay.
 *
 * @param isOpen      Whether the overlay is visible
 * @param cursorRow   Row in the 6×5 effect grid (0–4)
 * @param cursorCol   Column in the 6×5 effect grid (0–5; last row only reaches 1–4)
 */
data class FxHelperState(
    val isOpen: Boolean = false,
    val cursorRow: Int = 0,
    val cursorCol: Int = 0
) {
    /** Linear index into EffectProcessor.EFFECT_TYPES for the highlighted effect. */
    val cursorIndex: Int get() =
        if (cursorRow < FX_LAST_ROW) cursorRow * FX_GRID_COLS + cursorCol
        else FX_FULL_CELLS + (cursorCol - FX_LAST_ROW_FIRST_COL)
}

/** Open the overlay with the cursor on the cell currently holding [effectIndex]. */
fun fxHelperOpenedAt(effectIndex: Int): FxHelperState {
    val (row, col) = fxIndexToCell(effectIndex.coerceIn(0, EffectProcessor.EFFECT_TYPES.lastIndex))
    return FxHelperState(isOpen = true, cursorRow = row, cursorCol = col)
}

// ============================================================================
// NAVIGATION
// ============================================================================
// The centered last row is special: its empty edge cells are never landed on. Moving vertically out
// of an edge column (col 0 / col 5) rounds INTO the nearest reachable last-row cell (col 1 / col 4);
// from inside the last row, up/down move straight in the same column (which is 1–4, valid everywhere).

fun FxHelperState.fxMoveCursorUp(): FxHelperState = when (cursorRow) {
    0          -> copy(cursorRow = FX_LAST_ROW, cursorCol = clampColForRow(FX_LAST_ROW, cursorCol)) // wrap to last row
    FX_LAST_ROW -> copy(cursorRow = FX_LAST_ROW - 1)                                                // straight up, same col
    else       -> copy(cursorRow = cursorRow - 1)
}

fun FxHelperState.fxMoveCursorDown(): FxHelperState = when (cursorRow) {
    FX_LAST_ROW - 1 -> copy(cursorRow = FX_LAST_ROW, cursorCol = clampColForRow(FX_LAST_ROW, cursorCol)) // into last row
    FX_LAST_ROW     -> copy(cursorRow = 0)                                                              // wrap to top, same col
    else            -> copy(cursorRow = cursorRow + 1)
}

fun FxHelperState.fxMoveCursorLeft(): FxHelperState =
    if (cursorRow == FX_LAST_ROW)
        copy(cursorCol = if (cursorCol <= FX_LAST_ROW_FIRST_COL) FX_LAST_ROW_LAST_COL else cursorCol - 1)
    else
        copy(cursorCol = if (cursorCol == 0) FX_GRID_COLS - 1 else cursorCol - 1)

fun FxHelperState.fxMoveCursorRight(): FxHelperState =
    if (cursorRow == FX_LAST_ROW)
        copy(cursorCol = if (cursorCol >= FX_LAST_ROW_LAST_COL) FX_LAST_ROW_FIRST_COL else cursorCol + 1)
    else
        copy(cursorCol = (cursorCol + 1) % FX_GRID_COLS)

/** Effect type code (from EffectProcessor) for the currently highlighted cell. */
fun FxHelperState.selectedEffectCode(): Int =
    EffectProcessor.EFFECT_TYPES.getOrElse(cursorIndex) { EffectProcessor.FX_NONE }

/** Description lines for the currently highlighted effect. */
fun FxHelperState.descriptionLines(): List<String> =
    EFFECT_DESCRIPTIONS.getOrElse(cursorIndex) { listOf("---", "No effect") }
