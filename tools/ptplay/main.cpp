// ptplay — songcore S4 conformance harness (host tool, no device/NDK).
//
// Proves the C++ port of the sequencer SPINE reproduces the Kotlin golden event stream
// byte-for-byte:
//   * native/songcore/scheduler.h     — PlaybackController + TrackState (the doomed zone-C rewrite)
//   * native/songcore/router.h        — the MIDI event bus/router seam (Event records)
//   * native/songcore/trace_writer.h  — the schema-v1 conformance-trace serializer
//
// For each golden project it loads the /testdata .ptp (the SAME files the JVM GoldenTraceTest
// generated), drives the C++ Sequencer through the exact same modes/cadence as the Kotlin
// TraceHarness (render + live SONG/CHAIN/PHRASE at 44100 and 48000), and compares the produced
// trace against /testdata/traces/<project>.<sr>.<mode>.trace after the event-schema §4 canonical
// sort (frame, track, rank) — the same comparator TraceCompare.kt applies for cross-implementation
// runs. Any mismatch is a sequencing-parity bug, reported at the first divergent canonical line.
//
// The header's project= sha is SHA-1 of the .ptp bytes, which equals the JVM golden's sha
// (GoldenTraceTest writes the file as serializeProject(project), and hashes those same bytes) —
// so a header mismatch would also flag a serializer drift (S2 already proved the round-trip).
//
// Build (Windows, on-box MSVC):
//   call "...\VC\Auxiliary\Build\vcvars64.bat"
//   cl /std:c++17 /EHsc /O2 /nologo tools\ptplay\main.cpp /Fe:ptplay.exe
//   ptplay.exe testdata
// clang++ / g++ work equally (-std=c++17). Exit 0 = all green, 1 = any mismatch.
// Linux-port plan §4.3 (S4 — the spine).

#include "../../native/songcore/model.h"
#include "../../native/songcore/project_io.h"
#include "../../native/songcore/router.h"
#include "../../native/songcore/trace_writer.h"
#include "../../native/songcore/scheduler.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

using namespace songcore;

// ─── golden spec — mirrors GoldenProjects.all (name, render row range, live modes) ────────────────
struct LiveMode { std::string kind; int arg; int horizonPhrases; };
struct Spec { std::string name; int renderStart, renderEnd; std::vector<LiveMode> live; };

static const std::vector<Spec> SPECS = {
    {"g1-basics",   0, 0, {{"SONG", 0, 8}, {"PHRASE", 0, 4}, {"CHAIN", 0, 6}}},
    {"g2-timing",   0, 0, {{"SONG", 0, 8}, {"PHRASE", 0, 4}}},
    {"g3-retrig",   0, 0, {{"SONG", 0, 8}, {"PHRASE", 0, 4}}},
    {"g4-pitch",    0, 0, {{"PHRASE", 0, 4}}},
    {"g5-structure",0, 2, {{"SONG", 0, 12}}},
    {"g6-params",   0, 0, {{"PHRASE", 0, 4}}},
};

static const int SAMPLE_RATES[] = {44100, 48000};

// TraceHarness constants (verbatim).
static const int64_t LIVE_START_FRAME = 977;
static const int64_t CLOCK_STEP = 512;

// ─── file IO ──────────────────────────────────────────────────────────────────────────────────────
static bool read_file(const std::string& path, std::string& out) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    std::ostringstream ss;
    ss << f.rdbuf();
    out = ss.str();
    return true;
}

