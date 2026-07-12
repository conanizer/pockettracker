package com.conanizer.pockettracker.trace

import com.conanizer.pockettracker.core.data.framesPerStep
import com.conanizer.pockettracker.core.logic.FileController
import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Assert.fail
import org.junit.Test
import java.io.File
import kotlin.math.abs
import kotlin.math.sqrt

/**
 * songcore S7 — the measuring stick for the RANDOM FX, the one corner of the sequencer no byte-exact
 * golden can reach.
 *
 * CHA (chance gate), RND / RNL (randomize an FX value, or a note + instrument) and ARP mode RANDOM
 * all draw from `kotlin.random.Random`, which is seeded from the platform: the KOTLIN sequencer does
 * not produce the same event stream twice, so there is nothing for a `.trace` file to be. SC-1 kept
 * these FX out of g1..g7 for exactly that reason — and the hole that left is the whole point of this
 * test. songcore shipped `rng_int() → 0` for four sessions, which did not merely bias the draws, it
 * pinned every one of them to the LOWEST value in its range: CHA passed whenever its probability
 * nibble was nonzero, RND and RNL always emitted the bottom of their band, and a random arpeggio
 * always played the root. Every check we owned stayed green, because none of them looked here.
 *
 * WHAT IS COMPARED, since a sequence cannot be. Two claims, and the split matters:
 *
 *   1. The SUPPORT and the DRAW COUNT — recorded in `testdata/units/s7-random.txt` and compared
 *      EXACTLY, by both engines. `n=` is the number of draws one render makes (deterministic: the
 *      random FX change which value comes out, never how many are drawn), and `support=` is the set
 *      of values a draw can produce. This is where the bugs actually live — an off-by-one, a closed
 *      interval where Kotlin's is half-open, a stub stuck at the low end — and every one of them
 *      moves the support, so it is caught with certainty and no statistics. It is not flaky: at these
 *      sample sizes the chance of a reachable value going unseen is around e^-200.
 *
 *   2. The SHAPE — uniform, or Bernoulli at k/15 for a CHA gate. This one IS statistical, so each
 *      engine checks its own histogram against the law with a margin sized so the null hypothesis
 *      effectively never fires (6σ + slack). It is the weaker of the two claims and it is meant to
 *      be: it catches a gross error (a mode weighted 1/2 instead of 1/3), not a sub-ULP modulo bias,
 *      which no achievable N could resolve anyway.
 *
 * The `expect=` column is not asserted from thin air — THIS test measures Kotlin against it. So the
 * golden is the Kotlin sequencer's own behavior, stated in the only form a random process has one:
 * its support and its law. tools/ptrandom then holds C++ songcore to the identical file.
 *
 * Like the other goldens: missing file → generated; existing file → compared. To regenerate after a
 * deliberate change, delete testdata/units/s7-random.txt, rerun, and commit the diff with the change.
 */
class S7RandomGoldenTest {

