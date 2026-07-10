package com.conanizer.pockettracker.core.trace

import java.io.Flushable

/**
 * EVENT TRACE — the Kotlin sequencer's conformance-trace tap.
 *
 * Writes the schema-v1 trace text defined normatively in `cpp/songcore/event.h` (companion to
 * `docs/internal/event-schema.md`). This is the "measuring stick" of songcore Phase 1: the golden
 * traces recorded through this tap are the truth the C++ songcore must reproduce byte-for-byte.
 * It is the ONE allowed change to frozen zone-C code — pure observation, no behavior.
 *
 * Off unless a sink is attached ([begin]); every record method starts with a null-check, so the
 * scheduling hot path pays one comparison when tracing is off.
 *
 * Frames are recorded RELATIVE to the session start latched at [tPlay] (render sessions start at
 * 0 naturally; live sessions subtract the transport-start frame), so traces are playback-position
 * independent and device↔host comparable. Events outside a PLAY..STOP session are dropped —
 * stray previews and stop-cleanup kills never land after a `T STOP` line.
 *
 * Format (frozen 2026-07-10 — see event.h for the full contract):
 * ```
 * # schema=1 sr=44100 tempo=128 mode=render project=<sha1>
 * T PLAY RENDER rows=00-03
 * <frame> <track> <instr> <TT> k=v k=v ...
 * T STOP
 * ```
 * Ints render decimal, floats as `0x` + 8 uppercase hex digits of their raw binary32 bits
 * (exact comparison, no float fuzz), bools 0/1, instrument as 2-digit uppercase hex or -1.
 * Every payload field is always rendered, in schema order — the line shape is fixed.
 */
object EventTrace {

    const val SCHEMA_VERSION = 1

    @Volatile private var sink: Appendable? = null
    @Volatile private var inSession = false
    private var sessionStartFrame = 0L
    private var projectSha = "-"

    /** True when a sink is attached — for call sites that must pre-compute a tap argument. */
    val active: Boolean get() = sink != null

    /** SHA-1 hex of the canonical serialized project JSON (UTF-8) — the header's project id.
     *  One implementation for the app's debug toggle and the JVM golden harness. */
    fun projectSha1(serializedJson: String): String =
        java.security.MessageDigest.getInstance("SHA-1")
            .digest(serializedJson.toByteArray(Charsets.UTF_8))
            .joinToString("") { b -> ((b.toInt() and 0xFF) + 0x100).toString(16).substring(1) }

    /**
     * Attach a sink and start tracing. [projectSha1] identifies the project in session headers:
     * SHA-1 hex of the canonical serialized project JSON (FileController.serializeProject bytes,
     * UTF-8) — the same value on any platform that serializes the same project state.
     */
    @Synchronized
    fun begin(out: Appendable, projectSha1: String) {
        sink = out
        projectSha = projectSha1
        inSession = false
    }

    /** Detach the sink (flushing it if it can). Safe to call when not tracing. */
    @Synchronized
    fun end() {
        (sink as? Flushable)?.flush()
        sink = null
        inSession = false
    }

    // ── Transport records ────────────────────────────────────────────────────────────────────

    /**
     * Open a PLAY session: writes the `#` header + `T PLAY` line and latches [startFrame] as the
     * session frame base. [kind] ∈ SONG|CHAIN|PHRASE|RENDER; [detail] is the pre-rendered
     * argument (`row=00`, `id=0A`, `rows=00-03`).
     */
    @Synchronized
    fun tPlay(kind: String, detail: String, startFrame: Long, tempo: Int, sampleRate: Int) {
        val s = sink ?: return
        sessionStartFrame = startFrame
        inSession = true
        val mode = if (kind == "RENDER") "render" else "live"
        s.append("# schema=").append(SCHEMA_VERSION.toString())
            .append(" sr=").append(sampleRate.toString())
            .append(" tempo=").append(tempo.toString())
            .append(" mode=").append(mode)
            .append(" project=").append(projectSha).append('\n')
        s.append("T PLAY ").append(kind).append(' ').append(detail).append('\n')
    }

    /** Close the session with `T STOP`. No-op when no session is open (stop() runs before every play). */
    @Synchronized
    fun tStop() {
        if (!inSession) return
        val s = sink ?: return
        s.append("T STOP\n")
        inSession = false
        (s as? Flushable)?.flush()
    }

    // ── Event records — signatures mirror the AudioEngine seam args verbatim ────────────────────

