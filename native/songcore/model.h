#ifndef POCKETTRACKER_SONGCORE_MODEL_H
#define POCKETTRACKER_SONGCORE_MODEL_H

// The song data model behind the .ptp / .pti JSON (project_io.h).
// ⚠️ Field defaults, order and pool sizes are part of the file format: a key equal to its member
// initializer is omitted on save and a missing one loads as it. No floating point, so a .ptp
// round-trips exactly.

#include <array>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <string>
#include <vector>
#include <optional>

// For the CC-slot ids `resolve_cc_param` translates; event.h is a leaf, so no cycle.
#include "event.h"
#include "program.h"   // PROGRAM_SLOTS — the engine's program table is one row per instrument
#include "send_cells.h"

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
// SAMPLER and SOUNDFONT play in the engine; EXTERNAL leaves as MIDI (midi_out.h).
// ⚠️ EXTERNAL STAYS LAST: a build without MIDI hides it by cycling over one type fewer.
enum class InstrumentType { SAMPLER, SOUNDFONT, EXTERNAL };

// Each enum's names, in enum order: index 0 also answers for an out-of-range value.
inline constexpr const char* MOD_TYPE_NAMES[] = {"NONE", "AHD", "ADSR", "LFO", "DRUM", "TRIG",
                                                 "TRACKING", "SCALAR"};
inline constexpr const char* MOD_DEST_NAMES[] = {"NONE", "VOLUME", "PAN", "PITCH", "FINE_PITCH",
                                                 "FILTER_CUTOFF", "FILTER_RES", "SAMPLE_START",
                                                 "MOD_AMT", "MOD_RATE", "MOD_BOTH"};
inline constexpr const char* INSTRUMENT_TYPE_NAMES[] = {"SAMPLER", "SOUNDFONT", "EXTERNAL"};

// ⚠️ Display names, not the serialised ones above — swapping them widens the UI or breaks the file.
// DRUM plays as AHD and TRIG as ADSR; TRK has no engine implementation yet; SCL's `amount` IS the
// output.
inline constexpr const char* MOD_TYPE_DISPLAY_NAMES[] = {"---", "AHD", "ADSR", "LFO", "DRUM", "TRIG",
                                                         "TRK", "SCL"};
inline constexpr const char* MOD_DEST_DISPLAY_NAMES[] = {"---", "VOL", "PAN", "PITCH", "FINE", "CUT",
                                                         "RES", "STA", "MOD A", "MOD R", "MOD B"};

template <typename E, size_t N>
inline const char* enum_name(E e, const char* const (&names)[N]) {
    const auto i = static_cast<size_t>(e);
    return i < N ? names[i] : names[0];
}
template <typename E, size_t N>
inline bool enum_from_name(const std::string& s, const char* const (&names)[N], E& out) {
    for (size_t i = 0; i < N; ++i)
        if (s == names[i]) { out = static_cast<E>(i); return true; }
    return false;
}

inline const char* mod_type_name(ModType t) { return enum_name(t, MOD_TYPE_NAMES); }
inline const char* mod_dest_name(ModDest d) { return enum_name(d, MOD_DEST_NAMES); }
inline const char* instrument_type_name(InstrumentType t) { return enum_name(t, INSTRUMENT_TYPE_NAMES); }
inline const char* mod_type_display_name(ModType t) { return enum_name(t, MOD_TYPE_DISPLAY_NAMES); }
inline const char* mod_dest_display_name(ModDest d) { return enum_name(d, MOD_DEST_DISPLAY_NAMES); }
inline bool mod_type_from_name(const std::string& s, ModType& out) { return enum_from_name(s, MOD_TYPE_NAMES, out); }
inline bool mod_dest_from_name(const std::string& s, ModDest& out) { return enum_from_name(s, MOD_DEST_NAMES, out); }
inline bool instrument_type_from_name(const std::string& s, InstrumentType& out) {
    return enum_from_name(s, INSTRUMENT_TYPE_NAMES, out);
}

// What the TYPE and DEST cells cycle over.
inline constexpr int INSTRUMENT_TYPE_COUNT = static_cast<int>(std::size(INSTRUMENT_TYPE_NAMES));
inline constexpr int MOD_DEST_COUNT        = static_cast<int>(std::size(MOD_DEST_NAMES));
static_assert(std::size(MOD_TYPE_DISPLAY_NAMES) == std::size(MOD_TYPE_NAMES) &&
              std::size(MOD_DEST_DISPLAY_NAMES) == std::size(MOD_DEST_NAMES), "a name per entry");

