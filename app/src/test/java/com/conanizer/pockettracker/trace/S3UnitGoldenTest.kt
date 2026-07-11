package com.conanizer.pockettracker.trace

import com.conanizer.pockettracker.core.data.Groove
import com.conanizer.pockettracker.core.data.PhraseStep
import com.conanizer.pockettracker.core.data.TICS_PER_STEP
import com.conanizer.pockettracker.core.data.byteToSignedSemitones
import com.conanizer.pockettracker.core.data.framesPerStep
import com.conanizer.pockettracker.core.logic.EffectProcessor
import com.conanizer.pockettracker.core.logic.collectUsedInstruments
import org.junit.Assert.assertEquals
import org.junit.Test
import java.io.File

/**
 * songcore S3 unit goldens — the measuring stick for the *pure* pieces ported to C++ this session:
 * framesPerStep (Double→Long truncation), the groove timing math, byteToSignedSemitones, and
 * EffectProcessor.resolveStepParams. Emits one text golden, `testdata/units/s3-units.txt`, whose
 * every line is `<inputs> => <outputs>` computed by the REAL Kotlin functions.
 *
 * The C++ twins live in native/songcore/{timing,effects,traversal}.h; `tools/ptresolve` re-parses
 * each line's inputs, recomputes the RHS in C++, and byte-compares — proving JVM↔C++ equivalence
 * including the binary32 `volume` divide and the framesPerStep double rounding (the only real
 * cross-language numeric risks). collectUsedInstruments is proven against the real /testdata
 * projects (C++ loads the .ptp, Kotlin computes on the in-memory build — byte-exact per S2).
 *
 * Like GoldenTraceTest: missing file → generated; existing file → byte-compared (drift guard).
 * To regenerate after an intentional change: delete testdata/units/s3-units.txt, rerun, commit.
 */
class S3UnitGoldenTest {

    companion object {
        private fun repoRoot(): File {
            var dir = File(System.getProperty("user.dir")!!).absoluteFile
            while (true) {
                if (File(dir, "settings.gradle.kts").exists() || File(dir, "settings.gradle").exists()) return dir
                dir = dir.parentFile ?: error("repo root not found from ${System.getProperty("user.dir")}")
            }
        }

        private val unitsFile: File get() = File(repoRoot(), "testdata/units/s3-units.txt")
    }

    private val fx = EffectProcessor(FakeLogger)

    // ── formatting (must match tools/ptresolve/main.cpp byte-for-byte) ──────────────────────────
    private fun optI(v: Int?): String = v?.toString() ?: "-"
    private fun optL(v: Long?): String = v?.toString() ?: "-"
    private fun f32(v: Float): String = "0x%08X".format(java.lang.Float.floatToRawIntBits(v))
    private fun hex2(v: Int): String = "%02X".format(v and 0xFF)
    private fun vol(byte: Int): Float = (byte and 0xFF) / 255f   // VolumeUtils.hexToFloat

    /** A 16-slot groove whose active window is [active], the rest end-marker (-1). */
    private fun groove(vararg active: Int): Groove {
        val steps = IntArray(16) { -1 }
        for (i in active.indices) steps[i] = active[i]
        return Groove(0, steps)
    }
    private fun stepsCsv(g: Groove): String = g.steps.joinToString(",")

    // Effect codes whose resolveStepParams arm writes a field (the sweep targets).
    private val resolvableFx = listOf(
        EffectProcessor.FX_OFFSET, EffectProcessor.FX_VOLUME, EffectProcessor.FX_KILL,
        EffectProcessor.FX_ARC, EffectProcessor.FX_REPEAT, EffectProcessor.FX_HOP,
        EffectProcessor.FX_PSL, EffectProcessor.FX_PBN, EffectProcessor.FX_PVB, EffectProcessor.FX_PVX,
        EffectProcessor.FX_LAT, EffectProcessor.FX_PAN, EffectProcessor.FX_RSEND, EffectProcessor.FX_DSEND,
        EffectProcessor.FX_BCK, EffectProcessor.FX_EQN, EffectProcessor.FX_EQM,
        EffectProcessor.FX_TBL, EffectProcessor.FX_THO, EffectProcessor.FX_GRV,
        EffectProcessor.FX_PIT, EffectProcessor.FX_SLI,
    )
    // Effect codes with NO resolved field — must leave every output at its default (proves no-op).
    private val nonResolvingFx = listOf(
        EffectProcessor.FX_ARPEGGIO, EffectProcessor.FX_CHA, EffectProcessor.FX_RND,
        EffectProcessor.FX_RNL, EffectProcessor.FX_TIC,
    )

