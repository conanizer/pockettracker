package com.conanizer.pockettracker.core.audio

/**
 * The C++ sequencer (songcore) as seen from Kotlin — the event-schema §7 verb set, nothing more.
 *
 * songcore walks the song and emits the event stream; Kotlin pushes the project, drives the transport,
 * and reads back playheads. Everything musical happens on the far side of this interface, which is the
 * point: the Linux build has no Kotlin at all, so anything that decides how the song *sounds* must
 * live below this line, in C++, where both platforms share it.
 *
 * Kept in `core/` as an interface — with no android imports — so `core/` stays JVM-testable (the golden
 * trace harness runs there, on the Kotlin sequencer, with no songcore at all: a null [ISongcore] means
 * the Kotlin path is the only path).
 *
 * Threading: every call is made from the transport/UI thread, never the audio callback.
 */
interface ISongcore {

    /** Construct the native runtime. Call once, AFTER the audio engine exists (songcore reads its clock). */
    fun create()

    /** Destroy the native runtime (releases the trace file, if open). */
    fun destroy()

    /**
     * Hand songcore the whole project as canonical .ptp JSON, UTF-8 encoded — the exact bytes
     * `FileController.serializeProject` produces, which the C++ reader parses byte-exactly (S2).
     * Pushed on play, on the render path, and whenever data changes mid-playback.
     * @return false if the blob could not be parsed (the previous project is kept).
     */
    fun pushProject(blob: ByteArray): Boolean

    /**
     * The two per-instrument facts songcore cannot derive, because it never opens a file:
     * [sampleRateRatios] = deviceRate / fileRate per instrument slot (1.0 when nothing is loaded), and
     * [sfSlots] = the SF2 slot each instrument's soundfontPath resolved to (−1 = none → the note is
     * dropped, exactly as the Kotlin path drops it). Pushed with the project so they cannot drift.
     */
    fun pushRouting(sampleRateRatios: FloatArray, sfSlots: IntArray)

    /** Transport. Each returns the frame the C++ transport latched (the trace session's base frame). */
    fun playSong(startRow: Int): Long
    fun playChain(chainId: Int): Long
    fun playPhrase(phraseId: Int): Long

    fun stop()

    /** The lookahead poll — the UI's 60 Hz loop drives it, as it drove `updatePlaybackBuffer()`. */
    fun poll()

    /** songcore can stop itself (a chain running out), so the UI mirrors this rather than assuming. */
    fun isPlaying(): Boolean

    /** `AudioEngine.phraseTrackMask`'s twin: bit N = track N has had a note scheduled (OCTA scopes). */
    fun getTrackMask(): Int

    /**
     * The render path: schedule song rows [startRow]..[endRow] into the engine queue at frames 0..N.
     * @param trackFilter null renders every track; non-null restricts to those track ids (stems).
     * @return the total frame span scheduled.
     */
    fun scheduleSongRange(startRow: Int, endRow: Int, trackFilter: IntArray?): Long

    /**
     * An edit landed mid-playback: roll the lookahead back to the earliest unplayed phrase boundary and
     * drop the notes queued past it, so the edit is heard on the next phrase loop. Push the project first.
     */
    fun notifyDataChanged()

    /** `int[4] = {row, chainRow, phraseStep, songRow}` — the UI cursor (never goldened; SC-4). */
    fun getPlayheads(): IntArray

    /**
     * Stream the schema-v1 conformance trace to [path] — the same bytes, and the same file, the Kotlin
     * `EventTrace` tap writes, so the device cross-check compares either engine against the same goldens.
     * Push the project first: the header's `project=` is the SHA-1 of the pushed blob.
     */
    fun setTrace(enabled: Boolean, path: String)
}
