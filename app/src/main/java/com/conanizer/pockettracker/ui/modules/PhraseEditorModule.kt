package com.conanizer.pockettracker.ui.modules

import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.geometry.Size
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.drawscope.DrawScope
import com.conanizer.pockettracker.ui.theme.AppTheme
import com.conanizer.pockettracker.input.CursorContext
import com.conanizer.pockettracker.input.CursorContextFactory
import com.conanizer.pockettracker.ui.TrackerModule
import com.conanizer.pockettracker.ui.clearEffect
import com.conanizer.pockettracker.core.data.Note
import com.conanizer.pockettracker.core.data.Phrase
import com.conanizer.pockettracker.core.data.PhraseStep
import com.conanizer.pockettracker.core.logic.EffectProcessor
import com.conanizer.pockettracker.core.logic.InputAction
import com.conanizer.pockettracker.core.logic.InstrumentController
import com.conanizer.pockettracker.ui.drawBitmapText
import com.conanizer.pockettracker.ui.getEffectTypeName
import com.conanizer.pockettracker.ui.rowBgColor
import com.conanizer.pockettracker.ui.toHex1
import com.conanizer.pockettracker.ui.toHex2

/**
 * PHRASE EDITOR MODULE
 *
 * 16-step phrase with columns: Step | Note | Vol | Inst | FX1 | FX2 | FX3
 *
 * Size: 510×392 px
 */
class PhraseEditorModule : TrackerModule {
    override val width  = 510
    override val height = 392

    private val FONT_SCALE   = 3
    private val CHAR_SPACING = 2
    private val ROW_HEIGHT   = 21
    private val TEXT_PADDING = 3

    override fun DrawScope.draw(x: Int, y: Int, scale: Int, state: Any?) {
        val phraseState = state as? PhraseEditorState ?: return

        drawRect(
            color   = Color(phraseState.appTheme.background),
            topLeft = Offset((x * scale).toFloat(), (y * scale).toFloat()),
            size    = Size((width * scale).toFloat(), (height * scale).toFloat())
        )

        var colX      = x + 10
        val stepX     = colX; colX += 30 + 10
        val noteX     = colX; colX += 45 + 20
        val volX      = colX; colX += 30 + 15
        val instX     = colX; colX += 30 + 15
        val fx1NameX  = colX; colX += 45 + 10
        val fx1ValueX = colX; colX += 30 + 15
        val fx2NameX  = colX; colX += 45 + 10
        val fx2ValueX = colX; colX += 30 + 15
        val fx3NameX  = colX; colX += 45 + 10
        val fx3ValueX = colX

        val t = phraseState.appTheme
        var rowY = y + TEXT_PADDING
        drawBitmapText(
            text = "PHRASE ${phraseState.phrase.id.toHex2()}",
            x = x + 10, y = rowY, scale = scale,
            color = Color(t.textTitle), spacing = CHAR_SPACING, fontScale = FONT_SCALE
        )

        rowY = y + ROW_HEIGHT + 14 + TEXT_PADDING
        drawBitmapText("N",   noteX,    rowY, scale, Color(t.textParam), CHAR_SPACING, FONT_SCALE)
        drawBitmapText("V",   volX,     rowY, scale, Color(t.textParam), CHAR_SPACING, FONT_SCALE)
        drawBitmapText("I",   instX,    rowY, scale, Color(t.textParam), CHAR_SPACING, FONT_SCALE)
        drawBitmapText("FX1", fx1NameX, rowY, scale, Color(t.textParam), CHAR_SPACING, FONT_SCALE)
        drawBitmapText("FX2", fx2NameX, rowY, scale, Color(t.textParam), CHAR_SPACING, FONT_SCALE)
        drawBitmapText("FX3", fx3NameX, rowY, scale, Color(t.textParam), CHAR_SPACING, FONT_SCALE)

        phraseState.phrase.steps.forEachIndexed { index, step ->
            drawPhraseRow(
                x = x, y = y, scale = scale,
                index = index, step = step, state = phraseState,
                stepX = stepX, noteX = noteX, volX = volX, instX = instX,
                fx1NameX = fx1NameX, fx1ValueX = fx1ValueX,
                fx2NameX = fx2NameX, fx2ValueX = fx2ValueX,
                fx3NameX = fx3NameX, fx3ValueX = fx3ValueX
            )
        }
    }

