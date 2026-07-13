package com.conanizer.pockettracker.trace

import com.conanizer.pockettracker.core.audio.AudioEngine
import com.conanizer.pockettracker.core.data.Chain
import com.conanizer.pockettracker.core.data.Groove
import com.conanizer.pockettracker.core.data.Note
import com.conanizer.pockettracker.core.data.Phrase
import com.conanizer.pockettracker.core.data.PhraseStep
import com.conanizer.pockettracker.core.data.Project
import com.conanizer.pockettracker.core.data.Table
import com.conanizer.pockettracker.core.data.TableRow
import com.conanizer.pockettracker.core.logic.ClipboardManager
import com.conanizer.pockettracker.core.logic.EffectProcessor
import com.conanizer.pockettracker.core.logic.InputAction
import com.conanizer.pockettracker.core.logic.InputController
import com.conanizer.pockettracker.core.logic.InstrumentController
import com.conanizer.pockettracker.input.CursorContext
import com.conanizer.pockettracker.input.NO_DEFAULT
import com.conanizer.pockettracker.ui.modules.ChainEditorModule
import com.conanizer.pockettracker.ui.modules.ChainEditorState
import com.conanizer.pockettracker.ui.modules.GrooveModule
import com.conanizer.pockettracker.ui.modules.GrooveState
import com.conanizer.pockettracker.ui.modules.PhraseEditorModule
import com.conanizer.pockettracker.ui.modules.PhraseEditorState
import com.conanizer.pockettracker.ui.modules.SongEditorModule
import com.conanizer.pockettracker.ui.modules.SongEditorState
import com.conanizer.pockettracker.ui.modules.TableModule
import com.conanizer.pockettracker.ui.modules.TableState
import com.conanizer.pockettracker.ui.overlays.FxHelperState
import com.conanizer.pockettracker.ui.overlays.fxHelperOpenedAt
import com.conanizer.pockettracker.ui.overlays.fxMoveCursorDown
import com.conanizer.pockettracker.ui.overlays.fxMoveCursorLeft
import com.conanizer.pockettracker.ui.overlays.fxMoveCursorRight
import com.conanizer.pockettracker.ui.overlays.fxMoveCursorUp
import com.conanizer.pockettracker.ui.overlays.selectedEffectCode
import org.junit.Assert.assertEquals
import org.junit.Test
import java.io.File

/**
 * Phase-3 S3 input goldens — the measuring stick for the INPUT LAYER, which the Linux port rewrites
 * in C++ this session (`native/ui/{cursor,selection,clipboard,fx_helper,input_dispatcher}.h`).
 *
 * Emits `testdata/units/p3-input.txt`, every line computed by the REAL Kotlin
 * `InputController` + the five screen modules + `ClipboardManager` + `FxHelperOverlay`.
 * `tools/ptinput` re-parses each line's inputs, recomputes the right-hand side in C++, and
 * byte-compares.
 *
 * ── What is claimed, and why it takes three parts ────────────────────────────────────────────────
 *
 * An `EDIT` line asserts all three at once, and the third is the one that matters:
 *
 *   1. the CURSOR CONTEXT   — "the cursor is on a NOTE, it is empty, its range is 12..127"
 *   2. the RESOLVED ACTION  — what `handleAButton(ctx)` and its four siblings return
 *   3. the RESULTING CELL   — what the module's `handleInput` actually wrote
 *
 * Context and action alone would be a weak test: a module that resolves A on a velocity to
 * `SET_VALUE(0x40)` and then writes 0x40 into the *instrument* field passes both. Only the cell
 * afterwards catches it, so every line carries the cell before AND after.
 *
 * `SEL` covers the multi-tap selection machine, `CLIP` the clipboard, `FXH` the effect-picker grid.
 *
 * ── The clock ────────────────────────────────────────────────────────────────────────────────────
 *
 * `InputController.handleSelectB` reads `System.currentTimeMillis()` itself, so the JVM side cannot
 * be handed a fake clock (the C++ port takes `now_ms` as a parameter precisely so that it can). The
 * golden therefore records the tap's INTENT — `F` (inside the 500 ms window) or `S` (outside it) —
 * and each side realises it: Kotlin by running back-to-back or sleeping past the window, C++ by
 * advancing its clock 10 ms or 600 ms. What is under test is "tap fast" versus "tap slow", not "tap
 * at t=137ms", and this is the encoding that says so.
 *
 * Like every golden here: missing file → generated; existing file → byte-compared (the drift guard).
 * To regenerate after an intentional change: delete testdata/units/p3-input.txt, rerun, commit.
 */
class P3InputGoldenTest {

    companion object {
        private fun repoRoot(): File {
            var dir = File(System.getProperty("user.dir")!!).absoluteFile
            while (true) {
                if (File(dir, "settings.gradle.kts").exists() || File(dir, "settings.gradle").exists()) return dir
                dir = dir.parentFile ?: error("repo root not found from ${System.getProperty("user.dir")}")
            }
        }

        private val goldenFile: File get() = File(repoRoot(), "testdata/units/p3-input.txt")

        /** The five buttons the generic handlers answer. The whole editing vocabulary. */
        private val BUTTONS = listOf("A", "B", "AL", "AR", "AB")
    }

