package com.conanizer.pockettracker.trace

import com.conanizer.pockettracker.core.audio.AudioEngine
import com.conanizer.pockettracker.core.audio.IAudioBackend
import com.conanizer.pockettracker.core.data.Instrument
import com.conanizer.pockettracker.core.data.InstrumentType
import com.conanizer.pockettracker.core.data.ModDest
import com.conanizer.pockettracker.core.data.ModSlot
import com.conanizer.pockettracker.core.data.ModType
import com.conanizer.pockettracker.core.data.Note
import com.conanizer.pockettracker.core.data.Project
import org.junit.Assert.assertEquals
import org.junit.Test
import java.io.File

/**
 * songcore S5 — the measuring stick for the piece the conformance trace CANNOT see.
 *
 * The trace is captured at the router, ABOVE the consumer (event-schema §6: "everything below the seam
 * is consumer-side derivation and never rides the trace"). So all 32 golden traces can be byte-green
 * while the C++ consumer still computes a wrong frequency, a wrong slice window, or a wrong envelope —
 * and the only symptom would be "it sounds a bit off". This test closes that hole.
 *
 * It drives the REAL Kotlin `AudioEngine.scheduleNote()` over a matrix of instruments and seam
 * arguments, with a backend that records the exact engine calls it produces, and writes them to
 * `testdata/units/s5-consumer.txt` — floats as raw binary32 bits, so nothing can pass by being close.
 * `tools/ptvoice` then replays the same inputs through the C++ derivation (native/songcore/voice_derive.h)
 * and byte-compares. Same contract as S3's s3-units.txt / ptresolve.
 *
 * The cases target every branch that derives rather than copies: ROOT/detune/sample-rate-ratio in the
 * base frequency, the CUT/TRU/SLI slice windows, PIT and ARP pitch shifts (after slice selection), the
 * SF root transpose + velocity derivation + detune-as-pitch-wheel, the tick→frame conversions at
 * several tempos and sample rates, and every modulation type/destination.
 *
 * Missing file → generated. Existing file → byte-compared (the drift guard).
 * To regenerate deliberately: delete the file, rerun, commit.
 */
class S5ConsumerGoldenTest {

    companion object {
        private fun repoRoot(): File {
            var dir = File(System.getProperty("user.dir")!!).absoluteFile
            while (true) {
                if (File(dir, "settings.gradle.kts").exists() || File(dir, "settings.gradle").exists()) return dir
                dir = dir.parentFile ?: error("repo root not found from ${System.getProperty("user.dir")}")
            }
        }

        private val unitsFile: File get() = File(repoRoot(), "testdata/units/s5-consumer.txt")

        /** The fake backend reports this for every sample — slice windows are (marker × 255 / this). */
        const val SAMPLE_LENGTH = 44100
    }

    // ── the recording backend: captures the engine calls the consumer makes ──────────────────────
    // Delegates every inert method to FakeAudioBackend and overrides only what the consumer emits.
    private class RecordingBackend(sampleRate: Int) : IAudioBackend by FakeAudioBackend(sampleRate) {
        val calls = mutableListOf<String>()

        private fun f32(v: Float) = "0x%08X".format(java.lang.Float.floatToRawIntBits(v))

        override fun setTempo(tempo: Int) { calls += "TEMPO $tempo" }

        override fun loadTable(tableId: Int, rowData: ByteArray) { calls += "TABLE $tableId" }

        override fun setInstrumentModulation(
            sampleId: Int, slotIndex: Int, type: Int, dest: Int, amount: Float,
            attackSamples: Int, holdSamples: Int, decaySamples: Int,
            sustainLevel: Float, lfoHz: Float, oscShape: Int, releaseSamples: Int, lfoTrigMode: Int
        ) {
            calls += "MOD $sampleId $slotIndex $type $dest ${f32(amount)} " +
                "$attackSamples $holdSamples $decaySamples ${f32(sustainLevel)} ${f32(lfoHz)} " +
                "$oscShape $releaseSamples $lfoTrigMode"
        }

        override fun clearInstrumentModulation(sampleId: Int) { calls += "MODCLR $sampleId" }

        override fun setInstrumentEqSlot(instrId: Int, slot: Int) { calls += "EQSLOT $instrId $slot" }

