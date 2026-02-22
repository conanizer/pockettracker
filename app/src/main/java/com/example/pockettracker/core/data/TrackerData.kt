package com.example.pockettracker.core.data

import kotlinx.serialization.Serializable

// Note representation
@Serializable
data class Note(
    val pitch: Int,  // 0-119 (C-0 to B-9), -1 = empty
    val octave: Int  // 0-9
) {
    companion object {
        val EMPTY = Note(-1, 0)
        val NOTES = listOf("C-", "C#", "D-", "D#", "E-", "F-", "F#", "G-", "G#", "A-", "A#", "B-")

        fun fromMidi(midi: Int): Note {
            if (midi < 0 || midi > 119) return EMPTY
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
        return 440f * Math.pow(2.0, (midiNote - 69) / 12.0).toFloat()
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
    var instrument: Int = 0x00,  // 00-7F
    var volume: Int = 0xFF,      // 00-FF (FF = max)
    var fx1Type: Int = 0x00,     // Effect type (Milestone 2: NOW EDITABLE!)
    var fx1Value: Int = 0x00,    // Effect value
    var fx2Type: Int = 0x00,
    var fx2Value: Int = 0x00,
    var fx3Type: Int = 0x00,
    var fx3Value: Int = 0x00
) {
    fun isEmpty(): Boolean = note == Note.EMPTY
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
     * Get transpose in semitones (-127 to +128)
     * 0x00 = 0 semitones (no change)
     * 0x01-0x80 = +1 to +128 semitones (up)
     * 0xFF = -1 semitone
     * 0xFE = -2 semitones
     * 0x81 = -127 semitones (down)
     */
    fun getTransposeSemitones(index: Int): Int {
        val value = transposeValues[index]
        return if (value <= 0x80) {
            value  // 0x00-0x80 = 0 to +128
        } else {
            value - 256  // 0x81-0xFF = -127 to -1
        }
    }


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
    var name: String = "TBL${id.toString(16).padStart(2,'0').uppercase()}",
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

        /**
         * Convert transpose value to semitones
         * 00 = 0 semitones (no change)
         * 01-7F = +1 to +127 semitones (up)
         * 80-FF = -128 to -1 semitones (down)
         */
        fun transposeToSemitones(transpose: Int): Int {
            return if (transpose < 0x80) transpose else transpose - 256
        }
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
    TRACKING("TRK")   // future
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
    var amount: Int = 0x80,      // 00-FF

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

// Track in song = sequence of chain references
@Serializable
data class Track(
    val id: Int,  // 0-7
    val chainRefs: MutableList<Int> = mutableListOf(),  // Chain IDs
    var volume: Int = 0xFF,  // 00-FF track volume (FF = max)
    var mute: Boolean = false  // Track mute state
)

// Instrument definition
@Serializable
data class Instrument(
    val id: Int,  // 00-7F
    var name: String = "INST${id.toString(16).padStart(2,'0').uppercase()}",
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
    var sampleFilePath: String? = null,  // Path to loaded WAV file (null = use resource)

    // Table parameters
    var tableId: Int = -1,          // -1 = no table, 0-255 = table ID
    var tableTicRate: Int = 0x06,   // Default: 6 tics per row (2 rows per phrase step at 12 tics/step)

    // Modulation slots (4 per instrument)
    var modSlots: Array<ModSlot> = Array(4) { ModSlot() }
)

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
    fun formatHex(value: Int): String = (value and 0xFF).toString(16).uppercase().padStart(2, '0')
}

// The entire project
@Serializable
data class Project(
    var name: String = "UNTITLED",
    var tempo: Int = 128,  // BPM
    var transpose: Int = 0,
    var masterVolume: Int = 0xFF,  // 00-FF master volume (FF = max)

    // All phrases (256 slots)
    val phrases: Array<Phrase> = Array(256) { Phrase(it) },

    // All chains (256 slots)
    val chains: Array<Chain> = Array(256) { Chain(it) },

    // 8 tracks
    val tracks: Array<Track> = Array(8) { Track(it) },

    // Instruments (256 slots, but only first 12 initialized with resource samples)
    val instruments: Array<Instrument> = Array(256) { index ->
        if (index < 12) {
            // First 12 instruments (00-0B) map directly to the 12 resource samples
            // 00=kick, 01=snare, 02=hihat, 03=bass, 04=shimmer, 05=tambo,
            // 06=lofi, 07=choirstring, 08=apache162, 09=copta162, 0A=funky162, 0B=eightoeight
            Instrument(
                id = index,
                name = "INST${index.toString(16).padStart(2,'0').uppercase()}",
                sampleId = index  // Direct 1:1 mapping
            )
        } else {
            // Rest are empty instruments with no sample
            Instrument(
                id = index,
                name = "",
                sampleId = 0  // Default to kick, but name is empty so it's "uninitialized"
            )
        }
    },

    // Tables (256 slots, like phrases)
    val tables: Array<Table> = Array(256) { Table(it) },

    // Grooves (256 slots)
    val grooves: Array<Groove> = Array(256) { Groove(it) }
) {
    override fun equals(other: Any?): Boolean {
        if (this === other) return true
        if (javaClass != other?.javaClass) return false
        other as Project
        if (name != other.name) return false
        if (tempo != other.tempo) return false
        return true
    }

    override fun hashCode(): Int {
        var result = name.hashCode()
        result = 31 * result + tempo
        return result
    }
}