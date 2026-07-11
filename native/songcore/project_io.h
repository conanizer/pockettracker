#ifndef POCKETTRACKER_SONGCORE_PROJECT_IO_H
#define POCKETTRACKER_SONGCORE_PROJECT_IO_H

// ─── .ptp / .pti reader + writer + migrate/normalize ────────────────────────────────────────────
//
// The C++ twin of FileController.kt's project I/O. Reads with nlohmann/json (tolerant of unknown
// keys, like kotlinx ignoreUnknownKeys=true); writes with a hand-rolled emitter that reproduces
// kotlinx.serialization's pretty-print output BYTE-FOR-BYTE so a project saved by either side is
// identical (Linux-port plan §4.4). Proven by tools/ptroundtrip against /testdata/*.ptp.
//
// The kotlinx output contract we replicate (verified against the golden .ptp files, 2026-07):
//   * Json { prettyPrint = true; ignoreUnknownKeys = true }  → encodeDefaults defaults to FALSE.
//   * 4-space indent; "key": value (colon + one space); members/elements separated by ",\n";
//     empty object = "{}", empty array = "[]"; NO trailing newline at end of file; LF throughout.
//   * Keys emitted in @Serializable DECLARATION order.
//   * encodeDefaults=false omission, with kotlinx's value-vs-default comparison semantics:
//       - scalar / String / enum / Note / SFOverrides / List : omit when == the FIELD default
//         (List default is the empty list → omit when empty).
//       - Array / IntArray fields : ALWAYS emitted (kotlinx compares arrays by reference, so a
//         freshly-built default array is never "equal" to the instance → never omitted).
//       - nullable `= null` fields : omit when null.
//       - fields with NO default (ids, Note.pitch/octave, InstrumentPreset.instrument) : always.
//   * enums serialise by entry NAME; every number is an integer (no floats in this schema).
//
// Header-only and NOT part of the Android engine build (ENGINE_CORE_SOURCES) yet — songcore's JNI
// seam lands in a later Phase-1 session. For now this compiles into the host round-trip tool only.

#include "model.h"
#include "../vendor/nlohmann/json.hpp"

#include <string>
#include <vector>
#include <optional>
#include <cstdio>

