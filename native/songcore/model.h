#ifndef POCKETTRACKER_SONGCORE_MODEL_H
#define POCKETTRACKER_SONGCORE_MODEL_H

// ─── Song data model, as C++ structs ────────────────────────────────────────────────────────────
//
// A 1:1 mirror of the Kotlin @Serializable classes in
//   app/src/main/java/com/conanizer/pockettracker/core/data/TrackerData.kt
//   ...................................................../InstrumentPreset.kt
// — same fields, same declaration order, same defaults, same pool sizes. TrackerData.kt is the
// executable spec (Linux-port plan §4.3/§4.4); this header is its C++ twin. project_io.h reads and
// writes the .ptp/.pti JSON on top of these structs, byte-for-byte compatible with the Kotlin
// kotlinx.serialization output.
//
// Two flavours of "default" live here and MUST NOT be confused (project_io.h relies on the split):
//   * FIELD default   — the value declared on the @Serializable property. This is what
//                       encodeDefaults=false compares against to decide omission, and what a
//                       missing JSON key deserializes to. It is encoded in the member initializers
//                       and the (int id) constructors below.
//   * FACTORY value   — what a *fresh default Project* actually contains. Differs from the field
//                       default for exactly one field: Instrument.sampleId, which the Project
//                       factory sets to the slot index (not the field default -1) — so every
//                       instrument slot serialises its sampleId. See make_default_project().
//
// No floating-point anywhere in this schema — every value is int / int64 / bool / string / enum /
// nested struct / array. That is why the .ptp round-trip can be exact with no float formatting.
//
// This header has NO third-party dependency (no nlohmann) so the future C++ scheduler can include
// the model without pulling the JSON library.

#include <cstdint>
#include <string>
#include <vector>
#include <optional>

// The event schema — for the CC-slot ids `resolve_cc_param` below translates. event.h depends on
// nothing but <cstdint>/<cstring>, so this stays a leaf-ward include and no cycle is possible.
#include "event.h"

