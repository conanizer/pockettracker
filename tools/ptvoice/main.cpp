// ptvoice — songcore S5 conformance harness for the CONSUMER (host tool, no device/NDK).
//
// The conformance trace stops at the router, ABOVE the consumer (event-schema §6), so all 32 golden
// traces can be byte-green while the C++ consumer still derives a wrong frequency, a wrong slice
// window or a wrong envelope — and the only symptom would be "it sounds a bit off". This tool closes
// that hole, with the same method S3 used for the pure pieces:
//
//   * app/src/test/.../trace/S5ConsumerGoldenTest.kt drives the REAL Kotlin AudioEngine.scheduleNote
//     over a matrix of instruments and seam arguments, recording the exact engine calls it makes,
//     into testdata/units/s5-consumer.txt (floats as raw binary32 bits — nothing passes by being close);
//   * this tool re-parses each case's INPUTS, runs them through the C++ consumer's note path
//     (native/songcore/engine_consumer.h's plan_note_on, instantiated on a recorder instead of the
//     AudioEngine), and byte-compares the call sequence it produces.
//
// Because plan_note_on is a template over the engine, this checks the WHOLE note path — the derived
// values, which calls happen, in what order, and when a note is dropped instead — not just the math.
//
// Build (Windows, on-box MSVC):
//   call "...\VC\Auxiliary\Build\vcvars64.bat"
//   cl /std:c++17 /EHsc /O2 /nologo tools\ptvoice\main.cpp /Fe:ptvoice.exe
//   ptvoice.exe testdata/units/s5-consumer.txt
// clang++ / g++ work equally (-std=c++17). Exit 0 = all green, 1 = any mismatch.

#include "../../native/songcore/model.h"
#include "../../native/songcore/event.h"
#include "../../native/songcore/voice_derive.h"

#include <cstdint>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

using namespace songcore;

// ─── the recorder: satisfies plan_note_on's Engine template parameter ────────────────────────────
// Same method names as AudioEngine (that is the whole trick — no interface, no adapter, no virtuals),
// and it formats each call exactly as the Kotlin RecordingBackend does.
struct Recorder {
    std::vector<std::string> calls;
    int sampleRate = 44100;
    int sampleLength = 44100;

    static std::string f32(float v) {
        uint32_t b;
        std::memcpy(&b, &v, sizeof b);
        char buf[16];
        std::snprintf(buf, sizeof buf, "0x%08X", b);
        return buf;
    }
    static std::string i(long long v) { return std::to_string(v); }

    void add(const std::string& s) { calls.push_back(s); }

    // ── the surface plan_note_on uses ──
    int  getSampleRate() { return sampleRate; }
    int  getSampleLength(int) { return sampleLength; }
    void requestResume() {}   // no trace: Kotlin's resumeStream() is inert in the fake backend too

    void setTempo(int tempo) { add("TEMPO " + i(tempo)); }

    void loadTable(int tableId, const uint8_t*) { add("TABLE " + i(tableId)); }

    void setInstrumentModulation(int sampleId, int slotIndex, int type, int dest, float amount,
                                 int attackSamples, int holdSamples, int decaySamples,
                                 float sustainLevel, float lfoHz, int oscShape,
                                 int releaseSamples = 0, int lfoTrigMode = 1) {
        add("MOD " + i(sampleId) + " " + i(slotIndex) + " " + i(type) + " " + i(dest) + " " + f32(amount) +
            " " + i(attackSamples) + " " + i(holdSamples) + " " + i(decaySamples) +
            " " + f32(sustainLevel) + " " + f32(lfoHz) + " " + i(oscShape) +
            " " + i(releaseSamples) + " " + i(lfoTrigMode));
    }