namespace songcore {

using nlohmann::json;

// ─── parse helpers (missing / wrong-typed key → supplied default, mirroring kotlinx) ────────────

namespace detail {

inline int get_int(const json& j, const char* k, int def) {
    auto it = j.find(k);
    return (it != j.end() && it->is_number()) ? it->get<int>() : def;
}
inline int64_t get_i64(const json& j, const char* k, int64_t def) {
    auto it = j.find(k);
    return (it != j.end() && it->is_number()) ? it->get<int64_t>() : def;
}
inline bool get_bool(const json& j, const char* k, bool def) {
    auto it = j.find(k);
    return (it != j.end() && it->is_boolean()) ? it->get<bool>() : def;
}
inline std::string get_str(const json& j, const char* k, const std::string& def) {
    auto it = j.find(k);
    return (it != j.end() && it->is_string()) ? it->get<std::string>() : def;
}
// nullable string: absent OR json null → nullopt; string → value (matches kotlinx String? = null).
inline std::optional<std::string> get_opt_str(const json& j, const char* k) {
    auto it = j.find(k);
    if (it != j.end() && it->is_string()) return it->get<std::string>();
    return std::nullopt;
}

inline Note parse_note(const json& j) {
    Note n;
    n.pitch  = get_int(j, "pitch",  n.pitch);
    n.octave = get_int(j, "octave", n.octave);
    return n;
}

inline PhraseStep parse_phrase_step(const json& j) {
    PhraseStep s;
    auto it = j.find("note");
    if (it != j.end() && it->is_object()) s.note = parse_note(*it);
    s.instrument = get_int(j, "instrument", s.instrument);
    s.volume     = get_int(j, "volume",     s.volume);
    s.fx1Type = get_int(j, "fx1Type", s.fx1Type);  s.fx1Value = get_int(j, "fx1Value", s.fx1Value);
    s.fx2Type = get_int(j, "fx2Type", s.fx2Type);  s.fx2Value = get_int(j, "fx2Value", s.fx2Value);
    s.fx3Type = get_int(j, "fx3Type", s.fx3Type);  s.fx3Value = get_int(j, "fx3Value", s.fx3Value);
    return s;
}

inline Phrase parse_phrase(const json& j, int index) {
    Phrase p(get_int(j, "id", index));
    auto it = j.find("steps");
    if (it != j.end() && it->is_array()) {
        p.steps.clear();
        for (const auto& e : *it) p.steps.push_back(parse_phrase_step(e));
    }
    return p;
}

inline std::vector<int> parse_int_array(const json& j, const char* k, const std::vector<int>& def) {
    auto it = j.find(k);
    if (it == j.end() || !it->is_array()) return def;
    std::vector<int> v;
    v.reserve(it->size());
    for (const auto& e : *it) v.push_back(e.is_number() ? e.get<int>() : 0);
    return v;
}

inline Chain parse_chain(const json& j, int index) {
    Chain c(get_int(j, "id", index));
    c.phraseRefs      = parse_int_array(j, "phraseRefs",      c.phraseRefs);
    c.transposeValues = parse_int_array(j, "transposeValues", c.transposeValues);
    return c;
}

inline TableRow parse_table_row(const json& j) {
    TableRow r;
    r.transpose = get_int(j, "transpose", r.transpose);
    r.volume    = get_int(j, "volume",    r.volume);
    r.fx1Type = get_int(j, "fx1Type", r.fx1Type);  r.fx1Value = get_int(j, "fx1Value", r.fx1Value);
    r.fx2Type = get_int(j, "fx2Type", r.fx2Type);  r.fx2Value = get_int(j, "fx2Value", r.fx2Value);
    r.fx3Type = get_int(j, "fx3Type", r.fx3Type);  r.fx3Value = get_int(j, "fx3Value", r.fx3Value);
    return r;
}

inline Table parse_table(const json& j, int index) {
    Table t(get_int(j, "id", index));
    t.name = get_str(j, "name", t.name);
    auto it = j.find("rows");
    if (it != j.end() && it->is_array()) {
        t.rows.clear();
        for (const auto& e : *it) t.rows.push_back(parse_table_row(e));
    }
    return t;
}

inline ModSlot parse_mod_slot(const json& j) {
    ModSlot m;
    { auto it = j.find("type"); if (it != j.end() && it->is_string()) mod_type_from_name(it->get<std::string>(), m.type); }
    { auto it = j.find("dest"); if (it != j.end() && it->is_string()) mod_dest_from_name(it->get<std::string>(), m.dest); }
    m.amount      = get_int(j, "amount",      m.amount);
    m.attack      = get_int(j, "attack",      m.attack);
    m.hold        = get_int(j, "hold",        m.hold);
    m.decay       = get_int(j, "decay",       m.decay);
    m.sustain     = get_int(j, "sustain",     m.sustain);
    m.release     = get_int(j, "release",     m.release);
    m.oscShape    = get_int(j, "oscShape",    m.oscShape);
    m.lfoTrigMode = get_int(j, "lfoTrigMode", m.lfoTrigMode);
    m.lfoFreq     = get_int(j, "lfoFreq",     m.lfoFreq);
    return m;
}

inline Groove parse_groove(const json& j, int index) {
    Groove g(get_int(j, "id", index));
    g.steps = parse_int_array(j, "steps", g.steps);
    return g;
}

inline EqBand parse_eq_band(const json& j) {
    EqBand b;
    b.type = get_int(j, "type", b.type);
    b.freq = get_int(j, "freq", b.freq);
    b.gain = get_int(j, "gain", b.gain);
    b.q    = get_int(j, "q",    b.q);
    return b;
}

inline EqPreset parse_eq_preset(const json& j, int index) {
    EqPreset e(get_int(j, "id", index));
    auto it = j.find("bands");
    if (it != j.end() && it->is_array()) {
        e.bands.clear();
        for (const auto& b : *it) e.bands.push_back(parse_eq_band(b));
    }
    return e;
}

inline Track parse_track(const json& j, int index) {
    Track t(get_int(j, "id", index));
    auto it = j.find("chainRefs");
    if (it != j.end() && it->is_array()) {
        t.chainRefs.clear();
        for (const auto& e : *it) t.chainRefs.push_back(e.is_number() ? e.get<int>() : 0);
    }
    t.volume = get_int(j, "volume", t.volume);
    t.mute   = get_bool(j, "mute",  t.mute);
    return t;
}

inline SFOverrides parse_sf_overrides(const json& j) {
    SFOverrides s;
    s.ampAttack  = get_int(j, "ampAttack",  s.ampAttack);
    s.ampDecay   = get_int(j, "ampDecay",   s.ampDecay);
    s.ampSustain = get_int(j, "ampSustain", s.ampSustain);
    s.ampRelease = get_int(j, "ampRelease", s.ampRelease);
    s.filterCut  = get_int(j, "filterCut",  s.filterCut);
    s.filterRes  = get_int(j, "filterRes",  s.filterRes);
    return s;
}

inline Instrument parse_instrument(const json& j, int index) {
    Instrument i(get_int(j, "id", index));
    i.name           = get_str(j, "name", i.name);
    i.sampleId       = get_int(j, "sampleId", i.sampleId);   // absent → -1 (field default), NOT index
    i.volume         = get_int(j, "volume", i.volume);
    i.pan            = get_int(j, "pan", i.pan);
    { auto it = j.find("root"); if (it != j.end() && it->is_object()) i.root = parse_note(*it); }
    i.detune         = get_int(j, "detune", i.detune);
    i.drive          = get_int(j, "drive", i.drive);
    i.crush          = get_int(j, "crush", i.crush);
    i.downsample     = get_int(j, "downsample", i.downsample);
    i.filterType     = get_str(j, "filterType", i.filterType);
    i.filterCut      = get_int(j, "filterCut", i.filterCut);
    i.filterRes      = get_int(j, "filterRes", i.filterRes);
    i.sampleStart    = get_int(j, "sampleStart", i.sampleStart);
    i.sampleEnd      = get_int(j, "sampleEnd", i.sampleEnd);
    i.reverse        = get_bool(j, "reverse", i.reverse);
    i.loopMode       = get_str(j, "loopMode", i.loopMode);
    i.loopStart      = get_int(j, "loopStart", i.loopStart);
    i.loopEnd        = get_int(j, "loopEnd", i.loopEnd);
    i.sampleFilePath = get_opt_str(j, "sampleFilePath");
    i.tableId        = get_int(j, "tableId", i.tableId);
    i.tableTicRate   = get_int(j, "tableTicRate", i.tableTicRate);
    { auto it = j.find("modSlots");
      if (it != j.end() && it->is_array()) {
          i.modSlots.clear();
          for (const auto& m : *it) i.modSlots.push_back(parse_mod_slot(m));
      } }
    { auto it = j.find("instrumentType"); if (it != j.end() && it->is_string()) instrument_type_from_name(it->get<std::string>(), i.instrumentType); }
    i.soundfontPath  = get_opt_str(j, "soundfontPath");
    i.sfBank         = get_int(j, "sfBank", i.sfBank);
    i.sfPreset       = get_int(j, "sfPreset", i.sfPreset);
    { auto it = j.find("sfOverrides"); if (it != j.end() && it->is_object()) i.sfOverrides = parse_sf_overrides(*it); }
    i.reverbSend     = get_int(j, "reverbSend", i.reverbSend);
    i.delaySend      = get_int(j, "delaySend", i.delaySend);
    i.eqSlot         = get_int(j, "eqSlot", i.eqSlot);
    i.slicingMode    = get_int(j, "slicingMode", i.slicingMode);
    { auto it = j.find("sliceMarkers");
      if (it != j.end() && it->is_array())
          for (const auto& e : *it) i.sliceMarkers.push_back(e.is_number() ? e.get<int64_t>() : 0); }
    return i;
}

template <class T, class F>
inline std::vector<T> parse_pool(const json& j, const char* k, F&& parse_elem) {
    std::vector<T> v;
    auto it = j.find(k);
    if (it != j.end() && it->is_array()) {
        int idx = 0;
        for (const auto& e : *it) v.push_back(parse_elem(e, idx++));
    }
    return v;  // absent/empty → normalize() pads to canonical size
}

}  // namespace detail

// Parse a decoded .ptp JSON object into a Project (scalar/field defaults for anything missing).
// Pools are taken verbatim; call normalize_project() to repair pool sizes as the loader does.
inline Project parse_project(const json& j) {
    using namespace detail;
    Project p;  // scalar members hold their field defaults; pools start EMPTY
    p.version         = get_int(j, "version", p.version);
    p.name            = get_str(j, "name", p.name);
    p.tempo           = get_int(j, "tempo", p.tempo);
    p.transpose       = get_int(j, "transpose", p.transpose);
    p.masterVolume    = get_int(j, "masterVolume", p.masterVolume);
    p.ottDepth        = get_int(j, "ottDepth", p.ottDepth);
    p.masterBusFx     = get_int(j, "masterBusFx", p.masterBusFx);
    p.dustDepth       = get_int(j, "dustDepth", p.dustDepth);
    p.limiterPreGain  = get_int(j, "limiterPreGain", p.limiterPreGain);
    p.eqPresets       = parse_pool<EqPreset>(j, "eqPresets", parse_eq_preset);
    p.reverbFeedback  = get_int(j, "reverbFeedback", p.reverbFeedback);
    p.reverbDamp      = get_int(j, "reverbDamp", p.reverbDamp);
    p.reverbWet       = get_int(j, "reverbWet", p.reverbWet);
    p.reverbInputEq   = get_int(j, "reverbInputEq", p.reverbInputEq);
    p.delayTime       = get_int(j, "delayTime", p.delayTime);
    p.delaySync       = get_bool(j, "delaySync", p.delaySync);
    p.delayFeedback   = get_int(j, "delayFeedback", p.delayFeedback);
    p.delayWet        = get_int(j, "delayWet", p.delayWet);
    p.delayReverbSend = get_int(j, "delayReverbSend", p.delayReverbSend);
    p.delayInputEq    = get_int(j, "delayInputEq", p.delayInputEq);
    p.masterEqSlot    = get_int(j, "masterEqSlot", p.masterEqSlot);
    p.phrases     = parse_pool<Phrase>(j, "phrases", parse_phrase);
    p.chains      = parse_pool<Chain>(j, "chains", parse_chain);
    p.tracks      = parse_pool<Track>(j, "tracks", parse_track);
    p.instruments = parse_pool<Instrument>(j, "instruments", parse_instrument);
    p.tables      = parse_pool<Table>(j, "tables", parse_table);
    p.grooves     = parse_pool<Groove>(j, "grooves", parse_groove);
    return p;
}

inline InstrumentPreset parse_instrument_preset(const json& j) {
    InstrumentPreset ip;
    ip.version = detail::get_int(j, "version", ip.version);
    auto it = j.find("instrument");
    if (it != j.end() && it->is_object()) ip.instrument = detail::parse_instrument(*it, ip.instrument.id);
    auto tr = j.find("tableRows");
    if (tr != j.end() && tr->is_array()) {
        std::vector<TableRow> rows;
        for (const auto& e : *tr) rows.push_back(detail::parse_table_row(e));
        ip.tableRows = std::move(rows);
    }
    return ip;
}

// ─── migrate + normalize (mirror FileController.decodeAndMigrate) ────────────────────────────────

// Repair, don't reject: truncate over-long pools, pad short ones from a default Project. Returns
// true if anything changed. Mirrors FileController.normalizeProject.
inline bool normalize_project(Project& p) {
    if ((int)p.phrases.size() == POOL_PHRASES && (int)p.chains.size() == POOL_CHAINS &&
        (int)p.tracks.size() == POOL_TRACKS && (int)p.instruments.size() == POOL_INSTRUMENTS &&
        (int)p.tables.size() == POOL_TABLES && (int)p.grooves.size() == POOL_GROOVES &&
        (int)p.eqPresets.size() == POOL_EQPRESETS) {
        return false;
    }
    Project d = make_default_project();
    auto fix = [](auto& pool, auto& def, int n) {
        std::decay_t<decltype(pool)> out;
        out.reserve(n);
        for (int i = 0; i < n; ++i) out.push_back(i < (int)pool.size() ? pool[i] : def[i]);
        pool = std::move(out);
    };
    fix(p.phrases,     d.phrases,     POOL_PHRASES);
    fix(p.chains,      d.chains,      POOL_CHAINS);
    fix(p.tracks,      d.tracks,      POOL_TRACKS);
    fix(p.instruments, d.instruments, POOL_INSTRUMENTS);
    fix(p.tables,      d.tables,      POOL_TABLES);
    fix(p.grooves,     d.grooves,     POOL_GROOVES);
    fix(p.eqPresets,   d.eqPresets,   POOL_EQPRESETS);
    return true;
}

// Version 0 → 1: table rows with volume 0xFF meant "full" under the old scheme; the new scheme uses
// -1 for "no change". Mirrors FileController.migrateProject.
inline void migrate_project(Project& p) {
    if (p.version < 1) {
        for (auto& t : p.tables)
            for (auto& r : t.rows)
                if (r.volume == 0xFF) r.volume = -1;
        p.version = 1;
    }
}

// The full loader tail: normalize pools, then migrate. (decode is the caller's json::parse.)
inline void normalize_and_migrate(Project& p) {
    normalize_project(p);
    migrate_project(p);
}

// ─── writer: kotlinx-byte-exact pretty printer ──────────────────────────────────────────────────

class JsonWriter {
public:
    std::string out;