    private fun rsp(
        fx1: Pair<Int, Int>, fx2: Pair<Int, Int>, fx3: Pair<Int, Int>, base: Long, dvol: Float
    ): String {
        val step = PhraseStep(
            fx1Type = fx1.first, fx1Value = fx1.second,
            fx2Type = fx2.first, fx2Value = fx2.second,
            fx3Type = fx3.first, fx3Value = fx3.second,
        )
        val r = fx.resolveStepParams(step, base, dvol)
        val lhs = "RSP fx1=${hex2(fx1.first)},${hex2(fx1.second)} " +
                "fx2=${hex2(fx2.first)},${hex2(fx2.second)} " +
                "fx3=${hex2(fx3.first)},${hex2(fx3.second)} base=$base dvol=${f32(dvol)}"
        val rhs = "start=${r.startPoint} vol=${f32(r.volume)} vxx=${if (r.volumeFromVxx) 1 else 0} " +
                "kill=${optL(r.killAtFrame)} koff=${r.killOffsetTicks} arc=${optI(r.arcValue)} " +
                "rep=${optI(r.repeatCount)} repr=${optI(r.repeatVolRamp)} hop=${optI(r.hopValue)} " +
                "psl=${optI(r.pslDuration)} pbn=${optI(r.pbnValue)} pvb=${optI(r.pvbValue)} " +
                "pvx=${optI(r.pvxValue)} del=${optI(r.delayTicks)} tbl=${optI(r.tableOverride)} " +
                "tho=${optI(r.tableHopTarget)} grv=${optI(r.grooveId)} pit=${optI(r.pitSemitones)} " +
                "sli=${optI(r.sliIndex)} pan=${optI(r.panValue)} rev=${optI(r.reverbSendValue)} " +
                "dsend=${optI(r.delaySendValue)} bck=${optI(r.bckValue)} eqn=${optI(r.eqnSlot)} " +
                "eqm=${optI(r.eqmSlot)}"
        return "$lhs => $rhs\n"
    }