    companion object {
        /** Renders to run. Sized so the largest support (80 values) gets ~200 draws per value: enough
         *  that a reachable value going unobserved is impossible in practice, which is what lets the
         *  support be asserted exactly rather than with a tolerance. */
        const val RENDERS = 300

        /** g8's chains are eight rows of the same phrase, so one render walks each phrase eight times.
         *  Asserted below via the no-CHA control step, not merely assumed. */
        const val PHRASE_REPEATS = 8

        const val SAMPLE_RATE = 44100

        private fun repoRoot(): File {
            var dir = File(System.getProperty("user.dir")!!).absoluteFile
            while (true) {
                if (File(dir, "settings.gradle.kts").exists() || File(dir, "settings.gradle").exists()) return dir
                dir = dir.parentFile ?: error("repo root not found from ${System.getProperty("user.dir")}")
            }
        }

        private val testdata: File get() = File(repoRoot(), "testdata")
        private val goldenFile: File get() = File(testdata, "units/s7-random.txt")

        // ── The observable table — mirrored verbatim in tools/ptrandom/main.cpp ──────────────────
        //
        // Each random FX sits on its own track in g8-random so no two draws can interact, and each
        // one lands in the trace as an exact integer (see GoldenProjects.g8Random for why PIT is the
        // carrier). Order is the golden's line order.

        /** The law a draw is expected to follow. */
        sealed class Law {
            /** Every value in the support, equally likely. */
            object Uniform : Law() {
                override fun toString() = "uniform"
            }
            /** P(draw == [value]) == [k]/15 — a CHA gate, whose roll is nextInt(15) → 0..14. */
            data class Bernoulli(val value: Int, val k: Int) : Law() {
                override fun toString() = "p($value)=$k/15"
            }
        }

        val LAWS: LinkedHashMap<String, Law> = linkedMapOf(
            "rnd-pit"       to Law.Uniform,             // RND 37 → recalled PIT byte 0x30..0x7F → pit 48..127
            "rnl-note"      to Law.Uniform,             // RNL 53 → C-4 ± 5 semitones → MIDI 55..65
            "rnl-inst"      to Law.Uniform,             // RNL 53 → instrument 4 ± 3 → 1..7
            "rnl-left-pit"  to Law.Uniform,             // RNL 24 → FX1's PIT value 0x20..0x4F → pit 32..79
            "arp-random"    to Law.Uniform,             // ARP A37 under ARC mode 3 → offset ∈ {0, 3, 7}
            "cha-gate-p0"   to Law.Bernoulli(1, 0),     // CHA 00 → the note NEVER fires
            "cha-gate-p4"   to Law.Bernoulli(1, 4),
            "cha-gate-p8"   to Law.Bernoulli(1, 8),
            "cha-gate-p12"  to Law.Bernoulli(1, 12),
            "cha-gate-p15"  to Law.Bernoulli(1, 15),    // CHA F0 → the note ALWAYS fires (roll ≤ 14 < 15)
            "cha-gate-none" to Law.Bernoulli(1, 15),    // control: no CHA on the step → always fires
            "cha-clear-p8"  to Law.Bernoulli(5, 8),     // CHA 82 → a lost roll clears FX2, so pit reads 0 not 5
        )

        /** Which phrase-2 step carries which CHA probability (the gate's trials are counted from the
         *  step grid, since a gated note emits NOTHING and cannot be counted from the trace). */
        val CHA_GATE_STEPS: Map<String, List<Int>> = mapOf(
            "cha-gate-p0"   to listOf(0),
            "cha-gate-p4"   to listOf(2),
            "cha-gate-p8"   to listOf(4, 6),
            "cha-gate-p12"  to listOf(8),
            "cha-gate-p15"  to listOf(10),
            "cha-gate-none" to listOf(12),
        )
    }

    // ── Histogram ────────────────────────────────────────────────────────────────────────────────

    class Hist {
        private val counts = HashMap<Int, Int>()
        var n = 0; private set
        fun add(v: Int) { counts[v] = (counts[v] ?: 0) + 1; n++ }
        fun count(v: Int): Int = counts[v] ?: 0
        fun support(): List<Int> = counts.keys.sorted()

        /** Sorted support, runs of 3+ consecutive values collapsed to `a..b`. Must match ptrandom. */
        fun supportText(): String {
            val vs = support()
            if (vs.isEmpty()) return "-"
            val out = StringBuilder()
            var i = 0
            while (i < vs.size) {
                var j = i
                while (j + 1 < vs.size && vs[j + 1] == vs[j] + 1) j++
                if (out.isNotEmpty()) out.append(',')
                if (j - i >= 2) out.append("${vs[i]}..${vs[j]}") else {
                    for (k in i..j) { if (k > i) out.append(','); out.append(vs[k]) }
                }
                i = j + 1
            }
            return out.toString()
        }

        fun line(name: String, law: Law): String = "OBS $name n=$n support=${supportText()} expect=$law"

        /**
         * The shape check. Returns null if the histogram is consistent with [law], else a report.
         *
         * The thresholds are deliberately loose. A tight one would buy nothing — the errors this is
         * aimed at (a mode drawn 1/2 of the time instead of 1/3, a gate at 1.0 instead of 8/15) miss
         * by hundreds of sigma — while a tight one would make the JVM run flaky, since Kotlin's RNG
         * cannot be seeded and this really is a fresh sample every time. The exact claims live in the
         * support, which is compared byte-for-byte.
         */
        fun shapeProblem(name: String, law: Law): String? = when (law) {
            is Law.Uniform -> {
                val vs = support()
                val expected = n.toDouble() / vs.size
                val chi2 = vs.sumOf { v -> val d = count(v) - expected; d * d / expected }
                val df = vs.size - 1
                val limit = df + 6.0 * sqrt(2.0 * df) + 10.0
                if (chi2 > limit)
                    "$name: not uniform over ${vs.size} values — chi2=%.1f exceeds %.1f (df=$df, n=$n)"
                        .format(chi2, limit)
                else null
            }
            is Law.Bernoulli -> {
                val p = law.k / 15.0
                val rate = count(law.value).toDouble() / n
                // p(1-p) is 0 at both ends, so the deterministic gates (k=0, k=15) lean on the flat
                // slack — and on the support, which pins them exactly.
                val tol = 6.0 * sqrt(p * (1 - p) / n) + 0.01
                if (abs(rate - p) > tol)
                    "$name: P(draw==${law.value}) measured %.4f, expected %.4f = ${law.k}/15 (tolerance ±%.4f, n=$n)"
                        .format(rate, p, tol)
                else null
            }
        }
    }