    private val logger = FakeLogger
    private val inputController = InputController(logger, FakeStateObserver)
    private val clipboard = ClipboardManager(logger)

    private val phraseModule = PhraseEditorModule()
    private val chainModule = ChainEditorModule()
    private val songModule = SongEditorModule()
    private val tableModule = TableModule()
    private val grooveModule = GrooveModule()

    // handleInput(PHRASE) wants an InstrumentController purely to record `lastEditedInstrument` —
    // a side record. The C++ module returns it in its result instead of reaching back into a
    // controller, so nothing here depends on what this object does; it only has to exist.
    private val instrumentController = InstrumentController(
        AudioEngine(FakeAudioBackend(44100), FakeResourceLoader, logger), logger, FakeStateObserver
    )

    // ── Formatting (must match tools/ptinput/main.cpp byte-for-byte) ─────────────────────────────

    private fun hex2(v: Int): String = "%02X".format(v and 0xFF)

    /** A note as `<pitch>.<octave>`; empty is `-1.0`. '.' because ':' separates the cell's fields. */
    private fun noteStr(n: Note): String = "${n.pitch}.${n.octave}"

    private fun parseNote(s: String): Note {
        val (p, o) = s.split(".")
        return Note(p.toInt(), o.toInt())
    }

    /**
     * The eight capability flags, in declaration order, as one glanceable 8-char run:
     * canIncrement, canDecrement, canIncrementFast, canDecrementFast, canDelete, canInsert,
     * canCreate, isEmpty. A '.' is false. `+-><D..E` reads as "steps both ways, fast both ways,
     * deletable, and currently empty".
     */
    private fun capsStr(c: CursorContext): String = buildString {
        append(if (c.capabilities.canIncrement) '+' else '.')
        append(if (c.capabilities.canDecrement) '-' else '.')
        append(if (c.capabilities.canIncrementFast) '>' else '.')
        append(if (c.capabilities.canDecrementFast) '<' else '.')
        append(if (c.capabilities.canDelete) 'D' else '.')
        append(if (c.capabilities.canInsert) 'I' else '.')
        append(if (c.capabilities.canCreate) 'C' else '.')
        append(if (c.capabilities.isEmpty) 'E' else '.')
    }

    private fun ctxStr(c: CursorContext): String =
        "ctx=${c.valueType.name}|${capsStr(c)}|${c.currentValue}|${c.minValue}|${c.maxValue}|" +
            "${c.smallStep}|${c.largeStep}|${c.emptyValue}|${c.fxSlot}|" +
            (if (c.defaultValue == NO_DEFAULT) "-" else "${c.defaultValue}")

    private fun actStr(a: InputAction): String = when (a) {
        is InputAction.NONE -> "NONE"
        is InputAction.SET_VALUE -> "SET:${a.value}"
        is InputAction.DELETE -> "DELETE"
        is InputAction.INSERT_DEFAULT -> "INSERT"
        is InputAction.CREATE_NEW -> "CREATE"
        is InputAction.NAVIGATE_UP -> "NAV_UP"
        is InputAction.NAVIGATE_DOWN -> "NAV_DOWN"
        is InputAction.NAVIGATE_LEFT -> "NAV_LEFT"
        is InputAction.NAVIGATE_RIGHT -> "NAV_RIGHT"
        is InputAction.COPY -> "COPY"
        is InputAction.CUT -> "CUT"
        is InputAction.PASTE -> "PASTE"
    }

    /** Resolve a button against a context through the REAL InputController. */
    private fun resolve(btn: String, ctx: CursorContext): InputAction = when (btn) {
        "A" -> inputController.handleAButton(ctx)
        "B" -> inputController.handleBButton(ctx)
        "AL" -> inputController.handleALeft(ctx)
        "AR" -> inputController.handleARight(ctx)
        "AB" -> inputController.handleABCombo(ctx)
        else -> error("unknown button $btn")
    }

    // ── Cell encodings — each round-trips, so ptinput rebuilds exactly what Kotlin measured ──────

    private fun stepStr(s: PhraseStep): String =
        "step=${noteStr(s.note)}:${hex2(s.volume)}:${hex2(s.instrument)}:" +
            "${hex2(s.fx1Type)}/${hex2(s.fx1Value)}:${hex2(s.fx2Type)}/${hex2(s.fx2Value)}:" +
            "${hex2(s.fx3Type)}/${hex2(s.fx3Value)}"

    private fun parseStep(spec: String): PhraseStep {
        val f = spec.removePrefix("step=").split(":")
        fun fx(i: Int): Pair<Int, Int> {
            val (t, v) = f[i].split("/")
            return t.toInt(16) to v.toInt(16)
        }
        val (t1, v1) = fx(3); val (t2, v2) = fx(4); val (t3, v3) = fx(5)
        return PhraseStep(
            note = parseNote(f[0]), volume = f[1].toInt(16), instrument = f[2].toInt(16),
            fx1Type = t1, fx1Value = v1, fx2Type = t2, fx2Value = v2, fx3Type = t3, fx3Value = v3
        )
    }

    /** A chain row: phrase ref (decimal, −1 = empty) and transpose (hex). */
    private fun chainRowStr(c: Chain, row: Int): String =
        "crow=${c.phraseRefs[row]}:${hex2(c.transposeValues[row])}"