    void begin_object() { out += '{'; stack_.push_back(true); ++depth_; }
    void end_object()   { bool had = !stack_.back(); stack_.pop_back(); --depth_; if (had) newline_indent(); out += '}'; }
    void begin_array()  { out += '['; stack_.push_back(true); ++depth_; }
    void end_array()    { bool had = !stack_.back(); stack_.pop_back(); --depth_; if (had) newline_indent(); out += ']'; }

    // In an object: emit the separator + `"key": ` prefix. Follow with a value_* call or a nested
    // begin_object/begin_array.
    void key(const char* k) {
        if (!stack_.back()) out += ',';
        stack_.back() = false;
        newline_indent();
        out += '"'; escape_into(k); out += "\": ";
    }
    // In an array: emit the separator + indent before an element value.
    void element() {
        if (!stack_.back()) out += ',';
        stack_.back() = false;
        newline_indent();
    }

    void value_int(long long v)          { out += std::to_string(v); }
    void value_bool(bool b)              { out += b ? "true" : "false"; }
    void value_string(const std::string& s) { out += '"'; escape_into(s.c_str(), s.size()); out += '"'; }

    void field_int(const char* k, long long v)          { key(k); value_int(v); }
    void field_bool(const char* k, bool v)              { key(k); value_bool(v); }
    void field_string(const char* k, const std::string& v) { key(k); value_string(v); }

private:
    std::vector<bool> stack_;  // per-open-container: still empty?  (true = no members/elements yet)
    int depth_ = 0;

