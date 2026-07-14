// ptinput — Phase-3 S3 conformance harness for the INPUT LAYER (host tool, no device/NDK).
//
// Proves the C++ port of the input layer is equivalent to the Kotlin original it replaces:
//   * native/ui/cursor.h          — the CursorContext system + the five button handlers
//   * native/ui/selection.h       — the L+B multi-tap CELL/ROW/SCREEN selection machine
//   * native/ui/clipboard.h       — copy / cut / paste / delete over a selection
//   * native/ui/fx_helper.h       — the effect-picker grid
//   * native/ui/modules/*         — each editor's cursor_context() and handle_input()
//
// It reads testdata/units/p3-input.txt (emitted by the JVM P3InputGoldenTest from the REAL Kotlin
// InputController + screen modules + ClipboardManager), and for every `<inputs> => <outputs>` line it
// re-parses the inputs, recomputes the RHS in C++, and byte-compares against the golden RHS.
//
// ── What an EDIT line proves, and why the third part is the one that matters ──────────────────────
//
// Each EDIT line carries the cursor CONTEXT, the RESOLVED ACTION, and the CELL AFTERWARDS. The first
// two alone are a weak test: a module that resolves A on a velocity to SET_VALUE(0x40) and then
// writes 0x40 into the *instrument* field satisfies both of them. Only the cell afterwards catches
// it — so the golden records the cell before and after, and this tool rebuilds it, applies, and
// compares the bytes.
//
// ── The clock ────────────────────────────────────────────────────────────────────────────────────
//
// SEL scripts spell a tap as F (inside the 500 ms multi-tap window) or S (outside it) rather than as
// a timestamp, because the JVM side cannot be handed a fake clock — InputController.handleSelectB
// reads System.currentTimeMillis() itself. The C++ port takes `now_ms` as a parameter precisely so
// that it CAN, and this tool advances a fake clock by 10 ms for F and 600 ms for S. What is under
// test is "tap fast" vs "tap slow", and that is what the encoding says.
//
// Build + run via the tools/ CMake project — this is the `p3-input` ctest, run by CI on every push:
//   cmake -S tools -B tools/build -DCMAKE_BUILD_TYPE=Release
//   cmake --build tools/build --config Release
//   ctest --test-dir tools/build -R p3-input --output-on-failure -C Release
//
// Exit code 0 = all green, 1 = any mismatch.  Linux-port plan §4.7 (Phase 3 S3, the dispatcher).

#include "../../native/ui/clipboard.h"
#include "../../native/ui/cursor.h"
#include "../../native/ui/fx_helper.h"
#include "../../native/ui/modules/file_browser.h"
#include "../../native/ui/modules/qwerty_keyboard.h"
#include "../../native/ui/selection.h"
#include "../../native/ui/std_filesystem.h"
#include "../../native/ui/modules/chain_editor.h"
#include "../../native/ui/modules/effects_editor.h"
#include "../../native/ui/modules/groove_editor.h"
#include "../../native/ui/modules/instrument_editor.h"
#include "../../native/ui/modules/instrument_pool.h"
#include "../../native/ui/modules/mixer.h"
#include "../../native/ui/modules/modulation.h"
#include "../../native/ui/modules/phrase_editor.h"
#include "../../native/ui/modules/project_editor.h"
#include "../../native/ui/modules/sample_editor.h"
#include "../../native/ui/modules/settings_editor.h"
#include "../../native/ui/modules/song_editor.h"
#include "../../native/ui/modules/table_editor.h"
#include "../../native/ui/platform_caps.h"
#include "../../native/ui/theme.h"

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

using namespace pt::ui;
using songcore::Chain;
using songcore::Groove;
using songcore::Instrument;
using songcore::InstrumentType;
using songcore::ModDest;
using songcore::ModSlot;
using songcore::ModType;
using songcore::Note;
using songcore::Phrase;
using songcore::PhraseStep;
using songcore::Project;
using songcore::Table;
using songcore::TableRow;

// ─── small helpers (formatting MUST match P3InputGoldenTest.kt byte-for-byte) ─────────────────────

static bool read_file(const std::string& path, std::string& out) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    std::ostringstream ss;
    ss << f.rdbuf();
    out = ss.str();
    return true;
}

static std::vector<std::string> split(const std::string& s, char sep) {
    std::vector<std::string> v;
    std::string             cur;
    for (char c : s) {
        if (c == sep) {
            v.push_back(cur);
            cur.clear();
        } else {
            cur += c;
        }
    }
    v.push_back(cur);
    return v;
}

/** The whitespace-separated tokens of a line. */
static std::vector<std::string> tokens(const std::string& s) {
    std::vector<std::string> v;
    std::istringstream       in(s);
    std::string              t;
    while (in >> t) v.push_back(t);
    return v;
}

/** The value of `key=` among `toks`, or "" when absent. */
static std::string field(const std::vector<std::string>& toks, const std::string& key) {
    const std::string pre = key + "=";
    for (const std::string& t : toks)
        if (t.rfind(pre, 0) == 0) return t.substr(pre.size());
    return "";
}

static std::string hex2(int v) {
    char b[8];
    std::snprintf(b, sizeof(b), "%02X", v & 0xFF);
    return b;
}

static int from_hex(const std::string& s) { return static_cast<int>(std::strtol(s.c_str(), nullptr, 16)); }
static int from_dec(const std::string& s) { return static_cast<int>(std::strtol(s.c_str(), nullptr, 10)); }

// ─── cell encodings ──────────────────────────────────────────────────────────────────────────────

static std::string note_str(const Note& n) {
    return std::to_string(n.pitch) + "." + std::to_string(n.octave);
}
static Note parse_note(const std::string& s) {
    const std::vector<std::string> f = split(s, '.');
    return Note{from_dec(f[0]), from_dec(f[1])};
}

static std::string step_str(const PhraseStep& s) {
    return "step=" + note_str(s.note) + ":" + hex2(s.volume) + ":" + hex2(s.instrument) + ":" +
           hex2(s.fx1Type) + "/" + hex2(s.fx1Value) + ":" + hex2(s.fx2Type) + "/" +
           hex2(s.fx2Value) + ":" + hex2(s.fx3Type) + "/" + hex2(s.fx3Value);
}

static PhraseStep parse_step(const std::string& spec) {
    const std::vector<std::string> f = split(spec.substr(std::string("step=").size()), ':');
    PhraseStep                     s;
    s.note       = parse_note(f[0]);
    s.volume     = from_hex(f[1]);
    s.instrument = from_hex(f[2]);
    const std::vector<std::string> a = split(f[3], '/');
    const std::vector<std::string> b = split(f[4], '/');
    const std::vector<std::string> c = split(f[5], '/');
    s.fx1Type = from_hex(a[0]); s.fx1Value = from_hex(a[1]);
    s.fx2Type = from_hex(b[0]); s.fx2Value = from_hex(b[1]);
    s.fx3Type = from_hex(c[0]); s.fx3Value = from_hex(c[1]);
    return s;
}

static std::string chain_row_str(const Chain& c, int row) {
    return "crow=" + std::to_string(c.phraseRefs[static_cast<size_t>(row)]) + ":" +
           hex2(c.transposeValues[static_cast<size_t>(row)]);
}

static std::string table_row_str(const TableRow& r) {
    return "trow=" + hex2(r.transpose) + ":" + std::to_string(r.volume) + ":" + hex2(r.fx1Type) +
           "/" + hex2(r.fx1Value) + ":" + hex2(r.fx2Type) + "/" + hex2(r.fx2Value) + ":" +
           hex2(r.fx3Type) + "/" + hex2(r.fx3Value);
}

static TableRow parse_table_row(const std::string& spec) {
    const std::vector<std::string> f = split(spec.substr(std::string("trow=").size()), ':');
    TableRow                       r;
    r.transpose = from_hex(f[0]);
    r.volume    = from_dec(f[1]);
    const std::vector<std::string> a = split(f[2], '/');
    const std::vector<std::string> b = split(f[3], '/');
    const std::vector<std::string> c = split(f[4], '/');
    r.fx1Type = from_hex(a[0]); r.fx1Value = from_hex(a[1]);
    r.fx2Type = from_hex(b[0]); r.fx2Value = from_hex(b[1]);
    r.fx3Type = from_hex(c[0]); r.fx3Value = from_hex(c[1]);
    return r;
}

