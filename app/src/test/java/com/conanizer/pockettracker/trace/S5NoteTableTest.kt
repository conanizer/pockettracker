package com.conanizer.pockettracker.trace

import com.conanizer.pockettracker.core.data.Instrument
import com.conanizer.pockettracker.core.data.Note
import org.junit.Assert.assertEquals
import org.junit.Test
import java.io.File

/**
 * songcore S5 — GENERATES and then GUARDS `native/songcore/note_tables.h`.
 *
 * The C++ consumer (songcore/engine_consumer.h) has to turn a note into a frequency exactly as
 * Kotlin's `Note.toFrequency()` does — `440f * 2.0.pow((midi - 69) / 12.0).toFloat()` — and a detune
 * byte into a multiplier exactly as `Instrument.detuneMultiplier()` does. Both go through a Double
 * `pow`, and **nothing guarantees the JVM's pow and the device's libm agree to the last bit**. One ULP
 * of frequency error changes the resampling rate, which changes every rendered byte — so "close
 * enough" would quietly fail the KT-vs-C++ WAV comparison and leave us bisecting phantom scheduler bugs.
 *
 * The inputs are finite, so we don't need pow at runtime at all: 132 reachable MIDI numbers (0..131 —
 * an authored B-9 is 131, above the MIDI range but valid in the tracker) and 256 detune bytes. This
 * test evaluates the REAL Kotlin functions over both domains and writes their raw binary32 bits into a
 * C++ header. The C++ side just indexes it, so it is bit-identical to Kotlin by construction, on every
 * platform, forever — which is exactly what event-schema §5's "one vendored implementation" rule asks
 * for, in its strongest form.
 *
 * Missing header → generated. Existing header → byte-compared (the drift guard: if anyone edits
 * `toFrequency()`/`detuneMultiplier()`, this test goes red instead of the audio going subtly wrong).
 * To regenerate deliberately: delete the file, rerun, commit.
 */
class S5NoteTableTest {

    companion object {
        private fun repoRoot(): File {
            var dir = File(System.getProperty("user.dir")!!).absoluteFile
            while (true) {
                if (File(dir, "settings.gradle.kts").exists() || File(dir, "settings.gradle").exists()) return dir
                dir = dir.parentFile ?: error("repo root not found from ${System.getProperty("user.dir")}")
            }
        }

        private val headerFile: File get() = File(repoRoot(), "native/songcore/note_tables.h")

        /** MIDI numbers the tracker can actually produce: C-(-1)=0 … B-9=131 (octave 0..9 authored). */
        const val NOTE_COUNT = 132
        const val DETUNE_COUNT = 256
    }

    private fun bits(v: Float): String = "0x%08X".format(java.lang.Float.floatToRawIntBits(v))

