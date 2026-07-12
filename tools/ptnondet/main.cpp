// ptnondet — is this project's render a pure function of the project? (host tool, no device/NDK)
//
// Two jobs, one answer.
//
// AS A DIAGNOSTIC (`ptnondet some.ptp`): lists every clock-seeded element in a project. Run it before
// ANY KT-vs-C++ render comparison. If it reports anything, the two renders cannot match — the engines
// draw from different generators — and neither can two renders from the SAME engine. This is not a
// hypothetical: the first S7 device A/B came back with two differing WAVs and a `CHA 40` on one note,
// and that one effect explained every differing sample (docs/internal/songcore-s7-device-test.md §E).
//
// AS A TEST (`ptnondet <testdata>`, the `s7-determinism` ctest): asserts that each golden project is on
// the side of the line it is supposed to be on — g1..g7 deterministic, g8-random not. This guards a
// dependency that is otherwise invisible: **ptrender asserts that two renders of g7-audio are
// byte-identical**, which only holds while g7 stays free of anything clock-seeded. Add a CHA to g7 and
// ptrender goes red with "the two renders differ" — true, useless, and hours from the cause. This test
// fails first, and names it.
//
// What counts as clock-seeded, and why each one:
//   * CHA / RND / RNL / ARC-mode-3 — the random phrase FX. rng.h; seeded from the platform, per S7.
//   * oscShape >= 8 (RND / DRNK LFO shapes) — drawn from the engine's noteSeedEntropy, which is
//     re-seeded from the wall clock at every render ON PURPOSE. Per-render variation is the feature.
//   * DUST on the master bus — a random-walk drift.
//
// Build + run via the tools/ CMake project:
//   ctest --test-dir tools/build -R s7-determinism --output-on-failure -C Release
//   tools/build/ptnondet path/to/project.ptp        # the diagnostic

#include "../../native/songcore/model.h"
#include "../../native/songcore/project_io.h"
#include "../../native/songcore/effects.h"

#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

using namespace songcore;

struct Finding { std::string what; };

// Every clock-seeded element in the project, in report order.
static std::vector<Finding> scan(const Project& p) {
    std::vector<Finding> out;
    char buf[256];

    for (size_t ph = 0; ph < p.phrases.size(); ++ph) {
        const Phrase& phrase = p.phrases[ph];
        for (size_t st = 0; st < phrase.steps.size(); ++st) {
            const PhraseStep& s = phrase.steps[st];
            int types[3]  = {s.fx1Type,  s.fx2Type,  s.fx3Type};
            int values[3] = {s.fx1Value, s.fx2Value, s.fx3Value};
            for (int i = 0; i < 3; ++i) {
                const char* why = nullptr;
                if (types[i] == FX_CHA)      why = "chance gate";
                else if (types[i] == FX_RND) why = "randomizes the column's previous FX";
                else if (types[i] == FX_RNL) why = "randomizes the column to the left / note+instrument";
                else if (types[i] == FX_ARC && ((values[i] >> 4) & 0x0F) == 3)
                                             why = "arpeggio mode 3 = RANDOM";
                if (why) {
                    std::snprintf(buf, sizeof buf,
                                  "RANDOM FX   phrase %02zX step %02zX FX%d = %s %02X  (%s)",
                                  ph, st, i + 1, effect_name(types[i]).c_str(), values[i], why);
                    out.push_back({buf});
                }
            }
        }
    }

    for (size_t in = 0; in < p.instruments.size(); ++in) {
        const Instrument& ins = p.instruments[in];
        for (size_t m = 0; m < ins.modSlots.size(); ++m) {
            if (ins.modSlots[m].oscShape >= 8) {
                std::snprintf(buf, sizeof buf,
                              "RANDOM LFO  instrument %02zX mod slot %zu: oscShape=%d (%s) — drawn from "
                              "noteSeedEntropy, re-seeded from the clock every render",
                              in, m, ins.modSlots[m].oscShape,
                              ins.modSlots[m].oscShape == 8 ? "RND" : "DRNK");
                out.push_back({buf});
            }
        }
    }

    if (p.masterBusFx == 1 && p.dustDepth > 0) {
        std::snprintf(buf, sizeof buf,
                      "RANDOM DSP  masterBusFx = 1 (DUST), dustDepth = %02X — a random-walk drift",
                      p.dustDepth);
        out.push_back({buf});
    }
    return out;
}

