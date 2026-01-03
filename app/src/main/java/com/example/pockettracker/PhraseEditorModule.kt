package com.example.pockettracker

import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.geometry.Size
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.drawscope.DrawScope


/**
 * PHRASE EDITOR MODULE
 *
 * Displays 16-step phrase with columns:
 * Step | Note | Vol | Inst | FX1 | FX2
 *
 * Size: 620×392 pixels
 * State type: PhraseEditorState
 */
class PhraseEditorModule : TrackerModule {
    override val width = 510
    override val height = 392

    // Font constants
    private val FONT_SCALE = 3      // 5×5 bitmap scaled 3× = 15×15
    private val CHAR_SPACING = 2    // 2px between characters
    private val ROW_HEIGHT = 21     // Each row is 21px tall
    private val TEXT_PADDING = 3    // 3px padding above/below text

    override fun DrawScope.draw(x: Int, y: Int, scale: Int, state: Any?) {
        val phraseState = state as? PhraseEditorState ?: return

        // Module background
        drawRect(
            color = Color(0xFF0a0a0a),
            topLeft = Offset((x * scale).toFloat(), (y * scale).toFloat()),
            size = Size((width * scale).toFloat(), (height * scale).toFloat())
        )

        // Calculate column X positions (tight spacing, equal gaps)
        var colX = x + 10  // Start with 10px left padding
        val stepX = colX; colX += 30 + 10      // Step (2 chars)
        val noteX = colX; colX += 45 + 20      // Note (3 chars)
        val volX = colX; colX += 30 + 15       // Volume (2 chars)
        val instX = colX; colX += 45 + 15      // Instrument (2 chars)
        val fx1X = colX; colX += 75 + 20       // FX1 (5 chars)
        val fx2X = colX; colX += 75 + 20       // FX2 (5 chars)
        val fx3X = colX;

        // ROW 0: HEADER "PHRASE 00"
        var rowY = y + TEXT_PADDING
        drawBitmapText(
            text = "PHRASE ${phraseState.phrase.id.toString(16).padStart(2, '0').uppercase()}",

            x = x + 10,
            y = rowY,
            scale = scale,
            color = Color.Cyan,
            spacing = CHAR_SPACING,
            fontScale = FONT_SCALE
        )

        // 14px SPACER after header
        rowY = y + ROW_HEIGHT + 14 + TEXT_PADDING

        // ROW 1: COLUMN HEADERS
        drawBitmapText("N", noteX, rowY, scale, Color.Gray, CHAR_SPACING, FONT_SCALE)
        drawBitmapText("V", volX, rowY, scale, Color.Gray, CHAR_SPACING, FONT_SCALE)
        drawBitmapText("I", instX, rowY, scale, Color.Gray, CHAR_SPACING, FONT_SCALE)
        drawBitmapText("FX1", fx1X, rowY, scale, Color.Gray, CHAR_SPACING, FONT_SCALE)
        drawBitmapText("FX2", fx2X, rowY, scale, Color.Gray, CHAR_SPACING, FONT_SCALE)
        drawBitmapText("FX3", fx3X, rowY, scale, Color.Gray, CHAR_SPACING, FONT_SCALE)

        // ROWS 2-17: 16 DATA ROWS
        phraseState.phrase.steps.forEachIndexed { index, step ->
            drawPhraseRow(
                x = x,
                y = y,
                scale = scale,
                index = index,
                step = step,
                state = phraseState,
                stepX = stepX,
                noteX = noteX,
                volX = volX,
                instX = instX,
                fx1X = fx1X,
                fx2X = fx2X,
                fx3X = fx3X
            )
        }
    }

