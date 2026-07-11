package com.conanizer.pockettracker.trace

import com.conanizer.pockettracker.core.logic.FileController
import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Assert.fail
import org.junit.Assume.assumeTrue
import org.junit.Test
import java.io.File

/**
 * The songcore Phase 1 measuring stick.
 *
 * Regenerates every golden artifact (project .ptp files + event traces, render + live, at 44100
 * and 48000 Hz) from the REAL Kotlin sequencer and compares byte-for-byte against the committed
 * files in /testdata:
 *
 *  - Missing files are written (first run / deliberate regeneration after deleting them).
 *  - Existing files must match exactly — any sequencing-behavior drift in zone C fails here.
 *    This test is the Kotlin-side conformance guard until the Kotlin path is deleted (S7).
 *
 * To deliberately regenerate after an intentional behavior change: delete /testdata/traces
 * (and the .ptp files if the schema changed), rerun, and commit the diff with the change.
 */
class GoldenTraceTest {

    companion object {
        private val SAMPLE_RATES = intArrayOf(44100, 48000)

        private fun repoRoot(): File {
            var dir = File(System.getProperty("user.dir")!!).absoluteFile
            while (true) {
                if (File(dir, "settings.gradle.kts").exists() || File(dir, "settings.gradle").exists()) return dir
                dir = dir.parentFile ?: error("repo root not found from ${System.getProperty("user.dir")}")
            }
        }

        private val testdata: File get() = File(repoRoot(), "testdata")

        /** Every golden artifact as (path relative to /testdata) → exact content. */
        private fun generateAll(): LinkedHashMap<String, String> {
            val out = LinkedHashMap<String, String>()
            val fileController = FileController(JvmFileSystem(testdata), FakeLogger)
            for (spec in GoldenProjects.all) {
                val project = spec.build()
                val json = fileController.serializeProject(project)
                val sha = TraceHarness.projectSha1(json)
                out["${spec.name}.ptp"] = json
                for (sr in SAMPLE_RATES) {
                    out["traces/${spec.name}.$sr.render.trace"] =
                        TraceHarness(sr).renderTrace(project, spec.renderRows, sha)
                    for (mode in spec.liveModes) {
                        val tag = mode.kind.lowercase() + Integer.toHexString(mode.arg).padStart(2, '0')
                        // Fresh project per live trace: live scheduling mutates nothing in the
                        // project, but isolation keeps that a fact rather than an assumption.
                        out["traces/${spec.name}.$sr.$tag.trace"] =
                            TraceHarness(sr).liveTrace(spec.build(), mode, sha)
                    }
                }
            }
            return out
        }
    }

    @Test
    fun goldensMatchOrAreGenerated() {
        val artifacts = generateAll()
        val generated = mutableListOf<String>()
        val mismatches = mutableListOf<String>()

        for ((rel, content) in artifacts) {
            val file = File(testdata, rel)
            if (!file.exists()) {
                file.parentFile?.mkdirs()
                file.writeText(content, Charsets.UTF_8)
                generated += rel
            } else {
                val committed = file.readText(Charsets.UTF_8)
                if (committed != content) {
                    val detail = if (rel.endsWith(".trace"))
                        TraceCompare.diff(committed, content) ?: "(differs only in emission order — semantic order change!)"
                    else "(serialized project JSON drifted)"
                    mismatches += "$rel: $detail"
                }
            }
        }

        if (mismatches.isNotEmpty()) {
            fail(
                "golden drift in ${mismatches.size} file(s) — zone-C sequencing behavior changed.\n" +
                "If unintentional: fix the regression. If deliberate: delete /testdata/traces, rerun, " +
                "commit the regenerated goldens WITH the behavior change.\n\n" +
                mismatches.joinToString("\n\n")
            )
        }
        if (generated.isNotEmpty()) {
            println("generated ${generated.size} golden file(s):\n  " + generated.joinToString("\n  "))
        }
    }

    /**
     * A golden's stored `name` must equal its `.ptp` filename, or the device cross-check can never
     * pass. Loading a project through the file browser renames it to the file's basename
     * (`AppInputDispatcher`: `result.project.name = item.file.nameWithoutExtension.take(20)`), so a
     * golden named "G1BASICS" in a file called `g1-basics.ptp` comes back from a device load with
     * different bytes — and the trace header's `project=` sha, taken over the whole serialized
     * project, then matches no golden.
     *
     * That failure cost a full debugging session in S5: the device's events were byte-perfect against
     * every golden, and the run was still rejected — on identity, before a single event was compared.
     * Nothing reads `name`, so no amount of staring at the audio path could find it.
     */
    @Test
    fun goldenNameMatchesFilename() {
        for (spec in GoldenProjects.all) {
            assertEquals(
                "golden '${spec.name}' stores project.name='${spec.build().name}' but its file is " +
                    "'${spec.name}.ptp' — the file-browser load renames the project to the filename, so " +
                    "this golden can never round-trip on device (see the doc on GoldenProjects.Spec)",
                spec.name, spec.build().name
            )
        }
    }

