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

namespace songcore {

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
enum class InstrumentType { SAMPLER, SOUNDFONT };

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
    return t == InstrumentType::SOUNDFONT ? "SOUNDFONT" : "SAMPLER";
}
inline bool instrument_type_from_name(const std::string& s, InstrumentType& out) {
    if (s == "SAMPLER")   { out = InstrumentType::SAMPLER;   return true; }
    if (s == "SOUNDFONT") { out = InstrumentType::SOUNDFONT; return true; }
    return false;
}

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

struct Groove {
    int id = 0;
    std::vector<int> steps = std::vector<int>(16, -1);  // IntArray(16){-1}
    Groove() = default;
    explicit Groove(int id_) : id(id_) {}
};

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
    Instrument() = default;
    explicit Instrument(int id_) : id(id_), name(default_instrument_name(id_)) {}
};

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
};

struct InstrumentPreset {
    int version = 1;
    Instrument instrument;
    std::optional<std::vector<TableRow>> tableRows;  // null
};

// Canonical pool sizes (single source, mirrors TrackerData.kt).
constexpr int POOL_PHRASES     = 256;
constexpr int POOL_CHAINS      = 256;
constexpr int POOL_TRACKS      = 8;
constexpr int POOL_INSTRUMENTS = 128;
constexpr int POOL_TABLES      = 128;
constexpr int POOL_GROOVES     = 128;
constexpr int POOL_EQPRESETS   = 128;

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
    return p;
}

}  // namespace songcore

#endif  // POCKETTRACKER_SONGCORE_MODEL_H
