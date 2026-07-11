// ptresolve — songcore S3 conformance harness (host tool, no device/NDK).
//
// Proves the C++ ports of the sequencer's *pure* pieces are equivalent to the Kotlin originals:
//   * native/songcore/timing.h    — framesPerStep, framesPerTic, byteToSignedSemitones, groove math
//   * native/songcore/effects.h   — resolveStepParams + the ResolvedStepParams bundle
//   * native/songcore/traversal.h — collectUsedInstruments (loads the real /testdata .ptp projects)
//
// It reads testdata/units/s3-units.txt (emitted by the JVM S3UnitGoldenTest from the REAL Kotlin
// functions), and for every `<inputs> => <outputs>` line it re-parses the inputs, recomputes the
// RHS in C++, and byte-compares against the golden RHS. Any mismatch is a parity bug, reported with
// the offending line. This is where the framesPerStep double-rounding and the binary32 `volume`
// divide are proven bit-identical across the language boundary.
//
// Build (Windows, on-box MSVC):
//   cl /std:c++17 /EHsc /O2 /nologo tools/ptresolve/main.cpp /Fe:ptresolve.exe
// Run:
//   ptresolve.exe testdata
//
// Exit code 0 = all green, 1 = any mismatch.  Linux-port plan §4.3 (S3 pure-piece port).

#include "../../native/songcore/timing.h"
#include "../../native/songcore/effects.h"
#include "../../native/songcore/traversal.h"
#include "../../native/songcore/project_io.h"   // USEDINST loads .ptp via the S2 reader

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iostream>
#include <map>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

using namespace songcore;

// ── small parsing/formatting helpers (formatting MUST match S3UnitGoldenTest.kt) ──────────────────

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
    std::string cur;
    for (char c : s) {
        if (c == sep) { v.push_back(cur); cur.clear(); }
        else cur += c;
    }
    v.push_back(cur);
    return v;
}

// Space-split the input side (everything before " => ") into a KIND + a key=value map. The RSP
// values (e.g. "1F,C0") and groove step lists carry no spaces, so a space split is safe.
struct Line {
    std::string kind;
    std::map<std::string, std::string> kv;
};
static Line parse_lhs(const std::string& lhs) {
    Line ln;
    auto toks = split(lhs, ' ');
    bool first = true;
    for (auto& t : toks) {
        if (t.empty()) continue;
        if (first) { ln.kind = t; first = false; continue; }
        auto eq = t.find('=');
        if (eq != std::string::npos) ln.kv[t.substr(0, eq)] = t.substr(eq + 1);
    }
    return ln;
}

static int    to_int(const std::string& s)  { return static_cast<int>(std::strtol(s.c_str(), nullptr, 10)); }
static int64_t to_i64(const std::string& s) { return static_cast<int64_t>(std::strtoll(s.c_str(), nullptr, 10)); }
static int    hex_byte(const std::string& s){ return static_cast<int>(std::strtol(s.c_str(), nullptr, 16)); }

static std::string optI(const std::optional<int>& v)      { return v ? std::to_string(*v) : "-"; }
static std::string optL(const std::optional<int64_t>& v)  { return v ? std::to_string(*v) : "-"; }
static std::string f32(float v) {
    uint32_t bits;
    std::memcpy(&bits, &v, 4);
    char buf[16];
    std::snprintf(buf, sizeof buf, "0x%08X", bits);
    return buf;
}
static float f32_from_bits(const std::string& hex) {  // "0x3F800000" -> float
    uint32_t bits = static_cast<uint32_t>(std::strtoul(hex.c_str(), nullptr, 16));
    float v;
    std::memcpy(&v, &bits, 4);
    return v;
}

static Groove groove_from_csv(const std::string& csv) {
    Groove g;
    g.steps.clear();
    for (auto& tok : split(csv, ',')) g.steps.push_back(to_int(tok));
    return g;
}

static void parse_fx(const std::string& tv, int& type, int& value) {  // "1F,C0"
    auto parts = split(tv, ',');
    type  = hex_byte(parts.at(0));
    value = parts.size() > 1 ? hex_byte(parts.at(1)) : 0;
}

// ── the RHS recompute for each line kind ──────────────────────────────────────────────────────────