        override fun setInstrumentSendLevels(instrId: Int, reverbSend: Int, delaySend: Int) {
            calls += "SENDS $instrId $reverbSend $delaySend"
        }

        override fun setSoundfontEnvelopeOverrides(instrumentId: Int, atk: Int, dec: Int, sus: Int, rel: Int) {
            calls += "SFENV $instrumentId $atk $dec $sus $rel"
        }

        override fun setInstrumentParams(
            instrumentId: Int, startPoint: Int, endPoint: Int, reverse: Boolean, loopMode: Int,
            loopStart: Int, loopEnd: Int, drive: Int, crush: Int, downsample: Int,
            filterType: Int, filterCut: Int, filterRes: Int
        ) {
            calls += "PARAMS $instrumentId $startPoint $endPoint ${if (reverse) 1 else 0} $loopMode " +
                "$loopStart $loopEnd $drive $crush $downsample $filterType $filterCut $filterRes"
        }

        override fun scheduleNoteWithTable(
            frame: Long, sampleId: Int, trackId: Int, freq: Float, baseFreq: Float, vol: Float,
            phraseVol: Float, pan: Float, startPointOverride: Int, endPointOverride: Int,
            tableId: Int, tableTicRate: Int, noteOctave: Int, notePitch: Int,
            pslInitialOffset: Float, pslDuration: Float, pbnRate: Float,
            vibratoSpeed: Float, vibratoDepth: Float, tableStartRow: Int
        ) {
            calls += "NOTE $frame $sampleId $trackId ${f32(freq)} ${f32(baseFreq)} ${f32(vol)} " +
                "${f32(phraseVol)} ${f32(pan)} $startPointOverride $endPointOverride $tableId " +
                "$tableTicRate $noteOctave $notePitch ${f32(pslInitialOffset)} ${f32(pslDuration)} " +
                "${f32(pbnRate)} ${f32(vibratoSpeed)} ${f32(vibratoDepth)} $tableStartRow"
        }

        override fun scheduleSoundfontNote(
            frame: Long, trackId: Int, sfSlot: Int, midiNote: Int, velocity: Int, vol: Float, pan: Float,
            bank: Int, preset: Int, pslInitialOffset: Float, pslDuration: Float, pbnRate: Float,
            vibratoSpeed: Float, vibratoDepth: Float, phraseVol: Float, sampleId: Int, tableId: Int,
            tableTicRate: Int, noteOctave: Int, notePitch: Int, tableStartRow: Int, detuneSemitones: Float
        ) {
            calls += "SFNOTE $frame $trackId $sfSlot $midiNote $velocity ${f32(vol)} ${f32(pan)} " +
                "$bank $preset ${f32(pslInitialOffset)} ${f32(pslDuration)} ${f32(pbnRate)} " +
                "${f32(vibratoSpeed)} ${f32(vibratoDepth)} ${f32(phraseVol)} $sampleId $tableId " +
                "$tableTicRate $noteOctave $notePitch $tableStartRow ${f32(detuneSemitones)}"
        }

        override fun getSampleLength(id: Int) = SAMPLE_LENGTH
    }

    /** One case: the instrument under test plus the seam arguments of a single scheduleNote call. */
    private data class Case(
        val name: String,
        val instrument: Instrument,
        val sampleRate: Int = 44100,
        val tempo: Int = 128,
        val fileRate: Int = 44100,       // the sample's own rate → ratio = sampleRate / fileRate
        val sfSlot: Int = -1,            // ≥0 registers the instrument's soundfontPath at this slot
        val note: Note = Note.fromString("C-4"),
        val velocity: Int = -1,
        val volume: Float = 1.0f,
        val phraseVol: Float = 1.0f,
        val pan: Float = 0.5f,
        val start: Int = -1,
        val slice: Int = -1,
        val transpose: Int = 0,
        val pit: Int = 0,
        val arp: Int = 0,
        val tableId: Int = -1,
        val tableRow: Int = -1,
        val pslOff: Float = 0f,
        val pslDur: Float = 0f,
        val pbnRate: Float = 0f,
        val vibSpd: Float = 0f,
        val vibDep: Float = 0f,
    )

