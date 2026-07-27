// ptaac — convergence media-unification harness for the mp4/AAC decoder
// (native/audio-decoders.cpp decodeMp4File → vendored minimp4 demux + FAAD2 decode).
//
// The media-unification step vendors minimp4 + FAAD2 so AAC containers (.m4a/.mp4/.m4b/.mov/.3gp)
// load in place as samples, exactly as MP3/OGG do, and Android's MediaCodec can leave. A decoder
// vendored but never proven is dead code, so this exercises decodeMp4File on real fixtures — the same
// discipline ptdecode applies to the PNG reader.
//
// ── The independent invariant ──────────────────────────────────────────────────────────────────
// AAC is LOSSY, so there is no byte-exact golden to compare against — and that is the point of this
// harness's design. The fixtures under testdata/audio/ were encoded by ffmpeg (an encoder wholly
// independent of FAAD2) from PURE SINE TONES of KNOWN frequency:
//
//   * tone_stereo.m4a   L = 440 Hz, R = 660 Hz, 44100 Hz, 2 s, AAC-LC — channels, channel ORDER, rate
//   * tone_mono.m4a     440 Hz, mono, 44100 Hz, 2 s, AAC-LC              — mono → R empty
//
// The EXPECTED values here are those tone frequencies, hardcoded — NOT read back from a golden. A
// lossy codec cannot regenerate its way to green: the check is that the decoded audio's energy sits
// at the frequency the tone was BORN with, in the CHANNEL it was born in. That catches the failures
// that matter — a dead decoder (silence), a channel swap (L/R energy crossed), a wrong sample rate (a
// shifted detected pitch), and a broken normalization (samples outside [-1,1]). DELETING a fixture is
// a hard error (exit 2), never a vacuous pass. Regenerate via testdata/audio/make-audio-fixtures.sh
// and testdata/README.md §6.
//
// Links the engine static lib (decodeMp4File lives in audio-decoders.cpp, which pulls faad2 + minimp4)
// exactly as ptrender links it.
//
//   cmake --build tools/build --target ptaac
//   ctest --test-dir tools/build -R media-aac --output-on-failure
// Exit 0 = all green, 1 = a wrong/failed decode, 2 = a usage or missing/unreadable-fixture error.

#include "audio-decoders.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

namespace {

int g_checks = 0;
int g_fails  = 0;

void check(bool cond, const std::string& what) {
    ++g_checks;
    if (!cond) { ++g_fails; std::cerr << "FAIL: " << what << "\n"; }
    else       std::cout << "ok    " << what << "\n";
}

// A harness error (fixture missing/unreadable, bad usage) — distinct from a decode failure, which is a
// real test FAIL. Exit 2 so that deleting the fixtures can never masquerade as a pass.
[[noreturn]] void die(const std::string& msg) {
    std::cerr << "ptaac: " << msg << "\n";
    std::exit(2);
}

// Fail hard (exit 2) if a fixture is not present/readable — a decode assertion must never be able to
// pass because its input vanished.
void require_fixture(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) die("cannot open fixture: " + path);
    f.seekg(0, std::ios::end);
    if (f.tellg() <= 0) die("empty or unseekable fixture: " + path);
}

// Goertzel: energy of a real signal at frequency `f` (Hz), measured over a mid-signal window so the
// leading priming-silence and any trailing padding do not dilute it. Returned as mean power per sample
// so the two channels/tones compare on equal footing regardless of window length.
double tone_energy(const std::vector<float>& x, double f, int rate) {
    // Analyse a window clear of the encoder-priming silence at the very start.
    const size_t start = 4096;
    if (x.size() <= start + 1024) return 0.0;
    const size_t n = x.size() - start;
    const double w  = 2.0 * M_PI * f / rate;
    const double cw = std::cos(w);
    const double coeff = 2.0 * cw;
    double s0 = 0.0, s1 = 0.0, s2 = 0.0;
    for (size_t i = 0; i < n; ++i) {
        s0 = x[start + i] + coeff * s1 - s2;
        s2 = s1;
        s1 = s0;
    }
    const double power = s1 * s1 + s2 * s2 - coeff * s1 * s2;   // |X(f)|^2
    return power / static_cast<double>(n) / static_cast<double>(n) * 2.0;  // mean power per sample
}