    /** A table row: transpose (hex), volume (decimal, −1 = empty), three FX pairs (hex). */
    private fun tableRowStr(r: TableRow): String =
        "trow=${hex2(r.transpose)}:${r.volume}:" +
            "${hex2(r.fx1Type)}/${hex2(r.fx1Value)}:${hex2(r.fx2Type)}/${hex2(r.fx2Value)}:" +
            "${hex2(r.fx3Type)}/${hex2(r.fx3Value)}"

    private fun parseTableRow(spec: String): TableRow {
        val f = spec.removePrefix("trow=").split(":")
        fun fx(i: Int): Pair<Int, Int> {
            val (t, v) = f[i].split("/")
            return t.toInt(16) to v.toInt(16)
        }
        val (t1, v1) = fx(2); val (t2, v2) = fx(3); val (t3, v3) = fx(4)
        return TableRow(
            transpose = f[0].toInt(16), volume = f[1].toInt(),
            fx1Type = t1, fx1Value = v1, fx2Type = t2, fx2Value = v2, fx3Type = t3, fx3Value = v3
        )
    }

    /**
     * A song cell is a chain ref inside a VARIABLE-length track. `len` is the number of entries the
     * track has; a cursor row at or past it reads empty, and an edit GROWS the list. That growth is
     * the only interesting thing about the SONG cell context, so the length is part of the state.
     */
    private fun songCellStr(len: Int, ref: Int): String = "scell=$len:$ref"

    // ════════════════════════════════════════════════════════════════════════════════════════════
    // The sweeps
    // ════════════════════════════════════════════════════════════════════════════════════════════

    /** The phrase-step ladder: every state a phrase cell can be in that changes what a button does. */
    private fun phraseLadder(): List<PhraseStep> = listOf(
        PhraseStep(),  // factory default: empty note, vel 7F, inst 00, no FX
        PhraseStep(note = Note(0, 4), volume = 0x7F, instrument = 0x00),      // C-4, defaults
        PhraseStep(note = Note(0, 0), volume = 0x00, instrument = 0x7F),      // C-0 = the MIDI floor (12)
        PhraseStep(note = Note(7, 9), volume = 0x40, instrument = 0x05),      // G-9 = the MIDI ceiling (127)
        PhraseStep(note = Note(11, 3), volume = 0x01, instrument = 0x01),     // B-3 — the octave boundary below C-4
        // The FX matrix: a value-capped effect (TBL → max 0x7F), an uncapped one, and the boundaries.
        PhraseStep(note = Note(0, 4), fx1Type = EffectProcessor.FX_ARC, fx1Value = 0x00),
        PhraseStep(note = Note(0, 4), fx1Type = EffectProcessor.FX_TBL, fx1Value = 0x7F),
        PhraseStep(note = Note(0, 4), fx1Type = EffectProcessor.FX_TBL, fx1Value = 0x00),
        PhraseStep(note = Note(0, 4), fx1Type = EffectProcessor.FX_EQM, fx1Value = 0x7F),
        PhraseStep(note = Note(0, 4), fx2Type = EffectProcessor.FX_PIT, fx2Value = 0xFF),
        PhraseStep(note = Note(0, 4), fx3Type = EffectProcessor.FX_KILL, fx3Value = 0x00),
        PhraseStep(note = Note(0, 4), fx3Type = EffectProcessor.FX_EQM, fx3Value = 0x00),
        // Every slot loaded at once — proves the three FX columns do not cross-talk.
        PhraseStep(
            note = Note(3, 5), volume = 0x33, instrument = 0x10,
            fx1Type = EffectProcessor.FX_VOLUME, fx1Value = 0xFF,
            fx2Type = EffectProcessor.FX_PAN, fx2Value = 0x80,
            fx3Type = EffectProcessor.FX_GRV, fx3Value = 0x7F
        ),
    )

    private fun tableLadder(): List<TableRow> = listOf(
        TableRow(),                                                    // factory: transpose 00, volume EMPTY (−1)
        TableRow(transpose = 0x00, volume = 0x00),                     // volume present, at its floor
        TableRow(transpose = 0x01, volume = 0xFF),                     // volume at its ceiling
        TableRow(transpose = 0x80, volume = 0x40),                     // transpose = −128, the two's-complement edge
        TableRow(transpose = 0xFF, volume = -1),                       // transpose = −1, volume empty
        TableRow(transpose = 0x0C, volume = 0x7F, fx1Type = EffectProcessor.FX_TBL, fx1Value = 0x7F),
        TableRow(transpose = 0x00, volume = -1, fx2Type = EffectProcessor.FX_HOP, fx2Value = 0xFF),
        TableRow(
            transpose = 0x18, volume = 0x10,
            fx1Type = EffectProcessor.FX_ARC, fx1Value = 0x31,
            fx2Type = EffectProcessor.FX_EQN, fx2Value = 0x00,
            fx3Type = EffectProcessor.FX_PVB, fx3Value = 0x88
        ),
    )