    private fun DrawScope.drawPhraseRow(
        x: Int, y: Int, scale: Int,
        index: Int, step: PhraseStep, state: PhraseEditorState,
        stepX: Int, noteX: Int, volX: Int, instX: Int,
        fx1NameX: Int, fx1ValueX: Int,
        fx2NameX: Int, fx2ValueX: Int,
        fx3NameX: Int, fx3ValueX: Int
    ) {
        val dataRowY = y + ROW_HEIGHT + 14 + ROW_HEIGHT + (index * ROW_HEIGHT)
        val isRowSelected = state.selectionMode && (1..9).any { col -> state.isCellSelected(index, col) }

        val t = state.appTheme
        drawRect(
            color   = rowBgColor(
                index,
                state.cursorRow,
                state.playbackRow,
                state.isPlaying,
                isRowSelected,
                t
            ),
            topLeft = Offset((x * scale).toFloat(), (dataRowY * scale).toFloat()),
            size    = Size((width * scale).toFloat(), (ROW_HEIGHT * scale).toFloat())
        )

        val textY = dataRowY + TEXT_PADDING

        // Quarter-note rows (every 4th) drawn brighter as a beat-accent cue
        drawBitmapText(
            text = index.toHex1(),
            x = stepX, y = textY, scale = scale,
            color = when {
                index == state.cursorRow && state.cursorColumn == 0 -> Color(t.textCursor)
                index % 4 == 0 -> Color(t.textParam)
                else -> Color(t.textEmpty)
            },
            spacing = CHAR_SPACING, fontScale = FONT_SCALE
        )

        drawBitmapText(
            text = step.note.toString(),
            x = noteX, y = textY, scale = scale,
            color = when {
                index == state.cursorRow && state.cursorColumn == 1 -> Color(t.textCursor)
                state.selectionMode && state.isCellSelected(index, 1) -> Color(t.vizWave)
                step.note == Note.EMPTY -> Color(t.textEmpty)
                else -> Color(t.textValue)
            },
            spacing = CHAR_SPACING, fontScale = FONT_SCALE
        )

        drawBitmapText(
            text = step.volume.toHex2(),
            x = volX, y = textY, scale = scale,
            color = when {
                index == state.cursorRow && state.cursorColumn == 2 -> Color(t.textCursor)
                state.selectionMode && state.isCellSelected(index, 2) -> Color(t.vizWave)
                step.note == Note.EMPTY -> Color(t.textEmpty)
                else -> Color(t.textParam)
            },
            spacing = CHAR_SPACING, fontScale = FONT_SCALE
        )

        drawBitmapText(
            text = step.instrument.toHex2(),
            x = instX, y = textY, scale = scale,
            color = when {
                index == state.cursorRow && state.cursorColumn == 3 -> Color(t.textCursor)
                state.selectionMode && state.isCellSelected(index, 3) -> Color(t.vizWave)
                step.note == Note.EMPTY -> Color(t.textEmpty)
                else -> Color(t.textParam)
            },
            spacing = CHAR_SPACING, fontScale = FONT_SCALE
        )

        drawBitmapText(
            text = getEffectTypeName(step.fx1Type),
            x = fx1NameX, y = textY, scale = scale,
            color = when {
                index == state.cursorRow && state.cursorColumn == 4 -> Color(t.textCursor)
                state.selectionMode && state.isCellSelected(index, 4) -> Color(t.vizWave)
                step.fx1Type == 0x00 -> Color(t.textEmpty)
                else -> Color(t.textTitle)
            },
            spacing = CHAR_SPACING, fontScale = FONT_SCALE
        )

        drawBitmapText(
            text = step.fx1Value.toHex2(),
            x = fx1ValueX, y = textY, scale = scale,
            color = when {
                index == state.cursorRow && state.cursorColumn == 5 -> Color(t.textCursor)
                state.selectionMode && state.isCellSelected(index, 5) -> Color(t.vizWave)
                step.fx1Type == 0x00 -> Color(t.textEmpty)
                else -> Color(t.textParam)
            },
            spacing = CHAR_SPACING, fontScale = FONT_SCALE
        )

        drawBitmapText(
            text = getEffectTypeName(step.fx2Type),
            x = fx2NameX, y = textY, scale = scale,
            color = when {
                index == state.cursorRow && state.cursorColumn == 6 -> Color(t.textCursor)
                state.selectionMode && state.isCellSelected(index, 6) -> Color(t.vizWave)
                step.fx2Type == 0x00 -> Color(t.textEmpty)
                else -> Color(t.textTitle)
            },
            spacing = CHAR_SPACING, fontScale = FONT_SCALE
        )

        drawBitmapText(
            text = step.fx2Value.toHex2(),
            x = fx2ValueX, y = textY, scale = scale,
            color = when {
                index == state.cursorRow && state.cursorColumn == 7 -> Color(t.textCursor)
                state.selectionMode && state.isCellSelected(index, 7) -> Color(t.vizWave)
                step.fx2Type == 0x00 -> Color(t.textEmpty)
                else -> Color(t.textParam)
            },
            spacing = CHAR_SPACING, fontScale = FONT_SCALE
        )

        drawBitmapText(
            text = getEffectTypeName(step.fx3Type),
            x = fx3NameX, y = textY, scale = scale,
            color = when {
                index == state.cursorRow && state.cursorColumn == 8 -> Color(t.textCursor)
                state.selectionMode && state.isCellSelected(index, 8) -> Color(t.vizWave)
                step.fx3Type == 0x00 -> Color(t.textEmpty)
                else -> Color(t.textTitle)
            },
            spacing = CHAR_SPACING, fontScale = FONT_SCALE
        )

        drawBitmapText(
            text = step.fx3Value.toHex2(),
            x = fx3ValueX, y = textY, scale = scale,
            color = when {
                index == state.cursorRow && state.cursorColumn == 9 -> Color(t.textCursor)
                state.selectionMode && state.isCellSelected(index, 9) -> Color(t.vizWave)
                step.fx3Type == 0x00 -> Color(t.textEmpty)
                else -> Color(t.textParam)
            },
            spacing = CHAR_SPACING, fontScale = FONT_SCALE
        )
    }

