package com.conanizer.pockettracker.core.data

import kotlinx.serialization.Serializable
import kotlin.math.pow

// Tics per phrase step. Both the Kotlin scheduler and the C++ modulation engine use this.
// Changing it here propagates everywhere — do not hardcode 12 in either layer.
const val TICS_PER_STEP = 12

/**
 * Format an Int as a 2-digit uppercase hex string (e.g. 255 → "FF"). Masks to the lower 8 bits.
 *
 * Core-layer counterpart to the UI's `EditorHelpers.toHex2()`. The UI keeps its own copy because
 * `core/` must not depend on the `ui/` package; this one is the single source for core/data code.
 */
fun Int.toHex2(): String = (this and 0xFF).toString(16).uppercase().padStart(2, '0')

/**
 * Decode a 0x00–0xFF transpose byte to signed semitones using the two's-complement convention:
 * 0x00 = 0, 0x01–0x7F = +1..+127, 0x80–0xFF = −128..−1.
 *
 * Single source of truth for Chain / Project / TableRow transpose. Previously each had its own
 * decoder and they disagreed at exactly 0x80 (some treated it as +128, others −128), producing a
 * 256-semitone discontinuity when nudging transpose down past 0x80. Two's-complement is chosen so
 * that decrementing 0x00 → 0xFF → … → 0x80 is a smooth −1, −2, … −128.
 */
fun byteToSignedSemitones(b: Int): Int {
    val v = b and 0xFF
    return if (v < 0x80) v else v - 256
}

// Note representation
@Serializable
data class Note(
    val pitch: Int,  // 0-11 chromatic index into NOTES (C-=0 .. B-=11), -1 = empty
    val octave: Int  // octave number shown after the note name (C-0..B-9 editable; C-4 = middle C)
) {
    companion object {
        val EMPTY = Note(-1, 0)
        val NOTES = listOf("C-", "C#", "D-", "D#", "E-", "F-", "F#", "G-", "G#", "A-", "A#", "B-")

        fun fromMidi(midi: Int): Note {
            if (midi < 0 || midi > 127) return EMPTY  // standard MIDI 0..127; C-4 = 60, editable C-0..G-9
            val octave = midi / 12 - 1  // MIDI 60 = C-4, so octave = 60/12 - 1 = 4
            val pitch = midi % 12
            return Note(pitch, octave)
        }

        fun fromString(str: String): Note {
            if (str == "---") return EMPTY
            val noteName = str.substring(0, 2)
            val octave = str.substring(2).toIntOrNull() ?: return EMPTY
            val pitch = NOTES.indexOf(noteName)
            if (pitch == -1) return EMPTY
            return Note(pitch, octave)
        }
    }

    fun toMidi(): Int {
        if (pitch == -1) return -1
        return (octave + 1) * 12 + pitch  // C-4 = (4+1)*12 + 0 = 60 (standard MIDI)
    }

    fun toFrequency(): Float {
        if (pitch == -1) return 0f
        // MIDI note to frequency: f = 440 * 2^((n-69)/12)
        val midiNote = toMidi()
        return 440f * 2.0.pow((midiNote - 69) / 12.0).toFloat()
    }

    override fun toString(): String {
        if (pitch == -1) return "---"
        return "${NOTES[pitch]}$octave"
    }
}

// Single step in a phrase (one row)
@Serializable
data class PhraseStep(
    var note: Note = Note.EMPTY,
    var instrument: Int = 0x00,  // 00-FF (256 instrument slots)
    var volume: Int = 0x7F,      // 00-7F MIDI velocity (7F = max); sampler uses a squared curve, SF2 → TSF
    var fx1Type: Int = 0x00,     // Effect type (Milestone 2: NOW EDITABLE!)
    var fx1Value: Int = 0x00,    // Effect value
    var fx2Type: Int = 0x00,
    var fx2Value: Int = 0x00,
    var fx3Type: Int = 0x00,
    var fx3Value: Int = 0x00
) {
    fun isEmpty(): Boolean = note == Note.EMPTY

    // Indexed access to the 3 FX slots (1-3) so callers don't hand-expand a when(slot) over the flat
    // fxNType/fxNValue fields. The serialized fields stay flat for backward compatibility.
    /** (type, value) for FX slot 1-3; (0, 0) if out of range. */
    fun fx(slot: Int): Pair<Int, Int> = when (slot) {
        1 -> fx1Type to fx1Value
        2 -> fx2Type to fx2Value
        3 -> fx3Type to fx3Value
        else -> 0 to 0
    }
    /** Effect type for FX slot 1-3; 0 if out of range. */
    fun fxType(slot: Int): Int = fx(slot).first
    /** Set both type and value for FX slot 1-3 (no-op out of range). */
    fun setFx(slot: Int, type: Int, value: Int) {
        when (slot) {
            1 -> { fx1Type = type; fx1Value = value }
            2 -> { fx2Type = type; fx2Value = value }
            3 -> { fx3Type = type; fx3Value = value }
        }
    }
    /** Set only the value for FX slot 1-3 (no-op out of range). */
    fun setFxValue(slot: Int, value: Int) {
        when (slot) {
            1 -> fx1Value = value
            2 -> fx2Value = value
            3 -> fx3Value = value
        }
    }
}