    private fun generate(): String {
        val sb = StringBuilder()
        sb.append("# songcore S3 unit goldens — framesPerStep / groove timing / byteToSignedSemitones / ")
        sb.append("resolveStepParams / collectUsedInstruments\n")
        sb.append("# Generated by S3UnitGoldenTest from the Kotlin sequencer. tools/ptresolve re-parses each ")
        sb.append("line's inputs, recomputes the RHS in C++ (native/songcore/*.h), and byte-compares.\n")
        sb.append("# format: <KIND> <inputs...> => <outputs...>  — ints decimal, hex bytes 2-digit UPPER, ")
        sb.append("floats 0x+8 hex of binary32 bits, an absent optional = '-'.\n")

        sb.append("\n# framesPerStep(tempo, sr): (60000.0 / tempo / 4.0 * sr / 1000.0).toLong()\n")
        for (tempo in listOf(1, 40, 63, 120, 128, 137, 140, 150, 200, 255))
            for (sr in listOf(44100, 48000))
                sb.append("FPS tempo=$tempo sr=$sr => fps=${framesPerStep(tempo, sr)}\n")
        for (sr in listOf(22050, 32000, 96000))
            sb.append("FPS tempo=128 sr=$sr => fps=${framesPerStep(128, sr)}\n")

        sb.append("\n# framesPerTic = framesPerStep / TICS_PER_STEP (Long integer division)\n")
        for (tempo in listOf(120, 128, 137, 140, 200))
            for (sr in listOf(44100, 48000))
                sb.append("FPT tempo=$tempo sr=$sr => fpt=${framesPerStep(tempo, sr) / TICS_PER_STEP}\n")

        sb.append("\n# byteToSignedSemitones — two's-complement decode, boundary at 0x80\n")
        for (b in listOf(0, 1, 64, 127, 128, 129, 200, 255))
            sb.append("BSS b=$b => semi=${byteToSignedSemitones(b)}\n")

        val grooves = listOf(
            groove(8, 4),
            groove(6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6),
            groove(6, 0, 12),
            groove(12, 12, 12, 12),
            groove(),
            groove(0),
        )
        sb.append("\n# Groove.activeLength — steps before the first -1 end marker\n")
        for (g in grooves)
            sb.append("GACT steps=${stepsCsv(g)} => active=${g.activeLength()}\n")

        sb.append("\n# Groove.getTicksForStep(pos) — wraps around the active window; empty groove -> TICS_PER_STEP\n")
        for (g in grooves)
            for (pos in 0..5)
                sb.append("GTIC steps=${stepsCsv(g)} pos=$pos => ticks=${g.getTicksForStep(pos)}\n")

        sb.append("\n# groove step duration in frames, as PlaybackController.schedulePhrase composes it\n")
        for ((t, sr) in listOf(137 to 44100, 128 to 48000)) {
            val fps = framesPerStep(t, sr)
            val fpt = fps / TICS_PER_STEP
            for (g in listOf(grooves[0], grooves[2], grooves[3], grooves[4]))
                for (pos in 0..3) {
                    val dur = if (g.activeLength() > 0) fpt * g.getTicksForStep(pos) else fps
                    sb.append("GDUR tempo=$t sr=$sr steps=${stepsCsv(g)} pos=$pos => dur=$dur\n")
                }
        }

        sb.append("\n# resolveStepParams — baseline (no FX): volume passes through defaultVolume, rest default\n")
        for (dv in listOf(0xFF, 0xC0, 0x80, 0x00))
            for (base in listOf(0L, 977L))
                sb.append(rsp(0 to 0, 0 to 0, 0 to 0, base, vol(dv)))

        sb.append("\n# resolveStepParams — single-FX sweep (each resolvable code x representative values)\n")
        for (type in resolvableFx)
            for (v in listOf(0x00, 0x01, 0x7F, 0x80, 0xFF, 0x5A))
                sb.append(rsp(type to v, 0 to 0, 0 to 0, 977L, 1.0f))

        sb.append("\n# resolveStepParams — non-resolving FX (ARP/CHA/RND/RNL/TIC) must set no field\n")
        for (type in nonResolvingFx)
            sb.append(rsp(type to 0x5A, 0 to 0, 0 to 0, 977L, 1.0f))

        sb.append("\n# resolveStepParams — REPEAT RXY nibble logic (y!=0 -> count=y,ramp=x; y=0 -> count=x,ramp=0)\n")
        for (v in listOf(0x10, 0x50, 0xA0, 0x0A, 0x55, 0x23))
            sb.append(rsp(EffectProcessor.FX_REPEAT to v, 0 to 0, 0 to 0, 977L, 1.0f))

        sb.append("\n# resolveStepParams — multi-slot: last-wins + several effects coexisting\n")
        sb.append(rsp(EffectProcessor.FX_GRV to 0x10, 0 to 0, EffectProcessor.FX_GRV to 0x20, 977L, 1.0f))
        sb.append(rsp(EffectProcessor.FX_OFFSET to 0x40, EffectProcessor.FX_VOLUME to 0x80, EffectProcessor.FX_KILL to 0x06, 977L, 1.0f))
        sb.append(rsp(EffectProcessor.FX_PIT to 0xF8, EffectProcessor.FX_SLI to 0x02, EffectProcessor.FX_PAN to 0xC0, 977L, 1.0f))
        sb.append(rsp(EffectProcessor.FX_VOLUME to 0x30, EffectProcessor.FX_KILL to 0x00, EffectProcessor.FX_HOP to 0x04, 977L, 1.0f))

        sb.append("\n# resolveStepParams — KIL echoes baseFrame into killAtFrame (+ koff)\n")
        for (base in listOf(0L, 977L, 123456L))
            sb.append(rsp(EffectProcessor.FX_KILL to 0x00, 0 to 0, 0 to 0, base, 1.0f))
        sb.append(rsp(EffectProcessor.FX_KILL to 0x0C, 0 to 0, 0 to 0, 5169L, 1.0f))

        sb.append("\n# collectUsedInstruments over the real /testdata projects (C++ loads the .ptp)\n")
        for (spec in GoldenProjects.all) {
            val proj = spec.build()
            val songLen = (0..7).maxOf { proj.tracks[it].chainRefs.size }
            val end = (songLen - 1).coerceAtLeast(0)
            val full = proj.collectUsedInstruments(0, end).sorted().joinToString(",").ifEmpty { "-" }
            val first = proj.collectUsedInstruments(0, 0).sorted().joinToString(",").ifEmpty { "-" }
            sb.append("USEDINST project=${spec.name} start=0 end=$end => inst=$full\n")
            sb.append("USEDINST project=${spec.name} start=0 end=0 => inst=$first\n")
        }

        return sb.toString()
    }

    @Test
    fun unitGoldenMatchesOrIsGenerated() {
        val content = generate()
        val file = unitsFile
        if (!file.exists()) {
            file.parentFile?.mkdirs()
            file.writeText(content, Charsets.UTF_8)
            println("generated ${file.relativeTo(repoRoot())}  (${content.lines().size} lines)")
        } else {
            val committed = file.readText(Charsets.UTF_8)
            if (committed != content) {
                val cl = committed.lines(); val gl = content.lines()
                val at = (0 until minOf(cl.size, gl.size)).firstOrNull { cl[it] != gl[it] }
                val detail = if (at != null)
                    "first diff at line ${at + 1}:\n  committed: ${cl[at]}\n  generated: ${gl[at]}"
                else "length differs: committed ${cl.size} lines, generated ${gl.size} lines"
                assertEquals(
                    "S3 unit golden drift — a ported pure function changed.\nIf deliberate: delete " +
                        "testdata/units/s3-units.txt, rerun, commit.\n$detail",
                    committed, content
                )
            }
        }
    }

    @Test
    fun generationIsDeterministic() {
        assertEquals(generate(), generate())
    }
}