// ─── SHA-1 (project= header id; matches EventTrace.projectSha1 over the .ptp bytes) ───────────────
static std::string sha1_hex(const std::string& msg) {
    uint32_t h0 = 0x67452301, h1 = 0xEFCDAB89, h2 = 0x98BADCFE, h3 = 0x10325476, h4 = 0xC3D2E1F0;
    std::string data = msg;
    uint64_t ml = static_cast<uint64_t>(data.size()) * 8;
    data += static_cast<char>(0x80);
    while (data.size() % 64 != 56) data += static_cast<char>(0x00);
    for (int i = 7; i >= 0; --i) data += static_cast<char>((ml >> (i * 8)) & 0xFF);

    auto rol = [](uint32_t v, int n) { return (v << n) | (v >> (32 - n)); };
    for (size_t chunk = 0; chunk < data.size(); chunk += 64) {
        uint32_t w[80];
        for (int i = 0; i < 16; ++i) {
            w[i] = (static_cast<uint8_t>(data[chunk + i * 4]) << 24) |
                   (static_cast<uint8_t>(data[chunk + i * 4 + 1]) << 16) |
                   (static_cast<uint8_t>(data[chunk + i * 4 + 2]) << 8) |
                   (static_cast<uint8_t>(data[chunk + i * 4 + 3]));
        }
        for (int i = 16; i < 80; ++i) w[i] = rol(w[i-3] ^ w[i-8] ^ w[i-14] ^ w[i-16], 1);
        uint32_t a = h0, b = h1, c = h2, d = h3, e = h4;
        for (int i = 0; i < 80; ++i) {
            uint32_t f, k;
            if (i < 20)      { f = (b & c) | ((~b) & d);        k = 0x5A827999; }
            else if (i < 40) { f = b ^ c ^ d;                   k = 0x6ED9EBA1; }
            else if (i < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8F1BBCDC; }
            else             { f = b ^ c ^ d;                   k = 0xCA62C1D6; }
            uint32_t tmp = rol(a, 5) + f + e + k + w[i];
            e = d; d = c; c = rol(b, 30); b = a; a = tmp;
        }
        h0 += a; h1 += b; h2 += c; h3 += d; h4 += e;
    }
    char buf[41];
    std::snprintf(buf, sizeof buf, "%08x%08x%08x%08x%08x", h0, h1, h2, h3, h4);
    return buf;
}

// ─── canonical sort (event-schema §4 / TraceCompare.canonicalize) ────────────────────────────────
static int rank_of_type(const std::string& tt) {
    if (tt == "90") return 2;
    if (tt == "80") return 1;
    return 0;
}
struct Key { int64_t frame; int track; int rank; };
static Key key_of(const std::string& line) {
    size_t sp1 = line.find(' ');
    size_t sp2 = line.find(' ', sp1 + 1);
    size_t sp3 = line.find(' ', sp2 + 1);
    size_t sp4 = line.find(' ', sp3 + 1);
    size_t end = (sp4 == std::string::npos) ? line.size() : sp4;
    Key k;
    k.frame = std::strtoll(line.substr(0, sp1).c_str(), nullptr, 10);
    k.track = std::atoi(line.substr(sp1 + 1, sp2 - sp1 - 1).c_str());
    k.rank  = rank_of_type(line.substr(sp3 + 1, end - sp3 - 1));
    return k;
}
static bool is_event(const std::string& line) { return !line.empty() && line[0] >= '0' && line[0] <= '9'; }

static std::string canonicalize(const std::string& trace) {
    std::string out;
    std::vector<std::string> run;
    auto flush = [&]() {
        std::stable_sort(run.begin(), run.end(), [](const std::string& a, const std::string& b) {
            Key ka = key_of(a), kb = key_of(b);
            if (ka.frame != kb.frame) return ka.frame < kb.frame;
            if (ka.track != kb.track) return ka.track < kb.track;
            return ka.rank < kb.rank;
        });
        for (auto& l : run) { out += l; out += '\n'; }
        run.clear();
    };
    std::istringstream in(trace);
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();  // tolerate CRLF
        if (line.empty()) continue;
        if (is_event(line)) run.push_back(line);
        else { flush(); out += line; out += '\n'; }
    }
    flush();
    return out;
}

// Null when equal, else a first-divergence report.
static std::string diff(const std::string& expected, const std::string& actual) {
    std::string ce = canonicalize(expected), ca = canonicalize(actual);
    if (ce == ca) return "";
    std::vector<std::string> e, a;
    { std::istringstream in(ce); std::string l; while (std::getline(in, l)) e.push_back(l); }
    { std::istringstream in(ca); std::string l; while (std::getline(in, l)) a.push_back(l); }
    size_t n = std::max(e.size(), a.size());
    for (size_t i = 0; i < n; ++i) {
        std::string el = i < e.size() ? e[i] : "<end of trace>";
        std::string al = i < a.size() ? a[i] : "<end of trace>";
        if (el != al) {
            return "first divergence at canonical line " + std::to_string(i + 1) + ":\n" +
                   "    golden: " + el + "\n" +
                   "    c++   : " + al;
        }
    }
    return "traces differ in length only";
}