    /** (phraseRef, transpose) — a chain transpose is EMPTY (uneditable) whenever its ref is. */
    private fun chainLadder(): List<Pair<Int, Int>> = listOf(
        -1 to 0x00,    // empty ref → the transpose beside it is empty too
        -1 to 0x40,    // a stale transpose under an empty ref — still empty
        0 to 0x00,
        255 to 0xFF,
        7 to 0x80,     // −128
        7 to 0x01,
    )

    /** (trackLen, chainRefAtRow) — len 0 means the track is shorter than the cursor: an empty cell. */
    private fun songLadder(): List<Pair<Int, Int>> = listOf(
        0 to -1,       // the track does not reach this row at all
        16 to -1,      // it does, and the cell is empty
        16 to 0,
        16 to 255,
        16 to 42,
    )

    private fun grooveLadder(): List<Int> = listOf(-1, 0, 1, 6, 12, 255)

    // ── EDIT ────────────────────────────────────────────────────────────────────────────────────

    private fun sweepPhrase(out: MutableList<String>) {
        out += ""
        out += "# PHRASE — 10 columns (0 = the read-only step gutter) x the step ladder x 5 buttons."
        for (col in 0..9) {
            for (proto in phraseLadder()) {
                for (btn in BUTTONS) {
                    val phrase = Phrase(0)
                    phrase.steps[3] = proto.copy()
                    val before = stepStr(phrase.steps[3])

                    val state = PhraseEditorState(phrase, cursorRow = 3, cursorColumn = col,
                        playbackRow = 0, isPlaying = false)
                    val ctx = phraseModule.getCursorContext(state)
                    val act = resolve(btn, ctx)
                    phraseModule.handleInput(state, act, instrumentController)

                    out += "EDIT scr=PHRASE col=$col $before btn=$btn => ${ctxStr(ctx)} " +
                        "act=${actStr(act)} ${stepStr(phrase.steps[3])}"
                }
            }
        }
    }

    private fun sweepChain(out: MutableList<String>) {
        out += ""
        out += "# CHAIN — 3 columns x (phraseRef, transpose) x 5 buttons."
        for (col in 0..2) {
            for ((ref, tr) in chainLadder()) {
                for (btn in BUTTONS) {
                    val chain = Chain(0)
                    chain.phraseRefs[5] = ref
                    chain.transposeValues[5] = tr
                    val before = chainRowStr(chain, 5)

                    val state = ChainEditorState(chain, cursorRow = 5, cursorColumn = col)
                    val ctx = chainModule.getCursorContext(state)
                    val act = resolve(btn, ctx)
                    chainModule.handleInput(state, act)

                    out += "EDIT scr=CHAIN col=$col $before btn=$btn => ${ctxStr(ctx)} " +
                        "act=${actStr(act)} ${chainRowStr(chain, 5)}"
                }
            }
        }
    }

    private fun sweepSong(out: MutableList<String>) {
        out += ""
        out += "# SONG — the column IS the track and it is 1-based; the cell lives in a growing list."
        for (col in 0..8) {
            for ((len, ref) in songLadder()) {
                for (btn in BUTTONS) {
                    val project = Project()
                    val trackIndex = (col - 1).coerceIn(0, 7)
                    val track = project.tracks[trackIndex]
                    track.chainRefs.clear()
                    repeat(len) { track.chainRefs.add(-1) }
                    if (len > 9) track.chainRefs[9] = ref
                    val before = songCellStr(track.chainRefs.size,
                        if (9 < track.chainRefs.size) track.chainRefs[9] else -1)

                    val state = SongEditorState(project, cursorRow = 9, cursorTrack = col)
                    val ctx = songModule.getCursorContext(state)
                    val act = resolve(btn, ctx)
                    songModule.handleInput(state, act)

                    val after = songCellStr(track.chainRefs.size,
                        if (9 < track.chainRefs.size) track.chainRefs[9] else -1)
                    out += "EDIT scr=SONG col=$col $before btn=$btn => ${ctxStr(ctx)} " +
                        "act=${actStr(act)} $after"
                }
            }
        }
    }

    private fun sweepTable(out: MutableList<String>) {
        out += ""
        out += "# TABLE — 9 columns x the row ladder x 5 buttons."
        for (col in 0..8) {
            for (proto in tableLadder()) {
                for (btn in BUTTONS) {
                    val table = Table(0)
                    table.rows[4] = proto.copy()
                    val before = tableRowStr(table.rows[4])

                    val state = TableState(table, cursorRow = 4, cursorColumn = col)
                    val ctx = tableModule.getCursorContext(state)
                    val act = resolve(btn, ctx)
                    tableModule.handleInput(state, act)

                    out += "EDIT scr=TABLE col=$col $before btn=$btn => ${ctxStr(ctx)} " +
                        "act=${actStr(act)} ${tableRowStr(table.rows[4])}"
                }
            }
        }
    }

    private fun sweepGroove(out: MutableList<String>) {
        out += ""
        out += "# GROOVE — one editable column; −1 is the end-of-pattern marker, not a tick count."
        for (col in 0..1) {
            for (v in grooveLadder()) {
                for (btn in BUTTONS) {
                    val groove = Groove(0)
                    groove.steps[2] = v

                    val state = GrooveState(groove, cursorRow = 2, cursorColumn = col)
                    val ctx = grooveModule.getCursorContext(state)
                    val act = resolve(btn, ctx)
                    grooveModule.handleInput(state, act)

                    out += "EDIT scr=GROOVE col=$col grow=$v btn=$btn => ${ctxStr(ctx)} " +
                        "act=${actStr(act)} grow=${groove.steps[2]}"
                }
            }
        }
    }