    private fun render(): String {
        // The REAL Kotlin implementations — never a re-derivation of them.
        val noteBits = (0 until NOTE_COUNT).map { midi ->
            bits(Note(pitch = midi % 12, octave = midi / 12 - 1).toFrequency())
        }
        val detuneBits = (0 until DETUNE_COUNT).map { d ->
            bits(Instrument(id = 0).copy(detune = d).detuneMultiplier())
        }

        val sb = StringBuilder()
        sb.append(
            """
            |#ifndef POCKETTRACKER_SONGCORE_NOTE_TABLES_H
            |#define POCKETTRACKER_SONGCORE_NOTE_TABLES_H
            |
            |// ─── GENERATED FILE — do not edit ───────────────────────────────────────────────────────────────
            |//
            |// Written by app/src/test/.../trace/S5NoteTableTest.kt, which also guards it: the test fails if
            |// these values ever stop matching Kotlin's Note.toFrequency() / Instrument.detuneMultiplier().
            |// Regenerate deliberately by deleting this file and re-running :app:testDebugUnitTest.
            |//
            |// WHY A TABLE. Both Kotlin functions route through a Double pow(), and the JVM's pow and the
            |// device's libm are not guaranteed to agree to the last bit — nor are bionic and glibc, which
            |// would break golden reuse between the device and CI. A 1-ULP frequency error changes the
            |// resampling rate and therefore every rendered byte. The input domains are small and closed
            |// (132 reachable MIDI numbers, 256 detune bytes), so the transcendental is evaluated ONCE, by
            |// Kotlin, and baked here as raw binary32 bits. This is event-schema §5's "one vendored
            |// implementation on every platform" rule, in its strongest form: no runtime pow at all.
            |
            |#include <cstdint>
            |#include <cstring>
            |
            |namespace songcore {
            |
            |inline float f32_from_bits(uint32_t b) {
            |    float f;
            |    std::memcpy(&f, &b, sizeof f);
            |    return f;
            |}
            |
            |// Note.toFrequency() for MIDI 0..131 — (octave+1)*12+pitch, so index 131 = B-9 (the tracker's
            |// top authored note, deliberately past the 0..127 MIDI range: the trace records it verbatim).
            |constexpr uint32_t NOTE_HZ_BITS[$NOTE_COUNT] = {
            |
            """.trimMargin()
        )
        appendRows(sb, noteBits)
        sb.append(
            """
            |};
            |
            |// Instrument.detuneMultiplier() for every detune byte 0x00..0xFF — 2^(detuneSemitones/12),
            |// where detuneSemitones = (d>>4) + (d&0xF)/16 - 8. Index 0x80 is unity (1.0f).
            |constexpr uint32_t DETUNE_MUL_BITS[$DETUNE_COUNT] = {
            |
            """.trimMargin()
        )
        appendRows(sb, detuneBits)
        sb.append(
            """
            |};
            |
            |// 0 Hz for anything outside the authored range — mirrors Note.toFrequency()'s empty-note return.
            |inline float note_hz(int midi) {
            |    if (midi < 0 || midi >= $NOTE_COUNT) return 0.0f;
            |    return f32_from_bits(NOTE_HZ_BITS[midi]);
            |}
            |
            |inline float detune_multiplier(int detune) {
            |    return f32_from_bits(DETUNE_MUL_BITS[detune & 0xFF]);
            |}
            |
            |}  // namespace songcore
            |
            |#endif  // POCKETTRACKER_SONGCORE_NOTE_TABLES_H
            |
            """.trimMargin()
        )
        return sb.toString()
    }

    /** 8 values per line, 4-space indent — stable formatting so the byte-compare is meaningful. */
    private fun appendRows(sb: StringBuilder, values: List<String>) {
        values.chunked(8).forEach { row ->
            sb.append("    ").append(row.joinToString(", ")).append(",\n")
        }
    }

    @Test
    fun noteTablesMatchKotlin() {
        val expected = render()
        val file = headerFile
        if (!file.exists()) {
            file.parentFile.mkdirs()
            file.writeText(expected)
            println("📝 generated ${file.path} (${NOTE_COUNT} notes, ${DETUNE_COUNT} detunes)")
            return
        }
        assertEquals(
            "native/songcore/note_tables.h no longer matches Kotlin's Note.toFrequency() / " +
                "Instrument.detuneMultiplier(). If the change was intended, delete the file and rerun.",
            expected,
            file.readText()
        )
    }

    /** Spot-check the anchors, so a wholesale regeneration of garbage can't pass silently. */
    @Test
    fun anchorsAreCorrect() {
        assertEquals(440.0f, Note(pitch = 9, octave = 4).toFrequency(), 0.001f)   // A-4 = 440 Hz
        assertEquals(60, Note(pitch = 0, octave = 4).toMidi())                    // C-4 = MIDI 60
        assertEquals(131, Note(pitch = 11, octave = 9).toMidi())                  // B-9 = 131 (top authored)
        assertEquals(1.0f, Instrument(id = 0).copy(detune = 0x80).detuneMultiplier(), 0.0f)  // 0x80 = unity
    }
}