// ─── the driver — mirrors TraceHarness ────────────────────────────────────────────────────────────
static std::string render_trace(const Project& p, int startRow, int endRow, int sr, const std::string& sha) {
    std::string sb;
    TraceWriter w;
    w.begin(&sb, sha);
    MidiRouter router(&w);
    Sequencer seq(router, p, sr);
    seq.scheduleSongRowRange(startRow, endRow);
    w.end();
    return sb;
}

static std::string live_trace(const Project& p, const LiveMode& m, int sr, const std::string& sha) {
    std::string sb;
    TraceWriter w;
    w.begin(&sb, sha);
    MidiRouter router(&w);
    Sequencer seq(router, p, sr);
    int64_t horizon = frames_per_step(p.tempo, sr) * 16LL * m.horizonPhrases;
    seq.set_clock(LIVE_START_FRAME);
    if      (m.kind == "SONG")   seq.playSong(m.arg);
    else if (m.kind == "CHAIN")  seq.playChain(m.arg);
    else if (m.kind == "PHRASE") seq.playPhrase(m.arg);
    while (seq.clock() - LIVE_START_FRAME < horizon) {
        seq.updatePlaybackBuffer();
        seq.set_clock(seq.clock() + CLOCK_STEP);
    }
    seq.stop();
    w.end();
    return sb;
}

static std::string live_tag(const LiveMode& m) {
    std::string kind = m.kind;
    for (char& c : kind) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    char h[3];
    std::snprintf(h, sizeof h, "%02x", m.arg & 0xFF);  // lowercase, matches Integer.toHexString.padStart
    return kind + h;
}

int main(int argc, char** argv) {
    std::string testdata = (argc > 1) ? argv[1] : "testdata";

    int total = 0, failures = 0, missing = 0;

    for (const Spec& spec : SPECS) {
        std::string ptpPath = testdata + "/" + spec.name + ".ptp";
        std::string bytes;
        if (!read_file(ptpPath, bytes)) {
            std::cerr << "[FAIL] cannot read " << ptpPath << "\n";
            ++failures;
            continue;
        }
        std::string sha = sha1_hex(bytes);
        Project p;
        try {
            p = parse_project(json::parse(bytes));
        } catch (const std::exception& e) {
            std::cerr << "[FAIL] " << spec.name << ": parse threw: " << e.what() << "\n";
            ++failures;
            continue;
        }
        normalize_and_migrate(p);

        for (int sr : SAMPLE_RATES) {
            // render
            {
                std::string tag = "render";
                std::string goldenPath = testdata + "/traces/" + spec.name + "." + std::to_string(sr) + "." + tag + ".trace";
                std::string golden;
                if (!read_file(goldenPath, golden)) {
                    std::cerr << "[MISS] " << goldenPath << " not found\n"; ++missing; continue;
                }
                std::string gen = render_trace(p, spec.renderStart, spec.renderEnd, sr, sha);
                ++total;
                std::string d = diff(golden, gen);
                if (!d.empty()) { std::cerr << "[FAIL] " << spec.name << "." << sr << ".render:\n" << d << "\n"; ++failures; }
            }
            // live modes
            for (const LiveMode& m : spec.live) {
                std::string tag = live_tag(m);
                std::string goldenPath = testdata + "/traces/" + spec.name + "." + std::to_string(sr) + "." + tag + ".trace";
                std::string golden;
                if (!read_file(goldenPath, golden)) {
                    std::cerr << "[MISS] " << goldenPath << " not found\n"; ++missing; continue;
                }
                std::string gen = live_trace(p, m, sr, sha);
                ++total;
                std::string d = diff(golden, gen);
                if (!d.empty()) { std::cerr << "[FAIL] " << spec.name << "." << sr << "." << tag << ":\n" << d << "\n"; ++failures; }
            }
        }
    }

    std::cout << "== songcore S4 spine parity (C++ vs JVM golden traces) ==\n";
    std::cout << "checked " << total << " trace(s)";
    if (missing) std::cout << " (" << missing << " golden(s) missing)";
    std::cout << "\n\n";
    std::cout << (failures == 0 ? "ALL GREEN" : "FAILURES: " + std::to_string(failures)) << "\n";
    return failures == 0 ? 0 : 1;
}
