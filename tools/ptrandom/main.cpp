// ptrandom — songcore S7 conformance harness for the RANDOM FX (host tool, no device/NDK).
//
// The sixth and last tool on the conformance ladder, and the only one that does not compare bytes.
//
// CHA (the chance gate), RND / RNL (randomize an FX value, or a note + instrument) and ARP mode
// RANDOM draw from a clock-seeded RNG. The KOTLIN sequencer therefore does not repeat itself either,
// so there is no golden event stream for these FX to have — which is why SC-1 kept them out of
// g1..g7, and why songcore was able to ship `rng_int() → 0` for four sessions with every check green.
// That stub did not merely bias the draws: it pinned each one to the LOWEST value in its range. CHA
// passed whenever its probability nibble was nonzero, RND and RNL always emitted the bottom of their
// band, and a random arpeggio always played the root. Nothing we owned looked here.
//
// So this tool compares DISTRIBUTIONS, against the ones measured from the real Kotlin implementation
// and recorded in testdata/units/s7-random.txt (by app/src/test/.../S7RandomGoldenTest.kt, which is
// where the observables and their laws are documented). Two claims, and the split is the whole design:
//
//   n= and support=   EXACT, byte-compared against the golden. `n` is how many draws a render makes
//                     (deterministic — random FX change which value comes out, never how many are
//                     drawn), and `support` is the set of values a draw can produce. Every bug class
//                     that matters moves one of them: an off-by-one, a closed interval where Kotlin's
//                     is half-open, a stub stuck at the low end. Caught with certainty, no statistics.
//   expect=           STATISTICAL, and checked by this tool against its OWN histogram — never against
//                     Kotlin's counts, because two random samples never agree exactly and demanding
//                     that they do is how a test becomes flaky. Margins are 6-sigma-plus: this catches
//                     a gross error (a mode drawn 1/2 of the time instead of 1/3), not a modulo bias
//                     no achievable sample size could resolve anyway.
//
// Unlike the JVM side — which cannot seed kotlin.random.Random.Default and so really does draw a
// fresh sample every run — this tool SEEDS the sequencer per render. It is therefore bit-deterministic
// on every platform and in CI: it can fail, but it cannot flake.
//
// Build + run via the tools/ CMake project — this is the `s7-random` ctest, run by CI on every push:
//   cmake -S tools -B tools/build -DCMAKE_BUILD_TYPE=Release
//   cmake --build tools/build --config Release
//   ctest --test-dir tools/build -R s7-random --output-on-failure -C Release
// Exit 0 = all green, 1 = any mismatch. Linux-port plan §4.3 (S7 — the RNG, the last blocker).

#include "../../native/songcore/model.h"
#include "../../native/songcore/project_io.h"
#include "../../native/songcore/router.h"
#include "../../native/songcore/trace_writer.h"
#include "../../native/songcore/scheduler.h"
#include "../../native/songcore/timing.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

using namespace songcore;

// ─── Constants — these MUST equal S7RandomGoldenTest's, or the golden's n= counts cannot match ────
static const int RENDERS        = 300;   // S7RandomGoldenTest.RENDERS
static const int PHRASE_REPEATS = 8;     // g8's chains are eight rows of the same phrase
static const int SAMPLE_RATE    = 44100; // S7RandomGoldenTest.SAMPLE_RATE

// The seed stream. Fixed, so a failure here is reproducible and CI cannot flake; per-render, so the
// 300 renders are 300 independent samples rather than one repeated 300 times.
static uint64_t seed_for(int render) { return 0x5EED0000ULL + static_cast<uint64_t>(render); }

// ─── The law a draw is expected to follow (mirrors S7RandomGoldenTest.Law) ────────────────────────
struct Law {
    bool uniform = true;
    int value = 0;   // Bernoulli: the value whose probability is claimed
    int k = 0;       // Bernoulli: numerator over 15 (a CHA roll is nextInt(15) → 0..14)

    static Law Uniform() { return Law{true, 0, 0}; }
    static Law Bernoulli(int v, int k) { return Law{false, v, k}; }

    std::string text() const {
        if (uniform) return "uniform";
        return "p(" + std::to_string(value) + ")=" + std::to_string(k) + "/15";
    }
};

// ─── Histogram (mirrors S7RandomGoldenTest.Hist) ──────────────────────────────────────────────────
struct Hist {
    std::map<int, int> counts;   // std::map keeps the support sorted, which is what support_text wants
    int n = 0;

    void add(int v) { counts[v]++; ++n; }
    int count(int v) const { auto it = counts.find(v); return it == counts.end() ? 0 : it->second; }