// ─── leaf structs ───────────────────────────────────────────────────────────────────────────────

// A Note always serialises BOTH pitch and octave (no field defaults). The default ctor is EMPTY.
struct Note {
    int pitch  = -1;  // 0-11 chromatic, -1 = empty, -2 = OFF
    int octave = 0;
    bool operator==(const Note& o) const { return pitch == o.pitch && octave == o.octave; }
    bool operator!=(const Note& o) const { return !(*this == o); }
    static Note EMPTY() { return Note{-1, 0}; }
    static Note OFF()   { return Note{-2, 0}; }   // lets go of the note sounding on the track
    static Note C4()    { return Note{ 0, 4}; }
};

// ⚠️ A note cell has THREE states. "Not empty" is not "plays a note": ask the one you mean.
inline bool note_is_off(const Note& n)   { return n.pitch == -2; }
inline bool note_is_pitch(const Note& n) { return n.pitch >= 0; }

// ─── Note ↔ MIDI ↔ display ──────────────────────────────────────────────────────────────────────
inline int note_to_midi(const Note& n) {
    if (n.pitch < 0) return -1;
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

/** "C-4", "OFF", or "---" when empty — the 3-char cell every grid draws. */
inline std::string note_name(const Note& n) {
    if (note_is_off(n)) return "OFF";
    if (n.pitch < 0 || n.pitch > 11) return "---";
    return std::string(NOTE_NAMES[n.pitch]) + std::to_string(n.octave);
}

// One command cell: an effect code (FX_NONE = empty), its value, and R+B's switch (see `as_played`).
struct FxSlot {
    int  type  = 0x00;
    int  value = 0x00;
    bool off   = false;
};
inline bool operator==(const FxSlot& a, const FxSlot& b) {
    return a.type == b.type && a.value == b.value && a.off == b.off;
}
inline bool operator!=(const FxSlot& a, const FxSlot& b) { return !(a == b); }

// FX1..FX3. ⚠️ The file names them `fx1Type`, `fx2Value`, `fx3Off`…: the one place a model field does
// not mirror its JSON key, so `project_io.h` maps index to name.
inline constexpr int FX_SLOTS = 3;
using FxSlots = std::array<FxSlot, FX_SLOTS>;

struct PhraseStep {
    Note note = Note::EMPTY();
    int  instrument = 0x00;
    int  volume     = 0x7F;
    FxSlots fx{};
};

/** A step that triggers a note — an OFF does not, and neither does its instrument column. */
inline bool step_has_note(const PhraseStep& s) { return note_is_pitch(s.note); }

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

// The phrase a chain row names, or -1.
// ⚠️ Every bound on a chain row lives here: the arrays may be short and a hand-edited .ptp can hold
// 9999, which would go straight into `project.phrases[ref]`.
inline int chain_phrase_ref(const Chain& c, int row) {
    if (row < 0 || row >= CHAIN_ROWS || row >= static_cast<int>(c.phraseRefs.size())) return -1;
    const int ref = c.phraseRefs[static_cast<size_t>(row)];
    return (ref >= 0 && ref < POOL_PHRASES) ? ref : -1;
}

// A chain row with no phrase. ⚠️ Shared by the scheduler and `nominal_spp_beats`, so a song row's
// length means the same to both.
inline bool chain_is_empty(const Chain& c, int index) { return chain_phrase_ref(c, index) < 0; }

struct TableRow {
    int transpose = 0x00;
    int volume    = -1;
    FxSlots fx{};
};

// A step and a table row both carry `fx`; these read either. A slot NUMBER is 1..3, as on screen and
// in the playback state (0 = none); out of range it reads as an empty slot.
template <typename Row> const FxSlot& fx_slot(const Row& r, int slot) {
    static const FxSlot none{};
    return slot >= 1 && slot <= FX_SLOTS ? r.fx[static_cast<size_t>(slot - 1)] : none;
}
template <typename Row> FxSlot& fx_slot_ref(Row& r, int slot) {   // ⚠️ slot must be 1..3
    return r.fx[static_cast<size_t>(slot - 1)];
}
// The leftmost slot carrying `type`, or 0.
template <typename Row> int slot_with_fx(const Row& r, int type) {
    for (int slot = 1; slot <= FX_SLOTS; ++slot)
        if (r.fx[static_cast<size_t>(slot - 1)].type == type) return slot;
    return 0;
}
// True when any slot carries `type`.
template <typename Row> bool has_fx(const Row& r, int type) { return slot_with_fx(r, type) != 0; }

// ─── A command switched off ──────────────────────────────────────────────────────────────────────
// R+B on a phrase or table command switches it off: it stays in its cell and in the file, and plays
// as an empty slot.
// ⚠️ Playback reads a step or a table row through `as_played`. A reader that takes the raw slots
// plays a command the user has switched off.

template <typename Row> Row as_played(Row r) {
    for (FxSlot& f : r.fx) if (f.off) f = FxSlot{};
    return r;
}
// An empty slot is never off, so a command later typed or pasted into it starts on.
template <typename Row> void drop_off_from_empty(Row& r) {
    for (FxSlot& f : r.fx) if (f.type == 0x00) f.off = false;
}

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

// The rows this slot takes on the MODS screen, TYPE row included — read by cursor and renderer alike.
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

// A new groove: two plain 12-tic steps — no groove, but the pair its swing bends is on screen.
// 12 is `TICS_PER_STEP`, spelled out because timing.h includes this file (groove_bank.h asserts it).
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
    // Tics per plain step in `steps`: 6, 12 or 24. ⚠️ The groove's alone — an effect's tic stays 1/12.
    int ticksPerStep = 12;
    Groove() = default;
    explicit Groove(int id_) : id(id_) {}
};