    @Test
    fun tracesAreDeterministic() {
        val first = generateAll()
        val second = generateAll()
        assertEquals("golden generation is not deterministic (key sets differ)", first.keys, second.keys)
        for ((rel, content) in first) {
            assertEquals("nondeterministic golden: $rel", content, second[rel])
        }
    }

    @Test
    fun traceShapeSanity() {
        val artifacts = generateAll()
        val traces = artifacts.filterKeys { it.endsWith(".trace") }
        assertTrue(traces.isNotEmpty())
        for ((rel, content) in traces) {
            assertTrue("$rel: missing header", content.startsWith("# schema=1 "))
            assertTrue("$rel: missing T PLAY", content.contains("\nT PLAY "))
            assertTrue("$rel: missing T STOP", content.trimEnd().endsWith("T STOP"))
            assertTrue("$rel: no NoteOn events", content.lines().any { it.contains(" 90 note=") })
            // Canonicalization must be idempotent
            val canon = TraceCompare.canonicalize(content)
            assertEquals("$rel: canonicalize not idempotent", canon, TraceCompare.canonicalize(canon))
        }
        // The IB-10 divergence must be real: g1 live SONG schedules the muted track 1, render skips it
        val render = artifacts["traces/g1-basics.44100.render.trace"]!!
        val liveSong = artifacts["traces/g1-basics.44100.song00.trace"]!!
        val onTrack1 = Regex("^\\d+ 1 ")
        assertTrue("muted-track events unexpectedly present in render", render.lines().none { onTrack1.containsMatchIn(it) })
        assertTrue("muted-track events missing from live SONG", liveSong.lines().any { onTrack1.containsMatchIn(it) })
    }

    /**
     * Device cross-check (S1 acceptance): pull `Renders/event.trace` from a debug build into
     * /testdata/device/ — ANY filename ending in .trace, no renaming needed — and rerun this
     * test. Each PLAY..STOP session in the file is identified from its own header (project sha
     * → golden project, sr, mode, start arg) and compared against the matching golden:
     *
     *  - render sessions are self-terminating → byte equality after canonical sort;
     *  - live sessions stop wherever the user pressed stop, which never lands on the golden's
     *    horizon — but the emission stream is deterministic, so the device events must be a
     *    line-for-line PREFIX of the golden (or vice versa). Preview-lane (track 8) noise from
     *    stray button presses is ignored.
     *
     * Byte-identical float fields here prove JVM↔ART float identity (event-schema §5).
     */
    @Test
    fun deviceTracesMatchGoldens() {
        val deviceFiles = File(testdata, "device")
            .listFiles { f -> f.isFile && f.name.endsWith(".trace") }.orEmpty()
        assumeTrue("no device traces present — skipping cross-check", deviceFiles.isNotEmpty())

        val fileController = FileController(JvmFileSystem(testdata), FakeLogger)
        val shaToName = GoldenProjects.all.associate { spec ->
            TraceHarness.projectSha1(fileController.serializeProject(spec.build())) to spec.name
        }

        val problems = mutableListOf<String>()
        var matched = 0
        for (file in deviceFiles) {
            val sessions = splitSessions(file.readText(Charsets.UTF_8).replace("\r", ""))
            if (sessions.isEmpty()) { problems += "${file.name}: no trace sessions in file"; continue }
            for ((i, session) in sessions.withIndex()) {
                val problem = checkDeviceSession(session, shaToName, "${file.name} session ${i + 1}")
                if (problem == null) matched++ else problems += problem
            }
        }
        if (problems.isNotEmpty()) fail("device cross-check failed:\n\n" + problems.joinToString("\n\n"))
        println("device cross-check OK: $matched session(s) matched their goldens")
    }

    /** The live-prefix comparator must accept an early-stopped, preview-noised device session
     *  and reject a corrupted one — proven here in-memory so the on-device run can be trusted. */
    @Test
    fun deviceSessionCheckerSelfTest() {
        val spec = GoldenProjects.all.first()
        val mode = spec.liveModes.first()
        val fileController = FileController(JvmFileSystem(testdata), FakeLogger)
        val sha = TraceHarness.projectSha1(fileController.serializeProject(spec.build()))
        val golden = TraceHarness(48000).liveTrace(spec.build(), mode, sha)

        // Simulate a device session: stop early (first 30 events), sprinkle preview noise.
        val lines = golden.split('\n').filter { it.isNotEmpty() }
        val events = lines.filter { it[0].isDigit() }.take(30)
        val device = mutableListOf(lines[0], lines[1])
        device += events.take(7)
        device += "12345 8 FF 90 note=60 vel=127 velGain=0x3F800000 volGain=0x3F800000 pan=0x3F000000 start=-1 slice=-1 transpose=0 pit=0 arp=0 tableId=-1 tableRow=-1 pslOff=0x00000000 pslDur=0x00000000 pbnRate=0x00000000 vibSpd=0x00000000 vibDep=0x00000000"
        device += events.drop(7)
        device += "T STOP"
        assertEquals(null, compareLiveSession(golden, device, "selftest"))

        // A corrupted float must be caught.
        val corrupted = device.map { it.replace("velGain=0x3E820610", "velGain=0x3E820611") }
        assertTrue(compareLiveSession(golden, corrupted, "selftest") != null)
    }