    /// Sorted support, runs of 3+ consecutive values collapsed to `a..b`. Must match Hist.supportText().
    std::string support_text() const {
        if (counts.empty()) return "-";
        std::vector<int> vs;
        vs.reserve(counts.size());
        for (const auto& kv : counts) vs.push_back(kv.first);

        std::string out;
        size_t i = 0;
        while (i < vs.size()) {
            size_t j = i;
            while (j + 1 < vs.size() && vs[j + 1] == vs[j] + 1) ++j;
            if (!out.empty()) out += ',';
            if (j - i >= 2) {
                out += std::to_string(vs[i]) + ".." + std::to_string(vs[j]);
            } else {
                for (size_t k = i; k <= j; ++k) {
                    if (k > i) out += ',';
                    out += std::to_string(vs[k]);
                }
            }
            i = j + 1;
        }
        return out;
    }

    std::string line(const std::string& name, const Law& law) const {
        return "OBS " + name + " n=" + std::to_string(n) +
               " support=" + support_text() + " expect=" + law.text();
    }

    /// The shape check. Empty string = consistent with `law`; otherwise a report. `detail` receives a
    /// one-line summary of the statistic for the run log, pass or fail.
    std::string shape_problem(const std::string& name, const Law& law, std::string& detail) const {
        char buf[256];
        if (law.uniform) {
            double expected = static_cast<double>(n) / static_cast<double>(counts.size());
            double chi2 = 0.0;
            for (const auto& kv : counts) {
                double d = kv.second - expected;
                chi2 += d * d / expected;
            }
            int df = static_cast<int>(counts.size()) - 1;
            double limit = df + 6.0 * std::sqrt(2.0 * df) + 10.0;
            std::snprintf(buf, sizeof buf, "chi2=%.1f / %.1f (df=%d)", chi2, limit, df);
            detail = buf;
            if (chi2 > limit) {
                return name + ": not uniform over " + std::to_string(counts.size()) +
                       " values — " + detail + ", n=" + std::to_string(n);
            }
        } else {
            double p = law.k / 15.0;
            double rate = static_cast<double>(count(law.value)) / static_cast<double>(n);
            // p(1-p) is 0 at both ends, so the deterministic gates (k=0, k=15) lean on the flat slack
            // — and on the support, which pins them exactly.
            double tol = 6.0 * std::sqrt(p * (1.0 - p) / n) + 0.01;
            std::snprintf(buf, sizeof buf, "P(%d)=%.4f / %.4f ±%.4f", law.value, rate, p, tol);
            detail = buf;
            if (std::fabs(rate - p) > tol) {
                return name + ": " + detail + " — outside tolerance, n=" + std::to_string(n);
            }
        }
        return "";
    }
};

// ─── The observable table — mirrors S7RandomGoldenTest.LAWS, in the golden's line order ───────────
struct Obs { std::string name; Law law; };

static const std::vector<Obs> OBSERVABLES = {
    {"rnd-pit",       Law::Uniform()},          // RND 37 → recalled PIT byte 0x30..0x7F → pit 48..127
    {"rnl-note",      Law::Uniform()},          // RNL 53 → C-4 ± 5 semitones → MIDI 55..65
    {"rnl-inst",      Law::Uniform()},          // RNL 53 → instrument 4 ± 3 → 1..7
    {"rnl-left-pit",  Law::Uniform()},          // RNL 24 → FX1's PIT value 0x20..0x4F → pit 32..79
    {"arp-random",    Law::Uniform()},          // ARP A37 under ARC mode 3 → offset ∈ {0, 3, 7}
    {"cha-gate-p0",   Law::Bernoulli(1, 0)},    // CHA 00 → the note NEVER fires
    {"cha-gate-p4",   Law::Bernoulli(1, 4)},
    {"cha-gate-p8",   Law::Bernoulli(1, 8)},
    {"cha-gate-p12",  Law::Bernoulli(1, 12)},
    {"cha-gate-p15",  Law::Bernoulli(1, 15)},   // CHA F0 → the note ALWAYS fires (roll ≤ 14 < 15)
    {"cha-gate-none", Law::Bernoulli(1, 15)},   // control: no CHA on the step → always fires
    {"cha-clear-p8",  Law::Bernoulli(5, 8)},    // CHA 82 → a lost roll clears FX2, so pit reads 0 not 5
};