static std::string song_cell_str(int len, int ref) {
    return "scell=" + std::to_string(len) + ":" + std::to_string(ref);
}

// ─── the instrument screens (S4) ─────────────────────────────────────────────────────────────────
//
// The "cell" of an INSTRUMENT edit is the WHOLE INSTRUMENT: 22 writable fields, and which one an
// action lands in is precisely what is under test. A golden that carried only the field the row claims
// to own could not catch a handler writing the right value into the wrong place — which, on a screen
// whose row map SHIFTS with the instrument type, is the mistake most worth catching.

static std::string inst_str(const Instrument& i) {
    return std::string("inst=") + (i.instrumentType == InstrumentType::SOUNDFONT ? "SF" : "SM") + ":" +
           note_str(i.root) + ":" + hex2(i.detune) + ":" + hex2(i.tableTicRate) + ":" +
           hex2(i.volume) + ":" + std::to_string(i.slicingMode) + ":" + hex2(i.pan) + ":" +
           hex2(i.drive) + ":" + i.filterType + ":" + std::to_string(i.crush) + ":" +
           hex2(i.filterCut) + ":" + std::to_string(i.downsample) + ":" + hex2(i.filterRes) + ":" +
           hex2(i.reverbSend) + ":" + hex2(i.delaySend) + ":" + std::to_string(i.eqSlot) + ":" +
           i.loopMode + ":" + hex2(i.sampleStart) + ":" + hex2(i.loopStart) + ":" +
           hex2(i.sampleEnd) + ":" + hex2(i.loopEnd) + ":" + (i.reverse ? "1" : "0");
}

static Instrument parse_inst(const std::string& spec, int id) {
    const std::vector<std::string> f = split(spec.substr(std::string("inst=").size()), ':');
    Instrument i(id);
    i.instrumentType = (f[0] == "SF") ? InstrumentType::SOUNDFONT : InstrumentType::SAMPLER;
    i.root           = parse_note(f[1]);
    i.detune         = from_hex(f[2]);
    i.tableTicRate   = from_hex(f[3]);
    i.volume         = from_hex(f[4]);
    i.slicingMode    = from_dec(f[5]);
    i.pan            = from_hex(f[6]);
    i.drive          = from_hex(f[7]);
    i.filterType     = f[8];
    i.crush          = from_dec(f[9]);
    i.filterCut      = from_hex(f[10]);
    i.downsample     = from_dec(f[11]);
    i.filterRes      = from_hex(f[12]);
    i.reverbSend     = from_hex(f[13]);
    i.delaySend      = from_hex(f[14]);
    i.eqSlot         = from_dec(f[15]);
    i.loopMode       = f[16];
    i.sampleStart    = from_hex(f[17]);
    i.loopStart      = from_hex(f[18]);
    i.sampleEnd      = from_hex(f[19]);
    i.loopEnd        = from_hex(f[20]);
    i.reverse        = (f[21] == "1");
    return i;
}

static std::string slot_str(const ModSlot& s) {
    return std::string("slot=") + songcore::mod_type_name(s.type) + ":" +
           songcore::mod_dest_name(s.dest) + ":" + hex2(s.amount) + ":" + hex2(s.attack) + ":" +
           hex2(s.hold) + ":" + hex2(s.decay) + ":" + hex2(s.sustain) + ":" + hex2(s.release) + ":" +
           std::to_string(s.oscShape) + ":" + std::to_string(s.lfoTrigMode) + ":" + hex2(s.lfoFreq);
}

static ModSlot parse_slot(const std::string& spec) {
    const std::vector<std::string> f = split(spec.substr(std::string("slot=").size()), ':');
    ModSlot s;
    songcore::mod_type_from_name(f[0], s.type);
    songcore::mod_dest_from_name(f[1], s.dest);
    s.amount      = from_hex(f[2]);
    s.attack      = from_hex(f[3]);
    s.hold        = from_hex(f[4]);
    s.decay       = from_hex(f[5]);
    s.sustain     = from_hex(f[6]);
    s.release     = from_hex(f[7]);
    s.oscShape    = from_dec(f[8]);
    s.lfoTrigMode = from_dec(f[9]);
    s.lfoFreq     = from_hex(f[10]);
    return s;
}

// ─── MIXER + EFFECTS (S5) ────────────────────────────────────────────────────────────────────────
//
// Their "cell" is the PROJECT — every global field the screen can write. Same argument as the
// instrument's 22: an action that lands on the right VALUE in the wrong FIELD (track 4's volume into
// track 3, OTT's depth into DUST's) agrees on the context and on the action, and diverges only here.

static std::string mix_str(const Project& p) {
    std::string s = "mix=";
    for (int i = 0; i < 8; ++i) s += hex2(p.tracks[static_cast<size_t>(i)].volume) + ":";
    return s + hex2(p.masterVolume) + ":" + hex2(p.reverbWet) + ":" + hex2(p.delayWet) + ":" +
           std::to_string(p.masterEqSlot) + ":" + hex2(p.ottDepth) + ":" + hex2(p.dustDepth) + ":" +
           hex2(p.limiterPreGain) + ":" + std::to_string(p.masterBusFx);
}

static void parse_mix(const std::string& spec, Project& p) {
    const std::vector<std::string> f = split(spec.substr(std::string("mix=").size()), ':');
    for (int i = 0; i < 8; ++i) p.tracks[static_cast<size_t>(i)].volume = from_hex(f[static_cast<size_t>(i)]);
    p.masterVolume   = from_hex(f[8]);
    p.reverbWet      = from_hex(f[9]);
    p.delayWet       = from_hex(f[10]);
    p.masterEqSlot   = from_dec(f[11]);
    p.ottDepth       = from_hex(f[12]);
    p.dustDepth      = from_hex(f[13]);
    p.limiterPreGain = from_hex(f[14]);
    p.masterBusFx    = from_dec(f[15]);
}

static std::string fx_str(const Project& p) {
    return "fx=" + std::to_string(p.masterBusFx) + ":" + hex2(p.reverbFeedback) + ":" +
           hex2(p.reverbDamp) + ":" + std::to_string(p.reverbInputEq) + ":" + hex2(p.delayTime) + ":" +
           hex2(p.delayFeedback) + ":" + hex2(p.delayReverbSend) + ":" +
           std::to_string(p.delayInputEq) + ":" + (p.delaySync ? "1" : "0");
}

static void parse_fx(const std::string& spec, Project& p) {
    const std::vector<std::string> f = split(spec.substr(std::string("fx=").size()), ':');
    p.masterBusFx     = from_dec(f[0]);
    p.reverbFeedback  = from_hex(f[1]);
    p.reverbDamp      = from_hex(f[2]);
    p.reverbInputEq   = from_dec(f[3]);
    p.delayTime       = from_hex(f[4]);
    p.delayFeedback   = from_hex(f[5]);
    p.delayReverbSend = from_hex(f[6]);
    p.delayInputEq    = from_dec(f[7]);
    p.delaySync       = (f[8] == "1");
}

// ─── the SAMPLE EDITOR's cell (S6b) ──────────────────────────────────────────────────────────────
//
// Sixteen fields, because `handle_input` writes into sixteen of them depending on where the cursor is —
// and a bug that puts the right value in the wrong one (SENS into BY, the sync type into the amount)
// resolves to a byte-identical context and a byte-identical action. Only the cell can see it.

static std::string se_str(const SampleEditorState& s) {
    std::string markers;
    for (size_t i = 0; i < s.transientMarkers.size(); ++i) {
        if (i) markers += ",";
        markers += std::to_string(s.transientMarkers[i]);
    }
    return "se=" + std::to_string(s.zoomLevel) + ":" + std::to_string(s.sourceMode) + ":" +
           std::to_string(s.rateMode) + ":" + std::to_string(s.pitchSemitones) + ":" +
           std::to_string(s.durationIndex) + ":" + (s.snapEnabled ? "1" : "0") + ":" +
           std::to_string(s.sliceMethod) + ":" + hex2(s.sliceSensitivity) + ":" +
           hex2(s.sliceDivisions) + ":" + std::to_string(s.sliceIndex) + ":" +
           std::to_string(s.fxType) + ":" + hex2(s.fxValue) + ":" + std::to_string(s.syncType) + ":" +
           std::to_string(s.selectionStart) + ":" + std::to_string(s.selectionEnd) + ":[" + markers + "]";
}