namespace songcore {

// ─── pool sizes ─────────────────────────────────────────────────────────────────────────────────

// Canonical pool sizes (single source, mirrors TrackerData.kt). Declared above the structs so the
// members and the accessors below can both spell them.
constexpr int POOL_PHRASES     = 256;
constexpr int POOL_CHAINS      = 256;
constexpr int POOL_TRACKS      = 8;
constexpr int POOL_INSTRUMENTS = 128;
constexpr int POOL_TABLES      = 128;
constexpr int POOL_GROOVES     = 128;
constexpr int POOL_EQPRESETS   = 128;
constexpr int POOL_SCALES      = 16;

// Rows in one chain, and steps in one phrase. Fixed by the screen geometry, not by a pool.
constexpr int CHAIN_ROWS  = 16;
constexpr int PHRASE_ROWS = 16;

// ─── small helpers ────────────────────────────────────────────────────────────────────────────

// lower 8 bits as 2-digit UPPERCASE hex — mirrors TrackerData.Int.toHex2().
inline std::string hex2(int v) {
    static const char* H = "0123456789ABCDEF";
    unsigned b = static_cast<unsigned>(v) & 0xFFu;
    std::string s(2, '0');
    s[0] = H[(b >> 4) & 0xF];
    s[1] = H[b & 0xF];
    return s;
}
inline std::string default_instrument_name(int id) { return "INST" + hex2(id); }  // Instrument.name default
inline std::string default_table_name(int id)      { return "TBL"  + hex2(id); }  // Table.name default

// ─── enums (kotlinx serialises enum entries by their NAME) ──────────────────────────────────────

enum class ModType { NONE, AHD, ADSR, LFO, DRUM, TRIG, TRACKING, SCALAR };
enum class ModDest {
    NONE, VOLUME, PAN, PITCH, FINE_PITCH, FILTER_CUTOFF, FILTER_RES,
    SAMPLE_START, MOD_AMT, MOD_RATE, MOD_BOTH
};
// The instrument's ROUTING DESTINATION — which sound module consumes its event stream (MIDI plan §1),
// not "what kind of instrument it is". SAMPLER and SOUNDFONT are consumed by EngineConsumer; EXTERNAL
// leaves the process as MIDI bytes (ExternalConsumer, midi_out.h). Future synth modules append here.
// ⚠️ EXTERNAL STAYS LAST. A build without the MIDI surfaces (native/ui/platform_caps.h `midi`) walks
// the TYPE cell over one type fewer, which only hides EXTERNAL while EXTERNAL is the tail.
enum class InstrumentType { SAMPLER, SOUNDFONT, EXTERNAL };

inline const char* mod_type_name(ModType t) {
    switch (t) {
        case ModType::NONE:     return "NONE";
        case ModType::AHD:      return "AHD";
        case ModType::ADSR:     return "ADSR";
        case ModType::LFO:      return "LFO";
        case ModType::DRUM:     return "DRUM";
        case ModType::TRIG:     return "TRIG";
        case ModType::TRACKING: return "TRACKING";
        case ModType::SCALAR:   return "SCALAR";
    }
    return "NONE";
}
inline bool mod_type_from_name(const std::string& s, ModType& out) {
    if (s == "NONE") { out = ModType::NONE; return true; }
    if (s == "AHD")  { out = ModType::AHD;  return true; }
    if (s == "ADSR") { out = ModType::ADSR; return true; }
    if (s == "LFO")  { out = ModType::LFO;  return true; }
    if (s == "DRUM") { out = ModType::DRUM; return true; }
    if (s == "TRIG") { out = ModType::TRIG; return true; }
    if (s == "TRACKING") { out = ModType::TRACKING; return true; }
    if (s == "SCALAR")   { out = ModType::SCALAR;   return true; }
    return false;
}
inline const char* mod_dest_name(ModDest d) {
    switch (d) {
        case ModDest::NONE:          return "NONE";
        case ModDest::VOLUME:        return "VOLUME";
        case ModDest::PAN:           return "PAN";
        case ModDest::PITCH:         return "PITCH";
        case ModDest::FINE_PITCH:    return "FINE_PITCH";
        case ModDest::FILTER_CUTOFF: return "FILTER_CUTOFF";
        case ModDest::FILTER_RES:    return "FILTER_RES";
        case ModDest::SAMPLE_START:  return "SAMPLE_START";
        case ModDest::MOD_AMT:       return "MOD_AMT";
        case ModDest::MOD_RATE:      return "MOD_RATE";
        case ModDest::MOD_BOTH:      return "MOD_BOTH";
    }
    return "NONE";
}
inline bool mod_dest_from_name(const std::string& s, ModDest& out) {
    if (s == "NONE")          { out = ModDest::NONE;          return true; }
    if (s == "VOLUME")        { out = ModDest::VOLUME;        return true; }
    if (s == "PAN")           { out = ModDest::PAN;           return true; }
    if (s == "PITCH")         { out = ModDest::PITCH;         return true; }
    if (s == "FINE_PITCH")    { out = ModDest::FINE_PITCH;    return true; }
    if (s == "FILTER_CUTOFF") { out = ModDest::FILTER_CUTOFF; return true; }
    if (s == "FILTER_RES")    { out = ModDest::FILTER_RES;    return true; }
    if (s == "SAMPLE_START")  { out = ModDest::SAMPLE_START;  return true; }
    if (s == "MOD_AMT")       { out = ModDest::MOD_AMT;       return true; }
    if (s == "MOD_RATE")      { out = ModDest::MOD_RATE;      return true; }
    if (s == "MOD_BOTH")      { out = ModDest::MOD_BOTH;      return true; }
    return false;
}
inline const char* instrument_type_name(InstrumentType t) {
    switch (t) {
        case InstrumentType::SOUNDFONT: return "SOUNDFONT";
        case InstrumentType::EXTERNAL:  return "EXTERNAL";
        case InstrumentType::SAMPLER:   break;
    }
    return "SAMPLER";
}
inline bool instrument_type_from_name(const std::string& s, InstrumentType& out) {
    if (s == "SAMPLER")   { out = InstrumentType::SAMPLER;   return true; }
    if (s == "SOUNDFONT") { out = InstrumentType::SOUNDFONT; return true; }
    if (s == "EXTERNAL")  { out = InstrumentType::EXTERNAL;  return true; }
    return false;
}
/** How many entries InstrumentType has — the INSTRUMENT screen's TYPE cell cycles on it. */
inline constexpr int INSTRUMENT_TYPE_COUNT = 3;

// ─── display names (ModType.displayName / ModDest.displayName) ──────────────────────────────────
//
// ⚠️ NOT the same strings as `mod_type_name` / `mod_dest_name` above, and the difference is the whole
// reason both exist. Those are the SERIALISED names — kotlinx writes an enum entry by its Kotlin
// identifier, so "TRACKING" and "FILTER_CUTOFF" are load-bearing bytes in every .ptp and changing one
// breaks every saved project. These are what the MODS screen PAINTS ("TRK", "CUT"), chosen to fit a
// narrow cell. Reusing one for the other silently either widens the UI or corrupts the file format.

inline const char* mod_type_display_name(ModType t) {
    switch (t) {
        case ModType::NONE:     return "---";
        case ModType::AHD:      return "AHD";
        case ModType::ADSR:     return "ADSR";
        case ModType::LFO:      return "LFO";
        case ModType::DRUM:     return "DRUM";   // AHD semantics (engine type 4)
        case ModType::TRIG:     return "TRIG";   // ADSR semantics (engine type 5)
        case ModType::TRACKING: return "TRK";    // future — no engine implementation
        case ModType::SCALAR:   return "SCL";    // constant value — `amount` IS the output
    }
    return "---";
}

inline const char* mod_dest_display_name(ModDest d) {
    switch (d) {
        case ModDest::NONE:          return "---";
        case ModDest::VOLUME:        return "VOL";
        case ModDest::PAN:           return "PAN";
        case ModDest::PITCH:         return "PITCH";
        case ModDest::FINE_PITCH:    return "FINE";
        case ModDest::FILTER_CUTOFF: return "CUT";
        case ModDest::FILTER_RES:    return "RES";
        case ModDest::SAMPLE_START:  return "STA";
        case ModDest::MOD_AMT:       return "MOD A";
        case ModDest::MOD_RATE:      return "MOD R";
        case ModDest::MOD_BOTH:      return "MOD B";
    }
    return "---";
}

/** How many ModDest entries there are — the MODS screen's DEST cycle wraps on it. */
inline constexpr int MOD_DEST_COUNT = 11;

// ─── leaf structs ───────────────────────────────────────────────────────────────────────────────

// Note has no field defaults in Kotlin (both ctor params are required) — a Note object always
// serialises BOTH pitch and octave. Our default ctor is Note.EMPTY for convenience.
struct Note {
    int pitch  = -1;  // 0-11 chromatic, -1 = empty
    int octave = 0;
    bool operator==(const Note& o) const { return pitch == o.pitch && octave == o.octave; }
    bool operator!=(const Note& o) const { return !(*this == o); }
    static Note EMPTY() { return Note{-1, 0}; }
    static Note C4()    { return Note{ 0, 4}; }  // Note.fromString("C-4")
};

// ─── Note ↔ MIDI ↔ display (TrackerData.Note's own methods) ───────────────────────────────────────
// Note's arithmetic, not the scheduler's: it lived in scheduler.h until the UI needed it, and the UI
// has no business including the sequencer to name a note.
inline int note_to_midi(const Note& n) {
    if (n.pitch == -1) return -1;
    return (n.octave + 1) * 12 + n.pitch;  // C-4 = 60 (standard MIDI)
}
inline Note note_from_midi(int midi) {
    if (midi < 0 || midi > 127) return Note::EMPTY();
    return Note{midi % 12, midi / 12 - 1};
}

// VolumeUtils.hexToFloat — an authored 0x00-0xFF byte as a 0-1 gain. Here for note_to_midi's reason,
// and it moved for the third time the same argument has been made (note_to_midi, then chain_is_empty in
// phase C): it is the model's own arithmetic, three files outside the sequencer now need it, and a
// second copy is two rules that agree until one of them is edited. scheduler.h still sees it — it
// includes this header — so every existing caller is untouched.
inline float hex_to_float(int hex) { return (hex & 0xFF) / 255.0f; }

// Note.NOTES — the chromatic names, two chars each so every note renders in a fixed 3-char cell.
inline const char* const NOTE_NAMES[12] = {"C-", "C#", "D-", "D#", "E-", "F-",
                                           "F#", "G-", "G#", "A-", "A#", "B-"};

/** Note.toString(): "C-4", or "---" when empty. The 3-char cell every editor grid draws. */
inline std::string note_name(const Note& n) {
    if (n.pitch < 0 || n.pitch > 11) return "---";
    return std::string(NOTE_NAMES[n.pitch]) + std::to_string(n.octave);
}

struct PhraseStep {
    Note note = Note::EMPTY();
    int  instrument = 0x00;
    int  volume     = 0x7F;
    int  fx1Type = 0x00, fx1Value = 0x00;
    int  fx2Type = 0x00, fx2Value = 0x00;
    int  fx3Type = 0x00, fx3Value = 0x00;
};

// Indexed FX access on a PhraseStep — the flat fx{1,2,3}{Type,Value} fields read as a 1..3 slot
// array. Mirrors PhraseStep.fx / fxType / setFx / setFxValue.
//
// ⚠️ They live beside the struct rather than in the sequencer that walks it, for the reason
// `chain_is_empty` does: two layers now index a step's slots. The scheduler folds them last-wins and
// throws the slot NUMBER away; `automation.h` pairs AUS with the effect to its LEFT and therefore
// cannot. A second copy of the indexing would be two things that agree until the day one gains a
// fourth slot.
inline int  step_fx_type(const PhraseStep& s, int slot) {
    return slot == 1 ? s.fx1Type : slot == 2 ? s.fx2Type : slot == 3 ? s.fx3Type : 0;
}
inline int  step_fx_value(const PhraseStep& s, int slot) {
    return slot == 1 ? s.fx1Value : slot == 2 ? s.fx2Value : slot == 3 ? s.fx3Value : 0;
}
inline void step_set_fx(PhraseStep& s, int slot, int type, int value) {
    if (slot == 1) { s.fx1Type = type; s.fx1Value = value; }
    else if (slot == 2) { s.fx2Type = type; s.fx2Value = value; }
    else if (slot == 3) { s.fx3Type = type; s.fx3Value = value; }
}
inline void step_set_fx_value(PhraseStep& s, int slot, int value) {
    if (slot == 1) s.fx1Value = value;
    else if (slot == 2) s.fx2Value = value;
    else if (slot == 3) s.fx3Value = value;
}
/** True when any of the three slots carries `type` — the AUTHORED step, before CHA/RND touch it. */
inline bool step_has_fx(const PhraseStep& s, int type) {
    return s.fx1Type == type || s.fx2Type == type || s.fx3Type == type;
}
inline bool step_empty(const PhraseStep& s) { return s.note == Note::EMPTY(); }

struct Phrase {
    int id = 0;
    std::vector<PhraseStep> steps = std::vector<PhraseStep>(16);  // Array<PhraseStep>(16)
    Phrase() = default;
    explicit Phrase(int id_) : id(id_) {}
};

struct Chain {
    int id = 0;
    std::vector<int> phraseRefs      = std::vector<int>(16, -1);  // IntArray(16){-1}
    std::vector<int> transposeValues = std::vector<int>(16, 0);   // IntArray(16){0}
    Chain() = default;
    explicit Chain(int id_) : id(id_) {}
};

// The phrase a chain row names, or -1 for "there is nothing to play here".
//
// ⚠️ Every bound on a chain row lives in this one function, because each one is a property of the
// DATA rather than of any caller: a chain has CHAIN_ROWS rows, its arrays may be shorter than that
// (`parse_int_array` returns whatever length the JSON held, and normalize_project repairs pool
// sizes, not the arrays inside a Chain), and a ref outside the phrase pool names no phrase. A `.ptp`
// is a file users copy between devices, hand-edit, and recover from a half-written autosave, so a
// ref of 9999 is reachable without the editor ever producing one — and the value then goes straight
// into `project.phrases[ref]`.
inline int chain_phrase_ref(const Chain& c, int row) {
    if (row < 0 || row >= CHAIN_ROWS || row >= static_cast<int>(c.phraseRefs.size())) return -1;
    const int ref = c.phraseRefs[static_cast<size_t>(row)];
    return (ref >= 0 && ref < POOL_PHRASES) ? ref : -1;
}

// A chain row with no phrase in it. `-1` is the pool's "empty value" everywhere (never 0, never 0xFF).
//
// ⚠️ Lives HERE, not in scheduler.h where it was written, because two different layers now decide
// how long a song row is: `updatePlaybackBuffer` (which schedules it) and `nominal_spp_beats`
// (midi_clock.h, which tells a drum machine where the playhead landed). A second copy of the
// predicate would be two things that agree until the day they do not — and a Song Position Pointer
// computed from a different notion of "empty" than the scheduler's is a number that matches nothing.
//
// A row whose ref points outside the pool is empty for the same reason: it names no phrase, so
// nothing can be scheduled from it, so no layer may count it as length.
inline bool chain_is_empty(const Chain& c, int index) { return chain_phrase_ref(c, index) < 0; }

struct TableRow {
    int transpose = 0x00;
    int volume    = -1;
    int fx1Type = 0x00, fx1Value = 0x00;
    int fx2Type = 0x00, fx2Value = 0x00;
    int fx3Type = 0x00, fx3Value = 0x00;
};

struct Table {
    int id = 0;
    std::string name = default_table_name(0);
    std::vector<TableRow> rows = std::vector<TableRow>(16);  // Array<TableRow>(16)
    Table() = default;
    explicit Table(int id_) : id(id_), name(default_table_name(id_)) {}
};

struct ModSlot {
    ModType type = ModType::NONE;
    ModDest dest = ModDest::NONE;
    int amount = 0xFF;
    int attack = 0x00, hold = 0x00, decay = 0x00;
    int sustain = 0x80;
    int release = 0x00;
    int oscShape = 0x00, lfoTrigMode = 0x00;
    int lfoFreq = 0x40;
};

/**
 * ModSlot.rowCount() — how many rows this slot occupies on the MODS screen, TYPE row included.
 *
 * It belongs to the data model rather than to the UI because it is a fact about the TYPE — an AHD has
 * a HOLD and an ADSR has a SUSTAIN, and no screen gets to disagree. Both the MODS cursor (how far
 * down you can walk) and its renderer (how many rows to paint) read it, and if they read different
 * tables the cursor lands on a row that is not drawn.
 */
inline int mod_slot_row_count(const ModSlot& s) {
    switch (s.type) {
        case ModType::NONE:     return 1;  // TYPE
        case ModType::AHD:      return 6;  // TYPE, DEST, AMT, ATK, HOLD, DEC
        case ModType::ADSR:     return 7;  // TYPE, DEST, AMT, ATK, DEC, SUS, REL
        case ModType::LFO:      return 6;  // TYPE, DEST, AMT, OSC, TRIG, FREQ
        case ModType::DRUM:     return 6;  // as AHD
        case ModType::TRIG:     return 7;  // as ADSR
        case ModType::TRACKING: return 5;  // future
        case ModType::SCALAR:   return 3;  // TYPE, DEST, AMT
    }
    return 1;
}

struct Groove {
    int id = 0;
    std::vector<int> steps = std::vector<int>(16, -1);  // IntArray(16){-1}
    Groove() = default;
    explicit Groove(int id_) : id(id_) {}
};

/**
 * One of the project's 16 scales: which of the twelve chromatic intervals are IN the scale.
 *
 * The intervals are counted from the KEY, not from C — degree 0 is the root — so one Scale object
 * describes a shape (major, dorian, in-sen) that the key then positions. That is why the key is NOT
 * a field here: the same slot transposed to D must be the same slot.
 *
 * ⚠️ ALL TWELVE ENABLED IS THE CHROMATIC SCALE, and it is the default deliberately. A project that
 * has never seen this feature quantizes to every note, i.e. does not quantize — so the feature costs
 * an existing song nothing, with no migration step. Every consumer must keep that property.
 *
 * ⏸️ `offset` is WRITTEN AND NOT READ. Hundredths of a semitone, −2400..+2400, per degree: the
 * microtuning half of the feature, which reaches the pitch computation rather than the editor and is
 * deferred past 1.0. It is serialised from the first version so that switching it on later changes
 * no saved song.
 */
struct Scale {
    int id = 0;
    std::string name;                                        // "" = never named
    std::vector<int> enabled = std::vector<int>(12, 1);      // 1 = the degree is in the scale
    std::vector<int> offset  = std::vector<int>(12, 0);      // ⏸️ centi-semitones, −2400..+2400
    Scale() = default;
    explicit Scale(int id_) : id(id_) {}
};

/** Is every degree in? Then the scale constrains nothing and every quantizer is the identity. */
inline bool scale_is_chromatic(const Scale& s) {
    if (s.enabled.size() != 12) return true;   // a malformed pool must not silently mute notes
    for (int e : s.enabled)
        if (e == 0) return false;
    return true;
}

struct EqBand {
    int type = 0;
    int freq = 0x80;
    int gain = 120;
    int q    = 0x80;
};

struct EqPreset {
    int id = 0;
    std::vector<EqBand> bands = std::vector<EqBand>(3);  // Array<EqBand>(3)
    EqPreset() = default;
    explicit EqPreset(int id_) : id(id_) {}
};

struct Track {
    int id = 0;
    std::vector<int> chainRefs;   // mutableListOf() — empty default
    int  volume = 0xFF;
    bool mute   = false;
    bool solo   = false;
    Track() = default;
    explicit Track(int id_) : id(id_) {}
};

struct SFOverrides {
    int ampAttack = -1, ampDecay = -1, ampSustain = -1, ampRelease = -1;
    int filterCut = -1, filterRes = -1;
    bool operator==(const SFOverrides& o) const {
        return ampAttack == o.ampAttack && ampDecay == o.ampDecay && ampSustain == o.ampSustain &&
               ampRelease == o.ampRelease && filterCut == o.filterCut && filterRes == o.filterRes;
    }
    bool operator!=(const SFOverrides& o) const { return !(*this == o); }
};

/**
 * One of an EXTERNAL instrument's four CC slots (M8's CCA–CCJ, cut to four — MIDI plan §7).
 *
 * `cc` is the controller NUMBER the slot owns and `value` the default sent WITH each note-on; −1 in
 * either means "unused", which is the project-wide empty convention and not a magic 0xFF.
 */
struct MidiCcSlot {
    int cc    = -1;   // -1 = slot unused | 0-127
    int value = -1;   // -1 = send nothing on note-on | 0-127
    bool operator==(const MidiCcSlot& o) const { return cc == o.cc && value == o.value; }
    bool operator!=(const MidiCcSlot& o) const { return !(*this == o); }
};

constexpr int MIDI_CC_SLOTS = 4;

struct Instrument {
    int id = 0;
    std::string name = default_instrument_name(0);
    int sampleId = -1;                       // FIELD default -1 (factory overrides to slot index)
    int volume = 0xFF;
    int pan = 0x80;
    Note root = Note::C4();
    int detune = 0x80;
    int drive = 0x00, crush = 0x0, downsample = 0x0;
    std::string filterType = "off";
    int filterCut = 0x00, filterRes = 0x00;
    int sampleStart = 0x00, sampleEnd = 0xFF;
    bool reverse = false;
    std::string loopMode = "off";
    int loopStart = 0x00, loopEnd = 0xFF;
    std::optional<std::string> sampleFilePath;   // null
    int tableId = -1, tableTicRate = 0x06;
    std::vector<ModSlot> modSlots = std::vector<ModSlot>(4);  // Array<ModSlot>(4)
    InstrumentType instrumentType = InstrumentType::SAMPLER;
    std::optional<std::string> soundfontPath;    // null
    int sfBank = 0, sfPreset = 0;
    SFOverrides sfOverrides{};
    int reverbSend = 0x00, delaySend = 0x00;
    int eqSlot = -1;
    int slicingMode = 0;
    std::vector<int64_t> sliceMarkers;           // emptyList()