// Which phrase-2 step carries which CHA probability. A gated note emits NOTHING, so the gate's trials
// come from the step grid, not from the trace.
static const std::vector<std::pair<std::string, std::vector<int>>> CHA_GATE_STEPS = {
    {"cha-gate-p0",   {0}},
    {"cha-gate-p4",   {2}},
    {"cha-gate-p8",   {4, 6}},
    {"cha-gate-p12",  {8}},
    {"cha-gate-p15",  {10}},
    {"cha-gate-none", {12}},
};

// ─── Trace parsing (the JVM test parses the same lines with the same rules) ───────────────────────
struct NoteOn { int64_t frame; int track; int instrument; int note; int vel; int pit; int arp; };

static std::vector<NoteOn> parse_note_ons(const std::string& trace) {
    std::vector<NoteOn> out;
    std::istringstream in(trace);
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty() || line[0] < '0' || line[0] > '9') continue;   // headers, T PLAY / T STOP

        std::vector<std::string> tok;
        { std::istringstream ls(line); std::string t; while (ls >> t) tok.push_back(t); }
        if (tok.size() < 4 || tok[3] != "90") continue;                 // NoteOn only

        std::map<std::string, std::string> f;
        for (size_t i = 4; i < tok.size(); ++i) {
            size_t eq = tok[i].find('=');
            if (eq != std::string::npos) f[tok[i].substr(0, eq)] = tok[i].substr(eq + 1);
        }
        NoteOn n{};
        n.frame      = std::strtoll(tok[0].c_str(), nullptr, 10);
        n.track      = std::atoi(tok[1].c_str());
        n.instrument = (tok[2] == "-1") ? -1 : static_cast<int>(std::strtol(tok[2].c_str(), nullptr, 16));
        n.note = std::atoi(f["note"].c_str());
        n.vel  = std::atoi(f["vel"].c_str());
        n.pit  = std::atoi(f["pit"].c_str());
        n.arp  = std::atoi(f["arp"].c_str());
        out.push_back(n);
    }
    return out;
}

/// Fold one render's NoteOns into the histograms. Mirrors S7RandomGoldenTest.measure() exactly.
static bool measure(const std::vector<NoteOn>& notes, int64_t framesPerStep,
                    std::map<std::string, Hist>& hists, std::string& err) {
    // With no groove and no LAT anywhere in g8, every scheduled note lands on a multiple of
    // framesPerStep. Arpeggio retriggers do not — they are the only notes that fall between steps,
    // and they are also the only ones carrying vel=-1, which is what identifies them.
    std::vector<bool> chaSeen(static_cast<size_t>(16 * PHRASE_REPEATS), false);

    for (const NoteOn& n : notes) {
        bool isArpRetrig = (n.vel == -1);
        int globalStep = static_cast<int>(n.frame / framesPerStep);
        int step = globalStep % 16;

        if (!isArpRetrig && n.frame % framesPerStep != 0) {
            err = "track " + std::to_string(n.track) + " note at frame " + std::to_string(n.frame) +
                  " is not on a step boundary — a groove or LAT crept into g8 and the step keying is "
                  "no longer sound";
            return false;
        }

        switch (n.track) {
            // step 0 carries the PIT that SEEDS the column, not a RND draw — exclude it.
            case 0: if (step >= 2 && step % 2 == 0) hists["rnd-pit"].add(n.pit); break;
            case 1: if (step % 2 == 0) { hists["rnl-note"].add(n.note); hists["rnl-inst"].add(n.instrument); } break;
            case 2: if (globalStep >= 0 && globalStep < static_cast<int>(chaSeen.size())) chaSeen[globalStep] = true; break;
            case 3: if (step % 2 == 0) hists["cha-clear-p8"].add(n.pit); break;
            case 4: if (isArpRetrig) hists["arp-random"].add(n.arp); break;
            case 5: if (step % 2 == 0) hists["rnl-left-pit"].add(n.pit); break;
            default: break;
        }
    }

    // The CHA gate: every (repeat, step) pair is one Bernoulli trial — 1 if the note survived, 0 if not.
    for (const auto& entry : CHA_GATE_STEPS) {
        Hist& h = hists[entry.first];
        for (int repeat = 0; repeat < PHRASE_REPEATS; ++repeat)
            for (int s : entry.second)
                h.add(chaSeen[static_cast<size_t>(repeat * 16 + s)] ? 1 : 0);
    }
    return true;
}

// ─── file IO ──────────────────────────────────────────────────────────────────────────────────────
static bool read_file(const std::string& path, std::string& out) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    std::ostringstream ss;
    ss << f.rdbuf();
    out = ss.str();
    return true;
}