static SampleEditorState parse_se(const std::string& spec) {
    const std::vector<std::string> f = split(spec.substr(std::string("se=").size()), ':');

    SampleEditorState s;
    s.zoomLevel        = from_dec(f[0]);
    s.sourceMode       = from_dec(f[1]);
    s.rateMode         = from_dec(f[2]);
    s.pitchSemitones   = from_dec(f[3]);
    s.durationIndex    = from_dec(f[4]);
    s.snapEnabled      = (f[5] == "1");
    s.sliceMethod      = from_dec(f[6]);
    s.sliceSensitivity = from_hex(f[7]);
    s.sliceDivisions   = from_hex(f[8]);
    s.sliceIndex       = from_dec(f[9]);
    s.fxType           = from_dec(f[10]);
    s.fxValue          = from_hex(f[11]);
    s.syncType         = from_dec(f[12]);
    s.selectionStart   = std::strtoll(f[13].c_str(), nullptr, 10);
    s.selectionEnd     = std::strtoll(f[14].c_str(), nullptr, 10);

    // "[a,b,c]" or "[]" — the marker list. It is inside the cell because two edits rewrite it as a side
    // effect (switching to TRANSIENT, and changing the sensitivity), and that clearing is the signal the
    // detector re-runs on.
    const std::string m = f[15].substr(1, f[15].size() - 2);
    if (!m.empty())
        for (const std::string& v : split(m, ',')) s.transientMarkers.push_back(from_dec(v));

    return s;
}

/**
 * `env=<totalFrames>:<sampleRate>:<hasStereo>` — the three facts the module READS but no button writes.
 *
 * They are on the line, and separate from the cell, because the module's answer depends on them:
 * `hasStereoData` is what makes SOURCE a toggle rather than a read-only "MONO", `totalFrames` is what a
 * DIVIDE slice is a fraction OF, and `sampleRate` turns a frame count into "00:03.00".
 */
static void parse_se_env(const std::string& spec, SampleEditorState& s) {
    const std::vector<std::string> f = split(spec, ':');
    s.totalFrames   = from_dec(f[0]);
    s.sampleRate    = from_dec(f[1]);
    s.hasStereoData = (f[2] == "1");
}

// ─── context / action encodings ──────────────────────────────────────────────────────────────────

static const char* value_type_name(CursorValueType t) {
    switch (t) {
        case CursorValueType::HEX_BYTE:        return "HEX_BYTE";
        case CursorValueType::HEX_NIBBLE:      return "HEX_NIBBLE";
        case CursorValueType::SEMITONE_OFFSET: return "SEMITONE_OFFSET";
        case CursorValueType::NOTE:            return "NOTE";
        case CursorValueType::VOLUME:          return "VOLUME";
        case CursorValueType::GAIN:            return "GAIN";
        case CursorValueType::FREQ:            return "FREQ";
        case CursorValueType::PHRASE_REF:      return "PHRASE_REF";
        case CursorValueType::CHAIN_REF:       return "CHAIN_REF";
        case CursorValueType::INSTRUMENT_REF:  return "INSTRUMENT_REF";
        case CursorValueType::CHARACTER:       return "CHARACTER";
        case CursorValueType::TOGGLE_BINARY:   return "TOGGLE_BINARY";
        case CursorValueType::TOGGLE_TERNARY:  return "TOGGLE_TERNARY";
        case CursorValueType::EFFECT_TYPE:     return "EFFECT_TYPE";
        case CursorValueType::EFFECT_VALUE:    return "EFFECT_VALUE";
        case CursorValueType::EMPTY:           return "EMPTY";
        case CursorValueType::READ_ONLY:       return "READ_ONLY";
        case CursorValueType::NONE:            return "NONE";
    }
    return "?";
}

static std::string caps_str(const CursorContext& c) {
    std::string s;
    s += c.capabilities.canIncrement     ? '+' : '.';
    s += c.capabilities.canDecrement     ? '-' : '.';
    s += c.capabilities.canIncrementFast ? '>' : '.';
    s += c.capabilities.canDecrementFast ? '<' : '.';
    s += c.capabilities.canDelete        ? 'D' : '.';
    s += c.capabilities.canInsert        ? 'I' : '.';
    s += c.capabilities.canCreate        ? 'C' : '.';
    s += c.capabilities.isEmpty          ? 'E' : '.';
    return s;
}

static std::string ctx_str(const CursorContext& c) {
    return std::string("ctx=") + value_type_name(c.valueType) + "|" + caps_str(c) + "|" +
           std::to_string(c.currentValue) + "|" + std::to_string(c.minValue) + "|" +
           std::to_string(c.maxValue) + "|" + std::to_string(c.smallStep) + "|" +
           std::to_string(c.largeStep) + "|" + std::to_string(c.emptyValue) + "|" +
           std::to_string(c.fxSlot) + "|" +
           (c.defaultValue == NO_DEFAULT ? std::string("-") : std::to_string(c.defaultValue));
}

static std::string act_str(const InputAction& a) {
    switch (a.type) {
        case ActionType::NONE:           return "NONE";
        case ActionType::SET_VALUE:      return "SET:" + std::to_string(a.value);
        case ActionType::DELETE:         return "DELETE";
        case ActionType::INSERT_DEFAULT: return "INSERT";
        case ActionType::CREATE_NEW:     return "CREATE";
        case ActionType::NAVIGATE_UP:    return "NAV_UP";
        case ActionType::NAVIGATE_DOWN:  return "NAV_DOWN";
        case ActionType::NAVIGATE_LEFT:  return "NAV_LEFT";
        case ActionType::NAVIGATE_RIGHT: return "NAV_RIGHT";
        case ActionType::COPY:           return "COPY";
        case ActionType::CUT:            return "CUT";
        case ActionType::PASTE:          return "PASTE";
    }
    return "?";
}

/** The five buttons, resolved through the REAL generic handlers. */
static InputAction resolve(const std::string& btn, const CursorContext& c) {
    if (btn == "A")  return on_a(c);
    if (btn == "B")  return on_b(c);
    if (btn == "AL") return on_a_left(c);
    if (btn == "AR") return on_a_right(c);
    if (btn == "AB") return on_a_b(c);
    return InputAction::none();
}

// ─── EDIT ────────────────────────────────────────────────────────────────────────────────────────

static const PhraseEditorModule     kPhrase{};
static const ChainEditorModule      kChain{};
static const SongEditorModule       kSong{};
static const TableModule            kTable{};
static const GrooveModule           kGroove{};
static const InstrumentEditorModule kInstrument{};
static const InstrumentPoolModule   kPool{};
static const ModulationModule       kMods{};
static const MixerModule            kMixer{};
static const EffectModule           kEffects{};
static const SampleEditorModule     kSampleEditor{};
static const ProjectModule          kProject{};
static const SettingsModule         kSettings{};

// ─── PROJECT (S7) ────────────────────────────────────────────────────────────────────────────────

/** `prj=<tempo>:<transposeHex>:<nameAsAsciiHex>` — the name is hex because it can contain SPACES. */
static std::string prj_str(const Project& p) {
    std::string name;
    char        buf[3];
    for (char ch : p.name) {
        std::snprintf(buf, sizeof(buf), "%02X", static_cast<unsigned>(static_cast<unsigned char>(ch)));
        name += buf;
    }
    return "prj=" + std::to_string(p.tempo) + ":" + hex2(p.transpose) + ":" + name;
}

static void parse_prj(const std::string& spec, Project& p) {
    const std::vector<std::string> f = split(spec.substr(std::string("prj=").size()), ':');
    p.tempo     = from_dec(f[0]);
    p.transpose = from_hex(f[1]);

    p.name.clear();
    const std::string& hex = f[2];
    for (size_t i = 0; i + 1 < hex.size(); i += 2)
        p.name += static_cast<char>(from_hex(hex.substr(i, 2)));
}

