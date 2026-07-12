// make-golden-media — writes testdata/golden/{kick.wav, pad.wav, test.sf2}
//
// The six golden projects have always referenced these three files (`golden/kick.wav`,
// `golden/pad.wav`, `golden/test.sf2`), and the files have always been MISSING — which is why the
// goldens are described as "silent by design". That costs nothing for the event traces, which stop at
// the router, far above sample loading. It costs everything for tools/ptrender, which renders real
// audio through the real engine: with no media, every voice is dropped at the seam and the render is
// digital zero.
//
// So the media is SYNTHESIZED here rather than sampled. A public repo should not carry someone else's
// audio, and for a golden a formula beats a found sound anyway: it is exactly reproducible, it is
// auditable (this file *is* the sample's provenance), and each file can be shaped to exercise a
// specific engine path:
//
//   kick.wav   mono,   44100 Hz, 16-bit  — the plain path. Same rate as the render, so the engine's
//                                          sampleRateRatio is 1.0 and no resampling happens.
//   pad.wav    STEREO, 22050 Hz, 16-bit  — the awkward path, on purpose: two channels (the engine's
//                                          separate samplesRight buffer) at HALF the render rate, so
//                                          the rate-compensation ratio is 2.0 and the resampler runs.
//   test.sf2   a hand-built minimal SoundFont — bank 0, preset 5, which is exactly what the golden
//                                          projects ask for (`sfBank = 0; sfPreset = 5`).
//
// EVERY SOURCE DECAYS TO SILENCE, AND NOTHING LOOPS. That is a hard requirement, not a taste: songcore's
// render keeps rendering past the last step until the output falls below −90 dBFS (native/songcore/
// render.h), so one sample looping forever would stretch the tail out to its 30-second cap and make
// every assertion ptrender makes about tail length meaningless.
//
// Build and run from the repo root — it writes ./testdata/golden/:
//     cl /std:c++17 /EHsc /O2 testdata\golden\make-golden-media.cpp && make-golden-media.exe
//     g++ -std=c++17 -O2 testdata/golden/make-golden-media.cpp -o mgm && ./mgm
//
// This is a one-shot generator, not a build target: the COMMITTED files are the truth, and ptrender
// only ever reads them. (Regenerating under a different libm could flip the odd int16 LSB, since every
// sample is a rounded sin() — harmless, but it is why the output is committed rather than rebuilt.)

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

// ─── byte assembly (everything in RIFF is little-endian) ─────────────────────────────────────────
struct Bytes {
    std::vector<uint8_t> v;

    void u8(uint8_t x) { v.push_back(x); }
    void u16(uint16_t x) { u8(uint8_t(x & 0xFF)); u8(uint8_t((x >> 8) & 0xFF)); }
    void u32(uint32_t x) { u16(uint16_t(x & 0xFFFF)); u16(uint16_t((x >> 16) & 0xFFFF)); }
    void s16(int16_t x) { u16(uint16_t(x)); }
    void tag(const char* four) { for (int i = 0; i < 4; ++i) u8(uint8_t(four[i])); }

    // SF2 name fields are a fixed 20 bytes, NUL-padded (NOT NUL-terminated if exactly 20 chars).
    void name20(const std::string& s) {
        for (size_t i = 0; i < 20; ++i) u8(i < s.size() ? uint8_t(s[i]) : 0);
    }

    void patch_u32(size_t at, uint32_t x) {
        v[at + 0] = uint8_t(x & 0xFF);
        v[at + 1] = uint8_t((x >> 8) & 0xFF);
        v[at + 2] = uint8_t((x >> 16) & 0xFF);
        v[at + 3] = uint8_t((x >> 24) & 0xFF);
    }

    size_t size() const { return v.size(); }
};

static bool write_file(const std::string& path, const std::vector<uint8_t>& bytes) {
    std::FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) { std::printf("  ERROR: cannot open %s for writing\n", path.c_str()); return false; }
    const size_t n = std::fwrite(bytes.data(), 1, bytes.size(), f);
    std::fclose(f);
    if (n != bytes.size()) { std::printf("  ERROR: short write to %s\n", path.c_str()); return false; }
    std::printf("  %-28s %7zu bytes\n", path.c_str(), bytes.size());
    return true;
}