    void newline_indent() { out += '\n'; out.append((size_t)depth_ * 4, ' '); }

    void escape_into(const char* s) { escape_into(s, std::char_traits<char>::length(s)); }
    void escape_into(const char* s, size_t n) {
        for (size_t i = 0; i < n; ++i) {
            unsigned char c = (unsigned char)s[i];
            switch (c) {
                case '"':  out += "\\\""; break;
                case '\\': out += "\\\\"; break;
                case '\b': out += "\\b";  break;
                case '\t': out += "\\t";  break;
                case '\n': out += "\\n";  break;
                case '\f': out += "\\f";  break;
                case '\r': out += "\\r";  break;
                default:
                    if (c < 0x20) { char buf[8]; std::snprintf(buf, sizeof buf, "\\u%04x", c); out += buf; }
                    else out += (char)c;  // pass UTF-8 / printable ASCII through, like kotlinx
            }
        }
    }
};

namespace detail {

inline void emit_note(JsonWriter& w, const Note& n) {
    w.begin_object();
    w.field_int("pitch",  n.pitch);
    w.field_int("octave", n.octave);
    w.end_object();
}

inline void emit_phrase_step(JsonWriter& w, const PhraseStep& s) {
    w.begin_object();
    if (s.note != Note::EMPTY())  { w.key("note"); emit_note(w, s.note); }
    if (s.instrument != 0x00)     w.field_int("instrument", s.instrument);
    if (s.volume     != 0x7F)     w.field_int("volume",     s.volume);
    if (s.fx1Type  != 0x00)       w.field_int("fx1Type",  s.fx1Type);
    if (s.fx1Value != 0x00)       w.field_int("fx1Value", s.fx1Value);
    if (s.fx2Type  != 0x00)       w.field_int("fx2Type",  s.fx2Type);
    if (s.fx2Value != 0x00)       w.field_int("fx2Value", s.fx2Value);
    if (s.fx3Type  != 0x00)       w.field_int("fx3Type",  s.fx3Type);
    if (s.fx3Value != 0x00)       w.field_int("fx3Value", s.fx3Value);
    w.end_object();
}

inline void emit_int_array(JsonWriter& w, const char* key, const std::vector<int>& a) {
    w.key(key);
    w.begin_array();
    for (int v : a) { w.element(); w.value_int(v); }
    w.end_array();
}

inline void emit_phrase(JsonWriter& w, const Phrase& p) {
    w.begin_object();
    w.field_int("id", p.id);
    w.key("steps");
    w.begin_array();
    for (const auto& s : p.steps) { w.element(); emit_phrase_step(w, s); }
    w.end_array();
    w.end_object();
}

inline void emit_chain(JsonWriter& w, const Chain& c) {
    w.begin_object();
    w.field_int("id", c.id);
    emit_int_array(w, "phraseRefs",      c.phraseRefs);
    emit_int_array(w, "transposeValues", c.transposeValues);
    w.end_object();
}

inline void emit_table_row(JsonWriter& w, const TableRow& r) {
    w.begin_object();
    if (r.transpose != 0x00) w.field_int("transpose", r.transpose);
    if (r.volume    != -1)   w.field_int("volume",    r.volume);
    if (r.fx1Type  != 0x00)  w.field_int("fx1Type",  r.fx1Type);
    if (r.fx1Value != 0x00)  w.field_int("fx1Value", r.fx1Value);
    if (r.fx2Type  != 0x00)  w.field_int("fx2Type",  r.fx2Type);
    if (r.fx2Value != 0x00)  w.field_int("fx2Value", r.fx2Value);
    if (r.fx3Type  != 0x00)  w.field_int("fx3Type",  r.fx3Type);
    if (r.fx3Value != 0x00)  w.field_int("fx3Value", r.fx3Value);
    w.end_object();
}

inline void emit_table(JsonWriter& w, const Table& t) {
    w.begin_object();
    w.field_int("id", t.id);
    if (t.name != default_table_name(t.id)) w.field_string("name", t.name);
    w.key("rows");
    w.begin_array();
    for (const auto& r : t.rows) { w.element(); emit_table_row(w, r); }
    w.end_array();
    w.end_object();
}

inline void emit_mod_slot(JsonWriter& w, const ModSlot& m) {
    w.begin_object();
    if (m.type != ModType::NONE) w.field_string("type", mod_type_name(m.type));
    if (m.dest != ModDest::NONE) w.field_string("dest", mod_dest_name(m.dest));
    if (m.amount      != 0xFF) w.field_int("amount",      m.amount);
    if (m.attack      != 0x00) w.field_int("attack",      m.attack);
    if (m.hold        != 0x00) w.field_int("hold",        m.hold);
    if (m.decay       != 0x00) w.field_int("decay",       m.decay);
    if (m.sustain     != 0x80) w.field_int("sustain",     m.sustain);
    if (m.release     != 0x00) w.field_int("release",     m.release);
    if (m.oscShape    != 0x00) w.field_int("oscShape",    m.oscShape);
    if (m.lfoTrigMode != 0x00) w.field_int("lfoTrigMode", m.lfoTrigMode);
    if (m.lfoFreq     != 0x40) w.field_int("lfoFreq",     m.lfoFreq);
    w.end_object();
}

inline void emit_groove(JsonWriter& w, const Groove& g) {
    w.begin_object();
    w.field_int("id", g.id);
    emit_int_array(w, "steps", g.steps);
    w.end_object();
}

inline void emit_eq_band(JsonWriter& w, const EqBand& b) {
    w.begin_object();
    if (b.type != 0)    w.field_int("type", b.type);
    if (b.freq != 0x80) w.field_int("freq", b.freq);
    if (b.gain != 120)  w.field_int("gain", b.gain);
    if (b.q    != 0x80) w.field_int("q",    b.q);
    w.end_object();
}

inline void emit_eq_preset(JsonWriter& w, const EqPreset& e) {
    w.begin_object();
    w.field_int("id", e.id);
    w.key("bands");
    w.begin_array();
    for (const auto& b : e.bands) { w.element(); emit_eq_band(w, b); }
    w.end_array();
    w.end_object();
}

inline void emit_track(JsonWriter& w, const Track& t) {
    w.begin_object();
    w.field_int("id", t.id);
    if (!t.chainRefs.empty()) {
        w.key("chainRefs");
        w.begin_array();
        for (int v : t.chainRefs) { w.element(); w.value_int(v); }
        w.end_array();
    }
    if (t.volume != 0xFF) w.field_int("volume", t.volume);
    if (t.mute)           w.field_bool("mute", t.mute);
    w.end_object();
}

inline void emit_sf_overrides(JsonWriter& w, const SFOverrides& s) {
    w.begin_object();
    if (s.ampAttack  != -1) w.field_int("ampAttack",  s.ampAttack);
    if (s.ampDecay   != -1) w.field_int("ampDecay",   s.ampDecay);
    if (s.ampSustain != -1) w.field_int("ampSustain", s.ampSustain);
    if (s.ampRelease != -1) w.field_int("ampRelease", s.ampRelease);
    if (s.filterCut  != -1) w.field_int("filterCut",  s.filterCut);
    if (s.filterRes  != -1) w.field_int("filterRes",  s.filterRes);
    w.end_object();
}

inline void emit_instrument(JsonWriter& w, const Instrument& i) {
    w.begin_object();
    w.field_int("id", i.id);
    if (i.name != default_instrument_name(i.id)) w.field_string("name", i.name);
    if (i.sampleId != -1)   w.field_int("sampleId", i.sampleId);
    if (i.volume != 0xFF)   w.field_int("volume", i.volume);
    if (i.pan != 0x80)      w.field_int("pan", i.pan);
    if (i.root != Note::C4()) { w.key("root"); emit_note(w, i.root); }
    if (i.detune != 0x80)   w.field_int("detune", i.detune);
    if (i.drive != 0x00)    w.field_int("drive", i.drive);
    if (i.crush != 0x0)     w.field_int("crush", i.crush);
    if (i.downsample != 0x0) w.field_int("downsample", i.downsample);
    if (i.filterType != "off") w.field_string("filterType", i.filterType);
    if (i.filterCut != 0x00) w.field_int("filterCut", i.filterCut);
    if (i.filterRes != 0x00) w.field_int("filterRes", i.filterRes);
    if (i.sampleStart != 0x00) w.field_int("sampleStart", i.sampleStart);
    if (i.sampleEnd != 0xFF) w.field_int("sampleEnd", i.sampleEnd);
    if (i.reverse)          w.field_bool("reverse", i.reverse);
    if (i.loopMode != "off") w.field_string("loopMode", i.loopMode);
    if (i.loopStart != 0x00) w.field_int("loopStart", i.loopStart);
    if (i.loopEnd != 0xFF)  w.field_int("loopEnd", i.loopEnd);
    if (i.sampleFilePath)   w.field_string("sampleFilePath", *i.sampleFilePath);
    if (i.tableId != -1)    w.field_int("tableId", i.tableId);
    if (i.tableTicRate != 0x06) w.field_int("tableTicRate", i.tableTicRate);
    w.key("modSlots");
    w.begin_array();
    for (const auto& m : i.modSlots) { w.element(); emit_mod_slot(w, m); }
    w.end_array();
    if (i.instrumentType != InstrumentType::SAMPLER) w.field_string("instrumentType", instrument_type_name(i.instrumentType));
    if (i.soundfontPath)    w.field_string("soundfontPath", *i.soundfontPath);
    if (i.sfBank != 0)      w.field_int("sfBank", i.sfBank);
    if (i.sfPreset != 0)    w.field_int("sfPreset", i.sfPreset);
    if (i.sfOverrides != SFOverrides{}) { w.key("sfOverrides"); emit_sf_overrides(w, i.sfOverrides); }
    if (i.reverbSend != 0x00) w.field_int("reverbSend", i.reverbSend);
    if (i.delaySend != 0x00) w.field_int("delaySend", i.delaySend);
    if (i.eqSlot != -1)     w.field_int("eqSlot", i.eqSlot);
    if (i.slicingMode != 0) w.field_int("slicingMode", i.slicingMode);
    if (!i.sliceMarkers.empty()) {
        w.key("sliceMarkers");
        w.begin_array();
        for (int64_t v : i.sliceMarkers) { w.element(); w.value_int(v); }
        w.end_array();
    }
    w.end_object();
}

template <class T, class F>
inline void emit_pool(JsonWriter& w, const char* key, const std::vector<T>& pool, F&& emit_elem) {
    w.key(key);
    w.begin_array();
    for (const auto& e : pool) { w.element(); emit_elem(w, e); }
    w.end_array();
}

}  // namespace detail

// Serialize a Project to the exact bytes kotlinx.serialization would write (no trailing newline).
inline std::string serialize_project(const Project& p) {
    using namespace detail;
    JsonWriter w;
    w.begin_object();
    if (p.version != 0)         w.field_int("version", p.version);
    if (p.name != "UNTITLED")   w.field_string("name", p.name);
    if (p.tempo != 128)         w.field_int("tempo", p.tempo);
    if (p.transpose != 0)       w.field_int("transpose", p.transpose);
    if (p.masterVolume != 0xFF) w.field_int("masterVolume", p.masterVolume);
    if (p.ottDepth != 0)        w.field_int("ottDepth", p.ottDepth);
    if (p.masterBusFx != 0)     w.field_int("masterBusFx", p.masterBusFx);
    if (p.dustDepth != 0)       w.field_int("dustDepth", p.dustDepth);
    if (p.limiterPreGain != 0)  w.field_int("limiterPreGain", p.limiterPreGain);
    emit_pool(w, "eqPresets", p.eqPresets, emit_eq_preset);
    if (p.reverbFeedback != 0x60) w.field_int("reverbFeedback", p.reverbFeedback);
    if (p.reverbDamp != 0x80)     w.field_int("reverbDamp", p.reverbDamp);
    if (p.reverbWet != 0x80)      w.field_int("reverbWet", p.reverbWet);
    if (p.reverbInputEq != -1)    w.field_int("reverbInputEq", p.reverbInputEq);
    if (p.delayTime != 0x40)      w.field_int("delayTime", p.delayTime);
    if (p.delaySync)              w.field_bool("delaySync", p.delaySync);
    if (p.delayFeedback != 0x60)  w.field_int("delayFeedback", p.delayFeedback);
    if (p.delayWet != 0x80)       w.field_int("delayWet", p.delayWet);
    if (p.delayReverbSend != 0)   w.field_int("delayReverbSend", p.delayReverbSend);
    if (p.delayInputEq != -1)     w.field_int("delayInputEq", p.delayInputEq);
    if (p.masterEqSlot != -1)     w.field_int("masterEqSlot", p.masterEqSlot);
    emit_pool(w, "phrases",     p.phrases,     emit_phrase);
    emit_pool(w, "chains",      p.chains,      emit_chain);
    emit_pool(w, "tracks",      p.tracks,      emit_track);
    emit_pool(w, "instruments", p.instruments, emit_instrument);
    emit_pool(w, "tables",      p.tables,      emit_table);
    emit_pool(w, "grooves",     p.grooves,     emit_groove);
    w.end_object();
    return std::move(w.out);
}

// Serialize an InstrumentPreset (.pti) to kotlinx-exact bytes.
inline std::string serialize_instrument_preset(const InstrumentPreset& ip) {
    using namespace detail;
    JsonWriter w;
    w.begin_object();
    if (ip.version != 1) w.field_int("version", ip.version);
    w.key("instrument");
    emit_instrument(w, ip.instrument);
    if (ip.tableRows) {
        w.key("tableRows");
        w.begin_array();
        for (const auto& r : *ip.tableRows) { w.element(); emit_table_row(w, r); }
        w.end_array();
    }
    w.end_object();
    return std::move(w.out);
}

}  // namespace songcore

#endif  // POCKETTRACKER_SONGCORE_PROJECT_IO_H