double peak_abs(const std::vector<float>& x) {
    double m = 0.0;
    for (float v : x) m = std::max(m, std::fabs((double)v));
    return m;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::cerr << "usage: ptaac <audio-fixture-dir>\n";
        return 2;
    }
    const std::string dir = argv[1];
    const std::string stereo = dir + "/tone_stereo.m4a";
    const std::string mono   = dir + "/tone_mono.m4a";
    require_fixture(stereo);
    require_fixture(mono);

    const int    RATE  = 44100;
    const double F_LEFT = 440.0, F_RIGHT = 660.0;
    const size_t SRC_FRAMES = 2 * RATE;   // 2 s source; decoded is this + priming/padding, never less

    // ── tone_stereo.m4a : L=440 R=660, the core proof ────────────────────────────────────────────
    {
        std::vector<float> L, R;
        int rate = 0;
        const bool ok = ptdec::decodeMp4File(stereo.c_str(), L, R, rate);
        check(ok, "stereo: decodes");
        check(rate == RATE, "stereo: sample rate 44100 (got " + std::to_string(rate) + ")");
        check(!R.empty(), "stereo: has a right channel");
        check(R.size() == L.size(), "stereo: L and R same length");
        // Decoded length = source + encoder priming/padding, so >= source and within a generous band.
        check(L.size() >= SRC_FRAMES && L.size() <= SRC_FRAMES + 8192,
              "stereo: frame count ~source+priming (got " + std::to_string(L.size()) + ")");
        // Normalized output: a full-scale sine survives near 1.0. A decoder emitting 16-bit-scale
        // floats (the FLOAT_SCALE bug) would read ~32000 here.
        check(peak_abs(L) <= 1.05 && peak_abs(L) > 0.1, "stereo: L normalized to [-1,1], not silent");
        check(peak_abs(R) <= 1.05 && peak_abs(R) > 0.1, "stereo: R normalized to [-1,1], not silent");

        if (ok && !L.empty() && !R.empty()) {
            const double L440 = tone_energy(L, F_LEFT,  RATE), L660 = tone_energy(L, F_RIGHT, RATE);
            const double R440 = tone_energy(R, F_LEFT,  RATE), R660 = tone_energy(R, F_RIGHT, RATE);
            std::printf("      energies  L@440=%.4g L@660=%.4g  R@440=%.4g R@660=%.4g\n",
                        L440, L660, R440, R660);
            // Channel ORDER + separation: L is the 440 tone, R is the 660 tone, each strongly.
            check(L440 > L660 * 8.0, "stereo: left channel is the 440 Hz tone");
            check(R660 > R440 * 8.0, "stereo: right channel is the 660 Hz tone");
        }
    }

    // ── tone_mono.m4a : mono 440, R must be empty ────────────────────────────────────────────────
    {
        std::vector<float> L, R;
        int rate = 0;
        const bool ok = ptdec::decodeMp4File(mono.c_str(), L, R, rate);
        check(ok, "mono: decodes");
        check(rate == RATE, "mono: sample rate 44100 (got " + std::to_string(rate) + ")");
        check(R.empty(), "mono: right channel empty (mono convention)");
        check(peak_abs(L) <= 1.05 && peak_abs(L) > 0.1, "mono: normalized, not silent");
        if (ok && !L.empty()) {
            const double L440 = tone_energy(L, F_LEFT, RATE), L660 = tone_energy(L, F_RIGHT, RATE);
            std::printf("      energies  L@440=%.4g L@660=%.4g\n", L440, L660);
            check(L440 > L660 * 8.0, "mono: is the 440 Hz tone");
        }
    }

    // ── Negative controls: a decode that SHOULD fail must return false, not crash, not "succeed". ──
    //    A test whose pass is the absence of a failure cannot tell a working decoder from a broken
    //    instrument — so prove the failures fail.
    {
        std::vector<float> L, R;
        int rate = 0;

        L.clear(); R.clear(); rate = 0;
        check(!ptdec::decodeMp4File((dir + "/does_not_exist.m4a").c_str(), L, R, rate),
              "neg: missing file rejected");

        // A truncated real container (first 512 bytes) must fail cleanly, not decode garbage.
        {
            std::ifstream in(stereo, std::ios::binary);
            std::vector<char> whole((std::istreambuf_iterator<char>(in)), {});
            const std::string cut = dir + "/_trunc.m4a";
            std::ofstream out(cut, std::ios::binary);
            out.write(whole.data(), std::min<std::streamsize>(512, (std::streamsize)whole.size()));
            out.close();
            L.clear(); R.clear(); rate = 0;
            const bool decoded = ptdec::decodeMp4File(cut.c_str(), L, R, rate);
            std::remove(cut.c_str());
            check(!decoded, "neg: truncated container rejected");
        }

        // Non-container bytes (not ISO-BMFF at all) must fail, not be mistaken for audio.
        {
            const std::string garbage = dir + "/_garbage.m4a";
            std::ofstream out(garbage, std::ios::binary);
            const char junk[] = "this is not an mp4 file, not even close, 0123456789abcdef";
            out.write(junk, sizeof junk);
            out.close();
            L.clear(); R.clear(); rate = 0;
            const bool decoded = ptdec::decodeMp4File(garbage.c_str(), L, R, rate);
            std::remove(garbage.c_str());
            check(!decoded, "neg: non-container bytes rejected");
        }
    }

    std::cout << "checked " << g_checks << " decode assertion(s)\n";
    if (g_fails == 0) { std::cout << "ALL GREEN\n"; return 0; }
    std::cout << g_fails << " FAILED\n";
    return 1;
}