// Round-to-nearest, clamped. (The ENGINE's WAV writer truncates, to stay byte-compatible with the
// Kotlin one it replaced; that constraint is about not changing users' existing exports and does not
// apply to a source sample, where rounding is simply the more faithful conversion.)
static int16_t to_i16(double x) {
    const double scaled = x * 32767.0;
    const double r = scaled < 0.0 ? scaled - 0.5 : scaled + 0.5;
    if (r <= -32768.0) return -32768;
    if (r >= 32767.0) return 32767;
    return int16_t(r);
}

// Scale a buffer so its loudest sample sits exactly at `target`, and taper the last `fadeFrames` to
// zero so the sample cannot end on a step — a discontinuity at the end of a one-shot is an audible
// click, and it would also put energy in a render's final block where the tail check expects silence.
static void normalize_and_fade(std::vector<double>& buf, int channels, double target, int fadeFrames) {
    double peak = 0.0;
    for (double s : buf) peak = std::fmax(peak, std::fabs(s));
    if (peak <= 0.0) return;
    const double gain = target / peak;
    for (double& s : buf) s *= gain;

    const int frames = int(buf.size()) / channels;
    for (int i = 0; i < fadeFrames && i < frames; ++i) {
        const double w = double(i) / double(fadeFrames);   // 1 → 0 across the last fadeFrames
        const int frame = frames - 1 - i;
        for (int c = 0; c < channels; ++c) buf[size_t(frame) * channels + c] *= w;
    }
}

static std::vector<uint8_t> build_wav(const std::vector<double>& interleaved, int sampleRate, int channels) {
    const int frames = int(interleaved.size()) / channels;
    const uint32_t dataSize = uint32_t(frames) * uint32_t(channels) * 2u;

    Bytes b;
    b.tag("RIFF");  b.u32(36u + dataSize);  b.tag("WAVE");
    b.tag("fmt ");  b.u32(16);
    b.u16(1);                                             // PCM
    b.u16(uint16_t(channels));
    b.u32(uint32_t(sampleRate));
    b.u32(uint32_t(sampleRate * channels * 2));           // byte rate
    b.u16(uint16_t(channels * 2));                        // block align
    b.u16(16);                                            // bits per sample
    b.tag("data");  b.u32(dataSize);
    for (double s : interleaved) b.s16(to_i16(s));
    return b.v;
}

// ─── kick.wav — mono @ 44100 ─────────────────────────────────────────────────────────────────────
// A synthesised drum thump: a pitch envelope sweeping 120 Hz → 48 Hz, an exponential amplitude decay,
// and a short click transient on top so the sample has some high-frequency content for the filters,
// the OTT bands and the spectrum path to actually chew on.
static std::vector<uint8_t> make_kick() {
    const int sr = 44100;
    const int frames = 7938;   // 180 ms
    std::vector<double> buf(static_cast<size_t>(frames), 0.0);

    double phase = 0.0;
    for (int i = 0; i < frames; ++i) {
        const double t = double(i) / sr;
        const double freq = 48.0 + 72.0 * std::exp(-t / 0.028);
        phase += 2.0 * M_PI * freq / sr;

        const double body  = std::sin(phase) * std::exp(-t / 0.055);
        const double click = 0.22 * std::sin(2.0 * M_PI * 2100.0 * t) * std::exp(-t / 0.0015);
        const double attack = std::fmin(1.0, t / 0.0008);   // 0.8 ms ramp — no DC step at frame 0
        buf[size_t(i)] = (body + click) * attack;
    }
    normalize_and_fade(buf, 1, 0.90, 256);
    return build_wav(buf, sr, 1);
}

// ─── pad.wav — STEREO @ 22050 ────────────────────────────────────────────────────────────────────
// A decaying harmonic pad at C-4 (261.63 Hz), which is the instrument's default root, so a C-4 step
// plays it back at exactly its recorded pitch. The two channels are detuned against each other by
// ±1.5 cents-ish: it makes the stereo real (a mono-summed copy would not prove the right buffer is
// even read) and it beats slowly, which a resampling bug smears audibly.
static std::vector<uint8_t> make_pad() {
    const int sr = 22050;
    const int frames = 17640;   // 800 ms
    const double f0 = 261.6256;
    const double partials[4][2] = { {1.0, 1.00}, {2.0, 0.32}, {3.0, 0.16}, {4.0, 0.08} };

    std::vector<double> buf(size_t(frames) * 2);
    for (int i = 0; i < frames; ++i) {
        const double t = double(i) / sr;
        const double attack = 0.5 - 0.5 * std::cos(M_PI * std::fmin(1.0, t / 0.030));   // 30 ms raised cosine
        const double env = attack * std::exp(-t / 0.35);

        for (int ch = 0; ch < 2; ++ch) {
            const double detune = (ch == 0) ? 0.99913 : 1.00087;
            double s = 0.0;
            for (const auto& p : partials) s += p[1] * std::sin(2.0 * M_PI * f0 * detune * p[0] * t);
            buf[size_t(i) * 2 + size_t(ch)] = s * env;
        }
    }
    normalize_and_fade(buf, 2, 0.75, 441);
    return build_wav(buf, sr, 2);
}