    /**
     * Draw a single phrase row (extracted for clarity)
     */
    private fun DrawScope.drawPhraseRow(
        x: Int,
        y: Int,
        scale: Int,
        index: Int,
        step: PhraseStep,
        state: PhraseEditorState,
        stepX: Int,
        noteX: Int,
        volX: Int,
        instX: Int,
        fx1X: Int,
        fx2X: Int,
        fx3X: Int
    ) {
        // Calculate row Y position
        val dataRowY = y + ROW_HEIGHT + 14 + ROW_HEIGHT + (index * ROW_HEIGHT)

        // Row background color logic
        val bgColor = when {
            state.isPlaying && index == state.playbackRow -> Color(0xFF004400)  // Green playing
            index == state.cursorRow -> Color(0xFF333333)                        // Gray cursor
            index % 4 == 0 -> Color(0xFF151515)                                  // Lighter every 4
            else -> Color(0xFF0a0a0a)                                            // Default dark
        }

        // Draw row background
        drawRect(
            color = bgColor,
            topLeft = Offset((x * scale).toFloat(), (dataRowY * scale).toFloat()),
            size = Size((width * scale).toFloat(), (ROW_HEIGHT * scale).toFloat())
        )

        val textY = dataRowY + TEXT_PADDING

        // COLUMN 0: STEP NUMBER
        drawBitmapText(
            text = index.toString(16).uppercase(),
            x = stepX,
            y = textY,
            scale = scale,
            color = if (index == state.cursorRow && state.cursorColumn == 0)
                Color.Yellow else Color(0xFF666666),
            spacing = CHAR_SPACING,
            fontScale = FONT_SCALE
        )

        // COLUMN 1: NOTE
        drawBitmapText(
            text = step.note.toString(),
            x = noteX,
            y = textY,
            scale = scale,
            color = when {
                index == state.cursorRow && state.cursorColumn == 1 -> Color.Yellow
                step.note == Note.EMPTY -> Color(0xFF444444)
                else -> Color.White
            },
            spacing = CHAR_SPACING,
            fontScale = FONT_SCALE
        )

        // COLUMN 2: VOLUME
        drawBitmapText(
            text = step.volume.toString(16).padStart(2, '0').uppercase(),
            x = volX,
            y = textY,
            scale = scale,
            color = when {
                index == state.cursorRow && state.cursorColumn == 2 -> Color.Yellow
                step.note == Note.EMPTY -> Color(0xFF444444)
                else -> Color(0xFFaaaaaa)
            },
            spacing = CHAR_SPACING,
            fontScale = FONT_SCALE
        )

        // COLUMN 3: INSTRUMENT
        drawBitmapText(
            text = step.instrument.toString(16).padStart(2, '0').uppercase(),
            x = instX,
            y = textY,
            scale = scale,
            color = when {
                index == state.cursorRow && state.cursorColumn == 3 -> Color.Yellow
                step.note == Note.EMPTY -> Color(0xFF444444)
                else -> Color(0xFFaaaaaa)
            },
            spacing = CHAR_SPACING,
            fontScale = FONT_SCALE
        )

        // COLUMN 4: FX1 (mockup: ---00)
        drawBitmapText(
            text = "---${step.fx1Value.toString(16).padStart(2, '0').uppercase()}",
            x = fx1X,
            y = textY,
            scale = scale,
            color = when {
                index == state.cursorRow && state.cursorColumn == 4 -> Color.Yellow
                step.note == Note.EMPTY -> Color(0xFF444444)
                else -> Color(0xFFaaaaaa)
            },
            spacing = CHAR_SPACING,
            fontScale = FONT_SCALE
        )

        // COLUMN 5: FX2 (mockup: ---00)
        drawBitmapText(
            text = "---${step.fx2Value.toString(16).padStart(2, '0').uppercase()}",
            x = fx2X,
            y = textY,
            scale = scale,
            color = when {
                index == state.cursorRow && state.cursorColumn == 5 -> Color.Yellow
                step.note == Note.EMPTY -> Color(0xFF444444)
                else -> Color(0xFFaaaaaa)
            },
            spacing = CHAR_SPACING,
            fontScale = FONT_SCALE
        )

        // COLUMN 5: FX3 (mockup: ---00)
        drawBitmapText(
            text = "---${step.fx3Value.toString(16).padStart(2, '0').uppercase()}",
            x = fx3X,
            y = textY,
            scale = scale,
            color = when {
                index == state.cursorRow && state.cursorColumn == 6 -> Color.Yellow
                step.note == Note.EMPTY -> Color(0xFF444444)
                else -> Color(0xFFaaaaaa)
            },
            spacing = CHAR_SPACING,
            fontScale = FONT_SCALE
        )
    }