    // ── Trace parsing (the C++ tool parses the same lines with the same rules) ───────────────────

    /** One NoteOn from a schema-v1 trace line: `<frame> <track> <instr> 90 note=.. vel=.. …`. */
    data class NoteOn(val frame: Long, val track: Int, val instrument: Int,
                      val note: Int, val vel: Int, val pit: Int, val arp: Int)

    private fun parseNoteOns(trace: String): List<NoteOn> {
        val out = ArrayList<NoteOn>()
        for (line in trace.split('\n')) {
            if (line.isEmpty() || !line[0].isDigit()) continue   // headers, T PLAY / T STOP
            val tok = line.split(' ')
            if (tok.size < 4 || tok[3] != "90") continue         // NoteOn only
            val fields = HashMap<String, String>()
            for (k in 4 until tok.size) {
                val eq = tok[k].indexOf('=')
                if (eq > 0) fields[tok[k].substring(0, eq)] = tok[k].substring(eq + 1)
            }
            fun f(key: String) = fields[key]?.toInt() ?: error("trace line missing $key: $line")
            out += NoteOn(
                frame = tok[0].toLong(),
                track = tok[1].toInt(),
                instrument = if (tok[2] == "-1") -1 else tok[2].toInt(16),
                note = f("note"), vel = f("vel"), pit = f("pit"), arp = f("arp"),
            )
        }
        return out
    }

    /** Fold one render's NoteOns into the histograms. Mirrored exactly in ptrandom. */
    private fun measure(notes: List<NoteOn>, framesPerStep: Long, hists: Map<String, Hist>) {
        // A NoteOn's step is exact: with no groove and no LAT anywhere in g8, every scheduled note
        // lands on a multiple of framesPerStep. Arpeggio retriggers do not — they are the ONLY notes
        // that fall between steps, and they are also the only ones carrying vel=-1, which is what
        // identifies them (far steadier than frame arithmetic).
        val chaSeen = HashSet<Int>()   // global step indices on track 2 that produced a note

        for (n in notes) {
            val isArpRetrig = n.vel == -1
            val globalStep = (n.frame / framesPerStep).toInt()
            val step = globalStep % 16

            if (!isArpRetrig) {
                assertEquals("track ${n.track} note at frame ${n.frame} is not on a step boundary — " +
                    "a groove or LAT crept into g8 and the step keying is no longer sound",
                    0L, n.frame % framesPerStep)
            }

            when (n.track) {
                // step 0 carries the PIT that SEEDS the column, not a RND draw — exclude it.
                0 -> if (step >= 2 && step % 2 == 0) hists["rnd-pit"]!!.add(n.pit)
                1 -> if (step % 2 == 0) {
                    hists["rnl-note"]!!.add(n.note)
                    hists["rnl-inst"]!!.add(n.instrument)
                }
                2 -> chaSeen += globalStep
                3 -> if (step % 2 == 0) hists["cha-clear-p8"]!!.add(n.pit)
                4 -> if (isArpRetrig) hists["arp-random"]!!.add(n.arp)
                5 -> if (step % 2 == 0) hists["rnl-left-pit"]!!.add(n.pit)
            }
        }

        // The CHA gate: a losing roll emits NOTHING, so its trials come from the step grid, not the
        // trace. Every (repeat, step) pair is one Bernoulli trial — 1 if the note survived, 0 if not.
        for ((name, steps) in CHA_GATE_STEPS) {
            val h = hists[name]!!
            for (repeat in 0 until PHRASE_REPEATS)
                for (s in steps) h.add(if ((repeat * 16 + s) in chaSeen) 1 else 0)
        }
    }

    // ── The test ────────────────────────────────────────────────────────────────────────────────

    /** g8's `.ptp` is a golden in its own right — both engines must load the same bytes. It is not in
     *  GoldenProjects.all (it can own no trace), so GoldenTraceTest never writes it; this does. */
    @Test
    fun projectFileMatchesOrIsGenerated() {
        val spec = GoldenProjects.random
        assertEquals(
            "golden '${spec.name}' must store project.name == its filename (see GoldenProjects.Spec)",
            spec.name, spec.build().name
        )
        val json = FileController(JvmFileSystem(testdata), FakeLogger).serializeProject(spec.build())
        val file = File(testdata, "${spec.name}.ptp")
        if (!file.exists()) {
            file.parentFile?.mkdirs()
            file.writeText(json, Charsets.UTF_8)
            println("generated ${file.name}")
        } else {
            assertEquals("${spec.name}.ptp drifted — regenerate it with the change", file.readText(Charsets.UTF_8), json)
        }
    }