static std::string recompute(const Line& ln, const std::string& testdata, std::string& err) {
    const auto& kv = ln.kv;
    auto get = [&](const char* k) -> const std::string& { return kv.at(k); };

    if (ln.kind == "FPS") {
        return "fps=" + std::to_string(frames_per_step(to_int(get("tempo")), to_int(get("sr"))));
    }
    if (ln.kind == "FPT") {
        int64_t fps = frames_per_step(to_int(get("tempo")), to_int(get("sr")));
        return "fpt=" + std::to_string(frames_per_tic(fps));
    }
    if (ln.kind == "BSS") {
        return "semi=" + std::to_string(byte_to_signed_semitones(to_int(get("b"))));
    }
    if (ln.kind == "GACT") {
        Groove g = groove_from_csv(get("steps"));
        return "active=" + std::to_string(groove_active_length(g));
    }
    if (ln.kind == "GTIC") {
        Groove g = groove_from_csv(get("steps"));
        return "ticks=" + std::to_string(groove_ticks_for_step(g, to_int(get("pos"))));
    }
    if (ln.kind == "GDUR") {
        Groove g = groove_from_csv(get("steps"));
        int64_t fps = frames_per_step(to_int(get("tempo")), to_int(get("sr")));
        int64_t fpt = frames_per_tic(fps);
        return "dur=" + std::to_string(groove_step_duration(g, to_int(get("pos")), fps, fpt));
    }
    if (ln.kind == "RSP") {
        PhraseStep step;
        parse_fx(get("fx1"), step.fx1Type, step.fx1Value);
        parse_fx(get("fx2"), step.fx2Type, step.fx2Value);
        parse_fx(get("fx3"), step.fx3Type, step.fx3Value);
        int64_t base = to_i64(get("base"));
        float dvol = f32_from_bits(get("dvol"));
        ResolvedStepParams r = resolve_step_params(step, base, dvol);
        std::string s;
        s += "start=" + std::to_string(r.startPoint);
        s += " vol=" + f32(r.volume);
        s += " vxx=" + std::string(r.volumeFromVxx ? "1" : "0");
        s += " kill=" + optL(r.killAtFrame);
        s += " koff=" + std::to_string(r.killOffsetTicks);
        s += " arc=" + optI(r.arcValue);
        s += " rep=" + optI(r.repeatCount);
        s += " repr=" + optI(r.repeatVolRamp);
        s += " hop=" + optI(r.hopValue);
        s += " psl=" + optI(r.pslDuration);
        s += " pbn=" + optI(r.pbnValue);
        s += " pvb=" + optI(r.pvbValue);
        s += " pvx=" + optI(r.pvxValue);
        s += " del=" + optI(r.delayTicks);
        s += " tbl=" + optI(r.tableOverride);
        s += " tho=" + optI(r.tableHopTarget);
        s += " grv=" + optI(r.grooveId);
        s += " pit=" + optI(r.pitSemitones);
        s += " sli=" + optI(r.sliIndex);
        s += " pan=" + optI(r.panValue);
        s += " rev=" + optI(r.reverbSendValue);
        s += " dsend=" + optI(r.delaySendValue);
        s += " bck=" + optI(r.bckValue);
        s += " eqn=" + optI(r.eqnSlot);
        s += " eqm=" + optI(r.eqmSlot);
        return s;
    }
    if (ln.kind == "USEDINST") {
        std::string path = testdata + "/" + get("project") + ".ptp";
        std::string bytes;
        if (!read_file(path, bytes)) { err = "cannot read " + path; return ""; }
        Project p;
        try {
            p = parse_project(json::parse(bytes));
        } catch (const std::exception& e) {
            err = std::string("parse threw: ") + e.what();
            return "";
        }
        normalize_and_migrate(p);
        auto used = collect_used_instruments(p, to_int(get("start")), to_int(get("end")));
        if (used.empty()) return "inst=-";
        std::string s = "inst=";
        bool first = true;
        for (int id : used) { if (!first) s += ","; s += std::to_string(id); first = false; }
        return s;
    }

    err = "unknown line kind: " + ln.kind;
    return "";
}

int main(int argc, char** argv) {
    std::string testdata = (argc > 1) ? argv[1] : "testdata";
    std::string goldenPath = testdata + "/units/s3-units.txt";

    std::string text;
    if (!read_file(goldenPath, text)) {
        std::cerr << "[FAIL] cannot read " << goldenPath
                  << " — run the JVM S3UnitGoldenTest first to generate it.\n";
        return 1;
    }

    int total = 0, failures = 0;
    std::map<std::string, int> perKind;

    std::istringstream in(text);
    std::string line;
    int lineNo = 0;
    while (std::getline(in, line)) {
        ++lineNo;
        if (!line.empty() && line.back() == '\r') line.pop_back();  // tolerate CRLF
        if (line.empty() || line[0] == '#') continue;

        auto sep = line.find(" => ");
        if (sep == std::string::npos) {
            std::cerr << "[FAIL] line " << lineNo << ": no ' => ' separator: " << line << "\n";
            ++failures;
            continue;
        }
        std::string lhs = line.substr(0, sep);
        std::string rhsGolden = line.substr(sep + 4);

        Line ln = parse_lhs(lhs);
        std::string err;
        std::string rhsCpp = recompute(ln, testdata, err);
        ++total;
        ++perKind[ln.kind];

        if (!err.empty()) {
            std::cerr << "[FAIL] line " << lineNo << " (" << ln.kind << "): " << err << "\n";
            ++failures;
        } else if (rhsCpp != rhsGolden) {
            std::cerr << "[FAIL] line " << lineNo << " (" << ln.kind << ") parity mismatch:\n"
                      << "    input : " << lhs << "\n"
                      << "    golden: " << rhsGolden << "\n"
                      << "    c++   : " << rhsCpp << "\n";
            ++failures;
        }
    }

    std::cout << "== songcore S3 pure-piece parity (C++ vs JVM golden) ==\n";
    for (const auto& [k, n] : perKind)
        std::cout << "  " << k << ": " << n << " line(s)\n";
    std::cout << "checked " << total << " line(s)\n\n";
    std::cout << (failures == 0 ? "ALL GREEN" : "FAILURES: " + std::to_string(failures)) << "\n";
    return failures == 0 ? 0 : 1;
}