    // ── instrument fixtures ─────────────────────────────────────────────────────────────────────
    private fun sampler(
        id: Int = 3,
        root: String = "C-4",
        detune: Int = 0x80,
        volume: Int = 0xFF,
        pan: Int = 0x80,
        slicingMode: Int = 0,
        markers: List<Long> = emptyList(),
        ticRate: Int = 0x06,
        mods: List<ModSlot> = emptyList(),
        loaded: Boolean = true,
    ) = Instrument(id = id).copy(
        sampleId = id,
        // sampleFilePath == null is the single "empty slot" signal — an empty instrument must drop the
        // note, and that guard is exactly the kind of thing a below-seam port silently loses.
        sampleFilePath = if (loaded) "/fake/sample.wav" else null,
        root = Note.fromString(root),
        detune = detune,
        volume = volume,
        pan = pan,
        slicingMode = slicingMode,
        sliceMarkers = markers,
        tableTicRate = ticRate,
        eqSlot = 5,
        reverbSend = 0x40,
        delaySend = 0x20,
        drive = 0x10, crush = 0x2, downsample = 0x3,
        filterType = "lp", filterCut = 0x70, filterRes = 0x30,
        sampleStart = 0x08, sampleEnd = 0xF0,
        loopMode = "fwd", loopStart = 0x10, loopEnd = 0xE0,
        reverse = true,
        modSlots = Array(4) { i -> mods.getOrElse(i) { ModSlot() } },
    )

    private fun soundfont(
        id: Int = 7,
        root: String = "C-4",
        detune: Int = 0x80,
        bank: Int = 0,
        preset: Int = 12,
        mods: List<ModSlot> = emptyList(),
    ) = Instrument(id = id).copy(
        sampleId = id,
        instrumentType = InstrumentType.SOUNDFONT,
        soundfontPath = "/fake/font.sf2",
        root = Note.fromString(root),
        detune = detune,
        sfBank = bank,
        sfPreset = preset,
        eqSlot = 2,
        reverbSend = 0x30, delaySend = 0x10,
        modSlots = Array(4) { i -> mods.getOrElse(i) { ModSlot() } },
    )

    private fun mod(
        type: ModType, dest: ModDest, amount: Int = 0xC0,
        attack: Int = 0x04, hold: Int = 0x08, decay: Int = 0x10, sustain: Int = 0x60, release: Int = 0x0C,
        lfoFreq: Int = 0x40, oscShape: Int = 2, lfoTrigMode: Int = 0,
    ) = ModSlot(
        type = type, dest = dest, amount = amount,
        attack = attack, hold = hold, decay = decay, sustain = sustain, release = release,
        lfoFreq = lfoFreq, oscShape = oscShape, lfoTrigMode = lfoTrigMode
    )