    void clearInstrumentModulation(int sampleId) { add("MODCLR " + i(sampleId)); }
    void setInstrumentEqSlot(int instrId, int slot) { add("EQSLOT " + i(instrId) + " " + i(slot)); }
    void setInstrumentSendLevels(int instrId, int rsend, int dsend) {
        add("SENDS " + i(instrId) + " " + i(rsend) + " " + i(dsend));
    }
    void setSoundfontEnvelopeOverride(int instrumentId, int atk, int dec, int sus, int rel) {
        add("SFENV " + i(instrumentId) + " " + i(atk) + " " + i(dec) + " " + i(sus) + " " + i(rel));
    }
    void setInstrumentParams(int instrumentId, int start, int end, bool rev, int loop, int loopSt, int loopEn,
                             int drv, int crsh, int dwn, int fType, int fCut, int fRes) {
        add("PARAMS " + i(instrumentId) + " " + i(start) + " " + i(end) + " " + i(rev ? 1 : 0) + " " + i(loop) +
            " " + i(loopSt) + " " + i(loopEn) + " " + i(drv) + " " + i(crsh) + " " + i(dwn) +
            " " + i(fType) + " " + i(fCut) + " " + i(fRes));
    }
    void scheduleNote(int64_t frame, int sampleId, int trackId, float freq, float baseFreq, float vol,
                      float phraseVol, float pan, int startOv, int endOv, int tableId, int ticRate,
                      int noteOctave, int notePitch, float pslOff, float pslDur, float pbn,
                      float vibSpd, float vibDep, int tableStartRow) {
        add("NOTE " + i(frame) + " " + i(sampleId) + " " + i(trackId) + " " + f32(freq) + " " + f32(baseFreq) +
            " " + f32(vol) + " " + f32(phraseVol) + " " + f32(pan) + " " + i(startOv) + " " + i(endOv) +
            " " + i(tableId) + " " + i(ticRate) + " " + i(noteOctave) + " " + i(notePitch) +
            " " + f32(pslOff) + " " + f32(pslDur) + " " + f32(pbn) + " " + f32(vibSpd) + " " + f32(vibDep) +
            " " + i(tableStartRow));
    }
    void scheduleSoundfontNote(int64_t frame, int trackId, int sfSlot, int midiNote, int velocity,
                               float vol, float pan, int bank, int preset,
                               float pslOff, float pslDur, float pbn, float vibSpd, float vibDep,
                               float phraseVol, int sampleId, int tableId, int ticRate,
                               int noteOctave, int notePitch, int tableStartRow, float detune) {
        add("SFNOTE " + i(frame) + " " + i(trackId) + " " + i(sfSlot) + " " + i(midiNote) + " " + i(velocity) +
            " " + f32(vol) + " " + f32(pan) + " " + i(bank) + " " + i(preset) +
            " " + f32(pslOff) + " " + f32(pslDur) + " " + f32(pbn) + " " + f32(vibSpd) + " " + f32(vibDep) +
            " " + f32(phraseVol) + " " + i(sampleId) + " " + i(tableId) + " " + i(ticRate) +
            " " + i(noteOctave) + " " + i(notePitch) + " " + i(tableStartRow) + " " + f32(detune));
    }
};

// ─── input parsing (the CASE line's `key=value` pairs) ───────────────────────────────────────────
static std::map<std::string, std::string> parse_kv(const std::string& line) {
    std::map<std::string, std::string> kv;
    std::istringstream is(line);
    std::string tok;
    while (is >> tok) {
        if (tok == "|" || tok == "CASE") continue;
        const size_t eq = tok.find('=');
        if (eq == std::string::npos) continue;
        kv[tok.substr(0, eq)] = tok.substr(eq + 1);
    }
    return kv;
}

static int geti(const std::map<std::string, std::string>& kv, const char* k, int def = 0) {
    auto it = kv.find(k);
    return it == kv.end() ? def : std::stoi(it->second);
}
static std::string gets(const std::map<std::string, std::string>& kv, const char* k, const char* def = "") {
    auto it = kv.find(k);
    return it == kv.end() ? std::string(def) : it->second;
}
static uint32_t getbits(const std::map<std::string, std::string>& kv, const char* k) {
    auto it = kv.find(k);
    if (it == kv.end()) return 0;
    return static_cast<uint32_t>(std::stoul(it->second, nullptr, 16));
}
static std::vector<std::string> split(const std::string& s, char sep) {
    std::vector<std::string> out;
    std::string cur;
    std::istringstream is(s);
    while (std::getline(is, cur, sep)) out.push_back(cur);
    return out;
}

