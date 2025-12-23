package com.example.pockettracker

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

    fun toHexString(): String {
        if (pitch == -1) return "---"
        return String.format("%02X", toMidi())
    }
}

// Single step in a phrase (one row)
@Serializable
data class PhraseStep(
    var note: Note = Note.EMPTY,
    var instrument: Int = 0x00,  // 00-7F
    var volume: Int = 0xFF,      // 00-FF (FF = max)
    val fx1Type: Int = 0x00,     // Effect type
    val fx1Value: Int = 0x00,    // Effect value
    val fx2Type: Int = 0x00,
    val fx2Value: Int = 0x00,
    val fx3Type: Int = 0x00,
    val fx3Value: Int = 0x00
) {
    fun isEmpty(): Boolean = note == Note.EMPTY

    fun toHexString(): String {
        return "${note} ${instrument.toString(16).padStart(2,'0').uppercase()} " +
                "${volume.toString(16).padStart(2,'0').uppercase()} " +
                "${fx1Type.toString(16).padStart(2,'0').uppercase()}${fx1Value.toString(16).padStart(2,'0').uppercase()} " +
                "${fx2Type.toString(16).padStart(2,'0').uppercase()}${fx2Value.toString(16).padStart(2,'0').uppercase()}" +
                "${fx3Type.toString(16).padStart(2,'0').uppercase()}${fx3Value.toString(16).padStart(2,'0').uppercase()}"
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
    val phraseRefs: IntArray = IntArray(16) { 0xFF },  // 0xFF = empty
    val transposeValues: IntArray = IntArray(16) { 0x00 }  // Default: 0x00 (can cycle 00-FF)
) {
    /**
     * Check if a slot is empty
     */
    fun isEmpty(index: Int): Boolean = phraseRefs[index] == 0xFF

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

    /**
     * Set transpose from semitones value
     * Accepts -127 to +128
     */
    fun setTransposeSemitones(index: Int, semitones: Int) {
        transposeValues[index] = if (semitones >= 0) {
            semitones.coerceIn(0, 128)  // 0x00-0x80
        } else {
            (256 + semitones).coerceIn(129, 255)  // 0x81-0xFF
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

// Track in song = sequence of chain references
@Serializable
data class Track(
    val id: Int,  // 0-7
    val chainRefs: MutableList<Int> = mutableListOf()  // Chain IDs
)

// Instrument definition
@Serializable
data class Instrument(
    val id: Int,  // 00-7F
    var name: String = "INST${id.toString(16).padStart(2,'0').uppercase()}",
    var sampleId: Int = 0,  // Which sample from resources
    var volume: Float = 1.0f,
    var pan: Float = 0.5f,  // 0.0 = left, 0.5 = center, 1.0 = right

    // Sample playback parameters
    var root: Note = Note.fromString("C-4"),  // Root note for sample pitch
    var detune: Int = 0x80,  // 00-FF: high nibble=semitones, low nibble=1/16ths of semitone
    var sampleStart: Int = 0x00,  // 00-FF sample start point (00=start, FF=end)
    var sampleEnd: Int = 0xFF,  // 00-FF sample end point
    var reverse: Boolean = false,  // Reverse playback
    var loopMode: String = "off",  // "off", "fwd" (forward), "png" (ping-pong)
    var loopStart: Int = 0x00,  // 00-FF loop start point
    var sampleFilePath: String? = null  // Path to loaded WAV file (null = use resource)
)

// The entire project
@Serializable
data class Project(
    var name: String = "UNTITLED",
    var tempo: Int = 128,  // BPM
    var transpose: Int = 0,
    var masterVolume: Float = 1.0f,

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
    }
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