    private fun cases(): List<Case> {
        val out = mutableListOf<Case>()

        // ── the drop paths: an empty slot (sampleFilePath == null) must schedule NOTHING but the
        //    tempo push that precedes the check, and an out-of-range instrument must not even do that
        out += Case("empty-slot", sampler(loaded = false))
        out += Case("empty-slot-with-mods", sampler(loaded = false, mods = listOf(mod(ModType.AHD, ModDest.VOLUME))))

        // ── sampler: the base-frequency inputs (ROOT × rateRatio ÷ detune) ──
        out += Case("basic", sampler())
        out += Case("root-low", sampler(root = "C-2"))
        out += Case("root-high", sampler(root = "B-6"))
        out += Case("detune-flat", sampler(detune = 0x40))
        out += Case("detune-sharp", sampler(detune = 0xC8))
        out += Case("detune-max", sampler(detune = 0xFF))
        out += Case("detune-zero", sampler(detune = 0x00))
        out += Case("rate-48k-sample", sampler(), fileRate = 48000)
        out += Case("rate-22k-sample", sampler(), fileRate = 22050)
        out += Case("device-48k", sampler(), sampleRate = 48000)
        out += Case("device-48k-22k-sample", sampler(), sampleRate = 48000, fileRate = 22050)

        // ── sampler: the note itself (incl. the top authored note, MIDI 131) ──
        for (n in listOf("C-0", "C-4", "A-4", "B-9", "F#5")) out += Case("note-$n", sampler(), note = Note.fromString(n))

        // ── sampler: pitch shifts, applied after slice selection ──
        out += Case("pit-up", sampler(), pit = 7)
        out += Case("pit-down", sampler(), pit = -13)
        out += Case("pit-clamp-high", sampler(), note = Note.fromString("B-9"), pit = 12)
        out += Case("arp", sampler(), arp = 4)
        out += Case("pit-and-arp", sampler(), pit = 3, arp = 5)
        out += Case("transpose", sampler(), transpose = -5)

        // ── sampler: slice windows (CUT bounds the end; TRU/SLI-only play to the sample end) ──
        val markers = listOf(4410L, 11025L, 22050L, 33075L)
        out += Case("slice-cut-c4", sampler(slicingMode = 1, markers = markers))
        out += Case("slice-cut-c#4", sampler(slicingMode = 1, markers = markers), note = Note.fromString("C#4"))
        out += Case("slice-cut-e4", sampler(slicingMode = 1, markers = markers), note = Note.fromString("E-4"))
        out += Case("slice-cut-past-end", sampler(slicingMode = 1, markers = markers), note = Note.fromString("A-4"))
        out += Case("slice-cut-below-c4", sampler(slicingMode = 1, markers = markers), note = Note.fromString("A-3"))
        out += Case("slice-tru", sampler(slicingMode = 2, markers = markers), note = Note.fromString("D-4"))
        out += Case("slice-sli-explicit", sampler(slicingMode = 0, markers = markers), slice = 2)
        out += Case("slice-sli-overflow", sampler(slicingMode = 0, markers = markers), slice = 9)
        out += Case("slice-with-transpose", sampler(slicingMode = 1, markers = markers),
            note = Note.fromString("D-4"), transpose = 5)
        out += Case("slice-root-detune", sampler(root = "G-3", detune = 0x60, slicingMode = 1, markers = markers),
            note = Note.fromString("D#4"))
        out += Case("slice-then-pit", sampler(slicingMode = 1, markers = markers),
            note = Note.fromString("D-4"), pit = 2)
        // slicingMode set but NO markers → the slice branch is skipped entirely
        out += Case("slice-mode-no-markers", sampler(slicingMode = 1))

        // ── sampler: the seam's float/int passthroughs + tick→frame conversions ──
        out += Case("gains", sampler(), volume = 0.5f, phraseVol = 0.25f, pan = 0.75f, velocity = 96)
        out += Case("start-override", sampler(), start = 0x40)
        out += Case("table-override", sampler(), tableId = 9, tableRow = 5)
        out += Case("psl", sampler(), pslOff = -3.5f, pslDur = 6.0f)
        out += Case("pbn", sampler(), pbnRate = 0.75f)
        out += Case("pbn-negative", sampler(), pbnRate = -1.25f)
        out += Case("vibrato", sampler(), vibSpd = 5.25f, vibDep = 0.375f)
        out += Case("psl-pbn-tempo-60", sampler(), tempo = 60, pslDur = 6.0f, pbnRate = 0.5f)
        out += Case("psl-pbn-tempo-255", sampler(), tempo = 255, pslDur = 6.0f, pbnRate = 0.5f)
        out += Case("psl-pbn-48k", sampler(), sampleRate = 48000, pslDur = 6.0f, pbnRate = 0.5f)
        out += Case("tic-rate", sampler(ticRate = 0x03))

        // ── sampler: modulation slots (every type × a spread of destinations) ──
        out += Case("mod-ahd", sampler(mods = listOf(mod(ModType.AHD, ModDest.VOLUME))))
        out += Case("mod-adsr", sampler(mods = listOf(mod(ModType.ADSR, ModDest.FILTER_CUTOFF))))
        out += Case("mod-lfo", sampler(mods = listOf(mod(ModType.LFO, ModDest.PAN, lfoFreq = 0x00))))
        out += Case("mod-lfo-fast", sampler(mods = listOf(mod(ModType.LFO, ModDest.PITCH, lfoFreq = 0xFF, lfoTrigMode = 1))))
        out += Case("mod-drum", sampler(mods = listOf(mod(ModType.DRUM, ModDest.FINE_PITCH))))
        out += Case("mod-trig", sampler(mods = listOf(mod(ModType.TRIG, ModDest.FILTER_RES))))
        out += Case("mod-scalar", sampler(mods = listOf(mod(ModType.SCALAR, ModDest.SAMPLE_START))))
        out += Case("mod-dest-none", sampler(mods = listOf(mod(ModType.AHD, ModDest.NONE))))
        out += Case("mod-type-none", sampler(mods = listOf(ModSlot(type = ModType.NONE, dest = ModDest.VOLUME))))
        out += Case("mod-tracking", sampler(mods = listOf(mod(ModType.TRACKING, ModDest.VOLUME))))
        out += Case("mod-mod-to-mod", sampler(mods = listOf(
            mod(ModType.LFO, ModDest.MOD_AMT), mod(ModType.AHD, ModDest.VOLUME))))
        out += Case("mod-all-four", sampler(mods = listOf(
            mod(ModType.AHD, ModDest.VOLUME), mod(ModType.ADSR, ModDest.PAN),
            mod(ModType.LFO, ModDest.PITCH), mod(ModType.SCALAR, ModDest.FILTER_CUTOFF))))
        out += Case("mod-tempo-60", sampler(mods = listOf(mod(ModType.AHD, ModDest.VOLUME))), tempo = 60)
        out += Case("mod-tempo-200-48k", sampler(mods = listOf(mod(ModType.ADSR, ModDest.VOLUME))),
            tempo = 200, sampleRate = 48000)
        out += Case("mod-times-max", sampler(mods = listOf(
            mod(ModType.ADSR, ModDest.VOLUME, attack = 0xFF, decay = 0xFF, release = 0xFF, sustain = 0xFF, amount = 0xFF))))

        // ── SoundFont path ──
        out += Case("sf-basic", soundfont(), sfSlot = 1)
        out += Case("sf-velocity", soundfont(), sfSlot = 1, velocity = 100)
        out += Case("sf-velocity-derived", soundfont(), sfSlot = 1, velocity = -1, volume = 0.6f)
        out += Case("sf-velocity-floor", soundfont(), sfSlot = 1, velocity = 0)
        out += Case("sf-root-transpose", soundfont(root = "G-3"), sfSlot = 1, note = Note.fromString("E-4"))
        out += Case("sf-root-high", soundfont(root = "C-6"), sfSlot = 1, note = Note.fromString("C-4"))
        out += Case("sf-clamp-high", soundfont(root = "C-2"), sfSlot = 1, note = Note.fromString("B-9"))
        out += Case("sf-detune", soundfont(detune = 0x4C), sfSlot = 1)
        out += Case("sf-arp", soundfont(), sfSlot = 1, arp = 7)
        out += Case("sf-preset", soundfont(bank = 128, preset = 40), sfSlot = 3)
        out += Case("sf-psl-pbn", soundfont(), sfSlot = 1, pslOff = 2.0f, pslDur = 3.0f, pbnRate = -0.5f)
        out += Case("sf-table", soundfont(), sfSlot = 1, tableId = 4, tableRow = 2)
        out += Case("sf-mods", soundfont(mods = listOf(mod(ModType.LFO, ModDest.VOLUME))), sfSlot = 1)
        out += Case("sf-48k", soundfont(), sfSlot = 1, sampleRate = 48000, tempo = 90, pslDur = 6f, pbnRate = 1f)
        // slot unresolved (the SF2 never loaded) → Kotlin drops the note entirely
        out += Case("sf-no-slot", soundfont(), sfSlot = -1)

        return out
    }