    @Test
    fun randomDistributionsMatchOrAreGenerated() {
        val project = GoldenProjects.random.build()
        val sha = TraceHarness.projectSha1(
            FileController(JvmFileSystem(testdata), FakeLogger).serializeProject(project)
        )
        val fps = framesPerStep(project.tempo, SAMPLE_RATE)

        val hists = LAWS.keys.associateWith { Hist() }
        repeat(RENDERS) {
            // A fresh harness per render: fresh TrackStates, zero carry-over — exactly as
            // GoldenTraceTest drives the deterministic goldens. Only Random.Default's global stream
            // continues across them, which is precisely the app's own behavior.
            val trace = TraceHarness(SAMPLE_RATE).renderTrace(project, GoldenProjects.random.renderRows, sha)
            measure(parseNoteOns(trace), fps, hists)
        }

        // The control first: it validates the constant every CHA trial count is derived from. If g8's
        // chains ever stop being eight rows deep, this is what says so — rather than every gate
        // silently measuring against the wrong denominator.
        val control = hists["cha-gate-none"]!!
        assertEquals("the no-CHA control step did not fire on every walk — PHRASE_REPEATS is wrong " +
            "or the render is not walking all 8 chain rows",
            RENDERS * PHRASE_REPEATS, control.count(1))

        // (2) The shape — each engine checks its own draws against the law.
        val shapeProblems = LAWS.mapNotNull { (name, law) -> hists[name]!!.shapeProblem(name, law) }
        if (shapeProblems.isNotEmpty())
            fail("the Kotlin sequencer's random draws do not follow their expected law:\n  " +
                 shapeProblems.joinToString("\n  "))

        // (1) The support + draw counts — exact, and the file both engines are held to.
        val lines = LAWS.map { (name, law) -> hists[name]!!.line(name, law) }
        val content = header() + lines.joinToString("\n") + "\n"

        if (!goldenFile.exists()) {
            goldenFile.parentFile?.mkdirs()
            goldenFile.writeText(content, Charsets.UTF_8)
            println("generated ${goldenFile.name}:\n" + lines.joinToString("\n"))
            return
        }

        val committed = goldenFile.readText(Charsets.UTF_8).replace("\r", "")
        val committedObs = committed.lines().filter { it.startsWith("OBS ") }
        assertTrue("s7-random.txt holds ${committedObs.size} observables, the test measures ${lines.size}",
            committedObs.size == lines.size)

        val mismatches = lines.withIndex().mapNotNull { (i, line) ->
            val other = committedObs[i]
            if (other == line) null else "  golden: $other\n  kotlin: $line"
        }
        if (mismatches.isNotEmpty())
            fail("the Kotlin sequencer's random support/draw-count drifted from the golden:\n" +
                 mismatches.joinToString("\n\n"))
    }

    private fun header(): String = """
        |# songcore S7 — the random-FX distribution golden.  MEASURED FROM THE REAL KOTLIN SEQUENCER
        |# (PlaybackController + kotlin.random.Random) over $RENDERS renders of g8-random.ptp at $SAMPLE_RATE Hz.
        |# Written by S7RandomGoldenTest; read back by it AND by tools/ptrandom, which holds C++ songcore
        |# to the same lines.
        |#
        |# This is the one golden that is not a byte-for-byte recording of an event stream, because the FX
        |# it covers — CHA, RND, RNL, ARP mode RANDOM — draw from a clock-seeded RNG. Kotlin does not repeat
        |# itself either, so there is no stream to record. What is invariant, and what is compared, is:
        |#
        |#   n=        the number of draws one render makes. Deterministic: the random FX decide WHICH value
        |#             comes out, never HOW MANY are drawn. Compared exactly — it proves both engines ran the
        |#             same code paths the same number of times.
        |#   support=  the set of values a draw can produce, `a..b` for runs. Compared exactly. This is where
        |#             the bugs live: an off-by-one, a closed interval where Kotlin's is half-open, or a stub
        |#             pinned to the low end of the range (which is what songcore shipped until S7 — every
        |#             other check stayed green throughout). Not flaky: at these sample sizes the chance of a
        |#             reachable value going unseen is about e^-200.
        |#   expect=   the law the draws follow. Checked STATISTICALLY by each engine against its own
        |#             histogram (chi-square / Bernoulli, 6-sigma margins), never across engines — two random
        |#             samples never agree exactly, and demanding that they do is how a test becomes flaky.
        |#             The values here are not asserted from theory: the Kotlin measurement validates them.
        |#
        |# Regenerate deliberately: delete this file, rerun S7RandomGoldenTest, commit the diff with the change.
        |
    """.trimMargin()
}