static bool load(const std::string& path, Project& p) {
    std::ifstream f(path, std::ios::binary);
    if (!f) { std::fprintf(stderr, "cannot open %s\n", path.c_str()); return false; }
    std::ostringstream ss; ss << f.rdbuf();
    try { p = parse_project(json::parse(ss.str())); }
    catch (const std::exception& e) {
        std::fprintf(stderr, "%s: parse threw: %s\n", path.c_str(), e.what());
        return false;
    }
    normalize_and_migrate(p);
    return true;
}

static bool ends_with(const std::string& s, const char* suf) {
    std::string t(suf);
    return s.size() >= t.size() && s.compare(s.size() - t.size(), t.size(), t) == 0;
}

// ─── the golden projects and the side of the line each belongs on ─────────────────────────────────
struct Expect { const char* name; bool deterministic; const char* why; };
static const std::vector<Expect> GOLDENS = {
    {"g1-basics",    true,  "event golden"},
    {"g2-timing",    true,  "event golden"},
    {"g3-retrig",    true,  "event golden"},
    {"g4-pitch",     true,  "event golden"},
    {"g5-structure", true,  "event golden"},
    {"g6-params",    true,  "event golden"},
    {"g7-audio",     true,  "THE AUDIO GOLDEN — ptrender asserts two renders of it are BYTE-IDENTICAL"},
    {"g8-random",    false, "the random golden — ptrandom measures its distributions"},
};

int main(int argc, char** argv) {
    std::string arg = (argc > 1) ? argv[1] : "testdata";

    // ── diagnostic mode: one .ptp, tell me everything ──
    if (ends_with(arg, ".ptp")) {
        Project p;
        if (!load(arg, p)) return 2;
        std::printf("project \"%s\"  tempo=%d\n\n", p.name.c_str(), p.tempo);
        auto found = scan(p);
        for (const Finding& f : found) std::printf("  %s\n", f.what.c_str());
        if (found.empty()) {
            std::printf("DETERMINISTIC — nothing here is clock-seeded. Two renders of this project must be\n"
                        "byte-identical, and so must a KT render and a C++ one. If they are not, that is a\n"
                        "REAL sequencer divergence, and a release blocker.\n");
            return 0;
        }
        std::printf("\nNON-DETERMINISTIC — %zu source(s) of randomness above.\n"
                    "This project CANNOT be A/B'd between engines: KT and C++ draw from different generators,\n"
                    "and even one engine rendered twice will differ. A byte-identical render here would mean\n"
                    "the randomness is FAKE.\n", found.size());
        return 1;
    }

    // ── test mode: every golden must be on its expected side ──
    std::printf("== golden-project determinism (which projects may be byte-compared) ==\n\n");
    int failures = 0;
    for (const Expect& e : GOLDENS) {
        Project p;
        if (!load(arg + "/" + e.name + ".ptp", p)) { ++failures; continue; }
        auto found = scan(p);
        bool deterministic = found.empty();
        bool ok = (deterministic == e.deterministic);
        std::printf("  %-13s %-20s %s\n", e.name,
                    deterministic ? "DETERMINISTIC" : "NON-DETERMINISTIC", ok ? "OK" : "**");
        if (!ok) {
            ++failures;
            std::fprintf(stderr, "\n[FAIL] %s must be %s — %s\n", e.name,
                         e.deterministic ? "DETERMINISTIC" : "NON-DETERMINISTIC", e.why);
            for (const Finding& f : found) std::fprintf(stderr, "         %s\n", f.what.c_str());
            if (e.deterministic) {
                std::fprintf(stderr,
                    "       Anything clock-seeded in this project makes its render unreproducible. For\n"
                    "       g7-audio that breaks ptrender's byte-exact determinism check, which would then\n"
                    "       fail as \"the two renders differ\" — true, useless, and hours from the cause.\n");
            } else {
                std::fprintf(stderr,
                    "       This project exists to BE random; with the randomness gone, ptrandom would be\n"
                    "       measuring nothing and would pass vacuously.\n");
            }
        }
    }
    std::printf("\n%s\n", failures == 0 ? "ALL GREEN" : ("FAILURES: " + std::to_string(failures)).c_str());
    return failures == 0 ? 0 : 1;
}