    /** Render one case by driving the REAL AudioEngine.scheduleNote and capturing what it emits. */
    private fun runCase(c: Case): List<String> {
        val backend = RecordingBackend(c.sampleRate)
        val engine = AudioEngine(backend, FakeResourceLoader, FakeLogger)

        // Register the sample's rate exactly as a real load would (this is what sets sampleRateRatios).
        if (c.instrument.instrumentType == InstrumentType.SAMPLER) {
            engine.loadSampleData(c.instrument.id, FloatArray(8), null, c.fileRate)
        }
        engine.sfSlotProvider = { path -> if (c.sfSlot >= 0 && path == c.instrument.soundfontPath) c.sfSlot else null }

        val project = Project(version = 1)
        project.tempo = c.tempo
        project.instruments[c.instrument.id] = c.instrument

        backend.calls.clear()   // drop anything the sample registration produced
        engine.scheduleNote(
            targetFrame = 12345L,
            note = c.note,
            instrumentId = c.instrument.id,
            trackId = 2,
            volume = c.volume,
            phraseVol = c.phraseVol,
            midiVelocity = c.velocity,
            pan = c.pan,
            project = project,
            startPointOverride = c.start,
            pslInitialOffset = c.pslOff,
            pslDuration = c.pslDur,
            pbnRate = c.pbnRate,
            vibratoSpeed = c.vibSpd,
            vibratoDepth = c.vibDep,
            tableIdOverride = c.tableId,
            tableStartRow = c.tableRow,
            transposeSemitones = c.transpose,
            pitSemitones = c.pit,
            sliIndex = c.slice,
            arpSemitoneOffset = c.arp,
        )
        return backend.calls
    }