    /** Split a trace file into sessions — each starts at a `# schema=` header line. */
    private fun splitSessions(text: String): List<List<String>> {
        val sessions = mutableListOf<MutableList<String>>()
        for (line in text.split('\n')) {
            if (line.startsWith("# schema=")) sessions.add(mutableListOf())
            if (line.isNotEmpty()) sessions.lastOrNull()?.add(line)
        }
        return sessions
    }

    /** Identify the golden for one device session and compare. Null = OK, else a report. */
    private fun checkDeviceSession(
        session: List<String>, shaToName: Map<String, String>, where: String
    ): String? {
        val header = session.getOrNull(0) ?: return "$where: empty session"
        val fields = header.split(' ').mapNotNull { tok ->
            val eq = tok.indexOf('=')
            if (eq > 0) tok.substring(0, eq) to tok.substring(eq + 1) else null
        }.toMap()
        val sr = fields["sr"] ?: return "$where: header has no sr"
        val mode = fields["mode"] ?: return "$where: header has no mode"
        val name = shaToName[fields["project"]]
            ?: return "$where: project ${fields["project"]} matches no golden — the project that was " +
                      "traced is not byte-identical to any golden .ptp, so its events can't be " +
                      "attributed. To see WHAT differs rather than guess: a debug build dumps the exact " +
                      "bytes this sha is taken over to Renders/event.trace.ptp when TRACE turns ON — " +
                      "diff that against the .ptp you loaded. (Most likely: the on-device copy was " +
                      "edited or re-saved; re-copy the pristine .ptp from /testdata and reload it.)"
        val tPlay = session.getOrNull(1)?.takeIf { it.startsWith("T PLAY ") }
            ?: return "$where: missing T PLAY line"

        // Golden filename tag from the T PLAY line: RENDER → render; SONG row=00 → song00; …
        val play = tPlay.removePrefix("T PLAY ").split(' ')
        val tag = if (play[0] == "RENDER") "render"
                  else play[0].lowercase() + (play.getOrNull(1)?.substringAfter('=')?.lowercase() ?: "")
        val goldenFile = File(testdata, "traces/$name.$sr.$tag.trace")
        if (!goldenFile.exists())
            return "$where: no golden ${goldenFile.name} — playback didn't start where the golden " +
                   "does ($tPlay). For live checks start from the top (song row 0 / phrase 0)."
        val golden = goldenFile.readText(Charsets.UTF_8)

        return if (mode == "render") {
            TraceCompare.diff(golden, session.joinToString("\n", postfix = "\n"))
                ?.let { "$where vs ${goldenFile.name}:\n$it" }
        } else {
            compareLiveSession(golden, session, "$where vs ${goldenFile.name}")
        }
    }

    /** Live sessions: header + T PLAY must match exactly; events prefix-compare in raw emission
     *  order (deterministic per FIX-1), ignoring the preview lane (track 8). */
    private fun compareLiveSession(golden: String, session: List<String>, where: String): String? {
        val goldenLines = golden.split('\n')
        if (goldenLines[0] != session[0])
            return "$where: header differs\n  golden: ${goldenLines[0]}\n  device: ${session[0]}"
        if (goldenLines[1] != session[1])
            return "$where: T PLAY differs\n  golden: ${goldenLines[1]}\n  device: ${session[1]}"
        fun events(lines: List<String>) = lines.filter { it.isNotEmpty() && it[0].isDigit() }
        val deviceEvents = events(session).filterNot { it.split(' ')[1] == "8" }
        val goldenEvents = events(goldenLines)
        val k = minOf(deviceEvents.size, goldenEvents.size)
        if (k < 20) return "$where: only $k comparable events — let it play a few phrases before stopping"
        for (i in 0 until k) {
            if (deviceEvents[i] != goldenEvents[i]) {
                return "$where: first divergence at event ${i + 1} of $k:\n" +
                       "  golden: ${goldenEvents[i]}\n  device: ${deviceEvents[i]}"
            }
        }
        return null
    }
}