// ─── SETTINGS (S7) ───────────────────────────────────────────────────────────────────────────────
//
// ⚠️ Driven under `PlatformCaps::android(debug = true)`, which is the whole reason this comparison can
// exist. The C++ SETTINGS row map is caps-FILTERED — the shell has eight of its thirteen rows — and a
// module compiled with the other five #ifdef'd OUT could not be compared against a Kotlin module that
// HAS them. Because the caps are a VALUE, the same code answers for both platforms, and the Android
// answer is the one the golden holds it to, row for row, with no mapping and no exceptions.
//
// The VISUALIZER lives on the Theme, not in SettingsValues (Kotlin keeps it there too), so a Theme
// rides along beside the values and the cell reads its index back out of it.

static std::string set_str(const SettingsValues& v, const Theme& t) {
    const auto b = [](bool x) { return std::string(x ? "1" : "0"); };
    return "set=" + std::to_string(v.layoutIndex)  + "/" + std::to_string(v.layoutCount)  + ":" +
           std::to_string(v.skinIndex)    + "/" + std::to_string(v.skinCount)    + ":" +
           std::to_string(v.overlayIndex) + "/" + std::to_string(v.overlayCount) + ":" +
           hex2(v.overlayStrength) + ":" +
           b(v.buttonSoundEnabled) + ":" + hex2(v.buttonSoundVolume) + ":" +
           b(v.buttonVibroEnabled) + ":" + hex2(v.vibroPower) + ":" +
           b(v.scalingBilinear) + ":" + b(v.insertBefore) + ":" + b(v.cursorRemember) + ":" +
           b(v.notePreviewEnabled) + ":" +
           std::to_string(static_cast<int>(t.visualizerType)) + ":" +
           b(v.autosaveResumeAuto) + ":" + b(v.traceEnabled) + ":" + b(v.engineCpp);
}

static void parse_set(const std::string& spec, SettingsValues& v, Theme& t) {
    const std::vector<std::string> f = split(spec.substr(std::string("set=").size()), ':');

    const std::vector<std::string> layout  = split(f[0], '/');
    const std::vector<std::string> skin    = split(f[1], '/');
    const std::vector<std::string> overlay = split(f[2], '/');

    v.layoutIndex  = from_dec(layout[0]);   v.layoutCount  = from_dec(layout[1]);
    v.skinIndex    = from_dec(skin[0]);     v.skinCount    = from_dec(skin[1]);
    v.overlayIndex = from_dec(overlay[0]);  v.overlayCount = from_dec(overlay[1]);

    v.overlayStrength     = from_hex(f[3]);
    v.buttonSoundEnabled  = from_dec(f[4]) != 0;
    v.buttonSoundVolume   = from_hex(f[5]);
    v.buttonVibroEnabled  = from_dec(f[6]) != 0;
    v.vibroPower          = from_hex(f[7]);
    v.scalingBilinear     = from_dec(f[8]) != 0;
    v.insertBefore        = from_dec(f[9]) != 0;
    v.cursorRemember      = from_dec(f[10]) != 0;
    v.notePreviewEnabled  = from_dec(f[11]) != 0;
    t.visualizerType      = static_cast<VisualizerType>(from_dec(f[12]));
    v.autosaveResumeAuto  = from_dec(f[13]) != 0;
    v.traceEnabled        = from_dec(f[14]) != 0;
    v.engineCpp           = from_dec(f[15]) != 0;
}