    // ── SEL — the multi-tap selection machine ───────────────────────────────────────────────────

    /**
     * Scripts of `LB:F` / `LB:S` (an L+B tap, fast or slow), `E:UP|DOWN|LEFT|RIGHT` (a D-pad expand)
     * and `X` (L+R, exit). See the class doc for why F/S rather than a timestamp.
     */
    private fun sweepSelection(out: MutableList<String>) {
        val scripts = listOf(
            "LB:F",                              // one tap → CELL
            "LB:F,LB:F",                         // → ROW
            "LB:F,LB:F,LB:F",                    // → SCREEN
            "LB:F,LB:F,LB:F,LB:F",               // → back to CELL (the cycle closes)
            "LB:F,LB:S",                         // the second tap missed the window → exit
            "LB:F,LB:S,LB:F",                    // …and the NEXT tap re-enters on CELL, not ROW
            "LB:F,E:DOWN",
            "LB:F,E:DOWN,E:DOWN,E:RIGHT",
            "LB:F,E:UP",                         // the edge clamps at row 0 — the anchor stays put
            "LB:F,E:LEFT",                       // …and at column 1: the step gutter is not selectable
            "LB:F,E:RIGHT,E:RIGHT,E:RIGHT,E:RIGHT,E:RIGHT,E:RIGHT,E:RIGHT,E:RIGHT,E:RIGHT,E:RIGHT",
            "LB:F,LB:F,E:DOWN",                  // expand a ROW selection: it becomes a block
            "LB:F,LB:F,LB:F,E:DOWN",             // expand a SCREEN selection
            "LB:F,X",                            // L+R exits
            "LB:F,E:DOWN,X,E:DOWN",              // …and a D-pad after the exit expands nothing
            "E:DOWN",                            // an expand with no selection at all is a no-op
        )
        // (screen, cursorRow, cursorColumn, maxColumn, maxRow) — SONG's SCREEN scope spans all 256
        // rows, not the 16 on display, which is the whole reason maxRow is a parameter.
        val beds = listOf(
            Triple("PHRASE", 3 to 4, 9 to 15),
            Triple("CHAIN", 5 to 2, 2 to 15),
            Triple("SONG", 20 to 3, 8 to 255),
            Triple("TABLE", 7 to 6, 8 to 15),
        )

        out += ""
        out += "# SEL — the L+B multi-tap machine. F = a tap inside the 500ms window, S = outside it."
        for ((scr, cur, maxes) in beds) {
            val (row, col) = cur
            val (maxCol, maxRow) = maxes
            for (script in scripts) {
                inputController.exitSelectionMode()
                for (op in script.split(",")) {
                    when {
                        op == "LB:F" -> inputController.handleSelectB(row, col, maxCol, maxRow)
                        op == "LB:S" -> {
                            // Past the window. The only sleep in the suite, and it buys the one
                            // transition that cannot be observed any other way.
                            Thread.sleep(InputControllerWindowMs + 100)
                            inputController.handleSelectB(row, col, maxCol, maxRow)
                        }
                        op == "X" -> inputController.exitSelectionMode()
                        op.startsWith("E:") ->
                            inputController.expandSelection(op.removePrefix("E:"), maxRow, maxCol)
                        else -> error("unknown op $op")
                    }
                }
                val b = inputController.getSelectionBounds()
                val boundsStr =
                    if (!inputController.isSelectionModeActive() || b == null) "-"
                    else "${b.topLeftRow},${b.topLeftColumn}-${b.bottomRightRow},${b.bottomRightColumn}"
                val s = inputController.selectionStart
                val e = inputController.selectionEnd
                out += "SEL scr=$scr cur=$row,$col max=$maxCol,$maxRow script=$script => " +
                    "scope=${inputController.selectionScope.name} " +
                    "active=${if (inputController.isSelectionModeActive()) 1 else 0} " +
                    "start=${s?.let { "${it.row},${it.column}" } ?: "-"} " +
                    "end=${e?.let { "${it.row},${it.column}" } ?: "-"} " +
                    "bounds=$boundsStr info=${inputController.getSelectionInfo().ifEmpty { "-" }}"
            }
        }
        inputController.exitSelectionMode()
    }

    /** InputController.MULTI_TAP_WINDOW is private; this is the same 500 ms, named once. */
    private val InputControllerWindowMs = 500L

    // ── CLIP — the clipboard ────────────────────────────────────────────────────────────────────

    /** A phrase whose every row is distinct, so a mis-anchored paste is visible rather than lucky. */
    private fun seededPhrase(): Phrase {
        val p = Phrase(0)
        for (i in 0 until 16) {
            p.steps[i] = PhraseStep(
                note = Note(i % 12, 2 + i / 12), volume = 0x10 + i, instrument = 0x20 + i,
                fx1Type = EffectProcessor.FX_ARC, fx1Value = 0x30 + i,
                fx2Type = EffectProcessor.FX_PAN, fx2Value = 0x40 + i,
                fx3Type = EffectProcessor.FX_VOLUME, fx3Value = 0x50 + i
            )
        }
        return p
    }