    fun getCursorContext(state: PhraseEditorState): CursorContext {
        val step = state.phrase.steps[state.cursorRow]
        return when (state.cursorColumn) {
            0 -> CursorContextFactory.readOnly()
            1 -> {
                val isEmpty = step.note == Note.EMPTY
                CursorContextFactory.note(if (isEmpty) 0 else step.note.toMidi(), isEmpty)
            }
            2 -> CursorContextFactory.volume(step.volume)
            3 -> CursorContextFactory.instrument(step.instrument)
            4 -> CursorContextFactory.effectType(step.fx1Type, 1)
            5 -> CursorContextFactory.effectValue(step.fx1Value, 1, EffectProcessor.effectValueMax(step.fx1Type))
            6 -> CursorContextFactory.effectType(step.fx2Type, 2)
            7 -> CursorContextFactory.effectValue(step.fx2Value, 2, EffectProcessor.effectValueMax(step.fx2Type))
            8 -> CursorContextFactory.effectType(step.fx3Type, 3)
            9 -> CursorContextFactory.effectValue(step.fx3Value, 3, EffectProcessor.effectValueMax(step.fx3Type))
            else -> CursorContextFactory.none()
        }
    }

    fun handleInput(
        state: PhraseEditorState,
        action: InputAction,
        instrumentController: InstrumentController
    ): InputResult {
        val step = state.phrase.steps[state.cursorRow]
        var lastEditedNote: Note? = null
        var lastEditedVolume: Int? = null
        var lastEditedInstrument: Int? = null

        when (action) {
            is InputAction.SET_VALUE -> {
                when (state.cursorColumn) {
                    1 -> { step.note = Note.fromMidi(action.value); lastEditedNote = step.note }
                    2 -> { step.volume = action.value; lastEditedVolume = action.value }
                    3 -> {
                        step.instrument = action.value
                        instrumentController.lastEditedInstrument = action.value
                        lastEditedInstrument = action.value
                    }
                    // Effect type columns store an index into EFFECT_TYPES; convert back to the effect code
                    4 -> step.fx1Type = EffectProcessor.EFFECT_TYPES.getOrElse(action.value) { EffectProcessor.FX_NONE }
                    5 -> step.fx1Value = action.value
                    6 -> step.fx2Type = EffectProcessor.EFFECT_TYPES.getOrElse(action.value) { EffectProcessor.FX_NONE }
                    7 -> step.fx2Value = action.value
                    8 -> step.fx3Type = EffectProcessor.EFFECT_TYPES.getOrElse(action.value) { EffectProcessor.FX_NONE }
                    9 -> step.fx3Value = action.value
                }
            }
            is InputAction.DELETE -> {
                when (state.cursorColumn) {
                    1 -> step.note = Note.EMPTY
                    4, 6, 8 -> {
                        val fxSlot = when (state.cursorColumn) { 4 -> 1; 6 -> 2; else -> 3 }
                        clearEffect(step, fxSlot)
                    }
                }
            }
            is InputAction.INSERT_DEFAULT -> {
                if (state.cursorColumn == 1) {
                    step.note = Note.fromString("C-4")
                    lastEditedNote = step.note
                }
            }
            else -> {}
        }

        return InputResult(
            modified = action !is InputAction.NONE,
            lastEditedNote = lastEditedNote,
            lastEditedVolume = lastEditedVolume,
            lastEditedInstrument = lastEditedInstrument
        )
    }

    data class InputResult(
        val modified: Boolean,
        val lastEditedNote: Note? = null,
        val lastEditedVolume: Int? = null,
        val lastEditedInstrument: Int? = null
    )
}

data class PhraseEditorState(
    val phrase: Phrase,
    val cursorRow: Int,
    val cursorColumn: Int,
    val playbackRow: Int,
    val isPlaying: Boolean,
    val selectionMode: Boolean = false,
    val isCellSelected: (Int, Int) -> Boolean = { _, _ -> false },
    val appTheme: AppTheme = AppTheme.Companion.CLASSIC
)