    /**
     * Does this instrument follow note TRANSPOSITION at all? (M8's `TRANSP.`)
     *
     * ⚠️ It is not a scale switch, and the wider meaning is M8's rather than a choice made here: OFF
     * silences the scale quantizer, the chain TSP column AND the project transpose for this
     * instrument. That is what makes it useful — a drum kit whose slots are pitched by hand must not
     * move when the song is transposed either.
     *
     * ⏸️ Only the scale quantizer reads it today (`Sequencer::emit_note`); the two transposes join
     * when they are quantized.
     */
    bool transposeEnabled = true;

    // ── EXTERNAL (instrumentType == EXTERNAL) — MIDI plan §7 ─────────────────────────────────────
    // Every one of these is ignored by the other two types, and every one has a default, so an
    // instrument that is not EXTERNAL serialises exactly the bytes it always did.
    //
    // `volume` and `pan` are REUSED rather than duplicated: volume scales the note-on velocity
    // (LGPT-style) and pan becomes CC 10. An external instrument therefore mixes with the same two
    // cells as every other instrument, which is the point of one protocol.
    int midiChannel = 0;    // 0-15, shown 1-16
    int midiBank    = -1;   // -1 = send nothing; else the bank sent (CC0/CC32) before the program
    int midiProgram = -1;   // -1 = send nothing; else the program sent with the first note-on
    /**
     * Gate length in TICKS (LGPT's LEN), 0 = gate-to-next.
     *
     * ⚠️ Non-zero and gate-to-next are different mechanisms, not one with a special case: a LEN gate
     * schedules its own note-off at emit time, while 0 leaves the note sounding until the NEXT note-on
     * or KIL lands on the track — which is exactly what the sampler's cut behaviour does, and is why an
     * external synth follows a phrase's rests the way the internal one does.
     */
    int midiLen = 0;
    std::vector<MidiCcSlot> midiCC = std::vector<MidiCcSlot>(MIDI_CC_SLOTS);

