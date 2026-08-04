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
// Build + run via the tools/ CMake project — this is the `s2-project-io` ctest, run by CI on every
// push (see tools/CMakeLists.txt and tools/ptroundtrip/README.md):
//   cmake -S tools -B tools/build -DCMAKE_BUILD_TYPE=Release
//   cmake --build tools/build --config Release
//   ctest --test-dir tools/build -R s2-project-io --output-on-failure -C Release
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

    // Every golden project. g7-audio carries the EQ presets, mod slots and filter settings no other
    // project serializes; g8-random carries the CHA/RND/RNL effect codes, and g9-automation the
    // AUS/AUF pair — an effect code the writer has never been asked to put on paper before.
    const std::vector<std::string> golden = {
        "g1-basics", "g2-timing", "g3-retrig", "g4-pitch", "g5-structure", "g6-params",
        "g7-audio", "g8-random", "g9-automation",
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

    // ── EXTERNAL / MIDI fields carry their VALUES through a full round trip ──────────────────────
    //
    // ⚠️ Deliberately NOT another idempotence check, and that is the whole point of writing it out.
    // A field that is dropped on parse AND never emitted is perfectly idempotent — write→read→write
    // is stable because both writes emit nothing. The check that catches a half-wired field is a
    // VALUE check: set it, serialise, parse, and read it back. The eight goldens above prove the new
    // fields cost no bytes when unused; this proves they exist when used.
    std::cout << "\n== EXTERNAL instrument + project MIDI fields (value round trip) ==\n";
    {
        Project p = songcore::make_default_project();
        songcore::Instrument& ins = p.instruments.at(3);
        ins.instrumentType = songcore::InstrumentType::EXTERNAL;
        ins.midiChannel = 9;
        ins.midiBank    = 2;
        ins.midiProgram = 41;
        ins.midiLen     = 6;
        ins.midiCC[0] = songcore::MidiCcSlot{74, 100};
        ins.midiCC[2] = songcore::MidiCcSlot{7, 0};
        p.midiSyncOut = 3;
        p.midiSendProgramChange = false;
        p.midiInputChannels[0] = 1;
        p.midiInputChannels[7] = 15;

        const std::string blob = songcore::serialize_project(p);
        Project q = songcore::parse_project(songcore::json::parse(blob));
        songcore::normalize_and_migrate(q);
        const songcore::Instrument& r = q.instruments.at(3);

        struct Case { const char* what; long long got, want; };
        const Case cases[] = {
            {"instrumentType == EXTERNAL", (long long)(r.instrumentType == songcore::InstrumentType::EXTERNAL), 1},
            {"midiChannel",  r.midiChannel,  9},
            {"midiBank",     r.midiBank,     2},
            {"midiProgram",  r.midiProgram,  41},
            {"midiLen",      r.midiLen,      6},
            {"midiCC[0].cc",    r.midiCC.at(0).cc,    74},
            {"midiCC[0].value", r.midiCC.at(0).value, 100},
            {"midiCC[1].cc",    r.midiCC.at(1).cc,    -1},   // untouched slot stays empty
            {"midiCC[2].cc",    r.midiCC.at(2).cc,    7},
            {"midiCC[2].value", r.midiCC.at(2).value, 0},    // 0 is a VALUE, not "unused"
            {"midiSyncOut",  q.midiSyncOut,  3},
            {"midiSendProgramChange", (long long)q.midiSendProgramChange, 0},
            {"midiInputChannels[0]", q.midiInputChannels.at(0), 1},
            {"midiInputChannels[3]", q.midiInputChannels.at(3), -1},
            {"midiInputChannels[7]", q.midiInputChannels.at(7), 15},
        };
        for (const Case& c : cases) {
            // The NUMBER beside the verdict, always: a bare PASS/FAIL cannot tell a working check
            // from one comparing two things that are both wrong.
            if (c.got == c.want) {
                std::cout << "[PASS] " << c.what << " = " << c.got << "\n";
            } else {
                std::cerr << "[FAIL] " << c.what << " = " << c.got << ", expected " << c.want << "\n";
                ++failures;
            }
        }
        // …and the same instrument through the .pti path, which shares emit_instrument.
        InstrumentPreset ip;
        ip.instrument = ins;
        const songcore::InstrumentPreset back =
            songcore::parse_instrument_preset(songcore::json::parse(songcore::serialize_instrument_preset(ip)));
        if (back.instrument.midiChannel == 9 && back.instrument.midiCC.at(0).cc == 74 &&
            back.instrument.instrumentType == songcore::InstrumentType::EXTERNAL) {
            std::cout << "[PASS] .pti carries the EXTERNAL fields (chan=" << back.instrument.midiChannel
                      << " ccA=" << back.instrument.midiCC.at(0).cc << ")\n";
        } else {
            std::cerr << "[FAIL] .pti lost the EXTERNAL fields (chan=" << back.instrument.midiChannel
                      << " ccA=" << back.instrument.midiCC.at(0).cc << ")\n";
            ++failures;
        }
    }

    std::cout << "\n" << (failures == 0 ? "ALL GREEN" : "FAILURES: " + std::to_string(failures)) << "\n";
    return failures == 0 ? 0 : 1;
}