    private fun seededChain(): Chain {
        val c = Chain(0)
        for (i in 0 until 16) {
            c.phraseRefs[i] = if (i % 4 == 3) -1 else 0x10 + i   // some empties, on purpose
            c.transposeValues[i] = 0x20 + i
        }
        return c
    }

    private fun seededTable(): Table {
        val t = Table(0)
        for (i in 0 until 16) {
            t.rows[i] = TableRow(
                transpose = 0x10 + i, volume = if (i % 5 == 4) -1 else 0x20 + i,
                fx1Type = EffectProcessor.FX_TBL, fx1Value = 0x30 + i,
                fx2Type = EffectProcessor.FX_HOP, fx2Value = 0x40 + i,
                fx3Type = EffectProcessor.FX_PIT, fx3Value = 0x50 + i
            )
        }
        return t
    }

    private fun seededProject(): Project {
        val p = Project()
        for (t in 0 until 8) {
            val track = p.tracks[t]
            track.chainRefs.clear()
            for (r in 0 until 16) track.chainRefs.add(if (r % 3 == 2) -1 else t * 16 + r)
        }
        return p
    }

    private fun dumpPhrase(out: MutableList<String>, p: Phrase) {
        for (i in 0 until 16) out += "  R%02d %s".format(i, stepStr(p.steps[i]))
    }

    private fun dumpChain(out: MutableList<String>, c: Chain) {
        for (i in 0 until 16) out += "  R%02d %s".format(i, chainRowStr(c, i))
    }

    private fun dumpTable(out: MutableList<String>, t: Table) {
        for (i in 0 until 16) out += "  R%02d %s".format(i, tableRowStr(t.rows[i]))
    }

    private fun dumpSong(out: MutableList<String>, p: Project) {
        for (r in 0 until 16) {
            val cells = (0 until 8).joinToString(",") { t ->
                val cr = p.tracks[t].chainRefs
                (if (r < cr.size) cr[r] else -1).toString()
            }
            out += "  R%02d %s".format(r, cells)
        }
    }

    private fun pasteStr(r: ClipboardManager.PasteResult): String = when (r) {
        is ClipboardManager.PasteResult.NoClipboard -> "NO_CLIPBOARD"
        is ClipboardManager.PasteResult.Success -> "SUCCESS:${r.itemsPasted}"
        is ClipboardManager.PasteResult.Error -> "WRONG_SCREEN"
    }