    Instrument() = default;
    explicit Instrument(int id_) : id(id_), name(default_instrument_name(id_)) {}
};

/**
 * Is a note on this instrument a PITCH, or is it choosing a slice?
 *
 * ⚠️ **A SLICED INSTRUMENT'S NOTE IS A SELECTOR, NOT A PITCH** — C-4 is slice 0, C#4 is slice 1, and
 * the sound they make has nothing to do with the semitone between them. Anything that moves notes
 * around musically (the scale quantizer today, the transposes when they follow) must ask this first
 * and leave the note alone when the answer is true, or a drum kit plays a different drum.
 *
 * `sliceOverride` is the step's own SLI value, -1 for none: an SLI turns slice selection on for one
 * note even when the instrument's own slicing mode is off.
 *
 * ⚠️ It is written here, next to the two fields it reads, because `voice_derive.h` decides the very
 * same question when it picks the slice and the two answers MUST be the same one. Two copies of this
 * condition is a note quantized here and sliced there.
 */
inline bool note_selects_slice(const Instrument& ins, int sliceOverride) {
    return (ins.slicingMode != 0 || sliceOverride >= 0) && !ins.sliceMarkers.empty();
}

/**
 * Instrument.hasDefaultName() — the name is still the auto-generated "INSTxx", i.e. nobody has named
 * this slot. The INSTRUMENT screen draws "______" for it and the pool draws a dim placeholder row;
 * loading a sample or an SF2 overwrites it with the file's name.
 */
inline bool instrument_has_default_name(const Instrument& ins) {
    return ins.name == default_instrument_name(ins.id);
}

/**
 * Instrument.isFree() — this slot holds NOTHING and may be claimed for a new sample.
 *
 * ⚠️ `sampleFilePath == null` alone is NOT "empty", and this is the trap the predicate exists to close:
 * a fully configured SoundFont instrument ALSO has a null sampleFilePath. Search for a free slot with
 * that test and a resample will happily overwrite a SoundFont, leaving a SOUNDFONT-typed slot with a
 * WAV behind it — broken in a way that looks fine until it is played.
 *
 * (The sibling convention is unchanged and still holds: for "is there a sound in this slot to PLAY",
 * `sampleFilePath == null` IS the single signal, and the note consumer drops on it.)
 */
inline bool instrument_is_free(const Instrument& ins) {
    return !ins.sampleFilePath.has_value() && !ins.soundfontPath.has_value() &&
           ins.instrumentType == InstrumentType::SAMPLER;
}

/**
 * Does this instrument's event stream leave the process (MIDI plan §4.3)?
 *
 * The ONE routing question the bus asks, and it is deliberately a model predicate rather than a
 * consumer's private test: BOTH consumers ask it, and they must agree on every event or a note is
 * either played twice or dropped. See midi_out.h.
 */
inline bool instrument_routes_external(const Instrument& ins) {
    return ins.instrumentType == InstrumentType::EXTERNAL;
}

/**
 * The controller number a bus CC event names, for THIS instrument (MIDI phase D).
 *
 * A literal 0-127 passes through. A symbolic slot id (event.h `CC_SLOT_A`..`CC_SLOT_D`, what a
 * `CCA`-`CCD` phrase command emits) names a LETTER, and the number that letter stands for is the
 * instrument's own — `midiCC[slot].cc`. **−1 = nothing to move**: the slot is unassigned, or the id
 * is one this build does not know.
 *
 * ⚠️ It lives HERE, beside `instrument_routes_external`, for the identical reason and it is not
 * tidiness: BOTH consumers translate these ids (engine_consumer.h resolves them to engine params,
 * midi_out.h to bytes on a wire), and two private copies of the rule are two things that agree until
 * the day one of them is edited. Same argument that moved `chain_is_empty` out of the scheduler.
 *
 * ⚠️ And −1 must never be "fall back to the raw id": `CC_SLOT_A` is 128, which masks to CC 0 — BANK
 * SELECT. A `CCA` on an instrument with no slot A would re-bank the device instead of doing nothing.
 */
inline int resolve_cc_param(const Instrument& ins, uint8_t param) {
    const int slot = cc_slot_index(param);
    if (slot < 0) return param <= 127 ? static_cast<int>(param) : -1;
    if (static_cast<size_t>(slot) >= ins.midiCC.size()) return -1;
    return ins.midiCC[static_cast<size_t>(slot)].cc;
}

struct Project {
    int version = 0;
    std::string name = "UNTITLED";
    int tempo = 128;
    int transpose = 0;
    int masterVolume = 0xFF;
    int ottDepth = 0, masterBusFx = 0, dustDepth = 0, limiterPreGain = 0;
    std::vector<EqPreset> eqPresets;              // Array(128){EqPreset(it)} — filled by factory
    int reverbFeedback = 0x60, reverbDamp = 0x80, reverbWet = 0x80, reverbInputEq = -1;
    int delayTime = 0x40;
    bool delaySync = false;
    int delayFeedback = 0x60, delayWet = 0x80, delayReverbSend = 0x00, delayInputEq = -1;
    int masterEqSlot = -1;
    std::vector<Phrase>     phrases;              // Array(256){Phrase(it)}
    std::vector<Chain>      chains;               // Array(256){Chain(it)}
    std::vector<Track>      tracks;               // Array(8){Track(it)}
    std::vector<Instrument> instruments;          // Array(128){ Instrument(id=i, sampleId=i) }
    std::vector<Table>      tables;               // Array(128){Table(it)}
    std::vector<Groove>     grooves;              // Array(128){Groove(it)}
    std::vector<Scale>      scales;               // Array(16){Scale(it)} — slot 00 is every track's