static std::string recompute_edit(const std::vector<std::string>& toks, std::string& err) {
    const std::string scr = field(toks, "scr");
    const int         col = from_dec(field(toks, "col"));
    const std::string btn = field(toks, "btn");

    if (scr == "PHRASE") {
        const int row = 3;  // the golden's fixed probe row
        Phrase    phrase(0);
        phrase.steps[static_cast<size_t>(row)] = parse_step("step=" + field(toks, "step"));

        PhraseEditorState st{phrase};
        st.cursorRow    = row;
        st.cursorColumn = col;
        const CursorContext ctx = kPhrase.cursor_context(st);
        const InputAction   act = resolve(btn, ctx);
        kPhrase.handle_input(phrase, row, col, act);
        return ctx_str(ctx) + " act=" + act_str(act) + " " +
               step_str(phrase.steps[static_cast<size_t>(row)]);
    }
    if (scr == "CHAIN") {
        const int row = 5;
        Chain     chain(0);
        const std::vector<std::string> f = split(field(toks, "crow"), ':');
        chain.phraseRefs[static_cast<size_t>(row)]      = from_dec(f[0]);
        chain.transposeValues[static_cast<size_t>(row)] = from_hex(f[1]);

        ChainEditorState st{chain};
        st.cursorRow    = row;
        st.cursorColumn = col;
        const CursorContext ctx = kChain.cursor_context(st);
        const InputAction   act = resolve(btn, ctx);
        kChain.handle_input(chain, row, col, act);
        return ctx_str(ctx) + " act=" + act_str(act) + " " + chain_row_str(chain, row);
    }
    if (scr == "SONG") {
        const int row = 9;
        // ⚠️ NOT `Project p;` — songcore's default Project has EMPTY pools (model.h: "filled by
        // factory"), because project_io needs a blank one to parse into. Kotlin's default ctor IS the
        // factory, so the two only agree through make_default_project().
        Project p = songcore::make_default_project();

        const std::vector<std::string> f   = split(field(toks, "scell"), ':');
        const int                      len = from_dec(f[0]);
        const int                      ref = from_dec(f[1]);

        // The column IS the track and it is 1-based; col 0 is the read-only gutter, and the golden
        // probes it too (the module must answer `none()` there rather than index track −1).
        const int trackIndex = (col - 1) < 0 ? 0 : ((col - 1) > 7 ? 7 : (col - 1));
        songcore::Track& track = p.tracks[static_cast<size_t>(trackIndex)];
        track.chainRefs.assign(static_cast<size_t>(len), -1);
        if (len > 9) track.chainRefs[9] = ref;

        SongEditorState st{p};
        st.cursorRow   = row;
        st.cursorTrack = col;
        const CursorContext ctx = kSong.cursor_context(st);
        const InputAction   act = resolve(btn, ctx);
        kSong.handle_input(p, row, col, act);

        const int n = static_cast<int>(track.chainRefs.size());
        return ctx_str(ctx) + " act=" + act_str(act) + " " +
               song_cell_str(n, (9 < n) ? track.chainRefs[9] : -1);
    }
    if (scr == "TABLE") {
        const int row = 4;
        Table     table(0);
        table.rows[static_cast<size_t>(row)] = parse_table_row("trow=" + field(toks, "trow"));

        TableState st{table};
        st.cursorRow    = row;
        st.cursorColumn = col;
        const CursorContext ctx = kTable.cursor_context(st);
        const InputAction   act = resolve(btn, ctx);
        kTable.handle_input(table, row, col, act);
        return ctx_str(ctx) + " act=" + act_str(act) + " " +
               table_row_str(table.rows[static_cast<size_t>(row)]);
    }
    if (scr == "GROOVE") {
        const int row = 2;
        Groove    groove(0);
        groove.steps[static_cast<size_t>(row)] = from_dec(field(toks, "grow"));

        GrooveState st{groove};
        st.cursorRow    = row;
        st.cursorColumn = col;
        const CursorContext ctx = kGroove.cursor_context(st);
        const InputAction   act = resolve(btn, ctx);
        kGroove.handle_input(groove, row, col, act);
        return ctx_str(ctx) + " act=" + act_str(act) + " grow=" +
               std::to_string(groove.steps[static_cast<size_t>(row)]);
    }

    // ── INSTRUMENT ───────────────────────────────────────────────────────────────────────────────
    if (scr == "INSTRUMENT") {
        const int  row = from_dec(field(toks, "row"));
        Instrument ins = parse_inst("inst=" + field(toks, "inst"), 3);

        const std::vector<std::string> sfp = split(field(toks, "sfp"), ',');

        InstrumentEditorState st{ins};
        st.cursorRow     = row;
        st.cursorColumn  = col;
        st.sfPresetCount = from_dec(sfp[0]);
        st.sfPresetIndex = from_dec(sfp[1]);

        const CursorContext ctx = kInstrument.cursor_context(st);
        const InputAction   act = resolve(btn, ctx);

        // The PRESET row's result (`presetIndexChanged`) is deliberately NOT applied: resolving an
        // index to a bank+preset needs the SF2's own list, which only a live engine has. The Kotlin
        // golden records the same no-op — its InstrumentController has a fake backend and no loaded
        // SoundFont, so setSoundfontPresetByIndex returns before it writes. What the golden DOES pin is
        // the row's cursor CONTEXT, which is where the arithmetic lives (maxIdx = count − 1, floored
        // at 0) and where an off-by-one would hide.
        kInstrument.handle_input(ins, row, col, act);
        return ctx_str(ctx) + " act=" + act_str(act) + " " + inst_str(ins);
    }

    // ── INST.POOL ────────────────────────────────────────────────────────────────────────────────
    if (scr == "INST_POOL") {
        Project p = songcore::make_default_project();
        const int slot = 3;
        p.instruments[static_cast<size_t>(slot)] = parse_inst("inst=" + field(toks, "inst"), slot);

        InstrumentPoolState st{p};
        st.selectedInstrument = slot;
        st.cursorColumn       = col;

        const CursorContext ctx = kPool.cursor_context(st);
        const InputAction   act = resolve(btn, ctx);
        kPool.handle_input(p.instruments[static_cast<size_t>(slot)], col, act);
        return ctx_str(ctx) + " act=" + act_str(act) + " " +
               inst_str(p.instruments[static_cast<size_t>(slot)]);
    }

    // ── MODS ─────────────────────────────────────────────────────────────────────────────────────
    if (scr == "MODS") {
        const int pair = from_dec(field(toks, "pair"));
        const int side = from_dec(field(toks, "side"));
        const int row  = from_dec(field(toks, "row"));
        const int slotIndex = pair * 2 + side;

        Instrument ins(3);
        ins.modSlots[static_cast<size_t>(slotIndex)] = parse_slot("slot=" + field(toks, "slot"));

        ModulationState st{ins};
        st.cursorRow  = row;
        st.cursorPair = pair;
        st.cursorSide = side;

        const CursorContext ctx = kMods.cursor_context(st);
        const InputAction   act = resolve(btn, ctx);
        kMods.handle_input(ins, slotIndex, row, act);
        return ctx_str(ctx) + " act=" + act_str(act) + " " +
               slot_str(ins.modSlots[static_cast<size_t>(slotIndex)]);
    }

    // ── MIXER ────────────────────────────────────────────────────────────────────────────────────
    if (scr == "MIXER") {
        const int row = from_dec(field(toks, "row"));
        Project   p   = songcore::make_default_project();
        parse_mix("mix=" + field(toks, "mix"), p);

        MixerState st{p};
        st.mixerMasterRow = row;
        st.cursorColumn   = col;

        const CursorContext ctx = kMixer.cursor_context(st);
        const InputAction   act = resolve(btn, ctx);
        kMixer.handle_input(p, row, col, act);
        return ctx_str(ctx) + " act=" + act_str(act) + " " + mix_str(p);
    }

    // ── EFFECTS ──────────────────────────────────────────────────────────────────────────────────
    if (scr == "EFFECTS") {
        const int row = from_dec(field(toks, "row"));
        Project   p   = songcore::make_default_project();
        parse_fx("fx=" + field(toks, "fx"), p);

        EffectState st{p};
        st.cursorRow = row;

        const CursorContext ctx = kEffects.cursor_context(st);
        const InputAction   act = resolve(btn, ctx);
        kEffects.handle_input(p, row, act);
        return ctx_str(ctx) + " act=" + act_str(act) + " " + fx_str(p);
    }

    // ── PROJECT (S7) ─────────────────────────────────────────────────────────────────────────────
    if (scr == "PROJECT") {
        const int row = from_dec(field(toks, "row"));
        Project   p   = songcore::make_default_project();
        parse_prj("prj=" + field(toks, "prj"), p);

        ProjectState st{p};
        st.cursorRow    = row;
        st.cursorColumn = col;
        // The golden is recorded from ANDROID, which has no EXIT row — and the caps only reach
        // `cursor_context` for that one row, so this could be either. Said out loud rather than left
        // to the default, because the two platforms genuinely answer differently for row 7.
        st.caps = PlatformCaps::android(/*debug_build=*/true);

        const CursorContext ctx = kProject.cursor_context(st);
        const InputAction   act = resolve(btn, ctx);
        kProject.handle_input(p, row, col, act);
        return ctx_str(ctx) + " act=" + act_str(act) + " " + prj_str(p);
    }

    // ── SETTINGS (S7) ────────────────────────────────────────────────────────────────────────────
    if (scr == "SETTINGS") {
        const int      row = from_dec(field(toks, "row"));
        SettingsValues v{};
        Theme          t = theme_classic();
        parse_set("set=" + field(toks, "set"), v, t);

        const PlatformCaps caps = PlatformCaps::android(/*debug_build=*/true);

        SettingsState st{v};
        st.cursorRow    = row;
        st.cursorColumn = col;
        st.caps         = caps;
        st.theme        = t;

        const CursorContext ctx = kSettings.cursor_context(st);
        const InputAction   act = resolve(btn, ctx);
        kSettings.handle_input(v, t, caps, row, col, act);
        return ctx_str(ctx) + " act=" + act_str(act) + " " + set_str(v, t);
    }

    // ── SAMPLE EDITOR ────────────────────────────────────────────────────────────────────────────
    if (scr == "SAMPLE_EDITOR") {
        SampleEditorState se = parse_se("se=" + field(toks, "se"));
        parse_se_env(field(toks, "env"), se);
        se.cursorRow = from_dec(field(toks, "row"));
        se.cursorCol = col;

        const CursorContext ctx = kSampleEditor.cursor_context(se);
        const InputAction   act = resolve(btn, ctx);
        kSampleEditor.handle_input(se, act);   // mutates in place, as every C++ module does
        return ctx_str(ctx) + " act=" + act_str(act) + " " + se_str(se);
    }

    err = "unknown screen " + scr;
    return "";
}

// ─── SEROW — the sparse row map (the D-pad's whole behaviour on the sample editor) ────────────────
//
// Not reachable through `handle_input`, and therefore invisible to every EDIT line above: the row map is
// consulted by the DISPATCHER, not by the module's edit path. A port that let the cursor into row 11
// with slicing OFF would land it on a row the module does not draw — and all 4,800 EDIT lines would
// still match, because not one of them asks how the cursor got where it is.

static std::string recompute_serow(const std::vector<std::string>& toks, std::string& err) {
    (void)err;
    const int method = from_dec(field(toks, "method"));
    const int row    = from_dec(field(toks, "row"));
    return "above=" + std::to_string(SampleEditorModule::row_above(row, method)) +
           " below=" + std::to_string(SampleEditorModule::row_below(row, method)) +
           " maxcol=" + std::to_string(SampleEditorModule::max_col_for_row(row, method));
}

// ─── SEVIEW — the derived values: slice bounds, the zoom window, the duration readout ─────────────

static std::string recompute_seview(const std::vector<std::string>& toks, std::string& err) {
    (void)err;
    SampleEditorState se = parse_se("se=" + field(toks, "se"));
    parse_se_env(field(toks, "env"), se);
    se.cursorRow        = from_dec(field(toks, "row"));
    se.cursorCol        = from_dec(field(toks, "col"));
    se.playbackPosition = std::strtof(field(toks, "play").c_str(), nullptr);

    std::string bounds;
    for (int idx = 0; idx <= 3; ++idx) {
        int64_t a = 0, b = 0;
        se.slice_bounds(idx, a, b);
        if (idx) bounds += ",";
        bounds += std::to_string(a) + "-" + std::to_string(b);
    }

    return "vstart=" + std::to_string(se.view_start()) + " vend=" + std::to_string(se.view_end()) +
           " slicepos=" + std::to_string(se.effective_slice_position()) +
           " dur=" + se.duration_display() + " bounds=" + bounds;
}