int main(int argc, char** argv) {
    std::string testdata = (argc > 1) ? argv[1] : "testdata";

    // ── the golden, measured from the real Kotlin sequencer ──
    std::string goldenText;
    std::string goldenPath = testdata + "/units/s7-random.txt";
    if (!read_file(goldenPath, goldenText)) {
        std::cerr << "[FAIL] cannot read " << goldenPath
                  << " — run S7RandomGoldenTest (./gradlew :app:testDebugUnitTest) to generate it\n";
        return 1;
    }
    std::map<std::string, std::string> goldenLines;   // observable name → its full OBS line
    {
        std::istringstream in(goldenText);
        std::string line;
        while (std::getline(in, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (line.rfind("OBS ", 0) != 0) continue;
            size_t sp = line.find(' ', 4);
            if (sp == std::string::npos) continue;
            goldenLines[line.substr(4, sp - 4)] = line;
        }
    }

    // ── the project, the same bytes the JVM measured ──
    std::string bytes;
    std::string ptpPath = testdata + "/g8-random.ptp";
    if (!read_file(ptpPath, bytes)) {
        std::cerr << "[FAIL] cannot read " << ptpPath << "\n";
        return 1;
    }
    Project project;
    try {
        project = parse_project(json::parse(bytes));
    } catch (const std::exception& e) {
        std::cerr << "[FAIL] g8-random: parse threw: " << e.what() << "\n";
        return 1;
    }
    normalize_and_migrate(project);

    // ── measure C++ songcore ──
    const int64_t fps = frames_per_step(project.tempo, SAMPLE_RATE);
    std::map<std::string, Hist> hists;
    for (const Obs& o : OBSERVABLES) hists[o.name];   // fix the key set even if a draw never happens

    for (int r = 0; r < RENDERS; ++r) {
        std::string trace;
        TraceWriter w;
        w.begin(&trace, "-");           // the sha is irrelevant here: nothing compares trace headers
        MidiRouter router(&w);
        Sequencer seq(router, project, SAMPLE_RATE);
        seq.seed_rng(seed_for(r));      // reproducible: this tool can fail, but it cannot flake
        seq.scheduleSongRowRange(0, 0);
        w.end();

        std::string err;
        if (!measure(parse_note_ons(trace), fps, hists, err)) {
            std::cerr << "[FAIL] render " << r << ": " << err << "\n";
            return 1;
        }
    }

    // ── the control, which validates the denominator every CHA trial count is derived from ──
    int control = hists["cha-gate-none"].count(1);
    if (control != RENDERS * PHRASE_REPEATS) {
        std::cerr << "[FAIL] the no-CHA control step fired " << control << " times, expected "
                  << RENDERS * PHRASE_REPEATS << " — the render is not walking all "
                  << PHRASE_REPEATS << " chain rows\n";
        return 1;
    }

    // ── compare ──
    std::cout << "== songcore S7 random-FX parity (C++ vs the Kotlin-measured distributions) ==\n\n";

    int failures = 0;
    for (const Obs& o : OBSERVABLES) {
        const Hist& h = hists[o.name];
        std::string mine = h.line(o.name, o.law);

        // (1) support + draw count — exact, against Kotlin's measurement.
        auto it = goldenLines.find(o.name);
        bool exactOk = (it != goldenLines.end() && it->second == mine);

        // (2) shape — statistical, against the law, on this engine's own draws.
        std::string detail;
        std::string shape = h.shape_problem(o.name, o.law, detail);

        char row[320];
        std::snprintf(row, sizeof row, "  %-14s n=%-6d support=%-12s %-14s %-28s %s\n",
                      o.name.c_str(), h.n, h.support_text().c_str(), o.law.text().c_str(),
                      detail.c_str(), (exactOk && shape.empty()) ? "OK" : "**");
        std::cout << row;

        if (!exactOk) {
            ++failures;
            std::cerr << "\n[FAIL] " << o.name << ": support/draw-count differs from the Kotlin golden\n"
                      << "    kotlin: " << (it == goldenLines.end() ? "(no such observable)" : it->second) << "\n"
                      << "    c++   : " << mine << "\n";
        }
        if (!shape.empty()) {
            ++failures;
            std::cerr << "\n[FAIL] " << shape << "\n";
        }
    }

    std::cout << "\n" << RENDERS << " renders of g8-random, " << OBSERVABLES.size() << " observables\n";
    std::cout << (failures == 0 ? "ALL GREEN" : "FAILURES: " + std::to_string(failures)) << "\n";
    return failures == 0 ? 0 : 1;
}