    /**
     * The root note of the default scale, 0-11 (0 = C). Global, and the only half of "what scale am
     * I in" that is not the slot: a slot is a shape, the key is where that shape starts.
     *
     * A track can be moved off it at playback by the SCA command; nothing stores that, exactly as
     * nothing stores the groove GRV assigns.
     */
    int scaleKey = 0;

    // ── MIDI, the parts that are MUSICAL INTENT and so travel with the song (MIDI plan §7) ───────
    // The device PICKS do not live here — they are settings.json (settings_store.h), because a project
    // carried to another machine keeps its routing and its sync intent and re-picks its cables.
    int  midiSyncOut = 0;               // 0 OFF | 1 CLOCK | 2 TRANSPORT | 3 CLOCK+TRANSPORT (phase C)
    bool midiSendProgramChange = true;
    std::vector<int> midiInputChannels = std::vector<int>(POOL_TRACKS, -1);  // per-track input channel
};

// ── Which tracks are making sound ────────────────────────────────────────────────────────────────
//
// ⚠️ DERIVED, NEVER STORED. Solo is a property of the SET of tracks, not of one of them: the answer
// for track 3 changes when track 5 is soloed. A cached per-track "audible" flag would have to be
// rewritten on every toggle and would be wrong the first time a site forgot, so every consumer —
// both schedulers, the render, the traversal, the engine push and both screens — asks here instead.
//
// Mute wins over solo: a soloed track that is also muted stays silent, because MUTE is the explicit
// statement about that one channel and SOLO is a statement about the others.
inline bool any_solo(const Project& p) {
    for (const Track& t : p.tracks)
        if (t.solo) return true;
    return false;
}

inline bool track_audible(const Project& p, const Track& t) {
    return !t.mute && (t.solo || !any_solo(p));
}

inline bool track_audible(const Project& p, int trackId) {
    if (trackId < 0 || trackId >= static_cast<int>(p.tracks.size())) return false;
    return track_audible(p, p.tracks[static_cast<size_t>(trackId)]);
}

struct InstrumentPreset {
    int version = 1;
    Instrument instrument;
    std::optional<std::vector<TableRow>> tableRows;  // null
};

// A fresh default Project — the exact object graph kotlinx builds from `Project()`. Note the one
// factory-vs-field-default divergence: instrument.sampleId is set to the slot index here.
inline Project make_default_project() {
    Project p;
    p.eqPresets.reserve(POOL_EQPRESETS);
    for (int i = 0; i < POOL_EQPRESETS; ++i) p.eqPresets.emplace_back(i);
    p.phrases.reserve(POOL_PHRASES);
    for (int i = 0; i < POOL_PHRASES; ++i) p.phrases.emplace_back(i);
    p.chains.reserve(POOL_CHAINS);
    for (int i = 0; i < POOL_CHAINS; ++i) p.chains.emplace_back(i);
    p.tracks.reserve(POOL_TRACKS);
    for (int i = 0; i < POOL_TRACKS; ++i) p.tracks.emplace_back(i);
    p.instruments.reserve(POOL_INSTRUMENTS);
    for (int i = 0; i < POOL_INSTRUMENTS; ++i) {
        Instrument ins(i);
        ins.sampleId = i;  // factory value (Project's Array(128) initializer)
        p.instruments.push_back(std::move(ins));
    }
    p.tables.reserve(POOL_TABLES);
    for (int i = 0; i < POOL_TABLES; ++i) p.tables.emplace_back(i);
    p.grooves.reserve(POOL_GROOVES);
    for (int i = 0; i < POOL_GROOVES; ++i) p.grooves.emplace_back(i);
    p.scales.reserve(POOL_SCALES);
    for (int i = 0; i < POOL_SCALES; ++i) p.scales.emplace_back(i);
    return p;
}

}  // namespace songcore

#endif  // POCKETTRACKER_SONGCORE_MODEL_H