// ─── SEL ─────────────────────────────────────────────────────────────────────────────────────────

static std::string recompute_sel(const std::vector<std::string>& toks, std::string& err) {
    const std::vector<std::string> cur = split(field(toks, "cur"), ',');
    const std::vector<std::string> mx  = split(field(toks, "max"), ',');
    const int row    = from_dec(cur[0]);
    const int col    = from_dec(cur[1]);
    const int maxCol = from_dec(mx[0]);
    const int maxRow = from_dec(mx[1]);

    Selection sel;
    // The fake clock. F advances it 10 ms (inside the 500 ms window), S advances 600 (outside).
    // This is the whole reason handle_select_b takes `now_ms` instead of reading it.
    long long now = 1000;

    for (const std::string& op : split(field(toks, "script"), ',')) {
        if (op == "LB:F") {
            now += 10;
            sel.handle_select_b(now, row, col, maxCol, maxRow);
        } else if (op == "LB:S") {
            now += 600;
            sel.handle_select_b(now, row, col, maxCol, maxRow);
        } else if (op == "X") {
            sel.exit();
        } else if (op.rfind("E:", 0) == 0) {
            sel.expand(op.substr(2).c_str(), maxRow, maxCol);
        } else {
            err = "unknown op " + op;
            return "";
        }
    }

    const char* scopeName = "NONE";
    switch (sel.scope) {
        case SelectionScope::CELL:   scopeName = "CELL";   break;
        case SelectionScope::ROW:    scopeName = "ROW";    break;
        case SelectionScope::SCREEN: scopeName = "SCREEN"; break;
        case SelectionScope::NONE:   scopeName = "NONE";   break;
    }

    // Kotlin nulls start/end on exit and prints "-"; the C++ struct zeroes them and gates on `active`.
    const std::string startStr =
        sel.active ? std::to_string(sel.start.row) + "," + std::to_string(sel.start.column) : "-";
    const std::string endStr =
        sel.active ? std::to_string(sel.end.row) + "," + std::to_string(sel.end.column) : "-";
    std::string boundsStr = "-";
    if (sel.active) {
        const SelectionBounds b = sel.bounds();
        boundsStr = std::to_string(b.topLeftRow) + "," + std::to_string(b.topLeftColumn) + "-" +
                    std::to_string(b.bottomRightRow) + "," + std::to_string(b.bottomRightColumn);
    }
    const std::string info = sel.info().empty() ? "-" : sel.info();

    return std::string("scope=") + scopeName + " active=" + (sel.active ? "1" : "0") +
           " start=" + startStr + " end=" + endStr + " bounds=" + boundsStr + " info=" + info;
}

// ─── CLIP ────────────────────────────────────────────────────────────────────────────────────────
//
// The seeded grids: every row distinct, so a mis-anchored paste is visible rather than lucky. They
// must match P3InputGoldenTest's `seeded*()` builders exactly — a divergence here shows up as a
// mismatch in the FIRST dump row, which is what makes it easy to spot.

static Phrase seeded_phrase() {
    Phrase p(0);
    for (int i = 0; i < 16; ++i) {
        PhraseStep s;
        s.note       = Note{i % 12, 2 + i / 12};
        s.volume     = 0x10 + i;
        s.instrument = 0x20 + i;
        s.fx1Type    = songcore::FX_ARC;    s.fx1Value = 0x30 + i;
        s.fx2Type    = songcore::FX_PAN;    s.fx2Value = 0x40 + i;
        s.fx3Type    = songcore::FX_VOLUME; s.fx3Value = 0x50 + i;
        p.steps[static_cast<size_t>(i)] = s;
    }
    return p;
}

static Chain seeded_chain() {
    Chain c(0);
    for (int i = 0; i < 16; ++i) {
        c.phraseRefs[static_cast<size_t>(i)]      = (i % 4 == 3) ? -1 : (0x10 + i);
        c.transposeValues[static_cast<size_t>(i)] = 0x20 + i;
    }
    return c;
}

static Table seeded_table() {
    Table t(0);
    for (int i = 0; i < 16; ++i) {
        TableRow r;
        r.transpose = 0x10 + i;
        r.volume    = (i % 5 == 4) ? -1 : (0x20 + i);
        r.fx1Type   = songcore::FX_TBL; r.fx1Value = 0x30 + i;
        r.fx2Type   = songcore::FX_HOP; r.fx2Value = 0x40 + i;
        r.fx3Type   = songcore::FX_PIT; r.fx3Value = 0x50 + i;
        t.rows[static_cast<size_t>(i)] = r;
    }
    return t;
}

static Project seeded_project() {
    Project p = songcore::make_default_project();
    for (int t = 0; t < 8; ++t) {
        songcore::Track& track = p.tracks[static_cast<size_t>(t)];
        track.chainRefs.clear();
        for (int r = 0; r < 16; ++r) track.chainRefs.push_back((r % 3 == 2) ? -1 : (t * 16 + r));
    }
    return p;
}

static std::string row_label(int i) {
    char b[8];
    std::snprintf(b, sizeof(b), "  R%02d ", i);
    return b;
}

static std::string paste_str(const PasteResult& r) {
    switch (r.kind) {
        case PasteResult::Kind::NO_CLIPBOARD: return "NO_CLIPBOARD";
        case PasteResult::Kind::SUCCESS:      return "SUCCESS:" + std::to_string(r.itemsPasted);
        case PasteResult::Kind::WRONG_SCREEN: return "WRONG_SCREEN";
    }
    return "?";
}

static ScreenType screen_of(const std::string& s) {
    if (s == "PHRASE") return ScreenType::PHRASE;
    if (s == "CHAIN")  return ScreenType::CHAIN;
    if (s == "SONG")   return ScreenType::SONG;
    if (s == "TABLE")  return ScreenType::TABLE;
    return ScreenType::PHRASE;
}

/** A CLIP case: the headline RHS, plus the grid dump that follows it. */
struct ClipOut {
    std::string              rhs;
    std::vector<std::string> dump;
};