// Phrase = 16 steps
@Serializable
data class Phrase(
    val id: Int,  // 00-FF
    val steps: Array<PhraseStep> = Array(16) { PhraseStep() }
) {
    override fun equals(other: Any?): Boolean {
        if (this === other) return true
        if (javaClass != other?.javaClass) return false
        other as Phrase
        return id == other.id && steps.contentEquals(other.steps)
    }

    override fun hashCode(): Int {
        var result = id
        result = 31 * result + steps.contentHashCode()
        return result
    }
}

// Chain = 16 phrase references + 16 transpose values
@Serializable
data class Chain(
    val id: Int,  // 00-FF
    val phraseRefs: IntArray = IntArray(16) { -1 },  // -1 = empty (allows full 00-FF range)
    val transposeValues: IntArray = IntArray(16) { 0x00 }  // Default: 0x00 (can cycle 00-FF)
) {
    /**
     * Check if a slot is empty
     */
    fun isEmpty(index: Int): Boolean = phraseRefs[index] == -1

    /**
     * Get transpose in semitones for a slot. See [byteToSignedSemitones]:
     * 0x00 = 0, 0x01-0x7F = +1..+127, 0x80-0xFF = -128..-1.
     */
    fun getTransposeSemitones(index: Int): Int = byteToSignedSemitones(transposeValues[index])


    override fun equals(other: Any?): Boolean {
        if (this === other) return true
        if (javaClass != other?.javaClass) return false
        other as Chain
        if (id != other.id) return false
        if (!phraseRefs.contentEquals(other.phraseRefs)) return false
        if (!transposeValues.contentEquals(other.transposeValues)) return false
        return true
    }

    override fun hashCode(): Int {
        var result = id
        result = 31 * result + phraseRefs.contentHashCode()
        result = 31 * result + transposeValues.contentHashCode()
        return result
    }
}

/**
 * Table - A mini-sequencer that runs alongside an instrument
 *
 * Tables are powerful tools for:
 * - Arpeggios and chord patterns
 * - Volume slides and envelopes
 * - Pitch automation
 * - Multi-stage effects
 *
 * Each table has 16 rows with: transpose, volume, and 3 FX columns
 * Tables run at their own tick rate (set by TIC effect or instrument setting)
 */
@Serializable
data class Table(
    val id: Int,  // 00-FF
    var name: String = "TBL${id.toHex2()}",
    val rows: Array<TableRow> = Array(16) { TableRow() }
) {
    override fun equals(other: Any?): Boolean {
        if (this === other) return true
        if (javaClass != other?.javaClass) return false
        other as Table
        return id == other.id && rows.contentEquals(other.rows)
    }

    override fun hashCode(): Int {
        var result = id
        result = 31 * result + rows.contentHashCode()
        return result
    }
}

/**
 * Single row in a table
 */