    private fun sweepClipboard(out: MutableList<String>) {
        out += ""
        out += "# CLIP — copy/cut/paste/delete. The dump beneath each case is the WHOLE grid after it,"
        out += "# because a mis-anchored paste writes the right bytes into the wrong cells."

        // ── PHRASE ───────────────────────────────────────────────────────────────────────────────
        // A copy of two FX columns, pasted onto a DIFFERENT FX slot: the items remember they were a
        // type and a value, so they land as a type and a value — not as two types.
        for ((label, src, dst) in listOf(
            Triple("cell", listOf(2, 1, 2, 1), listOf(9, 1)),           // one cell → far away
            Triple("block", listOf(1, 1, 3, 3), listOf(8, 1)),          // 3x3 block
            Triple("fxpair", listOf(0, 4, 2, 5), listOf(5, 6)),         // FX1 type+value → FX2 slot
            Triple("fxtype-onto-value", listOf(0, 4, 0, 4), listOf(0, 5)),  // a TYPE onto a VALUE column: nothing
            Triple("notecol", listOf(0, 1, 15, 1), listOf(0, 1)),       // a whole column onto itself
            Triple("overhang", listOf(12, 1, 15, 3), listOf(14, 1)),    // runs off the bottom — clipped
            Triple("rowspan", listOf(4, 1, 4, 9), listOf(10, 1)),       // a full row
        )) {
            clipboard.clear()
            val phrase = seededPhrase()
            val project = Project()
            project.phrases[0] = phrase
            clipboard.copyPhraseSteps(project, 0, src[0], src[1], src[2], src[3])
            val res = clipboard.paste(project, "PHRASE", 0, dst[0], dst[1])
            out += "CLIP scr=PHRASE op=copy-paste name=$label src=${src.joinToString(",")} " +
                "dst=${dst.joinToString(",")} => ${pasteStr(res)} info=${clipboard.getClipboardInfo()}"
            dumpPhrase(out, project.phrases[0])
        }

        for ((label, sel) in listOf(
            "cell" to listOf(3, 2, 3, 2),
            "block" to listOf(2, 1, 5, 4),
            "allcols" to listOf(0, 1, 0, 9),
        )) {
            clipboard.clear()
            val project = Project()
            project.phrases[0] = seededPhrase()
            val n = clipboard.deletePhraseSteps(project, 0, sel[0], sel[1], sel[2], sel[3])
            val deleted = (n as? ClipboardManager.DeleteResult.Success)?.itemsDeleted ?: -1
            out += "CLIP scr=PHRASE op=delete name=$label sel=${sel.joinToString(",")} => n=$deleted"
            dumpPhrase(out, project.phrases[0])
        }

        run {  // cut = copy + delete, then paste it back somewhere else
            clipboard.clear()
            val project = Project()
            project.phrases[0] = seededPhrase()
            clipboard.cutPhraseSteps(project, 0, 1, 1, 2, 3)
            val res = clipboard.paste(project, "PHRASE", 0, 10, 1)
            out += "CLIP scr=PHRASE op=cut-paste name=roundtrip src=1,1,2,3 dst=10,1 => " +
                "${pasteStr(res)} info=${clipboard.getClipboardInfo()}"
            dumpPhrase(out, project.phrases[0])
        }

        // ── The cross-screen rejection ───────────────────────────────────────────────────────────
        run {
            clipboard.clear()
            val project = Project()
            project.phrases[0] = seededPhrase()
            clipboard.copyPhraseSteps(project, 0, 0, 1, 1, 2)
            for (target in listOf("CHAIN", "SONG", "TABLE")) {
                val res = clipboard.paste(project, target, 0, 0, 1)
                out += "CLIP scr=$target op=paste-foreign name=phrase-clip => ${pasteStr(res)}"
            }
        }
        run {  // …and pasting with nothing on the clipboard at all
            clipboard.clear()
            val project = Project()
            val res = clipboard.paste(project, "PHRASE", 0, 0, 1)
            out += "CLIP scr=PHRASE op=paste-empty name=no-clipboard => ${pasteStr(res)} " +
                "info=${clipboard.getClipboardInfo().ifEmpty { "-" }}"
        }

        // ── CHAIN ────────────────────────────────────────────────────────────────────────────────
        for ((label, src, dst) in listOf(
            Triple("both-cols", listOf(0, 1, 3, 2), listOf(8, 1)),
            Triple("refs-only", listOf(0, 1, 5, 1), listOf(4, 1)),
            Triple("transpose-onto-ref", listOf(0, 2, 3, 2), listOf(0, 1)),  // a transpose item on col 1: nothing
        )) {
            clipboard.clear()
            val project = Project()
            project.chains[0] = seededChain()
            clipboard.copyChainRows(project, 0, src[0], src[1], src[2], src[3])
            val res = clipboard.paste(project, "CHAIN", 0, dst[0], dst[1])
            out += "CLIP scr=CHAIN op=copy-paste name=$label src=${src.joinToString(",")} " +
                "dst=${dst.joinToString(",")} => ${pasteStr(res)} info=${clipboard.getClipboardInfo()}"
            dumpChain(out, project.chains[0])
        }
        run {
            clipboard.clear()
            val project = Project()
            project.chains[0] = seededChain()
            val n = clipboard.deleteChainRows(project, 0, 2, 1, 6, 2)
            val deleted = (n as? ClipboardManager.DeleteResult.Success)?.itemsDeleted ?: -1
            out += "CLIP scr=CHAIN op=delete name=block sel=2,1,6,2 => n=$deleted"
            dumpChain(out, project.chains[0])
        }

        // ── SONG ─────────────────────────────────────────────────────────────────────────────────
        for ((label, src, dst) in listOf(
            Triple("cross-track", listOf(0, 1, 3, 2), listOf(8, 5)),
            Triple("single", listOf(1, 3, 1, 3), listOf(12, 8)),
            Triple("overhang-right", listOf(0, 7, 2, 8), listOf(0, 8)),  // runs past track 8 — clipped
        )) {
            clipboard.clear()
            val project = seededProject()
            clipboard.copySongCells(project, src[0], src[1], src[2], src[3])
            val res = clipboard.paste(project, "SONG", 0, dst[0], dst[1])
            out += "CLIP scr=SONG op=copy-paste name=$label src=${src.joinToString(",")} " +
                "dst=${dst.joinToString(",")} => ${pasteStr(res)} info=${clipboard.getClipboardInfo()}"
            dumpSong(out, project)
        }
        run {
            clipboard.clear()
            val project = seededProject()
            val n = clipboard.deleteSongCells(project, 3, 2, 6, 5)
            val deleted = (n as? ClipboardManager.DeleteResult.Success)?.itemsDeleted ?: -1
            out += "CLIP scr=SONG op=delete name=block sel=3,2,6,5 => n=$deleted"
            dumpSong(out, project)
        }

        // ── TABLE ────────────────────────────────────────────────────────────────────────────────
        for ((label, src, dst) in listOf(
            Triple("block", listOf(0, 1, 3, 4), listOf(9, 1)),
            Triple("volcol", listOf(0, 2, 15, 2), listOf(0, 2)),   // the volume column, incl. its −1 empties
            Triple("fxpair", listOf(0, 3, 2, 4), listOf(4, 5)),    // FX1 type+value → FX2 slot
        )) {
            clipboard.clear()
            val project = Project()
            project.tables[0] = seededTable()
            clipboard.copyTableRows(project, 0, src[0], src[1], src[2], src[3])
            val res = clipboard.paste(project, "TABLE", 0, dst[0], dst[1])
            out += "CLIP scr=TABLE op=copy-paste name=$label src=${src.joinToString(",")} " +
                "dst=${dst.joinToString(",")} => ${pasteStr(res)} info=${clipboard.getClipboardInfo()}"
            dumpTable(out, project.tables[0])
        }
        run {
            clipboard.clear()
            val project = Project()
            project.tables[0] = seededTable()
            val n = clipboard.deleteTableRows(project, 0, 1, 1, 4, 8)
            val deleted = (n as? ClipboardManager.DeleteResult.Success)?.itemsDeleted ?: -1
            out += "CLIP scr=TABLE op=delete name=block sel=1,1,4,8 => n=$deleted"
            dumpTable(out, project.tables[0])
        }
        run {  // cut then paste back — the table's two "empty"s (transpose 00, volume −1) must survive
            clipboard.clear()
            val project = Project()
            project.tables[0] = seededTable()
            clipboard.cutTableRows(project, 0, 0, 1, 2, 8)
            val res = clipboard.paste(project, "TABLE", 0, 6, 1)
            out += "CLIP scr=TABLE op=cut-paste name=roundtrip src=0,1,2,8 dst=6,1 => " +
                "${pasteStr(res)} info=${clipboard.getClipboardInfo()}"
            dumpTable(out, project.tables[0])
        }
        clipboard.clear()
    }

