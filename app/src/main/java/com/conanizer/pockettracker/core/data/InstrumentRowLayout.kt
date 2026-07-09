package com.conanizer.pockettracker.core.data

/**
 * Row geometry of the INSTRUMENT screen, per instrument type — the ONE table the cursor
 * movement in TrackerController walks (row stepping, spacer skips, column snapping,
 * LEFT/RIGHT column stepping all derive from it).
 *
 * The drawn layout lives in InstrumentModule (ui), which this table must mirror: adding,
 * removing, or moving a row there means editing the matching array here — a single edit.
 * (Previously the same geometry was re-encoded in four movement sites as spacer-skip
 * literals and row sets; one miss stranded the cursor on a spacer or off-screen.)
 *
 * Column shape per kind:
 *   NAME   — columns 1..3, LEFT/RIGHT step by 1 (name field + preset buttons).
 *   TRIPLE — columns 1/3/5, LEFT/RIGHT step by 2.
 *   DUAL   — columns 1/3, LEFT/RIGHT jump between them.
 *   SOURCE — LOAD (2) / EDIT (3); cursor snaps to 2 on entry. SF caps at 2 — SoundFonts
 *            have no editable waveform, so there is no EDIT button.
 *   SINGLE — column 1 only.
 *   SPACER — not selectable; vertical movement skips it.
 */
enum class InstrumentRowKind { NAME, TRIPLE, DUAL, SOURCE, SINGLE, SPACER }

object InstrumentRowLayout {

    val SAMPLER: Array<InstrumentRowKind> = arrayOf(
        InstrumentRowKind.NAME,    // 0  NAME + preset LOAD/SAVE
        InstrumentRowKind.SINGLE,  // 1
        InstrumentRowKind.TRIPLE,  // 2  ROOT + DET + TIC
        InstrumentRowKind.TRIPLE,  // 3  VOL + SLICE + PAN
        InstrumentRowKind.SPACER,  // 4
        InstrumentRowKind.SOURCE,  // 5  sample LOAD / EDIT
        InstrumentRowKind.SPACER,  // 6
        InstrumentRowKind.DUAL,    // 7  DRIVE + FILTER
        InstrumentRowKind.DUAL,    // 8  CRUSH + FREQ
        InstrumentRowKind.DUAL,    // 9  DWNSMPL + RES
        InstrumentRowKind.SPACER,  // 10
        InstrumentRowKind.DUAL,    // 11
        InstrumentRowKind.SINGLE,  // 12
        InstrumentRowKind.DUAL,    // 13
        InstrumentRowKind.DUAL,    // 14
        InstrumentRowKind.DUAL     // 15
    )

    val SOUNDFONT: Array<InstrumentRowKind> = arrayOf(
        InstrumentRowKind.NAME,    // 0  NAME + preset LOAD/SAVE
        InstrumentRowKind.SINGLE,  // 1
        InstrumentRowKind.TRIPLE,  // 2  ROOT + DET + TIC
        InstrumentRowKind.DUAL,    // 3
        InstrumentRowKind.SPACER,  // 4
        InstrumentRowKind.SOURCE,  // 5  SF2 LOAD (no EDIT)
        InstrumentRowKind.SINGLE,  // 6
        InstrumentRowKind.SPACER,  // 7
        InstrumentRowKind.DUAL,    // 8  DRIVE + FILTER
        InstrumentRowKind.DUAL,    // 9  CRUSH + FREQ
        InstrumentRowKind.DUAL,    // 10 DWNSMPL + RES
        InstrumentRowKind.SPACER,  // 11
        InstrumentRowKind.SINGLE,  // 12
        InstrumentRowKind.SINGLE,  // 13
        InstrumentRowKind.SINGLE   // 14
    )

    fun rows(isSoundFont: Boolean): Array<InstrumentRowKind> =
        if (isSoundFont) SOUNDFONT else SAMPLER
}