// One of the 16 scales: which of the twelve intervals from the key are in it.
// ⚠️ All twelve (the default) means chromatic — no quantizing, which every consumer must keep.
struct Scale {
    int id = 0;
    std::string name;                                        // "" = never named
    std::vector<int> enabled = std::vector<int>(12, 1);      // 1 = the degree is in the scale
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
    bool mute   = false;   // the one switch R+B flips and R+A sets; mute and solo are not two states
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

// An EXTERNAL instrument's CC slot: the controller, and the value sent with each note-on.
struct MidiCcSlot {
    int cc    = -1;   // -1 = slot unused | 0-127
    int value = -1;   // -1 = send nothing on note-on | 0-127
    bool operator==(const MidiCcSlot& o) const { return cc == o.cc && value == o.value; }
    bool operator!=(const MidiCcSlot& o) const { return !(*this == o); }
};

constexpr int MIDI_CC_SLOTS = 4;

// An EXTERNAL instrument's BEND: the semitones its device bends at full wheel.
constexpr int MIDI_BEND_RANGE_MIN = 1, MIDI_BEND_RANGE_MAX = 48, MIDI_BEND_RANGE_DEFAULT = 12;

// One knob pointed at one parameter (midi_map.h). ⚠️ `dest` is a saved `MapDestId` (append-only).
// The range is in the destination's units and may be inverted.
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

    // OFF exempts the instrument from the scale quantizer, chain TSP and project transpose (a kit).
    bool transposeEnabled = true;

    // ── EXTERNAL only; `volume` scales velocity and `pan` becomes CC 10 ──
    int midiChannel = 0;    // 0-15, shown 1-16
    int midiBank    = -1;   // -1 = send nothing; else the bank sent (CC0/CC32) before the program
    int midiProgram = -1;   // -1 = send nothing; else the program sent with the first note-on
    // Gate length in tics; 0 holds the note until the next note or KIL.
    int midiLen = 0;
    // The pitch commands are scaled to it, and it is sent (RPN 0) before a take's first bend.
    int midiBendRange = MIDI_BEND_RANGE_DEFAULT;
    std::vector<MidiCcSlot> midiCC = std::vector<MidiCcSlot>(MIDI_CC_SLOTS);

    Instrument() = default;
    explicit Instrument(int id_) : id(id_), name(default_instrument_name(id_)) {}
};

// A note that picks a slice (C-4 = slice 0) rather than a pitch. `sliceOverride` = the step's SLI or -1.
// ⚠️ Whatever moves notes musically must leave such a note alone, or a kit plays a different drum.
inline bool note_selects_slice(const Instrument& ins, int sliceOverride) {
    return (ins.slicingMode != 0 || sliceOverride >= 0) && !ins.sliceMarkers.empty();
}

/** The name is still the auto-generated "INSTxx" — nobody named this slot. */
inline bool instrument_has_default_name(const Instrument& ins) {
    return ins.name == default_instrument_name(ins.id);
}