    // ── FXH — the effect-picker grid ────────────────────────────────────────────────────────────

    private fun sweepFxHelper(out: MutableList<String>) {
        out += ""
        out += "# FXH — the 6x5 grid. Its last row is CENTRED (cols 1..4); cols 0 and 5 there are"
        out += "# empty and unreachable, and every move that could land on them rounds inward."
        val scripts = listOf(
            "", "U", "D", "L", "R",
            "U,U", "D,D", "L,L", "R,R",
            "D,D,D,D",          // walk into the last row
            "D,D,D,D,L",        // …and off its left edge → wraps to col 4, not col 0
            "D,D,D,D,R,R,R,R",  // …round its right edge
            "U,L",              // wrap up into the last row from col 0 → rounds in to col 1
            "U,R,R,R,R,R",      // wrap up from col 5 → rounds in to col 4
            "D,D,D,D,U",        // back out of the last row, same column
            "D,D,D,D,D",        // wrap off the last row back to row 0
            "R,R,R,R,R,R",      // a full lap of a normal row
        )
        // Open on every effect index, so the last row's centring is exercised from inside it too.
        for (openAt in listOf(0, 1, 5, 6, 17, 23, 24, 25, 26, 27)) {
            for (script in scripts) {
                var s: FxHelperState = fxHelperOpenedAt(openAt)
                if (script.isNotEmpty()) {
                    for (m in script.split(",")) {
                        s = when (m) {
                            "U" -> s.fxMoveCursorUp()
                            "D" -> s.fxMoveCursorDown()
                            "L" -> s.fxMoveCursorLeft()
                            "R" -> s.fxMoveCursorRight()
                            else -> error("unknown move $m")
                        }
                    }
                }
                out += "FXH open=$openAt moves=${script.ifEmpty { "-" }} => " +
                    "row=${s.cursorRow} col=${s.cursorCol} idx=${s.cursorIndex} " +
                    "code=${hex2(s.selectedEffectCode())}"
            }
        }
    }

    // ════════════════════════════════════════════════════════════════════════════════════════════

    @Test
    fun inputGoldens() {
        val out = mutableListOf<String>()
        out += "# Phase-3 S3 input goldens — cursor contexts, resolved actions, the mutations they"
        out += "# produce, the selection machine, the clipboard, and the FX-helper grid."
        out += "# Generated by P3InputGoldenTest from the REAL Kotlin input layer. tools/ptinput"
        out += "# re-parses each line's inputs, recomputes the RHS in C++ (native/ui/*.h), byte-compares."
        out += "# format: <KIND> <inputs...> => <outputs...>   — hex bytes 2-digit UPPER, ints decimal."
        out += "# ctx=<TYPE>|<caps>|<cur>|<min>|<max>|<small>|<large>|<empty>|<fxSlot>|<default or ->"
        out += "# caps, in declaration order: + canIncrement, - canDecrement, > fast+, < fast-,"
        out += "#                            D canDelete, I canInsert, C canCreate, E isEmpty"

        sweepPhrase(out)
        sweepChain(out)
        sweepSong(out)
        sweepTable(out)
        sweepGroove(out)
        sweepSelection(out)
        sweepClipboard(out)
        sweepFxHelper(out)

        val text = out.joinToString("\n") + "\n"

        val f = goldenFile
        if (!f.exists()) {
            f.parentFile.mkdirs()
            f.writeText(text)
            println("WROTE golden ${f.absolutePath} (${out.size} lines) — review and commit it")
            return
        }
        val expected = f.readText()
        if (expected != text) {
            // Name the first divergent line rather than dumping two 3000-line blobs at the reader.
            val a = expected.lines()
            val b = text.lines()
            val i = a.indices.firstOrNull { it >= b.size || a[it] != b[it] } ?: b.size
            assertEquals(
                "p3-input.txt drifted at line ${i + 1} — the Kotlin input layer changed. " +
                    "If that was intentional, delete the golden, rerun, and commit it (ptinput will " +
                    "then hold C++ to the new behaviour).",
                a.getOrNull(i), b.getOrNull(i)
            )
            assertEquals("p3-input.txt line count", a.size, b.size)
        }
    }
}