    /** The inputs, in the exact order tools/ptvoice parses them. */
    private fun inputLine(c: Case): String {
        val i = c.instrument
        fun f32(v: Float) = "0x%08X".format(java.lang.Float.floatToRawIntBits(v))
        val mods = i.modSlots.joinToString(",") { m ->
            "${m.type.name}:${m.dest.name}:${m.amount}:${m.attack}:${m.hold}:${m.decay}:" +
                "${m.sustain}:${m.release}:${m.lfoFreq}:${m.oscShape}:${m.lfoTrigMode}"
        }
        val markers = if (i.sliceMarkers.isEmpty()) "-" else i.sliceMarkers.joinToString(",")
        return "CASE ${c.name} sr=${c.sampleRate} tempo=${c.tempo} fileRate=${c.fileRate} sfSlot=${c.sfSlot} " +
            "id=${i.id} sampleId=${i.sampleId} type=${i.instrumentType.name} " +
            "hasSample=${if (i.sampleFilePath != null) 1 else 0} hasSf=${if (i.soundfontPath != null) 1 else 0} " +
            "root=${i.root.pitch}:${i.root.octave} detune=${i.detune} ticRate=${i.tableTicRate} " +
            "slicing=${i.slicingMode} markers=$markers eqSlot=${i.eqSlot} rsend=${i.reverbSend} dsend=${i.delaySend} " +
            "sfBank=${i.sfBank} sfPreset=${i.sfPreset} " +
            "sfEnv=${i.sfOverrides.ampAttack}:${i.sfOverrides.ampDecay}:${i.sfOverrides.ampSustain}:${i.sfOverrides.ampRelease} " +
            "sStart=${i.sampleStart} sEnd=${i.sampleEnd} rev=${if (i.reverse) 1 else 0} " +
            "loop=${i.loopMode}:${i.loopStart}:${i.loopEnd} " +
            "drive=${i.drive} crush=${i.crush} down=${i.downsample} " +
            "filter=${i.filterType}:${i.filterCut}:${i.filterRes} " +
            "mods=$mods | " +
            "note=${c.note.pitch}:${c.note.octave} vel=${c.velocity} vol=${f32(c.volume)} pvol=${f32(c.phraseVol)} " +
            "pan=${f32(c.pan)} start=${c.start} slice=${c.slice} transpose=${c.transpose} pit=${c.pit} arp=${c.arp} " +
            "tableId=${c.tableId} tableRow=${c.tableRow} pslOff=${f32(c.pslOff)} pslDur=${f32(c.pslDur)} " +
            "pbn=${f32(c.pbnRate)} vibSpd=${f32(c.vibSpd)} vibDep=${f32(c.vibDep)}"
    }

    private fun render(): String {
        val sb = StringBuilder()
        sb.append("# songcore S5 consumer golden — the engine calls Kotlin's AudioEngine.scheduleNote makes.\n")
        sb.append("# Generated by S5ConsumerGoldenTest; verified against C++ by tools/ptvoice.\n")
        sb.append("# Floats are raw binary32 bits. sampleLength is fixed at $SAMPLE_LENGTH for every sample.\n")
        for (c in cases()) {
            sb.append(inputLine(c)).append('\n')
            for (call in runCase(c)) sb.append("  ").append(call).append('\n')
        }
        return sb.toString()
    }

    @Test
    fun consumerCallsMatchKotlin() {
        val expected = render()
        val file = unitsFile
        if (!file.exists()) {
            file.parentFile.mkdirs()
            file.writeText(expected)
            println("📝 generated ${file.path} (${cases().size} cases)")
            return
        }
        assertEquals(
            "testdata/units/s5-consumer.txt drifted from what Kotlin's AudioEngine.scheduleNote emits. " +
                "If the change was intended, delete the file, rerun, and re-run tools/ptvoice.",
            expected,
            file.readText()
        )
    }

    @Test
    fun generationIsDeterministic() {
        assertEquals(render(), render())
    }
}