// The slot holds nothing and may be claimed.
// ⚠️ A null `sampleFilePath` alone is not "free": a SoundFont instrument has one too.
inline bool instrument_is_free(const Instrument& ins) {
    return !ins.sampleFilePath.has_value() && !ins.soundfontPath.has_value() &&
           ins.instrumentType == InstrumentType::SAMPLER;
}

/** Does this instrument's event stream leave the process? Both consumers ask this one predicate, so
 *  a note is never played twice or dropped. */
inline bool instrument_routes_external(const Instrument& ins) {
    return ins.instrumentType == InstrumentType::EXTERNAL;
}

// The controller a bus CC names for this instrument: 0-127 passes, a slot id becomes its `cc`; −1 = none.
// ⚠️ −1 must never fall back to the raw id: 128 masks to CC 0, BANK SELECT.
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
    // ⚠️ RUNTIME ONLY, never saved: an incoming MIDI clock's tempo while it leads (SYNC IN); 0 = none.
    // A playback path reads `tempo_in_force()`, not `tempo`; an export plays `tempo`.
    int clockTempo = 0;
    int transpose = 0;
    int masterVolume = 0xFF;
    int ottDepth = 0, masterBusFx = 0, dustDepth = 0, limiterPreGain = 0;
    std::vector<EqPreset> eqPresets;              // 128, filled by the factory
    // The sends' cells (send_cells.h); the level, input EQ and preset name sit beside them.
    ReverbCells reverb;
    int reverbWet = 0x80, reverbInputEq = -1;
    DelayCells delay;
    int delayWet = 0x80, delayInputEq = -1;
    // The names the preset rows show — labels only. Empty = matched against the built-ins.
    std::string reverbPreset, delayPreset;
    // The two send RETURNS are mixer channels with a mute each. ⚠️ `dryMute` silences the tracks' own
    // sound but NOT what they feed the returns: muting the tracks instead would starve a return heard alone.
    bool reverbMute = false, delayMute = false, dryMute = false;
    int masterEqSlot = -1;
    std::vector<Phrase>     phrases;              // 256
    std::vector<Chain>      chains;               // 256
    std::vector<Track>      tracks;               // 8
    std::vector<Instrument> instruments;          // 128; an instrument's engine slot is its id
    std::vector<Table>      tables;               // 128
    std::vector<Groove>     grooves;              // 128
    std::vector<Scale>      scales;               // 16 — slot 00 is every track's default

    // The default scale's root, 0-11 (0 = C). SCA moves a track off it during playback, unsaved.
    int scaleKey = 0;

    // ── MIDI intent travels with the song; the device picks are in settings.json ──
    int  midiSyncOut = 0;               // 0 OFF | 1 CLOCK | 2 TRANSPORT | 3 CLOCK+TRANSPORT
    bool midiSendProgramChange = true;

    // Which knob moves which parameter — song content; the knobs' channel is in settings.json.
    std::vector<MidiMapping> midiMappings;
};

// The tempo the app plays at: a leading clock's, else the song's TEMPO.
inline int tempo_in_force(const Project& p) { return p.clockTempo > 0 ? p.clockTempo : p.tempo; }

inline bool track_audible(const Project& p, int trackId) {
    if (trackId < 0 || trackId >= static_cast<int>(p.tracks.size())) return false;
    return !p.tracks[static_cast<size_t>(trackId)].mute;
}

// The mixer's ten channels: 0-7 the tracks, 8 and 9 the reverb and delay returns.
constexpr int MIX_CH_REVERB = 8;
constexpr int MIX_CH_DELAY  = 9;

// Null for a channel the project does not have.
inline bool* mix_channel_mute(Project& p, int ch) {
    if (ch == MIX_CH_REVERB) return &p.reverbMute;
    if (ch == MIX_CH_DELAY)  return &p.delayMute;
    if (ch >= 0 && ch < static_cast<int>(p.tracks.size())) return &p.tracks[static_cast<size_t>(ch)].mute;
    return nullptr;
}

struct InstrumentPreset {
    int version = 1;
    Instrument instrument;
    std::optional<std::vector<TableRow>> tableRows;
};

// A table worth putting in a .pti: any row that transposes, sets a volume or runs an FX in any slot.
inline bool table_rows_have_content(const std::vector<TableRow>& rows) {
    for (const TableRow& r : rows) {
        if (r.transpose != 0 || r.volume != -1) return true;
        for (const FxSlot& f : r.fx) if (f.type != 0x00) return true;
    }
    return false;
}

// A fresh default Project.
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
    for (int i = 0; i < POOL_INSTRUMENTS; ++i) p.instruments.emplace_back(i);
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