@Serializable
data class TableRow(
    var transpose: Int = 0x00,      // Note transpose: 00-7F = +semitones, 80-FF = -semitones (signed)
    var volume: Int = -1,            // Volume multiplier: 00-FF, -1 = no change (empty)
    var fx1Type: Int = 0x00,        // Effect 1 type
    var fx1Value: Int = 0x00,       // Effect 1 value
    var fx2Type: Int = 0x00,        // Effect 2 type
    var fx2Value: Int = 0x00,       // Effect 2 value
    var fx3Type: Int = 0x00,        // Effect 3 type
    var fx3Value: Int = 0x00        // Effect 3 value
) {
    companion object {
        fun empty() = TableRow()

        /** Convert a transpose byte to signed semitones. See [byteToSignedSemitones]. */
        fun transposeToSemitones(transpose: Int): Int = byteToSignedSemitones(transpose)
    }

    /**
     * Check if this row is effectively empty (no transpose, full volume, no effects)
     */
    fun isEmpty(): Boolean {
        return transpose == 0x00 &&
               volume == -1 &&
               fx1Type == 0x00 &&
               fx2Type == 0x00 &&
               fx3Type == 0x00
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// MODULATION SYSTEM
// ─────────────────────────────────────────────────────────────────────────────

@Serializable
enum class ModType(val displayName: String) {
    NONE("---"),
    AHD("AHD"),
    ADSR("ADSR"),
    LFO("LFO"),
    DRUM("DRUM"),     // future
    TRIG("TRIG"),     // future
    TRACKING("TRK"),  // future
    SCALAR("SCL")     // constant value — amount field is the output (0x00–0xFF)
}

@Serializable
enum class ModDest(val displayName: String) {
    NONE("---"),
    VOLUME("VOL"),
    PAN("PAN"),
    PITCH("PITCH"),
    FINE_PITCH("FINE"),
    FILTER_CUTOFF("CUT"),
    FILTER_RES("RES"),
    SAMPLE_START("STA"),
    MOD_AMT("MOD A"),
    MOD_RATE("MOD R"),
    MOD_BOTH("MOD B")
}

@Serializable
data class ModSlot(
    var type: ModType = ModType.NONE,
    var dest: ModDest = ModDest.NONE,
    var amount: Int = 0xFF,      // 00-FF (default full — most mods are used at full depth)

    // Envelope: AHD, ADSR
    var attack: Int = 0x00,      // 00-FF ticks
    var hold: Int = 0x00,        // 00-FF ticks (AHD only)
    var decay: Int = 0x00,       // 00-FF ticks
    var sustain: Int = 0x80,     // 00-FF (ADSR sustain level)
    var release: Int = 0x00,     // 00-FF ticks (ADSR only)

    // LFO
    var oscShape: Int = 0x00,    // 0-9: TRI/SIN/RMP+/RMP-/EXP+/EXP-/SQU+/SQU-/RND/DRNK
    var lfoTrigMode: Int = 0x00, // 0=FREE, 1=RETRIG, 2=HOLD, 3=ONCE
    var lfoFreq: Int = 0x40      // 00-FF
) {
    /** Number of param rows this slot occupies in the UI (including TYPE row) */
    fun rowCount(): Int = when (type) {
        ModType.NONE     -> 1
        ModType.AHD      -> 6   // TYPE,DEST,AMT,ATK,HOLD,DEC
        ModType.ADSR     -> 7   // TYPE,DEST,AMT,ATK,DEC,SUS,REL
        ModType.LFO      -> 6   // TYPE,DEST,AMT,OSC,TRIG,FREQ
        ModType.DRUM     -> 6   // same as AHD for now
        ModType.TRIG     -> 7   // same as ADSR for now
        ModType.TRACKING -> 5   // future
        ModType.SCALAR   -> 3   // TYPE,DEST,AMT
    }
}

/**
 * Groove - A pattern of tick values that replaces uniform step timing.
 *
 * Instead of every step being TICS_PER_STEP (12) ticks,
 * a groove cycles through a list of tick values.
 * Example: [8, 4] creates a swing feel (long-short pattern).
 *
 * Default groove (id=0) means uniform timing (all steps = 12 ticks).
 */
@Serializable
data class Groove(
    val id: Int,  // 00-FF
    val steps: IntArray = IntArray(16) { -1 }  // -1 = end marker (loop), 00 = skip step, 01-FF = ticks for this step
) {
    /** Number of active steps (steps before the first -1 end marker) */
    fun activeLength(): Int = steps.indexOfFirst { it == -1 }.let { if (it < 0) steps.size else it }

    /** Get the tick duration for a groove position (wraps around active steps) */
    fun getTicksForStep(grooveStep: Int): Int {
        val len = activeLength()
        if (len == 0) return 12  // All empty — fall back to standard TICS_PER_STEP
        return steps[grooveStep % len]
    }

    override fun equals(other: Any?): Boolean {
        if (this === other) return true
        if (javaClass != other?.javaClass) return false
        other as Groove
        return id == other.id && steps.contentEquals(other.steps)
    }

    override fun hashCode(): Int {
        var result = id
        result = 31 * result + steps.contentHashCode()
        return result
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// EQ SYSTEM
// ─────────────────────────────────────────────────────────────────────────────

@Serializable
data class EqBand(
    var type: Int = 0,    // 0=off 1=loshelf 2=lowcut(hiPass) 3=bell 4=hishelf 5=hicut(loPass)
    var freq: Int = 0x80, // 00-FF → log 20Hz–20kHz
    var gain: Int = 120,  // 0..240 → −12.0dB to +12.0dB (120 = 0dB, 0.1 dB/step)
    var q: Int = 0x80     // 00-FF → log 0.1–10.0
)

@Serializable
data class EqPreset(
    val id: Int,
    val bands: Array<EqBand> = Array(3) { EqBand() }
) {
    override fun equals(other: Any?): Boolean {
        if (this === other) return true
        if (javaClass != other?.javaClass) return false
        other as EqPreset
        return id == other.id && bands.contentEquals(other.bands)
    }

    override fun hashCode(): Int {
        var result = id
        result = 31 * result + bands.contentHashCode()
        return result
    }
}

// Track in song = sequence of chain references
@Serializable
data class Track(
    val id: Int,  // 0-7
    val chainRefs: MutableList<Int> = mutableListOf(),  // Chain IDs
    var volume: Int = 0xFF,  // 00-FF track volume (FF = max)
    var mute: Boolean = false  // Track mute state
)

// Instrument type selector
@Serializable
enum class InstrumentType { SAMPLER, SOUNDFONT }

/**
 * SF2 envelope/filter overrides (per-instrument, static customization of TSF preset).
 * -1 = use SF2 preset default; 0-255 = override.
 * Envelope: 0=instant (~0.001s), 255=long (~10s). Sustain: 0=silence, 255=full.
 * Filter: same 0-255 scale as sampler filterCut/filterRes; -1 = bypass Phase-7 filter.
 */
@Serializable
data class SFOverrides(
    val ampAttack:  Int = -1,
    val ampDecay:   Int = -1,
    val ampSustain: Int = -1,
    val ampRelease: Int = -1,
    val filterCut:  Int = -1,
    val filterRes:  Int = -1
)

// Instrument definition
@Serializable
data class Instrument(
    val id: Int,  // 00-FF (256 instrument slots)
    var name: String = "INST${id.toHex2()}",
    var sampleId: Int = -1,  // Which sample from resources (-1 = empty/no sample)
    var volume: Int = 0xFF,  // 00-FF instrument volume (FF = max)
    var pan: Int = 0x80,  // 00-FF pan (00=left, 80=center, FF=right)

    // Sample playback parameters
    var root: Note = Note.fromString("C-4"),  // Root note for sample pitch
    var detune: Int = 0x80,  // 00-FF: high nibble=semitones, low nibble=1/16ths of semitone

    // Distortion/bitcrusher parameters
    var drive: Int = 0x00,  // 00-FF: pre-gain boost (00=1.0x, 80=1.0x, FF=2.0x)
    var crush: Int = 0x0,  // 0-F: bit depth reduction (0=16-bit/off, F=1-bit)
    var downsample: Int = 0x0,  // 0-F: sample rate reduction (0=off, 1=÷2, 2=÷4, etc.)

    // Filter parameters
    var filterType: String = "off",  // "off", "lp" (low-pass), "hp" (high-pass), "bp" (band-pass)
    var filterCut: Int = 0x00,  // 00-FF: cutoff frequency
    var filterRes: Int = 0x00,  // 00-FF: resonance

    var sampleStart: Int = 0x00,  // 00-FF sample start point (00=start, FF=end)
    var sampleEnd: Int = 0xFF,  // 00-FF sample end point
    var reverse: Boolean = false,  // Reverse playback
    var loopMode: String = "off",  // "off", "fwd" (forward), "png" (ping-pong)
    var loopStart: Int = 0x00,  // 00-FF loop start point
    // 00-FF loop end point (FF = sample end). The loop runs [loopStart, loopEnd]; with an ADSR VOL
    // envelope, on note-off the voice leaves the loop and plays [loopEnd, sampleEnd] as the release tail.
    // Default FF reproduces the old "loop to sample end" behaviour for projects saved before this field.
    var loopEnd: Int = 0xFF,
    var sampleFilePath: String? = null,  // Path to loaded WAV file (null = use resource)

    // Table parameters
    var tableId: Int = -1,          // -1 = no table, 0-127 = table ID
    var tableTicRate: Int = 0x06,   // Default: 6 tics per row (2 rows per phrase step at 12 tics/step)

    // Modulation slots (4 per instrument)
    var modSlots: Array<ModSlot> = Array(4) { ModSlot() },

    // Instrument type (defaults to SAMPLER for backward compatibility)
    var instrumentType: InstrumentType = InstrumentType.SAMPLER,

    // SoundFont-specific fields (only used when instrumentType == SOUNDFONT)
    var soundfontPath: String? = null,  // Absolute path to .sf2 or .sf3 file
    var sfBank: Int = 0,               // Bank number (0-127)
    var sfPreset: Int = 0,             // Program/preset number (0-127)

    // SF2 preset parameter overrides (only used when instrumentType == SOUNDFONT)
    var sfOverrides: SFOverrides = SFOverrides(),

    // Send levels (00-FF each; 00 = silent, FF = full send)
    var reverbSend: Int = 0x00,
    var delaySend: Int = 0x00,

    // Per-instrument EQ slot (-1 = off, 00-7F = EQ preset index)
    var eqSlot: Int = -1,

    // Slice playback mode (0=OFF, 1=CUT, 2=TRU)
    // When non-OFF, phrase notes select a slice (C-4 = slice 0 relative to root) instead of pitch.
    // CUT: play slice[n] → slice[n+1]. TRU: play slice[n] → sample end.
    var slicingMode: Int = 0,
    // Slice marker positions in samples (absolute frame indices), sourced from WAV cue chunk.
    var sliceMarkers: List<Long> = emptyList()
) {
    /** True if this slot is configured as a SoundFont instrument. */
    fun isSoundfont(): Boolean = instrumentType == InstrumentType.SOUNDFONT

    /** Auto-generated slot name, "INSTxx". */
    fun defaultName(): String = "INST${id.toHex2()}"

    /** True while the name is still the auto-generated "INSTxx" — treated as "unset": the instrument
     *  screen shows it blank, and loading a sample/SF2 overwrites it with the file's name. */
    fun hasDefaultName(): Boolean = name == defaultName()

    /**
     * True when this slot holds nothing — no sample WAV and no SoundFont — so it is free to claim for a
     * new sample. `sampleFilePath == null` alone is NOT "empty": a configured SoundFont also nulls
     * sampleFilePath, so any code searching for a free slot must use this (otherwise a resample can
     * overwrite a SoundFont and leave a broken SOUNDFONT-typed slot with a sample).
     */
    fun isFree(): Boolean =
        sampleFilePath == null && soundfontPath == null && instrumentType == InstrumentType.SAMPLER
}

/**
 * Volume conversion utilities for hex-based gain staging
 */
object VolumeUtils {
    /**
     * Convert 00-FF hex value to 0.0-1.0 float
     */
    fun hexToFloat(hex: Int): Float = (hex and 0xFF) / 255f

    /**
     * Convert 0.0-1.0 float to 00-FF hex
     */
    fun floatToHex(f: Float): Int = (f.coerceIn(0f, 1f) * 255).toInt()

    /**
     * Calculate final volume with 4-stage gain staging:
     * instrument × phrase × track × master
     *
     * All inputs are 00-FF hex values, output is 0.0-1.0 float
     *
     * NOTE: Use this for OFFLINE RENDERING where all volumes need to be baked in.
     * For REAL-TIME PLAYBACK, use calculateNoteVolume() instead, which only bakes
     * instrument × phrase and lets C++ apply track × master in real-time.
     */
    fun calculateFinalVolume(
        instrumentVol: Int,
        phraseVol: Int,
        trackVol: Int,
        masterVol: Int
    ): Float {
        return hexToFloat(instrumentVol) *
               hexToFloat(phraseVol) *
               hexToFloat(trackVol) *
               hexToFloat(masterVol)
    }

    /**
     * Calculate note volume with 2-stage gain staging:
     * instrument × phrase
     *
     * All inputs are 00-FF hex values, output is 0.0-1.0 float
     *
     * IMPORTANT: Use this for REAL-TIME PLAYBACK where track × master volumes
     * are applied in the C++ audio engine in real-time. This allows volume
     * changes in the mixer to take effect immediately without rescheduling notes.
     *
     * For OFFLINE RENDERING, use calculateFinalVolume() instead which bakes
     * all 4 volume stages into the scheduled note's volume.
     */
    fun calculateNoteVolume(
        instrumentVol: Int,
        phraseVol: Int
    ): Float {
        return hexToFloat(instrumentVol) * hexToFloat(phraseVol)
    }

    /**
     * Convert 00-FF pan value to left/right gains using constant-power pan law.
     *
     * @param pan 00=full left, 80=center, FF=full right
     * @return Pair(leftGain, rightGain) where each is 0.0-1.0
     */
    fun panToGains(pan: Int): Pair<Float, Float> {
        val p = hexToFloat(pan)  // 0.0 = left, 1.0 = right
        val angle = p * (kotlin.math.PI / 2)
        val leftGain = kotlin.math.cos(angle).toFloat()
        val rightGain = kotlin.math.sin(angle).toFloat()
        return Pair(leftGain, rightGain)
    }

    /**
     * Format volume as 2-digit hex string
     */
    fun formatHex(value: Int): String = value.toHex2()
}

// The entire project
@Serializable
data class Project(
    var version: Int = 0,  // File format version. 0 = pre-versioning (old files), 1 = current
    var name: String = "UNTITLED",
    var tempo: Int = 128,  // BPM
    var transpose: Int = 0,
    var masterVolume: Int = 0xFF,  // 00-FF master volume (FF = max)
    var ottDepth: Int = 0,         // 00=bypass, FF=full OTT wet
    var masterBusFx: Int = 0,     // 0=OTT, 1=DUST
    var dustDepth: Int = 0,       // 00=bypass, FF=full DUST (only active when masterBusFx==1)
    var limiterPreGain: Int = 0,  // 00=unity(1.0x), FF=max drive(4.0x) into limiter

    // EQ preset bank (128 shared slots, referenced by instruments/sends/master)
    val eqPresets: Array<EqPreset> = Array(128) { EqPreset(it) },

    // Reverb send channel parameters
    var reverbFeedback: Int = 0x60,  // 00-FF decay time (maps to 0.0–0.98 feedback)
    var reverbDamp: Int = 0x80,      // 00-FF damping LP cutoff (maps to ~1kHz–20kHz)
    var reverbWet: Int = 0x80,       // 00-FF reverb return gain (controlled from mixer; not a dry/wet mix)
    var reverbInputEq: Int = -1,     // -1=off, 00-7F = EQ preset slot applied before reverb

    // Delay send channel parameters
    var delayTime: Int = 0x40,       // 00-FF free mode (1ms–2000ms) or 00-0B sync division index
    var delaySync: Boolean = false,  // false=free ms, true=BPM-synced subdivision
    var delayFeedback: Int = 0x60,   // 00-FF
    var delayWet: Int = 0x80,          // 00-FF return gain (controlled from mixer)
    var delayReverbSend: Int = 0x00,   // 00-FF how much delay output feeds into reverb bus
    var delayInputEq: Int = -1,        // -1=off, 00-7F = EQ preset slot applied before delay

    // Master chain EQ
    var masterEqSlot: Int = -1,      // -1=off, 00-7F = EQ preset slot on master bus

    // All phrases (256 slots)
    val phrases: Array<Phrase> = Array(256) { Phrase(it) },

    // All chains (256 slots)
    val chains: Array<Chain> = Array(256) { Chain(it) },

    // 8 tracks
    val tracks: Array<Track> = Array(8) { Track(it) },

    // Instruments (128 slots, 0x00–0x7F, all identical — sampleFilePath = null means empty).
    // 128 rather than 256 to align with the MIDI program range.
    val instruments: Array<Instrument> = Array(128) { index ->
        // All 128 instrument slots use the same layout:
        // name = "INSTXX", sampleId = index (slot ID), sampleFilePath = null (nothing loaded yet)
        // "empty" state is determined by sampleFilePath == null
        Instrument(
            id = index,
            name = "INST${index.toHex2()}",
            sampleId = index
        )
    },

    // Tables (128 slots, 0x00–0x7F, like phrases)
    val tables: Array<Table> = Array(128) { Table(it) },

    // Grooves (128 slots, 0x00–0x7F)
    val grooves: Array<Groove> = Array(128) { Groove(it) }
) {
    /** Convert the project-global transpose byte to signed semitones. See [byteToSignedSemitones]. */
    fun getTransposeSemitones(): Int = byteToSignedSemitones(transpose)

    // NOTE: Project intentionally has NO custom equals()/hashCode(). A previous version compared only
    // name + tempo, which made two completely different songs compare "equal" — a trap for anything
    // that dirty-checks projects or holds one in a mutableStateOf (structuralEqualityPolicy). The data
    // class default (all fields; array fields by reference) is safe: it never reports false equality.
    // Recomposition is driven by TrackerController.projectVersion, not by Project equality.
}