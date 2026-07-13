package com.conanizer.pockettracker.trace

import com.conanizer.pockettracker.core.audio.AudioEngine
import com.conanizer.pockettracker.core.data.Chain
import com.conanizer.pockettracker.core.data.Groove
import com.conanizer.pockettracker.core.data.Instrument
import com.conanizer.pockettracker.core.data.InstrumentType
import com.conanizer.pockettracker.core.data.ModDest
import com.conanizer.pockettracker.core.data.ModSlot
import com.conanizer.pockettracker.core.data.ModType
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
import com.conanizer.pockettracker.core.storage.FileSortMode
import com.conanizer.pockettracker.input.CursorContext
import com.conanizer.pockettracker.input.NO_DEFAULT
import com.conanizer.pockettracker.ui.modules.ChainEditorModule
import com.conanizer.pockettracker.ui.modules.ChainEditorState
import com.conanizer.pockettracker.ui.modules.EffectModule
import com.conanizer.pockettracker.ui.modules.EffectState
import com.conanizer.pockettracker.ui.modules.GrooveModule
import com.conanizer.pockettracker.ui.modules.GrooveState
import com.conanizer.pockettracker.ui.modules.InstrumentModule
import com.conanizer.pockettracker.ui.modules.InstrumentPoolModule
import com.conanizer.pockettracker.ui.modules.InstrumentPoolState
import com.conanizer.pockettracker.ui.modules.InstrumentState
import com.conanizer.pockettracker.ui.modules.MixerModule
import com.conanizer.pockettracker.ui.modules.MixerState
import com.conanizer.pockettracker.ui.modules.ModulationModule
import com.conanizer.pockettracker.ui.modules.ModulationState
import com.conanizer.pockettracker.ui.modules.PhraseEditorModule
import com.conanizer.pockettracker.ui.modules.PhraseEditorState
import com.conanizer.pockettracker.ui.modules.SongEditorModule
import com.conanizer.pockettracker.ui.modules.SongEditorState
import com.conanizer.pockettracker.ui.modules.TableModule
import com.conanizer.pockettracker.ui.modules.TableState
import com.conanizer.pockettracker.ui.overlays.FxHelperState
import com.conanizer.pockettracker.ui.overlays.QwertyKeyboardState
import com.conanizer.pockettracker.ui.overlays.currentKey
import com.conanizer.pockettracker.ui.overlays.deleteChar
import com.conanizer.pockettracker.ui.overlays.fxHelperOpenedAt
import com.conanizer.pockettracker.ui.overlays.fxMoveCursorDown
import com.conanizer.pockettracker.ui.overlays.fxMoveCursorLeft
import com.conanizer.pockettracker.ui.overlays.fxMoveCursorRight
import com.conanizer.pockettracker.ui.overlays.fxMoveCursorUp
import com.conanizer.pockettracker.ui.overlays.insertCurrentKey
import com.conanizer.pockettracker.ui.overlays.moveCursorDown
import com.conanizer.pockettracker.ui.overlays.moveCursorLeft
import com.conanizer.pockettracker.ui.overlays.moveCursorRight
import com.conanizer.pockettracker.ui.overlays.moveCursorUp
import com.conanizer.pockettracker.ui.overlays.moveTextCursorLeft
import com.conanizer.pockettracker.ui.overlays.moveTextCursorRight
import com.conanizer.pockettracker.ui.overlays.selectedEffectCode
import com.conanizer.pockettracker.ui.overlays.withClampedCol
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
    private val instrumentModule = InstrumentModule()
    private val instrumentPoolModule = InstrumentPoolModule()
    private val modulationModule = ModulationModule()
    private val mixerModule = MixerModule()
    private val effectModule = EffectModule()

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

    // ── The instrument screens (Phase 3 S4) ─────────────────────────────────────────────────────
    //
    // INSTRUMENT is the first screen that is a FORM rather than a grid: its rows hold one, two or
    // three parameters, some are unreachable spacers, and THE ROW MAP DEPENDS ON THE INSTRUMENT TYPE
    // (a SoundFont gains a PRESET row and loses the four sample-window rows, shifting everything below
    // the source section down by one). So the sweep is over the whole (type × row × column) space, not
    // over a rectangle — and the columns that DO NOT EXIST on a given row are probed too, because
    // "the module answers none() there" is exactly what stops the cursor landing on a cell nobody draws.

    /** Every field of an instrument that any of the three screens can write. 22 of them. */
    private fun instStr(i: Instrument): String =
        "inst=${if (i.instrumentType == InstrumentType.SOUNDFONT) "SF" else "SM"}:" +
            "${noteStr(i.root)}:${hex2(i.detune)}:${hex2(i.tableTicRate)}:" +
            "${hex2(i.volume)}:${i.slicingMode}:${hex2(i.pan)}:" +
            "${hex2(i.drive)}:${i.filterType}:${i.crush}:${hex2(i.filterCut)}:" +
            "${i.downsample}:${hex2(i.filterRes)}:" +
            "${hex2(i.reverbSend)}:${hex2(i.delaySend)}:${i.eqSlot}:" +
            "${i.loopMode}:${hex2(i.sampleStart)}:${hex2(i.loopStart)}:" +
            "${hex2(i.sampleEnd)}:${hex2(i.loopEnd)}:${if (i.reverse) 1 else 0}"

    /** One mod slot, every field. */
    private fun slotStr(s: ModSlot): String =
        "slot=${s.type.name}:${s.dest.name}:${hex2(s.amount)}:" +
            "${hex2(s.attack)}:${hex2(s.hold)}:${hex2(s.decay)}:${hex2(s.sustain)}:${hex2(s.release)}:" +
            "${s.oscShape}:${s.lfoTrigMode}:${hex2(s.lfoFreq)}"

    /**
     * The instrument ladder — every state that changes what a button on this screen does.
     * `sfPresetCount`/`sfPresetIndex` travel with it because the PRESET row's RANGE is the SF2's own
     * list length, and that is engine knowledge the Project does not hold.
     */
    private data class InstProbe(val inst: Instrument, val sfCount: Int, val sfIndex: Int)

    private fun instrumentLadder(): List<InstProbe> = listOf(
        // A factory sampler: every value at its default, so "A resets to the default" is a no-op here
        // and any movement is a real edit.
        InstProbe(Instrument(id = 3), 0, 0),
        // A sampler with EVERY writable field off its default and distinct from every other field —
        // this is the case that catches a handler writing the right value into the wrong field.
        InstProbe(Instrument(id = 3).apply {
            root = Note.fromString("F#2"); detune = 0x41; tableTicRate = 0x0C
            volume = 0xC0; slicingMode = 2; pan = 0x20
            drive = 0x33; filterType = "hp"; crush = 7; filterCut = 0xA5
            downsample = 3; filterRes = 0x55
            reverbSend = 0x11; delaySend = 0x22; eqSlot = 9
            loopMode = "png"; sampleStart = 0x10; loopStart = 0x30
            sampleEnd = 0xE0; loopEnd = 0xD0; reverse = true
        }, 0, 0),
        // The boundaries: a nibble at its ceiling, bytes at both ends, an empty root, no EQ.
        InstProbe(Instrument(id = 3).apply {
            root = Note.EMPTY; detune = 0x00; tableTicRate = 0xFF
            volume = 0x00; slicingMode = 0; pan = 0xFF
            drive = 0xFF; filterType = "bp"; crush = 15; filterCut = 0x00
            downsample = 15; filterRes = 0xFF
            reverbSend = 0xFF; delaySend = 0x00; eqSlot = -1
            loopMode = "off"; sampleStart = 0xFF; loopStart = 0xFF
            sampleEnd = 0x00; loopEnd = 0x00; reverse = false
        }, 0, 0),
        // EQ at slot 0 — the boundary between "assigned" (deletable) and "empty" (insertable).
        InstProbe(Instrument(id = 3).apply { eqSlot = 0; filterType = "lp"; loopMode = "fwd" }, 0, 0),

        // ── SoundFonts. The preset list is the interesting axis: with no SF2 loaded the count is 0,
        // so the PRESET row's max index is 0 and stepping it goes nowhere — which is what makes the row
        // safe to draw before a file is ever opened.
        InstProbe(Instrument(id = 3).apply { instrumentType = InstrumentType.SOUNDFONT }, 0, 0),
        InstProbe(Instrument(id = 3).apply {
            instrumentType = InstrumentType.SOUNDFONT
            root = Note.fromString("A-5"); detune = 0x90; tableTicRate = 0x03
            volume = 0x80; pan = 0xC0
            drive = 0x0F; filterType = "lp"; crush = 2; filterCut = 0x40
            downsample = 1; filterRes = 0x70
            reverbSend = 0x66; delaySend = 0x77; eqSlot = 12
        }, 128, 4),
        // A single-preset SF2: the max index is 0, so the row exists but cannot move.
        InstProbe(Instrument(id = 3).apply {
            instrumentType = InstrumentType.SOUNDFONT; eqSlot = -1
        }, 1, 0),
        // …and one sitting on the LAST preset, where an increment must wrap rather than run off.
        InstProbe(Instrument(id = 3).apply {
            instrumentType = InstrumentType.SOUNDFONT; eqSlot = 127
        }, 8, 7),
    )

    private fun sweepInstrument(out: MutableList<String>) {
        out += ""
        out += "# INSTRUMENT — a FORM, not a grid. 16 rows (sampler) / 15 (SoundFont), and the row map"
        out += "# itself depends on the type. Columns 0..3 and 5 are probed on EVERY row, including the"
        out += "# ones where they do not exist: a module that fails to answer none() there is how a"
        out += "# cursor lands on a cell nobody draws. sfp=COUNT,INDEX is the SF2's preset list (engine"
        out += "# knowledge; the Project does not hold it), which is what the PRESET row's range is made of."
        for (probe in instrumentLadder()) {
            val isSf = probe.inst.instrumentType == InstrumentType.SOUNDFONT
            val rows = if (isSf) 15 else 16
            for (row in 0 until rows) {
                for (col in listOf(0, 1, 2, 3, 5)) {
                    for (btn in BUTTONS) {
                        // A fresh copy per case: handleInput mutates, and a ladder entry must not carry
                        // one case's edit into the next.
                        val inst = probe.inst.copy(modSlots = probe.inst.modSlots.copyOf())
                        val before = instStr(inst)

                        val state = InstrumentState(
                            instrument = inst, cursorRow = row, cursorColumn = col,
                            soundfontPresetCount = probe.sfCount, soundfontPresetIndex = probe.sfIndex
                        )
                        val ctx = instrumentModule.getCursorContext(state)
                        val act = resolve(btn, ctx)
                        instrumentModule.handleInput(state, act, instrumentController)

                        out += "EDIT scr=INSTRUMENT row=$row col=$col sfp=${probe.sfCount},${probe.sfIndex} " +
                            "$before btn=$btn => ${ctxStr(ctx)} act=${actStr(act)} ${instStr(inst)}"
                    }
                }
            }
        }
    }

    private fun sweepInstrumentPool(out: MutableList<String>) {
        out += ""
        out += "# INST.POOL — 5 columns. Column 0 (NAME) is selection-only: A loads a source into an"
        out += "# empty slot and A+B clears it, both the dispatcher's, so the module answers read_only()."
        for (probe in instrumentLadder()) {
            for (col in 0..4) {
                for (btn in BUTTONS) {
                    val project = Project()
                    val slot = 3
                    project.instruments[slot] = probe.inst.copy(modSlots = probe.inst.modSlots.copyOf())
                    val before = instStr(project.instruments[slot])

                    val state = InstrumentPoolState(
                        project = project, selectedInstrument = slot, cursorColumn = col
                    )
                    val ctx = instrumentPoolModule.getCursorContext(state)
                    val act = resolve(btn, ctx)
                    instrumentPoolModule.handleInput(state, act, instrumentController)

                    out += "EDIT scr=INST_POOL col=$col $before btn=$btn => ${ctxStr(ctx)} " +
                        "act=${actStr(act)} ${instStr(project.instruments[slot])}"
                }
            }
        }
    }

    /**
     * The mod-slot ladder. Its axis is the TYPE, because on MODS the type decides how many rows the
     * slot HAS and what each of them MEANS: row 4 is HOLD on an AHD, DEC on an ADSR and the LFO's
     * trigger mode on an LFO. There is no "the row 4 parameter" to test — only a `when(type)`.
     */
    private fun modLadder(): List<ModSlot> = listOf(
        ModSlot(),                                                     // NONE — one row, everything else read-only
        ModSlot(type = ModType.AHD, dest = ModDest.VOLUME, amount = 0x80,
                attack = 0x10, hold = 0x20, decay = 0x30),
        ModSlot(type = ModType.ADSR, dest = ModDest.FILTER_CUTOFF, amount = 0xFF,
                attack = 0x01, decay = 0x02, sustain = 0x03, release = 0x04),
        ModSlot(type = ModType.LFO, dest = ModDest.PAN, amount = 0x40,
                oscShape = 9, lfoTrigMode = 3, lfoFreq = 0xC0),        // both list values at their ceiling
        ModSlot(type = ModType.LFO, dest = ModDest.MOD_AMT, amount = 0x00,
                oscShape = 0, lfoTrigMode = 0, lfoFreq = 0x00),        // …and at their floor
        ModSlot(type = ModType.DRUM, dest = ModDest.PITCH, amount = 0x01,
                attack = 0xFE, hold = 0xFD, decay = 0xFC),             // AHD-shaped, but a different type
        ModSlot(type = ModType.TRIG, dest = ModDest.MOD_BOTH, amount = 0x7F,
                attack = 0xAA, decay = 0xBB, sustain = 0xCC, release = 0xDD),  // ADSR-shaped
        // A slot holding a HIDDEN type. A project saved before TRACKING was withdrawn from the cycle
        // still loads and still displays — and stepping its TYPE must move it to a real one rather than
        // sticking, which is what `indexOf(...).coerceAtLeast(0)` buys.
        ModSlot(type = ModType.TRACKING, dest = ModDest.FINE_PITCH, amount = 0x55),
    )

    private fun sweepMods(out: MutableList<String>) {
        out += ""
        out += "# MODS — no columns: the cursor is (pair, side, row), and `pair*2+side` is the slot."
        out += "# Rows 0..6 are probed against every type, including rows the type does not have (an"
        out += "# LFO has no REL) — the module must answer read_only() there. A+B resets the WHOLE slot."
        for ((pair, side) in listOf(0 to 0, 1 to 1)) {   // the two ends of the index math
            for (proto in modLadder()) {
                for (row in 0..6) {
                    for (btn in BUTTONS) {
                        val inst = Instrument(id = 3)
                        val slotIndex = pair * 2 + side
                        inst.modSlots[slotIndex] = proto.copy()
                        val before = slotStr(inst.modSlots[slotIndex])

                        val state = ModulationState(
                            instrument = inst, cursorRow = row, cursorPair = pair, cursorSide = side
                        )
                        val ctx = modulationModule.getCursorContext(state)
                        val act = resolve(btn, ctx)
                        modulationModule.handleInput(state, act)

                        out += "EDIT scr=MODS pair=$pair side=$side row=$row $before btn=$btn => " +
                            "${ctxStr(ctx)} act=${actStr(act)} ${slotStr(inst.modSlots[slotIndex])}"
                    }
                }
            }
        }
    }

    // ── MIXER + EFFECTS (Phase 3 S5) ────────────────────────────────────────────────────────────

    /**
     * The mixer's "cell" is every field the screen can WRITE — all eight track volumes, the master
     * volume, both send wets, the master EQ slot, both master-bus depths, the limiter — plus
     * `masterBusFx`, which the screen never writes but which DECIDES which of the two depths row 2
     * edits. Sixteen fields for one cursor cell looks excessive until you ask what a wrong one costs:
     * writing track 4's volume into track 3, or OTT's depth into DUST's, is exactly the bug that agrees
     * on the context AND on the action and diverges only in the model. The wide cell is what sees it.
     */
    private fun mixStr(p: Project): String =
        "mix=" + (0..7).joinToString(":") { hex2(p.tracks[it].volume) } +
            ":${hex2(p.masterVolume)}:${hex2(p.reverbWet)}:${hex2(p.delayWet)}:${p.masterEqSlot}" +
            ":${hex2(p.ottDepth)}:${hex2(p.dustDepth)}:${hex2(p.limiterPreGain)}:${p.masterBusFx}"

    private fun mixerLadder(): List<(Project) -> Unit> = listOf(
        { },                                              // the factory defaults
        { p ->                                            // every field distinct — a stray write cannot hide
            for (i in 0..7) p.tracks[i].volume = 0x10 + i * 0x11
            p.masterVolume = 0xC3; p.reverbWet = 0xA1; p.delayWet = 0x5E
            p.masterEqSlot = 0x22; p.ottDepth = 0x33; p.dustDepth = 0x44; p.limiterPreGain = 0x55
        },
        { p -> p.masterEqSlot = -1 },                     // unassigned → A INSERTS slot 0
        { p -> p.masterEqSlot = 127 },                    // its ceiling → A wraps back to 0
        { p ->                                            // ⚠️ DUST selected: row 2 must write dustDepth
            p.masterBusFx = 1; p.ottDepth = 0x11; p.dustDepth = 0xEE
        },
        { p ->                                            // both ends of every byte range at once
            for (i in 0..7) p.tracks[i].volume = if (i % 2 == 0) 0x00 else 0xFF
            p.masterVolume = 0x00; p.reverbWet = 0xFF; p.delayWet = 0x00
            p.ottDepth = 0xFF; p.dustDepth = 0x00; p.limiterPreGain = 0xFF
        },
    )

    private fun sweepMixer(out: MutableList<String>) {
        out += ""
        out += "# MIXER — NOT a grid. Rows 2 and 3 exist only in column 8 (the master strip); every"
        out += "# other (row, column) pair is unreachable by navigation. They are probed anyway, because"
        out += "# the module must answer none() there rather than reach for a track that is not there."
        for (row in 0..3) {
            for (col in 0..8) {
                for (probe in mixerLadder()) {
                    for (btn in BUTTONS) {
                        val project = Project()
                        probe(project)
                        val before = mixStr(project)

                        val state = MixerState(
                            project = project, cursorColumn = col, mixerMasterRow = row
                        )
                        val ctx = mixerModule.getCursorContext(state)
                        val act = resolve(btn, ctx)
                        mixerModule.handleInput(state, act) { }

                        out += "EDIT scr=MIXER row=$row col=$col $before btn=$btn => " +
                            "${ctxStr(ctx)} act=${actStr(act)} ${mixStr(project)}"
                    }
                }
            }
        }
    }

    /** The effects screen's cell: the eight fields it writes, plus the `delaySync` flag TIME reads. */
    private fun fxStr(p: Project): String =
        "fx=${p.masterBusFx}:${hex2(p.reverbFeedback)}:${hex2(p.reverbDamp)}:${p.reverbInputEq}:" +
            "${hex2(p.delayTime)}:${hex2(p.delayFeedback)}:${hex2(p.delayReverbSend)}:" +
            "${p.delayInputEq}:${if (p.delaySync) 1 else 0}"

    private fun effectsLadder(): List<(Project) -> Unit> = listOf(
        { },                                              // factory defaults: sync off, both EQs unassigned
        { p ->                                            // every field distinct
            p.reverbFeedback = 0x11; p.reverbDamp = 0x22; p.reverbInputEq = 0x33
            p.delayTime = 0x44; p.delayFeedback = 0x55; p.delayReverbSend = 0x66; p.delayInputEq = 0x77
        },
        { p -> p.masterBusFx = 1 },                       // TYPE at its ceiling (DUST) → A wraps to OTT
        { p -> p.delaySync = true; p.delayTime = 5 },     // ⚠️ TIME now speaks subdivisions, not bytes
        { p -> p.delaySync = true; p.delayTime = 11 },    // …at the top of its 12 → A wraps to 0
        { p -> p.delaySync = true; p.delayTime = 0 },     // …and at the bottom → B wraps to 11
        { p -> p.delayTime = 0xFF },                      // free-running, at the byte ceiling
        { p -> p.reverbInputEq = 127; p.delayInputEq = 0 },   // the EQ slots at their two ends
    )

    private fun sweepEffects(out: MutableList<String>) {
        out += ""
        out += "# EFFECTS — one column of eight rows (the screen draws fifteen: headers and spacers"
        out += "# between them). TIME is the interesting one: `delaySync` changes both the cell's"
        out += "# VOCABULARY (a byte, or one of twelve note divisions) and its cursor RANGE, so the same"
        out += "# button on the same row does two different things either side of it."
        for (row in 0..EffectModule.MAX_CURSOR_ROW) {
            for (probe in effectsLadder()) {
                for (btn in BUTTONS) {
                    val project = Project()
                    probe(project)
                    val before = fxStr(project)

                    val state = EffectState(project = project, cursorRow = row)
                    val ctx = effectModule.getCursorContext(state)
                    val act = resolve(btn, ctx)
                    effectModule.handleInput(state, act) { }

                    out += "EDIT scr=EFFECTS row=$row $before btn=$btn => " +
                        "${ctxStr(ctx)} act=${actStr(act)} ${fxStr(project)}"
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

    // ── SORT — the file browser's listing order (Phase 3 S6a) ───────────────────────────────────

    /**
     * The browser is the one screen with NO cursor context and NO handleInput — it is a navigator, not
     * an editor, so the EDIT lines above have nothing to say about it. What it DOES have is a pure
     * function with all of its logic in it: `sortItems`, over a list `buildItemList` has already put in
     * name order.
     *
     * That pairing is the whole test, and it is subtler than it looks. `sortedBy` is a STABLE sort, so
     * two files with the same key keep the order they arrived in — which is the NAME order, every time,
     * because the Android browser re-lists from `buildItemList` on every sort change
     * (`LaunchedEffect(currentDirectory, sortMode, listRefreshTick)` → `sortItems(buildItemList(…))`).
     * Ties are not exotic: every file a `git clone` writes shares an mtime, and so does every WAV a
     * sample-editor CHOP produces.
     *
     * ⚠️ The C++ port must therefore use `std::stable_sort` AND rebuild before sorting. `std::sort`
     * would order ties arbitrarily — differently on libstdc++, libc++ and MSVC — and re-sorting the
     * on-screen list in place would tie-break on whichever mode the user happened to arrive from. Both
     * bugs are INVISIBLE without a tie in the fixture, which is why two of the six files below share an
     * mtime and two more share a size.
     *
     * The fixture is synthetic (name, size, mtime as data — no real directory), so the JVM and C++
     * sides sort the identical input without either of them touching a disk.
     */
    private data class SortEntry(val name: String, val dir: Boolean, val size: Long, val mtime: Long)

    /**
     * ⚠️ **THIRTY-SIX files, and the number is the whole point.**
     *
     * The first version of this fixture had four, and a `std::sort` passed it — measured, not assumed.
     * Every standard `sort` falls back to INSERTION SORT below a small-range threshold (32 on MSVC's
     * `_ISORT_MAX`, 16 on libstdc++'s `_S_threshold`), and insertion sort happens to be stable. So a
     * small fixture cannot tell `sort` from `stable_sort` — it would go green on the dev box and could
     * go red on a different toolchain, for a reason nobody would ever guess.
     *
     * Past the threshold, introsort partitions and the tie order becomes arbitrary. Thirty-four files
     * therefore share ONE mtime, which is not a contrivance — it is the common case (every file a `git
     * clone` writes, every WAV a CHOP produces) — and they must come out of a DATE sort in pure NAME
     * order. The three size buckets do the same job for the SIZE modes.
     */
    private fun sortFixture(): List<SortEntry> = buildList {
        add(SortEntry("Zed", dir = true, size = 0L, mtime = 500L))
        add(SortEntry("alpha", dir = true, size = 0L, mtime = 900L))   // lowercase sorts FIRST — the
                                                                       // name sort is case-insensitive

        // The two that break the tie at either end, so a DATE sort is not simply a name sort.
        add(SortEntry("aaa.wav", dir = false, size = 5000L, mtime = 100L))   // oldest, biggest
        add(SortEntry("zzz.wav", dir = false, size = 1L, mtime = 9000L))     // newest, smallest

        // …and 34 that ALL share one mtime, in three size buckets. Added in an order that is neither
        // the name order nor the reverse of it, so a sort that merely preserved the input would fail too.
        val order = (0 until 34).map { (it * 13) % 34 }   // a permutation, deterministic and jumbled
        for (i in order) {
            add(SortEntry(
                name = "s%02d.wav".format(i),
                dir = false,
                size = 100L + (i % 3) * 1000L,   // three big size ties
                mtime = 1000L                    // ⚠️ ONE mtime for all 34
            ))
        }
    }

    /** The C++ twin of BrowserItem, built from the fixture the way `buildItemList` builds it. */
    private fun sweepSort(out: MutableList<String>) {
        out += ""
        out += "# SORT — the file browser's listing order. The browser has no cursor context and no"
        out += "# handleInput (it navigates, it does not edit), so this is what there is to measure:"
        out += "# sortItems over a list buildItemList has already put in NAME order."
        out += "#"
        out += "# ⚠️ 34 of the 36 files share ONE mtime, and the SIZE of the fixture is load-bearing."
        out += "# sortedBy is STABLE, so a tie must fall back on the name order the build left behind."
        out += "# A four-file fixture could NOT prove that: every std::sort falls back to insertion sort"
        out += "# below a threshold (32 on MSVC, 16 on libstdc++) and insertion sort is stable, so it"
        out += "# passed. Past the threshold introsort partitions and ties scramble. Measured, not"
        out += "# assumed — the first version of this fixture had four files and a std::sort went green."
        out += "#"
        out += "# '..' is always first; folders always precede files; the sort orders each GROUP."

        // buildItemList's own pre-sort: folders by name, then files by name, both case-insensitive.
        val entries = sortFixture()
        val folders0 = entries.filter { it.dir }.sortedBy { it.name.lowercase() }
        val files0 = entries.filter { !it.dir }.sortedBy { it.name.lowercase() }

        for (mode in FileSortMode.values()) {
            // The exact comparators FileBrowserModule.sortItems uses, applied to the pre-sorted groups.
            fun sort(v: List<SortEntry>): List<SortEntry> = when (mode) {
                FileSortMode.NAME_ASC -> v.sortedBy { it.name.lowercase() }
                FileSortMode.NAME_DESC -> v.sortedByDescending { it.name.lowercase() }
                FileSortMode.DATE_ASC -> v.sortedBy { it.mtime }
                FileSortMode.DATE_DESC -> v.sortedByDescending { it.mtime }
                FileSortMode.SIZE_ASC -> v.sortedBy { it.size }
                FileSortMode.SIZE_DESC -> v.sortedByDescending { it.size }
            }
            val order = (listOf("..") + sort(folders0).map { "[${it.name}]" } +
                sort(files0).map { it.name.substringBeforeLast('.') }).joinToString(",")
            out += "SORT mode=${mode.name} => $order"
        }
    }

    // ── KBD — the QWERTY keyboard (Phase 3 S6a) ─────────────────────────────────────────────────

    /**
     * The keyboard is a pure state machine over (text, textCursor, keyRow, keyCol, layout), and every
     * one of its verbs is an extension function on the state — so a script of gestures drives it with
     * no Compose, no engine and no screen. Exactly what a golden wants.
     *
     * The two flags are what make it worth 700 lines of cases rather than 20. `insertBefore` (a
     * SETTINGS row) flips the meaning of BOTH A and B at once — insert-at-cursor + backspace, or
     * insert-after + forward-delete — and `clearOnFirstB` makes the first B wipe the field instead of
     * deleting a character, which is what a "SAVE AS" that suggests a name needs.
     */
    private fun sweepQwerty(out: MutableList<String>) {
        out += ""
        out += "# KBD — the QWERTY keyboard's state machine. Gestures in, (text, textCursor, keyRow,"
        out += "# keyCol, layout) out. U/D/L/R move the KEY cursor; A types; B deletes; </> move the"
        out += "# TEXT cursor; 0/1 switch layout."
        out += "#"
        out += "# ⚠️ insertBefore flips A *and* B together (insert-at + backspace, or insert-after +"
        out += "# forward-delete); clearOnFirstB makes the FIRST B wipe the field. Both are set at OPEN."

        val scripts = listOf(
            "",
            "A", "A,A", "A,A,A",
            "D,A", "D,D,A", "D,D,D,A",          // walk down the rows, typing one from each
            "D,D,D,D,A",                        // …onto the ACTION row, where A types NOTHING
            "R,A", "R,R,A", "L,A",              // the key cursor wraps within its row
            "U,A",                              // UP from row 0 wraps to the ACTION row
            "U,U,A",                            // …and once more lands on the space bar
            "D,D,D,A",                          // the space bar itself types a space
            "B", "B,B", "B,B,B,B,B,B,B,B",      // delete past the start
            "A,B", "A,A,B",
            "<,A", "<,<,A", "<,<,<,<,<,A",      // type into the MIDDLE of the text
            ">,A", "<,>,A",
            "<,B", "<,<,B",                     // delete from the middle
            "1,A", "1,D,A", "1,D,D,A",          // the 123 layout
            "1,R,R,A,0,A",                      // …type in one layout, switch back, type in the other
            "1,R,R,R,R,R,R,R,R,R,0,A",          // ⚠️ col 9 in one layout must CLAMP in the other
            "A,A,A,A,A,A,A,A,A,A,A,A",          // past maxLength
        )

        for (insertBefore in listOf(true, false)) {
            for (clearOnFirstB in listOf(false, true)) {
                for (initial in listOf("", "AB", "KICK")) {
                    for (script in scripts) {
                        var s = QwertyKeyboardState(
                            isOpen = true,
                            text = initial,
                            maxLength = 8,               // small, so "past maxLength" is reachable
                            textCursor = initial.length,
                            insertBefore = insertBefore,
                            clearOnFirstB = clearOnFirstB
                        )
                        if (script.isNotEmpty()) {
                            for (g in script.split(",")) {
                                s = when (g) {
                                    "U" -> s.moveCursorUp()
                                    "D" -> s.moveCursorDown()
                                    "L" -> s.moveCursorLeft()
                                    "R" -> s.moveCursorRight()
                                    "A" -> s.insertCurrentKey()
                                    "B" -> s.deleteChar()
                                    "<" -> s.moveTextCursorLeft()
                                    ">" -> s.moveTextCursorRight()
                                    "0" -> s.copy(layout = 0).withClampedCol()
                                    "1" -> s.copy(layout = 1).withClampedCol()
                                    else -> error("unknown gesture $g")
                                }
                            }
                        }
                        val ib = if (insertBefore) 1 else 0
                        val cb = if (clearOnFirstB) 1 else 0
                        out += "KBD ib=$ib cb=$cb init=${initial.ifEmpty { "-" }} " +
                            "g=${script.ifEmpty { "-" }} => text=${s.text.ifEmpty { "-" }} " +
                            "tc=${s.textCursor} kr=${s.keyCursorRow} kc=${s.keyCursorCol} " +
                            "lay=${s.layout} key=${s.currentKey()} cb=${if (s.clearOnFirstB) 1 else 0}"
                    }
                }
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
        sweepInstrument(out)
        sweepInstrumentPool(out)
        sweepMods(out)
        sweepMixer(out)
        sweepEffects(out)
        sweepSelection(out)
        sweepClipboard(out)
        sweepFxHelper(out)
        sweepSort(out)
        sweepQwerty(out)

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
