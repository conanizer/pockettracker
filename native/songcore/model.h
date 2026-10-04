#ifndef POCKETTRACKER_SONGCORE_MODEL_H
#define POCKETTRACKER_SONGCORE_MODEL_H

// ─── Song data model ────────────────────────────────────────────────────────────────────────────
//
// The structs behind the .ptp / .pti JSON (project_io.h). Field order, defaults and pool sizes are
// part of the file format — existing projects depend on them.
//
// Two kinds of "default", which project_io.h relies on keeping apart:
//   * FIELD default   — the member initializer. A key equal to it is omitted on save, and a missing
//                       key loads as it.
//   * FACTORY value   — what a fresh Project holds. Differs only for Instrument.sampleId, which
//                       make_default_project() sets to the slot index, so every slot saves it.
//
// No floating point anywhere, so the .ptp round-trips exactly. No third-party includes, so the
// scheduler can use the model without the JSON library.

#include <cstdint>
#include <string>
#include <vector>
#include <optional>

// For the CC-slot ids `resolve_cc_param` translates; event.h is a leaf, so no cycle.
#include "event.h"
#include "program.h"   // PROGRAM_SLOTS — the engine's program table is one row per instrument

namespace songcore {

// ─── pool sizes ─────────────────────────────────────────────────────────────────────────────────

// Canonical pool sizes, above the structs so members and accessors can both use them.
constexpr int POOL_PHRASES     = 256;
constexpr int POOL_CHAINS      = 256;
constexpr int POOL_TRACKS      = 8;
constexpr int POOL_INSTRUMENTS = 128;
static_assert(POOL_INSTRUMENTS == PROGRAM_SLOTS,
              "the engine's program table is one row per instrument — the two sizes are one number");
constexpr int POOL_TABLES      = 128;
constexpr int POOL_GROOVES     = 128;
constexpr int POOL_EQPRESETS   = 128;
constexpr int POOL_SCALES      = 16;

// Rows in one chain and steps in one phrase, fixed by the screen geometry.
constexpr int CHAIN_ROWS  = 16;
constexpr int PHRASE_ROWS = 16;

// ─── small helpers ────────────────────────────────────────────────────────────────────────────

// Lower 8 bits as 2-digit UPPERCASE hex.
inline std::string hex2(int v) {
    static const char* H = "0123456789ABCDEF";
    unsigned b = static_cast<unsigned>(v) & 0xFFu;
    std::string s(2, '0');
    s[0] = H[(b >> 4) & 0xF];
    s[1] = H[b & 0xF];
    return s;
}
inline std::string default_instrument_name(int id) { return "INST" + hex2(id); }
inline std::string default_table_name(int id)      { return "TBL"  + hex2(id); }

// ─── enums (serialised by NAME) ─────────────────────────────────────────────────────────────────

enum class ModType { NONE, AHD, ADSR, LFO, DRUM, TRIG, TRACKING, SCALAR };
enum class ModDest {
    NONE, VOLUME, PAN, PITCH, FINE_PITCH, FILTER_CUTOFF, FILTER_RES,
    SAMPLE_START, MOD_AMT, MOD_RATE, MOD_BOTH
};
// The instrument's ROUTING DESTINATION: SAMPLER and SOUNDFONT go to EngineConsumer, EXTERNAL leaves as
// MIDI (ExternalConsumer, midi_out.h).
// ⚠️ EXTERNAL STAYS LAST: a build without MIDI (platform_caps.h `midi`) hides it by cycling the TYPE
// cell over one type fewer. A new module type goes before it.
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

// ─── display names ──────────────────────────────────────────────────────────────────────────────
//
// ⚠️ Not the serialised names above. "TRACKING" and "FILTER_CUTOFF" are bytes in every .ptp; these are
// what the MODS screen paints in a narrow cell. Swapping one for the other widens the UI or breaks
// the file format.

inline const char* mod_type_display_name(ModType t) {
    switch (t) {
        case ModType::NONE:     return "---";
        case ModType::AHD:      return "AHD";
        case ModType::ADSR:     return "ADSR";
        case ModType::LFO:      return "LFO";
        case ModType::DRUM:     return "DRUM";   // AHD semantics (engine type 4)
        case ModType::TRIG:     return "TRIG";   // ADSR semantics (engine type 5)
        case ModType::TRACKING: return "TRK";    // no engine implementation yet
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

// A Note always serialises BOTH pitch and octave (no field defaults). The default ctor is EMPTY.
struct Note {
    int pitch  = -1;  // 0-11 chromatic, -1 = empty
    int octave = 0;
    bool operator==(const Note& o) const { return pitch == o.pitch && octave == o.octave; }
    bool operator!=(const Note& o) const { return !(*this == o); }
    static Note EMPTY() { return Note{-1, 0}; }
    static Note C4()    { return Note{ 0, 4}; }
};

// ─── Note ↔ MIDI ↔ display ──────────────────────────────────────────────────────────────────────
inline int note_to_midi(const Note& n) {
    if (n.pitch == -1) return -1;
    return (n.octave + 1) * 12 + n.pitch;  // C-4 = 60 (standard MIDI)
}
inline Note note_from_midi(int midi) {
    if (midi < 0 || midi > 127) return Note::EMPTY();
    return Note{midi % 12, midi / 12 - 1};
}

// An authored 0x00-0xFF byte as a 0-1 gain. The model's own arithmetic, shared by the sequencer, UI
// and MIDI — one copy.
inline float hex_to_float(int hex) { return (hex & 0xFF) / 255.0f; }

// Two chars each, so every note renders in a fixed 3-char cell.
inline const char* const NOTE_NAMES[12] = {"C-", "C#", "D-", "D#", "E-", "F-",
                                           "F#", "G-", "G#", "A-", "A#", "B-"};

/** "C-4", or "---" when empty — the 3-char cell every grid draws. */
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

// The flat fx{1,2,3}{Type,Value} fields as slots 1..3. Beside the struct because both the scheduler
// (last-wins, slot discarded) and `automation.h` (AUS reads the slot to its LEFT) index them.
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
    std::vector<PhraseStep> steps = std::vector<PhraseStep>(16);
    Phrase() = default;
    explicit Phrase(int id_) : id(id_) {}
};

struct Chain {
    int id = 0;
    std::vector<int> phraseRefs      = std::vector<int>(16, -1);
    std::vector<int> transposeValues = std::vector<int>(16, 0);
    Chain() = default;
    explicit Chain(int id_) : id(id_) {}
};

// The phrase a chain row names, or -1 for "there is nothing to play here".
//
// ⚠️ Every bound on a chain row lives here: a chain has CHAIN_ROWS rows, its arrays may be shorter
// (whatever the JSON held), and a ref may lie outside the pool — a hand-edited or half-written .ptp
// can hold 9999, and the value goes straight into `project.phrases[ref]`.
inline int chain_phrase_ref(const Chain& c, int row) {
    if (row < 0 || row >= CHAIN_ROWS || row >= static_cast<int>(c.phraseRefs.size())) return -1;
    const int ref = c.phraseRefs[static_cast<size_t>(row)];
    return (ref >= 0 && ref < POOL_PHRASES) ? ref : -1;
}

// A chain row with no phrase in it (`-1` is the empty value, never 0 or 0xFF).
// ⚠️ Shared by the scheduler and `nominal_spp_beats` (midi_clock.h): a song row's length must mean the
// same to both. A ref outside the pool is empty too — it names no phrase.
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
    std::vector<TableRow> rows = std::vector<TableRow>(16);
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
 * How many rows this slot occupies on the MODS screen, TYPE row included. A fact about the TYPE, so
 * the MODS cursor and its renderer both read it here.
 */
inline int mod_slot_row_count(const ModSlot& s) {
    switch (s.type) {
        case ModType::NONE:     return 1;  // TYPE
        case ModType::AHD:      return 6;  // TYPE, DEST, AMT, ATK, HOLD, DEC
        case ModType::ADSR:     return 7;  // TYPE, DEST, AMT, ATK, DEC, SUS, REL
        case ModType::LFO:      return 6;  // TYPE, DEST, AMT, OSC, TRIG, FREQ
        case ModType::DRUM:     return 6;  // as AHD
        case ModType::TRIG:     return 7;  // as ADSR
        case ModType::TRACKING: return 5;
        case ModType::SCALAR:   return 3;  // TYPE, DEST, AMT
    }
    return 1;
}

/**
 * A new groove: two plain 12-tic steps, then end markers — audibly no groove at all, but the GROOVE
 * screen shows the pair its swing bends. An all-blank groove is also legal and also means "none".
 * ⚠️ 12 is `TICS_PER_STEP`, spelled out because timing.h includes this file; groove_bank.h asserts
 * they agree.
 */
inline std::vector<int> default_groove_steps() {
    std::vector<int> s(16, -1);
    s[0] = 12;
    s[1] = 12;
    return s;
}

struct Groove {
    int id = 0;
    std::string name;  // "" = unnamed; the screen then names it by its steps (groove_bank.h)
    std::vector<int> steps = default_groove_steps();
    Groove() = default;
    explicit Groove(int id_) : id(id_) {}
};

/**
 * One of the project's 16 scales: which of the twelve intervals FROM THE KEY are in it. The key is
 * not a field — the same shape transposed is the same slot.
 * ⚠️ All twelve enabled (the default) means chromatic, i.e. no quantizing — every consumer must keep
 * that, so the feature costs an existing song nothing.
 * ⏸️ `offset` (centi-semitones, −2400..+2400 per degree) is saved but not yet read — the microtuning
 * half, serialised from the start so enabling it changes no saved song.
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
    std::vector<EqBand> bands = std::vector<EqBand>(3);
    EqPreset() = default;
    explicit EqPreset(int id_) : id(id_) {}
};

struct Track {
    int id = 0;
    std::vector<int> chainRefs;   // empty default
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
 * One of an EXTERNAL instrument's four CC slots: `cc` is the controller it owns, `value` the default
 * sent with each note-on; −1 in either means unused.
 */
struct MidiCcSlot {
    int cc    = -1;   // -1 = slot unused | 0-127
    int value = -1;   // -1 = send nothing on note-on | 0-127
    bool operator==(const MidiCcSlot& o) const { return cc == o.cc && value == o.value; }
    bool operator!=(const MidiCcSlot& o) const { return !(*this == o); }
};

constexpr int MIDI_CC_SLOTS = 4;

/**
 * One knob pointed at one parameter of this song — the data half of `midi_map.h`.
 * ⚠️ `dest` is a `MapDestId` saved in the song (append-only). `scopeIndex` (which track/instrument) is
 * fixed at learn time. The range is in the destination's units and may be inverted.
 */
struct MidiMapping {
    uint8_t controller = 0;   // the CC number the knob sends, 0-127
    uint8_t dest       = 0;   // 0 = empty
    uint8_t scopeIndex = 0;
    int     rangeMin   = 0;
    int     rangeMax   = 255;

    bool operator==(const MidiMapping& o) const {
        return controller == o.controller && dest == o.dest && scopeIndex == o.scopeIndex &&
               rangeMin == o.rangeMin && rangeMax == o.rangeMax;
    }
    bool operator!=(const MidiMapping& o) const { return !(*this == o); }
};

struct Instrument {
    int id = 0;
    std::string name = default_instrument_name(0);
    int sampleId = -1;                       // FIELD default -1; the factory sets the slot index
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
    std::vector<ModSlot> modSlots = std::vector<ModSlot>(4);
    InstrumentType instrumentType = InstrumentType::SAMPLER;
    std::optional<std::string> soundfontPath;    // null
    int sfBank = 0, sfPreset = 0;
    SFOverrides sfOverrides{};
    int reverbSend = 0x00, delaySend = 0x00;
    int eqSlot = -1;
    int slicingMode = 0;
    std::vector<int64_t> sliceMarkers;

    /**
     * Does this instrument follow note TRANSPOSITION? OFF exempts it from the scale
     * quantizer, the chain TSP column AND the project transpose — a hand-pitched drum kit must not
     * move when the song does (`effective_transpose_semitones`, `Sequencer::emit_note`).
     */
    bool transposeEnabled = true;

    // ── EXTERNAL only ────────────────────────────────────────────────────────────────────────────
    // Ignored by the other types, all defaulted, so a non-EXTERNAL instrument saves the same bytes.
    // `volume` and `pan` are reused: volume scales note-on velocity and pan becomes CC 10.
    int midiChannel = 0;    // 0-15, shown 1-16
    int midiBank    = -1;   // -1 = send nothing; else the bank sent (CC0/CC32) before the program
    int midiProgram = -1;   // -1 = send nothing; else the program sent with the first note-on
    /**
     * Gate length in TICKS, 0 = gate-to-next. A LEN gate schedules its own note-off; 0 holds the note
     * until the next note-on or KIL on the track, like the sampler's cut.
     */
    int midiLen = 0;
    std::vector<MidiCcSlot> midiCC = std::vector<MidiCcSlot>(MIDI_CC_SLOTS);

    Instrument() = default;
    explicit Instrument(int id_) : id(id_), name(default_instrument_name(id_)) {}
};

/**
 * Is a note on this instrument a PITCH, or choosing a slice? Sliced, C-4 is slice 0 and C#4 slice 1.
 * ⚠️ Anything that moves notes musically (quantizer, transposes) must ask this and leave a slice
 * selector alone, or a drum kit plays a different drum. `voice_derive.h` asks the same question when
 * picking the slice, so the answer lives here once.
 * `sliceOverride` is the step's SLI value (-1 none): it turns slicing on for one note.
 */
inline bool note_selects_slice(const Instrument& ins, int sliceOverride) {
    return (ins.slicingMode != 0 || sliceOverride >= 0) && !ins.sliceMarkers.empty();
}

/** The name is still the auto-generated "INSTxx" — nobody named this slot. */
inline bool instrument_has_default_name(const Instrument& ins) {
    return ins.name == default_instrument_name(ins.id);
}

/**
 * This slot holds NOTHING and may be claimed for a new sample.
 * ⚠️ `sampleFilePath == null` alone is not "free": a SoundFont instrument has it too, and claiming
 * one leaves a SOUNDFONT-typed slot with a WAV behind it. (For "is there a sample to PLAY", the null
 * path IS the signal.)
 */
inline bool instrument_is_free(const Instrument& ins) {
    return !ins.sampleFilePath.has_value() && !ins.soundfontPath.has_value() &&
           ins.instrumentType == InstrumentType::SAMPLER;
}

/** Does this instrument's event stream leave the process? Both consumers ask this one predicate, so
 *  a note is never played twice or dropped. */
inline bool instrument_routes_external(const Instrument& ins) {
    return ins.instrumentType == InstrumentType::EXTERNAL;
}

/**
 * The controller number a bus CC event names for THIS instrument. A literal 0-127 passes through; a
 * slot id (`CC_SLOT_A`..`D`, from `CCA`-`CCD`) becomes `midiCC[slot].cc`. −1 = nothing to move.
 * Shared by both consumers.
 * ⚠️ −1 must never fall back to the raw id: 128 masks to CC 0, BANK SELECT.
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
    std::vector<EqPreset> eqPresets;              // 128, filled by the factory
    // ⚠️ `reverbFeedback` is the DCAY cell (the tail's length); the JSON key keeps the old name. The
    // room is `reverbSize`.
    int reverbFeedback = 0x60, reverbDamp = 0x80, reverbWet = 0x80, reverbInputEq = -1;
    // The reverb's character: three independent cells. The EFFECTS screen's TYPE row writes them (with
    // DCAY, SIZE and DAMP) from a preset and reads the name back by matching
    // (effects/modules/reverb-presets.h); the name is not stored.
    // ⚠️ These defaults are what a project saved before the cells existed plays with: PRE 00 (no line),
    // WIDE 80 (mid/side skipped). MOD is lower than the old fixed 40 — on a short tail that much
    // wander sounds like detuning.
    int reverbPreDelay = 0x00, reverbWidth = 0x80, reverbMod = 0x10;
    // The SIZE cell. ⚠️ 0x60 is exactly the original room (`reverb_room_scale` is 1), so older projects
    // sound as they did.
    int reverbSize = 0x60;
    // Which reverb reads the cells above (`kReverbAlgo*`). ⚠️ 0 is the original, and a number is an
    // identity — append, never insert. Switching rewrites no cell.
    int reverbAlgo = 0;
    int delayTime = 0x40;
    bool delaySync = false;
    int delayFeedback = 0x60, delayWet = 0x80, delayReverbSend = 0x00, delayInputEq = -1;
    // The delay's character, written by the TYPE row from a preset and named back by matching
    // (effects/modules/delay-presets.h). ⚠️ Defaults are what older projects play with: TONE FF (filter
    // switched out), WOBL 00.
    bool delayPong = false;
    int  delayTone = 0xFF, delayWobble = 0x00;
    // The two send RETURNS are mixer channels with their own mute/solo. ⚠️ A return SOLO never stops a
    // track — its feed must keep sounding — so it is not part of `track_audible` (see `dry_audible`).
    bool reverbMute = false, reverbSolo = false;
    bool delayMute  = false, delaySolo  = false;
    int masterEqSlot = -1;
    std::vector<Phrase>     phrases;              // 256
    std::vector<Chain>      chains;               // 256
    std::vector<Track>      tracks;               // 8
    std::vector<Instrument> instruments;          // 128, sampleId = slot index
    std::vector<Table>      tables;               // 128
    std::vector<Groove>     grooves;              // 128
    std::vector<Scale>      scales;               // 16 — slot 00 is every track's default

    /**
     * The default scale's root, 0-11 (0 = C): a slot is a shape, the key is where it starts. SCA can
     * move a track off it during playback; that is never stored.
     */
    int scaleKey = 0;

    // ── MIDI: the musical intent, which travels with the song ────────────────────────────────────
    // The device picks are settings.json (settings_store.h): a song moved to another machine keeps
    // its routing and re-picks its cables.
    int  midiSyncOut = 0;               // 0 OFF | 1 CLOCK | 2 TRANSPORT | 3 CLOCK+TRANSPORT
    bool midiSendProgramChange = true;

    /**
     * Which knob moves which parameter (`midi_map.h`). In the song because a mapping names song content
     * ("instrument 3's cutoff"); the knobs' channel describes the desk, so it is in settings.json.
     * A growing list — empty is empty.
     */
    std::vector<MidiMapping> midiMappings;
};

// ── Which tracks are making sound ────────────────────────────────────────────────────────────────
//
// ⚠️ DERIVED, never stored: solo is a property of the SET — track 3's answer changes when track 5 is
// soloed. Every consumer asks here. Mute wins over solo.
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

// ── …and which send RETURN is ────────────────────────────────────────────────────────────────────
//
// A separate solo set. Soloing a track leaves the returns alone; soloing a return stops no track (a
// return with no feed is silence). The two sets meet at the DRY sum, which a soloed return mutes.
inline bool any_send_solo(const Project& p) { return p.reverbSolo || p.delaySolo; }

inline bool reverb_return_audible(const Project& p) {
    return !p.reverbMute && (p.reverbSolo || !any_send_solo(p));
}

inline bool delay_return_audible(const Project& p) {
    return !p.delayMute && (p.delaySolo || !any_send_solo(p));
}

/** Is the dry mix heard? A soloed return silences it — unless a TRACK is soloed too, which asks for
 *  that track's dry signal alongside the return. */
inline bool dry_audible(const Project& p) { return !any_send_solo(p) || any_solo(p); }

// ── The mixer's ten channels ─────────────────────────────────────────────────────────────────────
//
// 0-7 the tracks, 8 and 9 the reverb and delay returns. Resolved in one place so a chord's
// snapshot, toggle and undo agree on where a channel's flags live.
constexpr int MIX_CH_REVERB = 8;
constexpr int MIX_CH_DELAY  = 9;

struct MixChannelFlags {
    bool* mute = nullptr;
    bool* solo = nullptr;
};

inline MixChannelFlags mix_channel_flags(Project& p, int ch) {
    if (ch == MIX_CH_REVERB) return {&p.reverbMute, &p.reverbSolo};
    if (ch == MIX_CH_DELAY)  return {&p.delayMute, &p.delaySolo};
    if (ch >= 0 && ch < static_cast<int>(p.tracks.size())) {
        Track& t = p.tracks[static_cast<size_t>(ch)];
        return {&t.mute, &t.solo};
    }
    return {};
}

struct InstrumentPreset {
    int version = 1;
    Instrument instrument;
    std::optional<std::vector<TableRow>> tableRows;
};

// A fresh default Project. The one factory-vs-field difference: sampleId = slot index.
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
        ins.sampleId = i;  // factory value
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
