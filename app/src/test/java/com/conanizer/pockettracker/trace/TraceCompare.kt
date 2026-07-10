package com.conanizer.pockettracker.trace

/**
 * Canonical trace comparison (event-schema §4/§6): event lines within a PLAY..STOP segment sort
 * STABLY by (frame, track, rank) — rank: NoteOn=2, NoteOff=1, everything else 0 — then the whole
 * trace compares byte-for-byte. Stability preserves the relative order of equal-key lines, which
 * is semantic (FX slots resolve 1→3, last-wins).
 *
 * Kotlin-vs-Kotlin golden regeneration is compared RAW (emission order is deterministic too);
 * canonicalization exists for the cross-implementation comparisons: device-vs-host Kotlin now,
 * C++ songcore vs golden in Phase 1 S4.
 */
object TraceCompare {

    private fun rank(typeToken: String): Int = when (typeToken) {
        "90" -> 2
        "80" -> 1
        else -> 0
    }

    private data class Key(val frame: Long, val track: Int, val rank: Int) : Comparable<Key> {
        override fun compareTo(other: Key): Int {
            frame.compareTo(other.frame).let { if (it != 0) return it }
            track.compareTo(other.track).let { if (it != 0) return it }
            return rank.compareTo(other.rank)
        }
    }

    private fun keyOf(line: String): Key {
        val sp1 = line.indexOf(' ')
        val sp2 = line.indexOf(' ', sp1 + 1)
        val sp3 = line.indexOf(' ', sp2 + 1)
        val end = if (line.indexOf(' ', sp3 + 1) >= 0) line.indexOf(' ', sp3 + 1) else line.length
        return Key(
            frame = line.substring(0, sp1).toLong(),
            track = line.substring(sp1 + 1, sp2).toInt(),
            rank = rank(line.substring(sp3 + 1, end))
        )
    }

    /** True for event lines (they start with a digit); meta lines (`#`, `T`) pin segment bounds. */
    private fun isEvent(line: String) = line.isNotEmpty() && line[0].isDigit()

    /** Stable-sort event lines within each meta-delimited segment; meta lines keep their position. */
    fun canonicalize(trace: String): String {
        val out = StringBuilder(trace.length)
        val run = mutableListOf<String>()
        fun flush() {
            run.sortedBy { keyOf(it) }.forEach { out.append(it).append('\n') }  // sortedBy is stable
            run.clear()
        }
        for (line in trace.split('\n')) {
            if (line.isEmpty()) continue
            if (isEvent(line)) run.add(line) else { flush(); out.append(line).append('\n') }
        }
        flush()
        return out.toString()
    }

    /** Canonical comparison. Returns null when equal, else a first-divergence report. */
    fun diff(expected: String, actual: String): String? {
        val e = canonicalize(expected).split('\n')
        val a = canonicalize(actual).split('\n')
        for (i in 0 until maxOf(e.size, a.size)) {
            val el = e.getOrNull(i)
            val al = a.getOrNull(i)
            if (el != al) {
                return "first divergence at canonical line ${i + 1}:\n  expected: ${el ?: "<end of trace>"}\n  actual:   ${al ?: "<end of trace>"}"
            }
        }
        return null
    }
}