static ClipOut recompute_clip(const std::vector<std::string>& toks, std::string& err) {
    ClipOut           out;
    const std::string scr = field(toks, "scr");
    const std::string op  = field(toks, "op");

    // Every case builds its own clipboard and its own grid. Carrying one across cases would make the
    // tool's answer depend on the ORDER the golden happens to list them in, and a case that only
    // passed because the previous one left the right thing behind is not a case that proves anything.
    Clipboard gClip;

    auto ints = [](const std::string& csv) {
        std::vector<int> v;
        for (const std::string& s : split(csv, ',')) v.push_back(from_dec(s));
        return v;
    };

    if (op == "copy-paste" || op == "cut-paste") {
        const std::vector<int> src = ints(field(toks, "src"));
        const std::vector<int> dst = ints(field(toks, "dst"));
        gClip.clear();

        if (scr == "PHRASE") {
            Project p = songcore::make_default_project();
            p.phrases[0] = seeded_phrase();
            if (op == "copy-paste") gClip.copy_phrase_steps(p, 0, src[0], src[1], src[2], src[3]);
            else                    gClip.cut_phrase_steps(p, 0, src[0], src[1], src[2], src[3]);
            const PasteResult r = gClip.paste(p, ScreenType::PHRASE, 0, dst[0], dst[1]);
            out.rhs = paste_str(r) + " info=" + gClip.info();
            for (int i = 0; i < 16; ++i)
                out.dump.push_back(row_label(i) + step_str(p.phrases[0].steps[static_cast<size_t>(i)]));
        } else if (scr == "CHAIN") {
            Project p = songcore::make_default_project();
            p.chains[0] = seeded_chain();
            if (op == "copy-paste") gClip.copy_chain_rows(p, 0, src[0], src[1], src[2], src[3]);
            else                    gClip.cut_chain_rows(p, 0, src[0], src[1], src[2], src[3]);
            const PasteResult r = gClip.paste(p, ScreenType::CHAIN, 0, dst[0], dst[1]);
            out.rhs = paste_str(r) + " info=" + gClip.info();
            for (int i = 0; i < 16; ++i) out.dump.push_back(row_label(i) + chain_row_str(p.chains[0], i));
        } else if (scr == "SONG") {
            Project p = seeded_project();
            if (op == "copy-paste") gClip.copy_song_cells(p, src[0], src[1], src[2], src[3]);
            else                    gClip.cut_song_cells(p, src[0], src[1], src[2], src[3]);
            const PasteResult r = gClip.paste(p, ScreenType::SONG, 0, dst[0], dst[1]);
            out.rhs = paste_str(r) + " info=" + gClip.info();
            for (int i = 0; i < 16; ++i) {
                std::string cells;
                for (int t = 0; t < 8; ++t) {
                    const std::vector<int>& cr = p.tracks[static_cast<size_t>(t)].chainRefs;
                    if (t) cells += ",";
                    cells += std::to_string(i < static_cast<int>(cr.size()) ? cr[static_cast<size_t>(i)] : -1);
                }
                out.dump.push_back(row_label(i) + cells);
            }
        } else if (scr == "TABLE") {
            Project p = songcore::make_default_project();
            p.tables[0] = seeded_table();
            if (op == "copy-paste") gClip.copy_table_rows(p, 0, src[0], src[1], src[2], src[3]);
            else                    gClip.cut_table_rows(p, 0, src[0], src[1], src[2], src[3]);
            const PasteResult r = gClip.paste(p, ScreenType::TABLE, 0, dst[0], dst[1]);
            out.rhs = paste_str(r) + " info=" + gClip.info();
            for (int i = 0; i < 16; ++i)
                out.dump.push_back(row_label(i) + table_row_str(p.tables[0].rows[static_cast<size_t>(i)]));
        } else {
            err = "unknown clip screen " + scr;
        }
        return out;
    }

    if (op == "delete") {
        const std::vector<int> sel = ints(field(toks, "sel"));
        gClip.clear();
        int n = -1;
        if (scr == "PHRASE") {
            Project p = songcore::make_default_project();
            p.phrases[0] = seeded_phrase();
            n = gClip.delete_phrase_steps(p, 0, sel[0], sel[1], sel[2], sel[3]);
            out.rhs = "n=" + std::to_string(n);
            for (int i = 0; i < 16; ++i)
                out.dump.push_back(row_label(i) + step_str(p.phrases[0].steps[static_cast<size_t>(i)]));
        } else if (scr == "CHAIN") {
            Project p = songcore::make_default_project();
            p.chains[0] = seeded_chain();
            n = gClip.delete_chain_rows(p, 0, sel[0], sel[1], sel[2], sel[3]);
            out.rhs = "n=" + std::to_string(n);
            for (int i = 0; i < 16; ++i) out.dump.push_back(row_label(i) + chain_row_str(p.chains[0], i));
        } else if (scr == "SONG") {
            Project p = seeded_project();
            n = gClip.delete_song_cells(p, sel[0], sel[1], sel[2], sel[3]);
            out.rhs = "n=" + std::to_string(n);
            for (int i = 0; i < 16; ++i) {
                std::string cells;
                for (int t = 0; t < 8; ++t) {
                    const std::vector<int>& cr = p.tracks[static_cast<size_t>(t)].chainRefs;
                    if (t) cells += ",";
                    cells += std::to_string(i < static_cast<int>(cr.size()) ? cr[static_cast<size_t>(i)] : -1);
                }
                out.dump.push_back(row_label(i) + cells);
            }
        } else if (scr == "TABLE") {
            Project p = songcore::make_default_project();
            p.tables[0] = seeded_table();
            n = gClip.delete_table_rows(p, 0, sel[0], sel[1], sel[2], sel[3]);
            out.rhs = "n=" + std::to_string(n);
            for (int i = 0; i < 16; ++i)
                out.dump.push_back(row_label(i) + table_row_str(p.tables[0].rows[static_cast<size_t>(i)]));
        } else {
            err = "unknown clip screen " + scr;
        }
        return out;
    }

    if (op == "paste-foreign") {
        // A PHRASE clip aimed at CHAIN / SONG / TABLE. The copy is redone per line rather than left
        // over from the previous one — same reasoning as the fresh Clipboard above.
        Project p = songcore::make_default_project();
        p.phrases[0] = seeded_phrase();
        gClip.copy_phrase_steps(p, 0, 0, 1, 1, 2);
        const PasteResult r = gClip.paste(p, screen_of(scr), 0, 0, 1);
        out.rhs             = paste_str(r);
        return out;
    }

    if (op == "paste-empty") {
        gClip.clear();
        Project           p = songcore::make_default_project();
        const PasteResult r = gClip.paste(p, screen_of(scr), 0, 0, 1);
        out.rhs = paste_str(r) + " info=" + (gClip.info().empty() ? "-" : gClip.info());
        return out;
    }

    err = "unknown clip op " + op;
    return out;
}

// ─── FXH ─────────────────────────────────────────────────────────────────────────────────────────

static std::string recompute_fxh(const std::vector<std::string>& toks, std::string& err) {
    FxHelperState s     = fx_helper_opened_at(from_dec(field(toks, "open")));
    const std::string m = field(toks, "moves");

    if (m != "-") {
        for (const std::string& mv : split(m, ',')) {
            if (mv == "U")      fx_move_up(s);
            else if (mv == "D") fx_move_down(s);
            else if (mv == "L") fx_move_left(s);
            else if (mv == "R") fx_move_right(s);
            else {
                err = "unknown move " + mv;
                return "";
            }
        }
    }
    return "row=" + std::to_string(s.cursorRow) + " col=" + std::to_string(s.cursorCol) +
           " idx=" + std::to_string(s.cursor_index()) + " code=" + hex2(s.selected_effect_code());
}

// ─── SORT — the file browser's listing order (S6a) ───────────────────────────────────────────────
//
// The browser is a NAVIGATOR: no cursor context, no handle_input, nothing an EDIT line could measure.
// What it has is `sort_items`, and that is where every bug in a file listing actually lives.
//
// The fixture is SYNTHETIC — name, size and mtime as data, never a real directory — so both sides sort
// the identical input with no disk between them. It mirrors P3InputGoldenTest.sortFixture() exactly;
// the two must be edited together, and the golden's own header says which two files tie.

static std::vector<BrowserItem> sort_fixture() {
    struct Entry {
        std::string name;
        bool        dir;
        int64_t     size;
        int64_t     mtime;
    };

    // ⚠️ **THIRTY-SIX files, and the number is load-bearing.** The first version of this fixture had
    // four, and swapping `std::stable_sort` for `std::sort` in sort_items STILL PASSED — measured, not
    // assumed. Every standard sort falls back to insertion sort below a small-range threshold (32 on
    // MSVC's `_ISORT_MAX`, 16 on libstdc++'s `_S_threshold`), and insertion sort is stable, so a small
    // fixture cannot tell the two apart: green on the dev box, and free to go red on another toolchain
    // for a reason nobody would guess. Past the threshold introsort partitions and ties scramble.
    //
    // So 34 of them share ONE mtime — which is not a contrivance but the common case (every file a
    // `git clone` writes; every WAV a CHOP produces) — and must come out of a DATE sort in pure NAME
    // order. Mirrors P3InputGoldenTest.sortFixture(); the two are edited together.
    std::vector<Entry> entries = {
        {"Zed", true, 0, 500},
        {"alpha", true, 0, 900},
        {"aaa.wav", false, 5000, 100},   // oldest, biggest
        {"zzz.wav", false, 1, 9000},     // newest, smallest
    };
    for (int k = 0; k < 34; ++k) {
        const int i = (k * 13) % 34;     // a jumbled but deterministic permutation
        char      name[16];
        std::snprintf(name, sizeof(name), "s%02d.wav", i);
        entries.push_back({name, false, 100 + (i % 3) * 1000, 1000});
    }

    // build_item_list's own pre-sort: each group by lowercased name. Reproduced here rather than
    // called, because build_item_list takes a FileSystem and this fixture has no directory behind it.
    std::vector<BrowserItem> folders, files;
    for (const Entry& e : entries) {
        BrowserItem it;
        it.path         = "/fixture/" + e.name;
        it.sortName     = to_lower(e.name);
        it.size         = e.size;
        it.lastModified = e.mtime;
        if (e.dir) {
            it.kind        = BrowserItem::Kind::FOLDER;
            it.displayName = "[" + e.name + "]";
            folders.push_back(std::move(it));
        } else {
            it.kind        = BrowserItem::Kind::FILE;
            it.extension   = path_extension(e.name);
            it.displayName = path_stem(e.name);
            files.push_back(std::move(it));
        }
    }
    auto by_name = [](const BrowserItem& a, const BrowserItem& b) { return a.sortName < b.sortName; };
    std::stable_sort(folders.begin(), folders.end(), by_name);
    std::stable_sort(files.begin(), files.end(), by_name);

    BrowserItem parent;
    parent.kind        = BrowserItem::Kind::PARENT;
    parent.displayName = "..";

    std::vector<BrowserItem> out{parent};
    out.insert(out.end(), folders.begin(), folders.end());
    out.insert(out.end(), files.begin(), files.end());
    return out;
}

