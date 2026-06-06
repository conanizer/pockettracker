package com.conanizer.pockettracker

/**
 * EQ EDITOR — state types
 *
 * Identifies which EQ slot reference opened the editor, so B+←→ preset cycling
 * can update the correct field in Project.
 */
sealed class EqCallerContext {
    object MasterEq      : EqCallerContext()
    object ReverbInputEq : EqCallerContext()
    object DelayInputEq  : EqCallerContext()
    data class InstrumentEq(val instrId: Int) : EqCallerContext()
    object SampleEditorFx : EqCallerContext()
}

val EQ_BAND_TYPE_NAMES = listOf("OFF", "LOSHELF", "LOWCUT", "BELL", "HISHELF", "HICUT")

data class EqEditorState(
    val isOpen:        Boolean         = false,
    val slotIndex:     Int             = 0,      // 0-127 EQ preset slot being edited
    val cursorRow:     Int             = 0,      // 0-11 (band*4 + param index)
    val callerContext: EqCallerContext = EqCallerContext.MasterEq
)

val EqEditorState.cursorBand:  Int get() = cursorRow / 4
val EqEditorState.cursorParam: Int get() = cursorRow % 4
