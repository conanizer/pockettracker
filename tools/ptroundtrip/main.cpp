// ptroundtrip — songcore .ptp / .pti conformance harness (host tool, no device/NDK).
//
// Proves the C++ songcore serializer is byte-for-byte compatible with the Kotlin
// kotlinx.serialization output: it reads each golden .ptp from /testdata, runs the real load path
// (parse → normalize → migrate) and re-serializes, then compares to the ORIGINAL bytes. Any
// difference is a schema-drift bug and is reported with the exact offset + context.
//
// Also exercises the .pti (InstrumentPreset) reader/writer via a write→read→write idempotence check
// (no Kotlin-authored golden .pti exists yet; Instrument emission itself is byte-proven through the
// .ptp round-trip, since a Project embeds 128 instruments).
//
// Build (Windows, on-box MSVC):
//   cl /std:c++17 /EHsc /O2 /nologo tools/ptroundtrip/main.cpp /Fe:ptroundtrip.exe
// Run:
//   ptroundtrip.exe testdata
//
// Exit code 0 = all green, 1 = any mismatch.  Linux-port plan §4.4 (schema round-trip in CI).

#include "../../native/songcore/project_io.h"

#include <fstream>
#include <sstream>
#include <iostream>
#include <string>
#include <vector>

using songcore::Project;
using songcore::InstrumentPreset;

static bool read_file(const std::string& path, std::string& out) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    std::ostringstream ss;
    ss << f.rdbuf();
    out = ss.str();
    return true;
}

// Human-readable location + context for the first byte at which a and b differ.
static void report_diff(const std::string& a, const std::string& b) {
    size_t n = std::min(a.size(), b.size());
    size_t i = 0;
    while (i < n && a[i] == b[i]) ++i;

    // line/col of the first difference (1-based line, 1-based col)
    size_t line = 1, col = 1;
    for (size_t k = 0; k < i && k < a.size(); ++k) {
        if (a[k] == '\n') { ++line; col = 1; } else { ++col; }
    }
    std::cerr << "    first difference at byte " << i << " (line " << line << ", col " << col << ")\n";
    std::cerr << "    original size " << a.size() << ", produced size " << b.size() << "\n";

    auto slice = [](const std::string& s, size_t at) {
        size_t start = at > 30 ? at - 30 : 0;
        size_t len = std::min<size_t>(60, s.size() - start);
        std::string chunk = s.substr(start, len);
        std::string esc;
        for (char c : chunk) {
            if (c == '\n') esc += "\\n";
            else if (c == '\t') esc += "\\t";
            else esc += c;
        }
        return esc;
    };
    std::cerr << "    original : ..." << slice(a, i) << "...\n";
    std::cerr << "    produced : ..." << slice(b, i) << "...\n";
}

int main(int argc, char** argv) {
    std::string testdata = (argc > 1) ? argv[1] : "testdata";

    const std::vector<std::string> golden = {
        "g1-basics", "g2-timing", "g3-retrig", "g4-pitch", "g5-structure", "g6-params",
    };

    int failures = 0;

    std::cout << "== .ptp byte-for-byte round-trip (parse -> normalize -> migrate -> serialize) ==\n";
    for (const auto& name : golden) {
        std::string path = testdata + "/" + name + ".ptp";
        std::string original;
        if (!read_file(path, original)) {
            std::cerr << "[FAIL] " << name << ": cannot read " << path << "\n";
            ++failures;
            continue;
        }

        Project p;
        try {
            songcore::json j = songcore::json::parse(original);
            p = songcore::parse_project(j);
        } catch (const std::exception& e) {
            std::cerr << "[FAIL] " << name << ": parse threw: " << e.what() << "\n";
            ++failures;
            continue;
        }
        songcore::normalize_and_migrate(p);
        std::string produced = songcore::serialize_project(p);

        if (produced == original) {
            std::cout << "[PASS] " << name << ".ptp  (" << original.size() << " bytes)\n";
        } else {
            std::cerr << "[FAIL] " << name << ".ptp  — re-serialization differs from golden\n";
            report_diff(original, produced);
            ++failures;
        }
    }

    // ── .pti (InstrumentPreset) write -> read -> write idempotence ──────────────────────────────
    std::cout << "\n== .pti InstrumentPreset write/read idempotence ==\n";
    {
        std::string original;
        if (!read_file(testdata + "/g1-basics.ptp", original)) {
            std::cerr << "[FAIL] .pti: cannot read g1-basics.ptp for a source instrument\n";
            ++failures;
        } else {
            Project p = songcore::parse_project(songcore::json::parse(original));
            songcore::normalize_and_migrate(p);

            auto check = [&](const char* label, const InstrumentPreset& ip) {
                std::string s1 = songcore::serialize_instrument_preset(ip);
                InstrumentPreset ip2 = songcore::parse_instrument_preset(songcore::json::parse(s1));
                std::string s2 = songcore::serialize_instrument_preset(ip2);
                if (s1 == s2) {
                    std::cout << "[PASS] " << label << " (" << s1.size() << " bytes, stable across write/read/write)\n";
                } else {
                    std::cerr << "[FAIL] " << label << " — not idempotent\n";
                    report_diff(s1, s2);
                    ++failures;
                }
                return s1;
            };

            // Case 1: a loaded instrument with several non-default fields, no embedded table.
            InstrumentPreset ip1;
            ip1.instrument = p.instruments.at(1);  // pad: volume/pan/sampleFilePath set
            std::string sample = check("instrument-only preset", ip1);

            // Case 2: embedded table rows present (exercises the tableRows array branch).
            InstrumentPreset ip2;
            ip2.instrument = p.instruments.at(0);
            std::vector<songcore::TableRow> rows(2);
            rows[1].volume = 0x40;
            rows[1].transpose = 3;
            ip2.tableRows = rows;
            check("preset with embedded table", ip2);

            std::cout << "\n--- sample .pti bytes (instrument-only) ---\n" << sample << "\n--- end ---\n";
        }
    }

    std::cout << "\n" << (failures == 0 ? "ALL GREEN" : "FAILURES: " + std::to_string(failures)) << "\n";
    return failures == 0 ? 0 : 1;
}