static ModType mod_type_of(const std::string& n) {
    if (n == "AHD") return ModType::AHD;
    if (n == "ADSR") return ModType::ADSR;
    if (n == "LFO") return ModType::LFO;
    if (n == "DRUM") return ModType::DRUM;
    if (n == "TRIG") return ModType::TRIG;
    if (n == "TRACKING") return ModType::TRACKING;
    if (n == "SCALAR") return ModType::SCALAR;
    return ModType::NONE;
}
static ModDest mod_dest_of(const std::string& n) {
    if (n == "VOLUME") return ModDest::VOLUME;
    if (n == "PAN") return ModDest::PAN;
    if (n == "PITCH") return ModDest::PITCH;
    if (n == "FINE_PITCH") return ModDest::FINE_PITCH;
    if (n == "FILTER_CUTOFF") return ModDest::FILTER_CUTOFF;
    if (n == "FILTER_RES") return ModDest::FILTER_RES;
    if (n == "SAMPLE_START") return ModDest::SAMPLE_START;
    if (n == "MOD_AMT") return ModDest::MOD_AMT;
    if (n == "MOD_RATE") return ModDest::MOD_RATE;
    if (n == "MOD_BOTH") return ModDest::MOD_BOTH;
    return ModDest::NONE;
}

int main(int argc, char** argv) {
    const std::string path = (argc > 1) ? argv[1] : "testdata/units/s5-consumer.txt";
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        std::cerr << "cannot open " << path << "\n";
        return 1;
    }

    std::cout << "== songcore S5 consumer parity (C++ vs the Kotlin AudioEngine golden) ==\n";

    std::string line;
    int cases = 0, failures = 0;
    std::string caseName;
    std::vector<std::string> expected;
    std::vector<std::string> actual;

    auto flush_case = [&]() {
        if (caseName.empty()) return;
        ++cases;
        if (actual != expected) {
            ++failures;
            std::cout << "FAIL " << caseName << "\n";
            const size_t n = std::max(expected.size(), actual.size());
            for (size_t i = 0; i < n; ++i) {
                const std::string e = i < expected.size() ? expected[i] : "<missing>";
                const std::string a = i < actual.size() ? actual[i] : "<missing>";
                if (e != a) {
                    std::cout << "   kotlin: " << e << "\n";
                    std::cout << "   c++   : " << a << "\n";
                }
            }
        }
        caseName.clear();
        expected.clear();
        actual.clear();
    };

    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty() || line[0] == '#') continue;

        if (line.rfind("CASE ", 0) == 0) {
            flush_case();

            std::istringstream is(line);
            std::string kw;
            is >> kw >> caseName;

            const auto kv = parse_kv(line);

            // ── rebuild the instrument ──
            Instrument ins(geti(kv, "id"));
            ins.sampleId       = geti(kv, "sampleId");
            ins.instrumentType = (gets(kv, "type") == "SOUNDFONT") ? InstrumentType::SOUNDFONT
                                                                   : InstrumentType::SAMPLER;
            if (geti(kv, "hasSample")) ins.sampleFilePath = std::string("/fake/sample.wav");
            if (geti(kv, "hasSf"))     ins.soundfontPath  = std::string("/fake/font.sf2");

            const auto rootParts = split(gets(kv, "root"), ':');
            ins.root = Note{std::stoi(rootParts[0]), std::stoi(rootParts[1])};

            ins.detune       = geti(kv, "detune");
            ins.tableTicRate = geti(kv, "ticRate");
            ins.slicingMode  = geti(kv, "slicing");
            ins.eqSlot       = geti(kv, "eqSlot");
            ins.reverbSend   = geti(kv, "rsend");
            ins.delaySend    = geti(kv, "dsend");
            ins.sfBank       = geti(kv, "sfBank");
            ins.sfPreset     = geti(kv, "sfPreset");
            ins.sampleStart  = geti(kv, "sStart");
            ins.sampleEnd    = geti(kv, "sEnd");
            ins.reverse      = geti(kv, "rev") != 0;
            ins.drive        = geti(kv, "drive");
            ins.crush        = geti(kv, "crush");
            ins.downsample   = geti(kv, "down");

            const auto sfEnv = split(gets(kv, "sfEnv"), ':');
            ins.sfOverrides.ampAttack  = std::stoi(sfEnv[0]);
            ins.sfOverrides.ampDecay   = std::stoi(sfEnv[1]);
            ins.sfOverrides.ampSustain = std::stoi(sfEnv[2]);
            ins.sfOverrides.ampRelease = std::stoi(sfEnv[3]);

            const auto loop = split(gets(kv, "loop"), ':');
            ins.loopMode  = loop[0];
            ins.loopStart = std::stoi(loop[1]);
            ins.loopEnd   = std::stoi(loop[2]);

            const auto filter = split(gets(kv, "filter"), ':');
            ins.filterType = filter[0];
            ins.filterCut  = std::stoi(filter[1]);
            ins.filterRes  = std::stoi(filter[2]);

            const std::string markers = gets(kv, "markers");
            if (markers != "-") {
                for (const std::string& m : split(markers, ',')) ins.sliceMarkers.push_back(std::stoll(m));
            }

            const auto mods = split(gets(kv, "mods"), ',');
            for (size_t s = 0; s < mods.size() && s < 4; ++s) {
                const auto f = split(mods[s], ':');
                ModSlot& m = ins.modSlots[s];
                m.type        = mod_type_of(f[0]);
                m.dest        = mod_dest_of(f[1]);
                m.amount      = std::stoi(f[2]);
                m.attack      = std::stoi(f[3]);
                m.hold        = std::stoi(f[4]);
                m.decay       = std::stoi(f[5]);
                m.sustain     = std::stoi(f[6]);
                m.release     = std::stoi(f[7]);
                m.lfoFreq     = std::stoi(f[8]);
                m.oscShape    = std::stoi(f[9]);
                m.lfoTrigMode = std::stoi(f[10]);
            }

            // ── the project songcore was handed ──
            Project project = make_default_project();
            project.tempo = geti(kv, "tempo");
            project.instruments[ins.id] = ins;

            Routing routing;
            const int sr = geti(kv, "sr", 44100);
            const int fileRate = geti(kv, "fileRate", 44100);
            // AudioEngine.loadSampleData: sampleRateRatios[id] = deviceRate / fileRate
            routing.sampleRateRatio[ins.id] = static_cast<float>(sr) / static_cast<float>(fileRate);
            routing.sfSlot[ins.id] = geti(kv, "sfSlot", -1);

            // ── the NoteOn record the router would have built ──
            const auto noteParts = split(gets(kv, "note"), ':');
            const int notePitch  = std::stoi(noteParts[0]);
            const int noteOctave = std::stoi(noteParts[1]);

            Event ev{};
            ev.frame      = 12345;
            ev.track      = 2;
            ev.instrument = static_cast<int16_t>(ins.id);
            ev.type       = EV_NOTE_ON;
            NoteOnPayload& n = ev.noteOn;
            n.note        = static_cast<uint8_t>((noteOctave + 1) * 12 + notePitch);
            n.velocity    = static_cast<int8_t>(geti(kv, "vel", -1));
            n.velGainBits = getbits(kv, "vol");
            n.volGainBits = getbits(kv, "pvol");
            n.panBits     = getbits(kv, "pan");
            n.start       = geti(kv, "start", -1);
            n.slice       = geti(kv, "slice", -1);
            n.transpose   = geti(kv, "transpose");
            n.pit         = geti(kv, "pit");
            n.arp         = geti(kv, "arp");
            n.tableId     = geti(kv, "tableId", -1);
            n.tableRow    = geti(kv, "tableRow", -1);
            n.pslOffBits  = getbits(kv, "pslOff");
            n.pslDurBits  = getbits(kv, "pslDur");
            n.pbnRateBits = getbits(kv, "pbn");
            n.vibSpdBits  = getbits(kv, "vibSpd");
            n.vibDepBits  = getbits(kv, "vibDep");

            Recorder rec;
            rec.sampleRate = sr;
            rec.sampleLength = 44100;   // S5ConsumerGoldenTest.SAMPLE_LENGTH

            bool tableLoaded[POOL_TABLES] = {false};
            plan_note_on(rec, ev, project, routing, tableLoaded);
            actual = rec.calls;
            continue;
        }

        // an expected engine call ("  NOTE …")
        size_t start = line.find_first_not_of(" \t");
        if (start == std::string::npos) continue;
        expected.push_back(line.substr(start));
    }
    flush_case();

    std::cout << "checked " << cases << " case(s)\n\n";
    if (failures == 0) {
        std::cout << "ALL GREEN\n";
        return 0;
    }
    std::cout << failures << " FAILED\n";
    return 1;
}