    /** 0x90 — the full trigger bundle, tapped at AudioEngine.scheduleNote entry. */
    @Synchronized
    fun noteOn(
        frame: Long, track: Int, instrument: Int,
        notePitch: Int, noteOctave: Int,
        velocity: Int, velGain: Float, volGain: Float, pan: Float,
        start: Int, slice: Int,
        transpose: Int, pit: Int, arp: Int,
        tableId: Int, tableRow: Int,
        pslOff: Float, pslDur: Float, pbnRate: Float, vibSpd: Float, vibDep: Float
    ) {
        val s = line(frame, track, instrument, 0x90) ?: return
        s.append(" note=").append(((noteOctave + 1) * 12 + notePitch).toString())
            .append(" vel=").append(velocity.toString())
            .append(" velGain=").append(fbits(velGain))
            .append(" volGain=").append(fbits(volGain))
            .append(" pan=").append(fbits(pan))
            .append(" start=").append(start.toString())
            .append(" slice=").append(slice.toString())
            .append(" transpose=").append(transpose.toString())
            .append(" pit=").append(pit.toString())
            .append(" arp=").append(arp.toString())
            .append(" tableId=").append(tableId.toString())
            .append(" tableRow=").append(tableRow.toString())
            .append(" pslOff=").append(fbits(pslOff))
            .append(" pslDur=").append(fbits(pslDur))
            .append(" pbnRate=").append(fbits(pbnRate))
            .append(" vibSpd=").append(fbits(vibSpd))
            .append(" vibDep=").append(fbits(vibDep))
            .append('\n')
    }

    const val NOTE_OFF_RELEASE = 0  // scheduleNoteOff — KIL soft kill / ADSR release
    const val NOTE_OFF_CUT = 1      // scheduleKill / killTrack — declick fade

    /** 0x80 */
    @Synchronized
    fun noteOff(frame: Long, track: Int, mode: Int) {
        line(frame, track, -1, 0x80)?.append(" mode=")?.append(mode.toString())?.append('\n')
    }

    const val CC_VOLUME = 7        // scheduleTrackPhraseVol (instr vol | Vxx/255 float)
    const val CC_PAN = 10          // scheduleVoicePan
    const val CC_REVERB_SEND = 91  // scheduleVoiceReverbSend
    const val CC_DELAY_SEND = 93   // scheduleVoiceDelaySend

    /** 0xB0 */
    @Synchronized
    fun cc(frame: Long, track: Int, param: Int, value: Float) {
        line(frame, track, -1, 0xB0)
            ?.append(" param=")?.append(param.toString())
            ?.append(" value=")?.append(fbits(value))?.append('\n')
    }

    /** 0x01 — PBN on an empty step (a rate, not an absolute bend). */
    @Synchronized
    fun extPitchRate(frame: Long, track: Int, rate: Float, tempo: Int) {
        line(frame, track, -1, 0x01)
            ?.append(" rate=")?.append(fbits(rate))
            ?.append(" tempo=")?.append(tempo.toString())?.append('\n')
    }

    /** 0x02 — PVB/PVX on an empty step (atomic speed+depth pair). */
    @Synchronized
    fun extVibrato(frame: Long, track: Int, speed: Float, depth: Float) {
        line(frame, track, -1, 0x02)
            ?.append(" speed=")?.append(fbits(speed))
            ?.append(" depth=")?.append(fbits(depth))?.append('\n')
    }

    /** 0x03 — THO on an empty step. */
    @Synchronized
    fun extTableRow(frame: Long, track: Int, row: Int) {
        line(frame, track, -1, 0x03)?.append(" row=")?.append(row.toString())?.append('\n')
    }

    /** 0x04 — BCK. */
    @Synchronized
    fun extReverse(frame: Long, track: Int, reverse: Boolean, restart: Boolean) {
        line(frame, track, -1, 0x04)
            ?.append(" reverse=")?.append(if (reverse) "1" else "0")
            ?.append(" restart=")?.append(if (restart) "1" else "0")?.append('\n')
    }

    /** 0x05 — EQN. */
    @Synchronized
    fun extEqSlot(frame: Long, track: Int, slot: Int) {
        line(frame, track, -1, 0x05)?.append(" slot=")?.append(slot.toString())?.append('\n')
    }

    /** 0x06 — EQM (global: track 255). */
    @Synchronized
    fun extMasterEq(frame: Long, slot: Int) {
        line(frame, 255, -1, 0x06)?.append(" slot=")?.append(slot.toString())?.append('\n')
    }

    // ── Rendering ────────────────────────────────────────────────────────────────────────────

    /** Common `<frame> <track> <instr> <TT>` prefix, or null when not recording. */
    private fun line(frame: Long, track: Int, instrument: Int, type: Int): Appendable? {
        val s = sink ?: return null
        if (!inSession) return null
        s.append((frame - sessionStartFrame).toString()).append(' ')
            .append(track.toString()).append(' ')
            .append(if (instrument < 0) "-1" else hex2(instrument)).append(' ')
            .append(hex2(type))
        return s
    }

    /** Raw binary32 bits as `0x` + exactly 8 uppercase hex digits — locale-free. */
    private fun fbits(v: Float): String {
        val bits = java.lang.Float.floatToRawIntBits(v)
        return "0x" + Integer.toHexString(bits).uppercase().padStart(8, '0')
    }

    private fun hex2(v: Int): String =
        Integer.toHexString(v and 0xFF).uppercase().padStart(2, '0')
}