static std::string recompute_sort(const std::vector<std::string>& toks, std::string& err) {
    const std::string mode = field(toks, "mode");

    FileSortMode m{};
    if (mode == "DATE_DESC")      m = FileSortMode::DATE_DESC;
    else if (mode == "DATE_ASC")  m = FileSortMode::DATE_ASC;
    else if (mode == "NAME_ASC")  m = FileSortMode::NAME_ASC;
    else if (mode == "NAME_DESC") m = FileSortMode::NAME_DESC;
    else if (mode == "SIZE_ASC")  m = FileSortMode::SIZE_ASC;
    else if (mode == "SIZE_DESC") m = FileSortMode::SIZE_DESC;
    else {
        err = "unknown sort mode " + mode;
        return "";
    }

    std::vector<BrowserItem> items = sort_fixture();
    sort_items(items, m);

    std::string out;
    for (const BrowserItem& it : items) {
        if (!out.empty()) out += ",";
        out += it.displayName;
    }
    return out;
}

// ─── KBD — the QWERTY keyboard's state machine (S6a) ─────────────────────────────────────────────

static std::string recompute_kbd(const std::vector<std::string>& toks, std::string& err) {
    QwertyKeyboardState s{};
    s.isOpen        = true;
    s.maxLength     = 8;   // small, so the golden's "past maxLength" script is reachable
    s.insertBefore  = from_dec(field(toks, "ib")) != 0;
    s.clearOnFirstB = from_dec(field(toks, "cb")) != 0;

    const std::string init = field(toks, "init");
    s.text       = (init == "-") ? "" : init;
    s.textCursor = static_cast<int>(s.text.size());

    const std::string g = field(toks, "g");
    if (g != "-") {
        for (const std::string& gesture : split(g, ',')) {
            if (gesture == "U")      move_key_cursor_up(s);
            else if (gesture == "D") move_key_cursor_down(s);
            else if (gesture == "L") move_key_cursor_left(s);
            else if (gesture == "R") move_key_cursor_right(s);
            else if (gesture == "A") insert_current_key(s);
            else if (gesture == "B") delete_char(s);
            else if (gesture == "<") move_text_cursor_left(s);
            else if (gesture == ">") move_text_cursor_right(s);
            // ⚠️ The layout switch CLAMPS the column, because the two layouts' rows can differ in
            // length. Kotlin's R+UP/R+DOWN handlers call `.withClampedCol()` for the same reason.
            else if (gesture == "0") { s.layout = 0; clamp_col(s); }
            else if (gesture == "1") { s.layout = 1; clamp_col(s); }
            else {
                err = "unknown gesture " + gesture;
                return "";
            }
        }
    }

    return "text=" + (s.text.empty() ? std::string("-") : s.text) +
           " tc=" + std::to_string(s.textCursor) + " kr=" + std::to_string(s.keyCursorRow) +
           " kc=" + std::to_string(s.keyCursorCol) + " lay=" + std::to_string(s.layout) +
           " key=" + std::string(1, s.current_key()) +
           " cb=" + std::to_string(s.clearOnFirstB ? 1 : 0);
}

// ─── main ────────────────────────────────────────────────────────────────────────────────────────

int main(int argc, char** argv) {
    if (argc < 2) {
        std::cerr << "usage: ptinput <testdata/units/p3-input.txt>\n";
        return 2;
    }
    const std::string goldenPath = argv[1];

    std::string text;
    if (!read_file(goldenPath, text)) {
        std::cerr << "[FAIL] cannot read " << goldenPath
                  << " — run the JVM P3InputGoldenTest first to generate it.\n";
        return 1;
    }

    // Whole-file, because a CLIP case owns the indented `  R##` lines that follow it.
    std::vector<std::string> lines;
    {
        std::istringstream in(text);
        std::string        l;
        while (std::getline(in, l)) {
            if (!l.empty() && l.back() == '\r') l.pop_back();  // tolerate CRLF
            lines.push_back(l);
        }
    }

    int                        total = 0, failures = 0;
    std::map<std::string, int> perKind;

    for (size_t i = 0; i < lines.size(); ++i) {
        const std::string& line = lines[i];
        if (line.empty() || line[0] == '#' || line[0] == ' ') continue;  // ' ' = a dump row

        const size_t sep = line.find(" => ");
        if (sep == std::string::npos) {
            std::cerr << "[FAIL] line " << (i + 1) << ": no ' => ' separator: " << line << "\n";
            ++failures;
            continue;
        }
        const std::string lhs       = line.substr(0, sep);
        const std::string rhsGolden = line.substr(sep + 4);

        const std::vector<std::string> toks = tokens(lhs);
        const std::string              kind = toks[0];

        std::string err;
        std::string rhsCpp;
        std::vector<std::string> dumpCpp;

        if (kind == "EDIT")      rhsCpp = recompute_edit(toks, err);
        else if (kind == "SEL")  rhsCpp = recompute_sel(toks, err);
        else if (kind == "FXH")  rhsCpp = recompute_fxh(toks, err);
        else if (kind == "SORT") rhsCpp = recompute_sort(toks, err);
        else if (kind == "KBD")  rhsCpp = recompute_kbd(toks, err);
        else if (kind == "SEROW")  rhsCpp = recompute_serow(toks, err);
        else if (kind == "SEVIEW") rhsCpp = recompute_seview(toks, err);
        else if (kind == "CLIP") {
            const ClipOut o = recompute_clip(toks, err);
            rhsCpp          = o.rhs;
            dumpCpp         = o.dump;
        } else {
            err = "unknown kind " + kind;
        }

        ++total;
        ++perKind[kind];

        if (!err.empty()) {
            std::cerr << "[FAIL] line " << (i + 1) << " (" << kind << "): " << err << "\n";
            ++failures;
            continue;
        }
        if (rhsCpp != rhsGolden) {
            std::cerr << "[FAIL] line " << (i + 1) << " (" << kind << ") parity mismatch:\n"
                      << "    input : " << lhs << "\n"
                      << "    golden: " << rhsGolden << "\n"
                      << "    c++   : " << rhsCpp << "\n";
            ++failures;
            continue;
        }

        // The grid dump beneath a CLIP case, row by row — this is where a mis-anchored paste shows.
        for (size_t d = 0; d < dumpCpp.size(); ++d) {
            const size_t gi = i + 1 + d;
            const std::string golden = (gi < lines.size()) ? lines[gi] : std::string("<missing>");
            if (golden != dumpCpp[d]) {
                std::cerr << "[FAIL] line " << (gi + 1) << " (CLIP dump) mismatch:\n"
                          << "    case  : " << lhs << "\n"
                          << "    golden: " << golden << "\n"
                          << "    c++   : " << dumpCpp[d] << "\n";
                ++failures;
            }
        }
    }

    std::cout << "== Phase-3 S3 input-layer parity (C++ vs JVM golden) ==\n";
    for (const auto& kv : perKind) std::cout << "  " << kv.first << ": " << kv.second << " case(s)\n";
    std::cout << "checked " << total << " case(s)\n\n";
    std::cout << (failures == 0 ? "ALL GREEN" : "FAILURES: " + std::to_string(failures)) << "\n";
    return failures == 0 ? 0 : 1;
}