    /**
     * Get cursor context for generic input handling
     *
     * Maps cursor position to appropriate CursorContext based on which column
     * the cursor is in, enabling A+direction value editing.
     *
     * Phrase columns:
     * 0 = Step number (read-only)
     * 1 = Note (C-0 to B-9, can be empty)
     * 2 = Volume (00-FF hex byte, A+left/right cycles)
     * 3 = Instrument (00-FF hex byte, A+left/right cycles)
     * 4-6 = FX1, FX2, FX3 (read-only for now)
     */
    fun getCursorContext(state: PhraseEditorState): CursorContext {
        val step = state.phrase.steps[state.cursorRow]

        return when (state.cursorColumn) {
            // Column 0: Step number (read-only)
            0 -> CursorContextFactory.readOnly()

            // Column 1: Note (C-0 to B-9)
            1 -> {
                val isEmpty = step.note == Note.EMPTY
                val currentValue = if (isEmpty) 0 else step.note.toMidi()
                CursorContextFactory.note(currentValue, isEmpty)
            }

            // Column 2: Volume (00-FF)
            2 -> {
                CursorContextFactory.volume(step.volume)
            }

            // Column 3: Instrument (0-3)
            3 -> {
                CursorContextFactory.instrument(step.instrument)
            }

            // Columns 4-6: FX slots (read-only for now)
            4, 5, 6 -> CursorContextFactory.readOnly()

            // Invalid column
            else -> CursorContextFactory.none()
        }
    }

    /**
     * Handle input action for phrase editor.
     * Applies the action to the phrase and returns information about what changed.
     *
     * @param state Current phrase editor state
     * @param action Input action to apply
     * @param instrumentController Reference to instrument controller for tracking last edited instrument
     * @return Result containing what was modified
     */
    fun handleInput(
        state: PhraseEditorState,
        action: com.example.pockettracker.core.logic.InputAction,
        instrumentController: com.example.pockettracker.core.logic.InstrumentController
    ): InputResult {
        val step = state.phrase.steps[state.cursorRow]
        var lastEditedNote: Note? = null
        var lastEditedVolume: Int? = null

        when (action) {
            is com.example.pockettracker.core.logic.InputAction.SET_VALUE -> {
                when (state.cursorColumn) {
                    1 -> {
                        // Note column: Convert MIDI value back to Note
                        step.note = Note.fromMidi(action.value)
                        lastEditedNote = step.note
                    }
                    2 -> {
                        // Volume column
                        step.volume = action.value
                        lastEditedVolume = action.value
                    }
                    3 -> {
                        // Instrument column
                        step.instrument = action.value
                        instrumentController.lastEditedInstrument = action.value
                    }
                }
            }
            is com.example.pockettracker.core.logic.InputAction.DELETE -> {
                when (state.cursorColumn) {
                    1 -> {
                        // Clear note
                        step.note = Note.EMPTY
                    }
                }
            }
            is com.example.pockettracker.core.logic.InputAction.INSERT_DEFAULT -> {
                // Insert default note (C-4)
                if (state.cursorColumn == 1) {
                    step.note = Note.fromString("C-4")
                    lastEditedNote = step.note
                }
            }
            else -> { /* NONE or unhandled - do nothing */ }
        }

        return InputResult(
            modified = action !is com.example.pockettracker.core.logic.InputAction.NONE,
            lastEditedNote = lastEditedNote,
            lastEditedVolume = lastEditedVolume
        )
    }

    /**
     * Result of input handling
     */
    data class InputResult(
        val modified: Boolean,
        val lastEditedNote: Note? = null,
        val lastEditedVolume: Int? = null
    )
}

/**
 * State data for phrase editor module
 */
data class PhraseEditorState(
    val phrase: Phrase,
    val cursorRow: Int,
    val cursorColumn: Int,
    val playbackRow: Int,
    val isPlaying: Boolean
)