// ─── test.sf2 — a minimal SoundFont ──────────────────────────────────────────────────────────────
// TinySoundFont (native/vendor/tsf/tsf.h, tsf_load) is unforgiving in exactly one way: its `pdta` list
// must contain ALL NINE hydra chunks — phdr, pbag, pmod, pgen, inst, ibag, imod, igen, shdr — or the
// font is rejected as incomplete, even when the missing ones would hold nothing but their mandatory
// terminal record. So all nine are written, most of them holding only that terminator.
//
// The hydra is a chain of index tables, and every level is bounded by the NEXT record's index (tsf
// reads `pphdr[1].presetBagNdx` to find where preset 0's bags end). That is why each table carries one
// extra sentinel record — EOP / EOI / EOS — whose only job is to point one past the real data.
//
//   phdr[0] "PT Tone" bank 0 preset 5  ──▶ pbag[0] ──▶ pgen[0] = { instrument → inst[0] }
//   inst[0] "PT Tone"                  ──▶ ibag[0] ──▶ igen[0] = { sampleModes = 0 (no loop) }
//                                                      igen[1] = { sampleID → shdr[0] }   ← must be LAST
//   shdr[0] "PT Tone" = the smpl data, originalPitch 60 (C-4), no loop
//
// The tone itself is a decaying harmonic pluck at C-4 — one-shot, so a SoundFont voice, which the
// sequencer never sends a note-off to (note-offs are KIL-only), still ends on its own.
static std::vector<uint8_t> make_sf2() {
    const int sr = 44100;
    const int frames = 17640;   // 400 ms
    const double f0 = 261.6256; // C-4 == originalPitch 60, so pitchCorrection is 0
    const double partials[4][2] = { {1.0, 0.90}, {2.0, 0.30}, {3.0, 0.15}, {5.0, 0.06} };

    std::vector<double> tone(static_cast<size_t>(frames), 0.0);
    for (int i = 0; i < frames; ++i) {
        const double t = double(i) / sr;
        const double attack = std::fmin(1.0, t / 0.004);
        double s = 0.0;
        for (const auto& p : partials) s += p[1] * std::sin(2.0 * M_PI * f0 * p[0] * t);
        tone[size_t(i)] = s * attack * std::exp(-t / 0.13);
    }
    normalize_and_fade(tone, 1, 0.80, 441);

    // The spec requires at least 46 zero frames after each sample; tsf reads the whole smpl chunk as
    // one buffer and clamps a region's end to it, so the padding also guarantees the zone can never
    // read into the next sample's data.
    const uint32_t sampleFrames = uint32_t(frames);
    const uint32_t ZERO_PAD = 46;

    // ── generator operator numbers (SF2 spec §8.1) ──
    const uint16_t GEN_INSTRUMENT  = 41;
    const uint16_t GEN_SAMPLE_ID   = 53;
    const uint16_t GEN_SAMPLE_MODE = 54;

    Bytes b;
    b.tag("RIFF");
    const size_t riffSizeAt = b.size();
    b.u32(0);                     // patched below
    b.tag("sfbk");

    // ── LIST INFO ──
    b.tag("LIST");
    const size_t infoSizeAt = b.size();
    b.u32(0);
    const size_t infoStart = b.size();
    b.tag("INFO");
    b.tag("ifil");  b.u32(4);   b.u16(2); b.u16(1);            // SoundFont spec version 2.01
    b.tag("isng");  b.u32(8);   for (const char* s = "EMU8000"; *s; ++s) b.u8(uint8_t(*s)); b.u8(0);
    b.tag("INAM");  b.u32(16);  { const std::string n = "PocketTracker Test";
                                  for (size_t i = 0; i < 16; ++i) b.u8(i < n.size() ? uint8_t(n[i]) : 0); }
    b.patch_u32(infoSizeAt, uint32_t(b.size() - infoStart));

    // ── LIST sdta — the raw int16 sample data ──
    b.tag("LIST");
    const size_t sdtaSizeAt = b.size();
    b.u32(0);
    const size_t sdtaStart = b.size();
    b.tag("sdta");
    b.tag("smpl");  b.u32((sampleFrames + ZERO_PAD) * 2u);
    for (int i = 0; i < frames; ++i) b.s16(to_i16(tone[size_t(i)]));
    for (uint32_t i = 0; i < ZERO_PAD; ++i) b.s16(0);
    b.patch_u32(sdtaSizeAt, uint32_t(b.size() - sdtaStart));

    // ── LIST pdta — the hydra ──
    b.tag("LIST");
    const size_t pdtaSizeAt = b.size();
    b.u32(0);
    const size_t pdtaStart = b.size();
    b.tag("pdta");

    b.tag("phdr");  b.u32(2 * 38);
    b.name20("PT Tone");  b.u16(5); b.u16(0); b.u16(0);  b.u32(0); b.u32(0); b.u32(0);  // preset 5, bank 0
    b.name20("EOP");      b.u16(0); b.u16(0); b.u16(1);  b.u32(0); b.u32(0); b.u32(0);  // terminal

    b.tag("pbag");  b.u32(2 * 4);
    b.u16(0); b.u16(0);                                   // zone 0 → pgen[0..1)
    b.u16(1); b.u16(0);                                   // terminal

    b.tag("pmod");  b.u32(1 * 10);
    b.u16(0); b.u16(0); b.s16(0); b.u16(0); b.u16(0);     // terminal (no modulators)

    b.tag("pgen");  b.u32(2 * 4);
    b.u16(GEN_INSTRUMENT); b.u16(0);                      // → inst[0]
    b.u16(0); b.u16(0);                                   // terminal

    b.tag("inst");  b.u32(2 * 22);
    b.name20("PT Tone");  b.u16(0);                       // → ibag[0]
    b.name20("EOI");      b.u16(1);                       // terminal

    b.tag("ibag");  b.u32(2 * 4);
    b.u16(0); b.u16(0);                                   // zone 0 → igen[0..2)
    b.u16(2); b.u16(0);                                   // terminal

    b.tag("imod");  b.u32(1 * 10);
    b.u16(0); b.u16(0); b.s16(0); b.u16(0); b.u16(0);     // terminal

    b.tag("igen");  b.u32(3 * 4);
    b.u16(GEN_SAMPLE_MODE); b.u16(0);                     // 0 = play through once, no loop
    b.u16(GEN_SAMPLE_ID);   b.u16(0);                     // → shdr[0]. The spec requires sampleID LAST.
    b.u16(0); b.u16(0);                                   // terminal

    b.tag("shdr");  b.u32(2 * 46);
    b.name20("PT Tone");
    b.u32(0);                      // start
    b.u32(sampleFrames);           // end
    b.u32(0);                      // startLoop  (unused — sampleModes says no loop)
    b.u32(sampleFrames);           // endLoop
    b.u32(uint32_t(sr));
    b.u8(60);                      // originalPitch — MIDI 60 = C-4, the tone's actual pitch
    b.u8(0);                       // pitchCorrection (cents) — none needed, f0 IS C-4
    b.u16(0);                      // sampleLink
    b.u16(1);                      // sampleType = monoSample
    b.name20("EOS");
    b.u32(0); b.u32(0); b.u32(0); b.u32(0); b.u32(0);
    b.u8(0); b.u8(0); b.u16(0); b.u16(0);                 // terminal

    b.patch_u32(pdtaSizeAt, uint32_t(b.size() - pdtaStart));
    b.patch_u32(riffSizeAt, uint32_t(b.size() - (riffSizeAt + 4)));
    return b.v;
}

int main() {
    std::printf("make-golden-media — writing testdata/golden/\n");
    bool ok = true;
    ok &= write_file("testdata/golden/kick.wav", make_kick());
    ok &= write_file("testdata/golden/pad.wav", make_pad());
    ok &= write_file("testdata/golden/test.sf2", make_sf2());
    if (!ok) {
        std::printf("FAILED — run this from the repo root, so ./testdata/golden/ exists\n");
        return 1;
    }
    std::printf("done\n");
    return 0;
}
