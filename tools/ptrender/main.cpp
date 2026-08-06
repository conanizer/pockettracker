// ptrender — songcore S6b conformance harness for the RENDER (host tool, no device/NDK).
//
// The other four tools stop at songcore's edge: they prove the project parses, the pure functions
// compute, the scheduler emits the right events and the consumer turns them into the right engine
// calls. None of them makes a sound — they are all header-only, and none links the engine.
//
// This one does. It drives the whole path with no app code at all:
//
//     push_project(.ptp)  →  load_media(samples + SoundFonts)  →  render_song_to_wav()
//
// which is exactly what the SDL shell will do on Linux, and exactly what Android now does through JNI.
// So a green ptrender is also the standing proof that songcore is genuinely platform-free.
//
// ─── WHY THIS CANNOT BE A BYTE-COMPARE, AND WHAT IT IS INSTEAD ───────────────────────────────────
//
// Every other golden in this repo is compared BIT-FOR-BIT, and the trace goldens can be, because
// songcore's own translation units are pinned to IEEE arithmetic (-fno-fast-math -ffp-contract=off).
// Audio cannot be, and that is MEASURED on all three CI runners rather than assumed. Diff each runner's
// g7-audio.wav against the MSVC/x86-64 one, sample by sample:
//
//     vs MSVC/x86-64            differing   max delta                  first divergence
//     gcc/x86-64 (no fast-math)      9.7%    5 LSB of 32767 (−76 dBFS)   t = 1.627 s
//     clang/arm64 (-ffast-math)     17.0%   16 LSB of 32767 (−66 dBFS)   t = 0.002 s
//
// Two mechanisms, and the timings give them away. On gcc/x86-64 the DRY path is bit-reproducible —
// g1-basics (sampler + SoundFont, no send buses) comes out BYTE-IDENTICAL — and g7 only drifts at
// 1.627 s, once the reverb and delay have built up: sin/exp/pow's last bits are a libm implementation
// detail, and a FEEDBACK chain recirculates them. On clang/arm64 divergence starts at sample 140, before
// any tail exists, because -ffast-math reassociates the dry path too.
//
// Both are inaudible, and every energy measurement still lands inside 1 dB. But a byte-exact audio golden
// would be RED ON TWO OF THE THREE RUNNERS today. So the checks below are instead the ones that are both
// toolchain-proof AND actually catch the two bugs S6b fixed:
//
//   (i)  DETERMINISM — the same project, rendered twice through the same engine, must produce a
//        BYTE-IDENTICAL file. This is a same-binary comparison, so it is exact, and it is the
//        permanent regression net for the render-inherits-the-last-render's-reverb-tail bug. Note the
//        A → B → A shape below: a DIFFERENT project is rendered in between, because the bug was
//        precisely that engine state (reverb lines, delay buffers, ReverbSc's random-lineseg LCG,
//        limiter envelopes) survived from one render into the next.
//
//   (ii) HEALTH — the file is not silent, the render appended a decay tail instead of stopping dead at
//        the last step, that tail ENDED because the audio decayed (not because render.h's 30-second
//        runaway cap fired), and the file does not end at full amplitude. That is the net for the
//        every-export-is-cut-mid-waveform bug.
//
//   (iii) A FINGERPRINT, compared with a TOLERANCE — peak, RMS and per-second RMS in dBFS. Numerical
//        noise between toolchains moves these by far less than 0.01 dB; a real regression (a send that
//        stopped routing, a bypassed master EQ, an inverted gain) moves them by many dB. The gap
//        between those two magnitudes is what makes a 1.0 dB tolerance meaningful rather than arbitrary.
//
// Build + run via the tools/ CMake project — this is the `s6-render` ctest, run by CI on every push
// (see tools/CMakeLists.txt and tools/ptrender/README.md):
//   cmake -S tools -B tools/build -DCMAKE_BUILD_TYPE=Release
//   cmake --build tools/build --config Release
//   ctest --test-dir tools/build -R s6-render --output-on-failure -C Release
// Exit 0 = all green, 1 = any failure. Linux-port plan §4.3 (S6b — the render).

#include "../../native/songcore/host.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

using namespace songcore;

// ─── policy ──────────────────────────────────────────────────────────────────────────────────────

static const int RENDER_SAMPLE_RATE = 44100;   // kick.wav's rate; pad.wav is 22050, so the resampler runs

// The file must not end at full amplitude — the truncation bug ended it at 100% of the render's own
// peak. A tail that decayed properly ends tens of dB down, so this is nowhere near a close call.
static const double END_DECAY_MIN_DB = 20.0;
// Non-silent. A render that lost its media, or dropped every note at the seam, is digital zero.
static const double MIN_PEAK_DBFS = -40.0;

// g9's master fader runs 0xFF → 0x20 across the song, which is 20·log10(32/255) = −18 dB of gain by
// the last step. Ten is comfortably inside that and comfortably outside anything the unfaded g7
// reaches on the same measurement, which is the control printed beside it.
static const double FADE_MIN_DROP_DB = 10.0;

// How much of that −18 dB the SEND RETURNS have to show, measured on the reverb stem and over and
// above what the identical material reaches with a master that never moves (g7's stem, printed beside
// it). Both routings were measured rather than one being assumed:
//
//     the fader multiplies the summed bus        →  +14.15 dB   ← the whole −18 dB, less the reverb's
//     the returns are summed downstream of it    →   −3.00 dB     own opinion about when it comes out
//
// so the threshold sits ~6 dB clear on each side. The control's number is the one that matters: with
// the returns outside the fader, a fade to 0x20 is worth NOTHING to them — g9's return fades slightly
// LESS than the unfaded g7's.
static const double MASTER_CARRIES_SENDS_DB = 8.0;

// Fingerprint tolerances. 1.0 dB is ~12% in amplitude: orders of magnitude above cross-toolchain
// numerical noise, orders of magnitude below any regression worth catching.
static const double FINGERPRINT_TOL_DB = 1.0;
// Below this, two values are both "digital silence" and comparing them in dB is meaningless.
static const double SILENCE_FLOOR_DB = -80.0;
// The tail stops at the first chunk that peaks below −90 dBFS. Which chunk that is can legitimately
// shift by one when the decay curve differs in its last bits, so allow a couple of chunks of slack.
static const int64_t TOTAL_FRAMES_TOL = 2 * TAIL_CHUNK_FRAMES;

// ─── file I/O ────────────────────────────────────────────────────────────────────────────────────

static bool read_file(const std::string& path, std::string& out) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    std::ostringstream ss;
    ss << f.rdbuf();
    out = ss.str();
    return true;
}

static bool write_file(const std::string& path, const std::string& text) {
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f) return false;
    f.write(text.data(), static_cast<std::streamsize>(text.size()));
    return f.good();
}

// ─── the WAV, read back ──────────────────────────────────────────────────────────────────────────
// Deliberately strict: songcore's WavStreamWriter always emits the same canonical 44-byte header, so
// anything else is a bug in the writer rather than a file this tool should tolerate. Reading the file
// back rather than trusting the render's own numbers is the point — this is what a user would open.

struct Wav {
    int     sampleRate = 0;
    int     channels   = 0;
    int64_t frames     = 0;
    std::vector<float> samples;   // interleaved, −1.0 .. +1.0
    std::string error;
    bool ok() const { return error.empty(); }
};

static uint32_t le32(const uint8_t* p) {
    return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
}
static uint16_t le16(const uint8_t* p) { return uint16_t(uint32_t(p[0]) | (uint32_t(p[1]) << 8)); }

static Wav read_wav(const std::string& path) {
    Wav w;
    std::string raw;
    if (!read_file(path, raw)) { w.error = "cannot open " + path; return w; }
    if (raw.size() < 44) { w.error = "shorter than a WAV header"; return w; }

    const uint8_t* b = reinterpret_cast<const uint8_t*>(raw.data());
    if (std::memcmp(b, "RIFF", 4) != 0 || std::memcmp(b + 8, "WAVE", 4) != 0 ||
        std::memcmp(b + 12, "fmt ", 4) != 0 || std::memcmp(b + 36, "data", 4) != 0) {
        w.error = "not the canonical RIFF/WAVE/fmt/data header songcore writes";
        return w;
    }
    if (le32(b + 16) != 16 || le16(b + 20) != 1 || le16(b + 34) != 16) {
        w.error = "not 16-bit PCM";
        return w;
    }
    w.channels   = le16(b + 22);
    w.sampleRate = int(le32(b + 24));
    const uint32_t dataSize = le32(b + 40);
    if (w.channels != 2) { w.error = "not stereo"; return w; }
    if (dataSize + 44u != raw.size()) {
        w.error = "data chunk size (" + std::to_string(dataSize) + ") does not match the file (" +
                  std::to_string(raw.size() - 44) + " bytes after the header)";
        return w;
    }
    if (le32(b + 4) + 8u != raw.size()) { w.error = "RIFF size field does not match the file"; return w; }

    const size_t sampleCount = dataSize / 2;
    w.frames = int64_t(sampleCount) / w.channels;
    w.samples.resize(sampleCount);
    for (size_t i = 0; i < sampleCount; ++i) {
        const int16_t s = int16_t(le16(b + 44 + i * 2));
        w.samples[i] = float(s) / 32768.0f;
    }
    return w;
}

// ─── measurement ─────────────────────────────────────────────────────────────────────────────────

static double db(double amplitude) {
    if (amplitude <= 1e-9) return -180.0;
    return 20.0 * std::log10(amplitude);
}

struct Measurements {
    double peakDb    = -180.0;
    double rmsDb     = -180.0;
    double endPeakDb = -180.0;          // the last TAIL_CHUNK_FRAMES of the file
    std::vector<double> secondsDb;      // RMS of each WHOLE second (a partial final second is dropped:
                                        // its content depends on where the tail happened to stop)
};

static Measurements measure(const Wav& w) {
    Measurements m;
    double peak = 0.0, energy = 0.0;
    for (float s : w.samples) {
        const double a = std::fabs(double(s));
        if (a > peak) peak = a;
        energy += double(s) * double(s);
    }
    m.peakDb = db(peak);
    m.rmsDb  = db(w.samples.empty() ? 0.0 : std::sqrt(energy / double(w.samples.size())));

    const int64_t endFrom = std::max<int64_t>(0, w.frames - TAIL_CHUNK_FRAMES);
    double endPeak = 0.0;
    for (int64_t i = endFrom * w.channels; i < int64_t(w.samples.size()); ++i)
        endPeak = std::fmax(endPeak, std::fabs(double(w.samples[size_t(i)])));
    m.endPeakDb = db(endPeak);

    const int64_t wholeSeconds = w.frames / w.sampleRate;
    for (int64_t s = 0; s < wholeSeconds; ++s) {
        double e = 0.0;
        const int64_t from = s * w.sampleRate * w.channels;
        const int64_t to   = from + int64_t(w.sampleRate) * w.channels;
        for (int64_t i = from; i < to; ++i) e += double(w.samples[size_t(i)]) * double(w.samples[size_t(i)]);
        m.secondsDb.push_back(db(std::sqrt(e / double(int64_t(w.sampleRate) * w.channels))));
    }
    return m;
}

// ─── the fingerprint golden ──────────────────────────────────────────────────────────────────────

struct Fingerprint {
    int     sr          = 0;
    int64_t songFrames  = 0;
    int64_t totalFrames = 0;
    double  peakDb      = 0.0;
    double  rmsDb       = 0.0;
    std::vector<double> secondsDb;
};

static std::string format_fingerprint(const std::string& project, const Fingerprint& f) {
    std::ostringstream o;
    o.setf(std::ios::fixed);
    o.precision(2);
    o << "# ptrender audio fingerprint — schema 1 — " << project << "\n"
      << "#\n"
      << "# NOT a byte golden, and it cannot be one: the DSP uses transcendentals and is built with\n"
      << "# -ffast-math on arm, so gcc/x86-64, MSVC/x86-64 and clang/arm64 disagree on the last bit of\n"
      << "# a reverb tail. These are energy measurements in dBFS, compared with a "
      << FINGERPRINT_TOL_DB << " dB tolerance —\n"
      << "# far above the numerical noise between toolchains, far below any regression worth catching.\n"
      << "#\n"
      << "# songFrames is EXACT: it comes from the sequencer, whose arithmetic is IEEE-pinned.\n"
      << "# totalFrames = songFrames + the decay tail, which may shift by a chunk or two.\n"
      << "#\n"
      << "# Regenerate deliberately: delete this file and re-run ptrender.\n"
      << "sr " << f.sr << "\n"
      << "songFrames " << f.songFrames << "\n"
      << "totalFrames " << f.totalFrames << "\n"
      << "peak " << f.peakDb << "\n"
      << "rms " << f.rmsDb << "\n";
    for (size_t i = 0; i < f.secondsDb.size(); ++i) o << "sec " << i << " " << f.secondsDb[i] << "\n";
    return o.str();
}

static bool parse_fingerprint(const std::string& text, Fingerprint& f) {
    std::istringstream in(text);
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty() || line[0] == '#') continue;
        std::istringstream ls(line);
        std::string key;
        ls >> key;
        if      (key == "sr")          ls >> f.sr;
        else if (key == "songFrames")  ls >> f.songFrames;
        else if (key == "totalFrames") ls >> f.totalFrames;
        else if (key == "peak")        ls >> f.peakDb;
        else if (key == "rms")         ls >> f.rmsDb;
        else if (key == "sec") { int idx; double v; ls >> idx >> v; f.secondsDb.push_back(v); }
        else return false;
    }
    return f.sr > 0;
}

// Both below the silence floor = both digital silence; comparing their dB is meaningless.
static bool db_matches(double a, double b) {
    if (a < SILENCE_FLOOR_DB && b < SILENCE_FLOOR_DB) return true;
    return std::fabs(a - b) <= FINGERPRINT_TOL_DB;
}

// ─── the checks ──────────────────────────────────────────────────────────────────────────────────

struct Report {
    int failures = 0;
    void check(bool ok, const std::string& what, const std::string& detail = "") {
        if (ok) {
            std::cout << "    ok   " << what << "\n";
        } else {
            ++failures;
            std::cout << "  FAIL   " << what << (detail.empty() ? "" : "\n         " + detail) << "\n";
        }
    }
};

struct RenderRun {
    RenderStats  stats;
    Wav          wav;
    Measurements m;
};

/**
 * RMS in dBFS over a half-open FRAME range — the measurement a fade needs, and the reason
 * `Measurements::secondsDb` will not do: it buckets by whole seconds, and g9's whole song is two of
 * them. A window has to be small enough that the quantity being claimed varies across the span, and
 * anchored to the SONG so the decay tail never lands inside it.
 */
static double rms_db_range(const Wav& w, int64_t fromFrame, int64_t toFrame) {
    if (fromFrame < 0) fromFrame = 0;
    if (toFrame > w.frames) toFrame = w.frames;
    if (toFrame <= fromFrame || w.channels <= 0) return -180.0;

    double       energy = 0.0;
    const size_t begin  = static_cast<size_t>(fromFrame) * static_cast<size_t>(w.channels);
    const size_t end    = static_cast<size_t>(toFrame) * static_cast<size_t>(w.channels);
    for (size_t i = begin; i < end && i < w.samples.size(); ++i)
        energy += double(w.samples[i]) * double(w.samples[i]);

    const double rms = std::sqrt(energy / double(end - begin));
    return rms <= 0.0 ? -180.0 : 20.0 * std::log10(rms);
}

// One project: push it, load its media, render it, read the file back.
//
// `expectTail` is a property of the PROJECT, not of the renderer. g7-audio is built to still be
// ringing when its last step ends, so a zero tail there means the truncation bug is back. g1-basics is
// not: its last notes are SoundFont notes transposed up an octave, so they play out in a third of the
// sample's length and the song genuinely ends in digital silence. Asserting a tail there would be
// asserting something untrue.
static bool render_project(SongcoreHost& host, const std::string& testdata, const std::string& name,
                           const std::string& wavPath, bool expectTail, RenderRun& run, Report& rep) {
    std::string blob;
    if (!read_file(testdata + "/" + name + ".ptp", blob)) {
        rep.check(false, name + ": load .ptp", "cannot read " + testdata + "/" + name + ".ptp");
        return false;
    }
    if (!host.push_project(blob)) {
        rep.check(false, name + ": push_project", "the .ptp did not parse");
        return false;
    }

    // The golden projects store their media paths RELATIVE to /tools/testdata ("golden/kick.wav"), which is
    // what makes them portable; base_dir resolves them. A device project stores absolute paths, and
    // resolve_media_path() honours those unchanged.
    const MediaLoadResult media = host.load_media(testdata);

    run.stats = host.render_song_to_wav(wavPath);
    if (!run.stats.ok) {
        rep.check(false, name + ": render", "render_song_to_wav failed (is " + wavPath + "'s directory writable?)");
        return false;
    }
    run.wav = read_wav(wavPath);
    if (!run.wav.ok()) {
        rep.check(false, name + ": the rendered WAV is malformed", run.wav.error);
        return false;
    }
    run.m = measure(run.wav);

    std::printf("    media: %d loaded, %d failed   song: %lld frames + %lld tail = %lld (%.2f s)\n",
                media.loaded, media.failed,
                static_cast<long long>(run.stats.songFrames), static_cast<long long>(run.stats.tailFrames),
                static_cast<long long>(run.stats.totalFrames),
                double(run.stats.totalFrames) / RENDER_SAMPLE_RATE);
    std::printf("    peak %.2f dBFS   rms %.2f dBFS   last %d frames peak %.2f dBFS\n",
                run.m.peakDb, run.m.rmsDb, TAIL_CHUNK_FRAMES, run.m.endPeakDb);

    // Every instrument the project references must have loaded — a project whose media silently failed
    // to open renders near-silence, and would then "pass" a weaker check by decaying very fast.
    rep.check(media.failed == 0, name + ": all media loaded",
              std::to_string(media.failed) + " sample(s)/SoundFont(s) failed to load from " + testdata + "/golden/");

    // ── health ──
    rep.check(run.wav.sampleRate == RENDER_SAMPLE_RATE, name + ": sample rate");
    rep.check(run.wav.frames == run.stats.totalFrames, name + ": the file holds exactly what was rendered",
              "wav has " + std::to_string(run.wav.frames) + " frames, the render reported " +
                  std::to_string(run.stats.totalFrames));
    rep.check(run.m.peakDb > MIN_PEAK_DBFS, name + ": not silent",
              "peak is " + std::to_string(run.m.peakDb) + " dBFS — did the media load?");

    // The two nets for the truncation bug, which ended every export mid-waveform at 100% of its peak.
    if (expectTail) {
        rep.check(run.stats.tailFrames > 0, name + ": the decay tail was appended",
                  "tailFrames == 0, but this project is still ringing at its last step — the render "
                  "stopped dead at the scheduler's span");
    }
    rep.check(run.m.peakDb - run.m.endPeakDb >= END_DECAY_MIN_DB, name + ": the file ends decayed",
              "the last chunk peaks only " + std::to_string(run.m.peakDb - run.m.endPeakDb) +
                  " dB below the render's peak");

    // The tail must have stopped because the audio DECAYED, not because render.h's runaway cap fired.
    // A voice that never ends — or a DSP that has gone non-finite, since NaN compares false against
    // every threshold — would run the tail all the way out to the cap.
    const int64_t maxTail = int64_t(TAIL_MAX_SECONDS) * RENDER_SAMPLE_RATE;
    rep.check(run.stats.tailFrames < maxTail, name + ": the tail decayed rather than hitting the cap",
              "tail ran to the " + std::to_string(TAIL_MAX_SECONDS) +
                  "s cap — a voice never ends, or the DSP is producing non-finite samples");
    return true;
}

// LIVE == RENDER — what you hear is what you export.
//
// ⚠️ **This guards a bug class that the other seven tools are structurally blind to, and it took
// Phase 3 S4 to notice.** The engine holds a great deal of state on its OWN behalf — the mixer, the
// master bus, reverb, delay, the 128-slot EQ bank, and every instrument's drive / crush / filter /
// sample window / loop. None of it is an event, so ptplay cannot see it. None of it is issued by a
// note, so ptvoice cannot see it. And ptrender RENDERS — which goes through prepare_render, the one
// path that pushed it. So for the whole of Phase 2 and three Phase-3 sessions the SDL shell
// EXPORTED g7-audio correctly and PLAYED it on the engine's factory defaults: 84% of the bytes
// differed, and nothing anywhere went red.
//
// The check is the equivalence itself: set the engine up the way a LIVE app does (push_params, with
// no render preparation in front of it) and the audio must come out identical to the render's, to
// the byte. It costs one extra render per project and it is the only test in the tree that would
// have caught this.
static void check_live_equals_render(SongcoreHost& host, AudioEngine& engine,
                                     const std::string& testdata, const std::string& name,
                                     const std::string& wavPath, const std::string& renderedBytes,
                                     Report& rep) {
    std::string blob;
    if (!read_file(testdata + "/" + name + ".ptp", blob) || !host.push_project(blob)) {
        rep.check(false, "live: reload " + name);
        return;
    }
    host.load_media(testdata);
    const SongBounds bounds = find_song_bounds(host.project());

    // A live app's engine state: the chains at their factory defaults (nothing has rendered), and
    // then the project's own params pushed over them. NO prepare_render.
    engine.setOfflineRendering(true);   // …only so the result can be written to a file at all
    engine.stopAll();
    engine.clearScheduledNotes();
    engine.resetFrameCounter();
    engine.resetEffectState();          // = the engine as it boots
    host.push_params();                 // ← what the SDL shell now does after load_media()

    const int64_t frames = host.schedule_song_range(bounds.startRow, bounds.endRow, nullptr);
    host.render_to_wav(wavPath, frames, /*stemsMode=*/0, /*applyMasterBus=*/true);
    host.finish_render();

    std::string liveBytes;
    if (!read_file(wavPath, liveBytes) || liveBytes.empty()) {
        rep.check(false, name + ": live playback and the render produce identical audio",
                  "could not read " + wavPath + " back");
        return;
    }
    if (liveBytes.size() == renderedBytes.size() && liveBytes == renderedBytes) {
        rep.check(true, name + ": live playback and the render produce identical audio (" +
                            std::to_string(liveBytes.size()) + " bytes)");
        return;
    }
    size_t at = 0;
    while (at < liveBytes.size() && at < renderedBytes.size() && liveBytes[at] == renderedBytes[at]) ++at;
    rep.check(false, name + ": live playback and the render produce identical audio",
              "they diverge at byte " + std::to_string(at) +
                  " — the engine state a PLAYING app has is not the state a RENDER has. "
                  "Something the project owns but the engine holds (the mixer, the master "
                  "bus, reverb/delay, the EQ bank, an instrument's playback params) is "
                  "pushed on one path and not the other. See songcore::push_live_params.");
}

// Compare against the committed fingerprint — or write it, if this is the first run.
static void check_fingerprint(const std::string& path, const std::string& name,
                              const RenderRun& run, Report& rep) {
    Fingerprint got;
    got.sr          = run.wav.sampleRate;
    got.songFrames  = run.stats.songFrames;
    got.totalFrames = run.stats.totalFrames;
    got.peakDb      = run.m.peakDb;
    got.rmsDb       = run.m.rmsDb;
    got.secondsDb   = run.m.secondsDb;

    std::string committed;
    if (!read_file(path, committed)) {
        if (write_file(path, format_fingerprint(name, got))) {
            std::cout << "    WROTE the missing fingerprint " << path << " — commit it\n";
        } else {
            rep.check(false, name + ": fingerprint", "cannot write " + path);
        }
        return;
    }

    Fingerprint want;
    if (!parse_fingerprint(committed, want)) {
        rep.check(false, name + ": fingerprint parses", path + " is malformed");
        return;
    }

    rep.check(want.sr == got.sr, name + ": fingerprint sample rate");
    // Exact: the span comes from the sequencer, whose arithmetic is IEEE-pinned on every toolchain.
    rep.check(want.songFrames == got.songFrames, name + ": fingerprint songFrames (exact)",
              "golden " + std::to_string(want.songFrames) + ", got " + std::to_string(got.songFrames) +
                  " — the SEQUENCER changed, which is a trace-golden failure, not an audio one");
    rep.check(std::llabs(want.totalFrames - got.totalFrames) <= TOTAL_FRAMES_TOL,
              name + ": fingerprint totalFrames (±2 tail chunks)",
              "golden " + std::to_string(want.totalFrames) + ", got " + std::to_string(got.totalFrames));
    rep.check(db_matches(want.peakDb, got.peakDb), name + ": fingerprint peak",
              "golden " + std::to_string(want.peakDb) + " dBFS, got " + std::to_string(got.peakDb));
    rep.check(db_matches(want.rmsDb, got.rmsDb), name + ": fingerprint rms",
              "golden " + std::to_string(want.rmsDb) + " dBFS, got " + std::to_string(got.rmsDb));

    if (want.secondsDb.size() != got.secondsDb.size()) {
        rep.check(false, name + ": fingerprint second count",
                  "golden has " + std::to_string(want.secondsDb.size()) + " whole seconds, got " +
                      std::to_string(got.secondsDb.size()));
        return;
    }
    for (size_t i = 0; i < want.secondsDb.size(); ++i) {
        if (!db_matches(want.secondsDb[i], got.secondsDb[i])) {
            rep.check(false, name + ": fingerprint second " + std::to_string(i),
                      "golden " + std::to_string(want.secondsDb[i]) + " dBFS, got " +
                          std::to_string(got.secondsDb[i]));
            return;   // one divergence is the finding; the rest are noise
        }
    }
    rep.check(true, name + ": fingerprint per-second energy (" + std::to_string(want.secondsDb.size()) + " s)");
}

// ─── main ────────────────────────────────────────────────────────────────────────────────────────

int main(int argc, char** argv) {
    const std::string testdata = (argc > 1) ? argv[1] : "tools/testdata";
    const std::string outDir   = (argc > 2) ? argv[2] : ".";

    std::cout << "== songcore S6b render conformance (the real engine, no app) ==\n";

    // ⚠️ HEAP. AudioEngine's per-block DSP scratch, spectrum rings and 256-slot table pool are members,
    // and they blow a 1 MB stack instantly (0xC00000FD) if it is constructed as a local.
    auto engine = std::make_unique<AudioEngine>();
    engine->setDeviceSampleRate(RENDER_SAMPLE_RATE);

    SongcoreHost host(engine.get(), RENDER_SAMPLE_RATE);
    Report rep;

    // A → B → A, through ONE engine. B exists to be a DIFFERENT project between the two A renders:
    // the bug this guards against was engine state (reverb lines, delay buffers, ReverbSc's LCG,
    // limiter envelopes) surviving from one render into the next, and rendering A twice back-to-back
    // is a strictly weaker test of that than rendering something else in between.
    const std::string wavA1 = outDir + "/g7-audio.1.wav";
    const std::string wavB  = outDir + "/g1-basics.wav";
    const std::string wavA2 = outDir + "/g7-audio.2.wav";

    RenderRun a1, b, a2;
    std::cout << "\n[1/3] g7-audio — reverb + delay + master bus + SoundFont + a resampled stereo pad\n";
    const bool okA1 = render_project(host, testdata, "g7-audio", wavA1, /*expectTail=*/true, a1, rep);
    std::cout << "\n[2/3] g1-basics — a DIFFERENT project, rendered in between (the state-leak trap)\n";
    const bool okB = render_project(host, testdata, "g1-basics", wavB, /*expectTail=*/false, b, rep);
    std::cout << "\n[3/3] g7-audio again — the same bytes, or the engine kept something it should not have\n";
    const bool okA2 = render_project(host, testdata, "g7-audio", wavA2, /*expectTail=*/true, a2, rep);

    if (!okA1 || !okB || !okA2) {
        std::cout << "\n" << rep.failures << " FAILED (a render did not complete)\n";
        return 1;
    }

    // ── (i) determinism — the one exact, byte-for-byte check in this tool ──
    std::cout << "\n== determinism ==\n";
    std::string bytes1, bytes2;
    // Guard the reads: two files that both failed to open would compare "equal" as empty strings, and
    // this check would pass by having compared nothing at all.
    if (!read_file(wavA1, bytes1) || !read_file(wavA2, bytes2) || bytes1.empty()) {
        rep.check(false, "the two g7-audio renders are byte-identical",
                  "could not read the rendered WAVs back for comparison");
    } else if (bytes1.size() == bytes2.size() && bytes1 == bytes2) {
        rep.check(true, "the two g7-audio renders are byte-identical (" + std::to_string(bytes1.size()) + " bytes)");
    } else if (bytes1.size() != bytes2.size()) {
        rep.check(false, "the two g7-audio renders are byte-identical",
                  "different LENGTHS: " + std::to_string(bytes1.size()) + " vs " + std::to_string(bytes2.size()) +
                      " bytes — the tail decayed differently, so the engine carried state across renders");
    } else {
        size_t at = 0;
        while (at < bytes1.size() && bytes1[at] == bytes2[at]) ++at;
        rep.check(false, "the two g7-audio renders are byte-identical",
                  "first difference at byte " + std::to_string(at) + " (frame " +
                      std::to_string((at >= 44 ? at - 44 : 0) / 4) +
                      ") — a render is not a pure function of the project: the engine carried state over from " +
                      "the render in between (prepare_render must resetEffectState() AND re-push the project)");
    }

    // ── (ii) LIVE == RENDER — what you hear is what you export ──
    std::cout << "\n== live == render (the engine state a PLAYING app has, vs the one a render has) ==\n";
    check_live_equals_render(host, *engine, testdata, "g7-audio", outDir + "/g7-audio.live.wav",
                             bytes1, rep);

    // ── (iii) AUTOMATION — a project whose audio IS a ramp ──
    //
    // ⚠️ Everything above renders parameters that are set once and then stand still. A fade is the
    // first thing here whose correctness is a SHAPE OVER TIME, and the master fader it moves is engine
    // state that outlives a render — so g9 reaches two blind channels at once: live-vs-render on a
    // MOVING parameter, and residue left behind for the NEXT render.
    //
    // ⚠️ **The fixture is asserted before it is measured.** g9's entire meaning is four FX cells, and a
    // fixture that silently lost them would let every check below pass by having no fade to get wrong.
    // So: ask the LOADED project what it declares, and print the answer.
    std::cout << "\n== automation (g9) — the fade, live, rendered, and what it leaves behind ==\n";
    RenderRun r9;
    const std::string wav9 = outDir + "/g9-automation.wav";
    if (!render_project(host, testdata, "g9-automation", wav9, /*expectTail=*/true, r9, rep)) {
        std::cout << "\n" << rep.failures << " FAILED (the automation render did not complete)\n";
        return 1;
    }
    {
        const std::vector<RampSpec> master = find_ramps(host.project().phrases[0]);
        const std::vector<RampSpec> voice  = find_ramps(host.project().phrases[2]);
        std::printf("    fixture: phrase 0 declares %zu ramp(s), phrase 2 declares %zu\n",
                    master.size(), voice.size());
        const bool masterFade = master.size() == 1 && master[0].fxCode == songcore::FX_VMV &&
                                master[0].startByte == 0xFF && master[0].destByte == 0x20;
        const bool voiceFade  = voice.size() == 1 && voice[0].fxCode == songcore::FX_VOLUME &&
                                voice[0].startByte == 0xFF && voice[0].destByte == 0x20;
        rep.check(masterFade, "g9: the loaded fixture really declares the MASTER fade it is named for",
                  "phrase 0 does not pair VMV FF -> 20 — the .ptp lost its cells, and every "
                  "measurement below would then be measuring g7 with a different arrangement");
        rep.check(voiceFade, "g9: ...and the per-voice VOL fade under it",
                  "phrase 2 does not pair VOL FF -> 20");

        // The shape, derived from those cells and not from a recording. The master runs FF → 20
        // linearly across the song, so the mean gain over its first eighth is ≈(255+227)/2/255 = 0.94
        // and over its last ≈(60+32)/2/255 = 0.18 — a ratio of 5.2, or 14 dB, before the pad's own
        // VOL fade adds to it. The threshold sits below that and above what the UNFADED g7 reaches on
        // the identical measurement, which is the control printed beside it: without that number this
        // would be reading how the material happens to end.
        auto edge_drop = [](const RenderRun& r) {
            const int64_t eighth = r.stats.songFrames / 8;
            return rms_db_range(r.wav, 0, eighth) -
                   rms_db_range(r.wav, r.stats.songFrames - eighth, r.stats.songFrames);
        };
        const double drop9 = edge_drop(r9);
        const double drop7 = edge_drop(a1);
        std::printf("    first vs last eighth of the SONG (tail excluded): g9 drops %.2f dB | "
                    "g7, no ramp in it, drops %.2f dB\n", drop9, drop7);
        rep.check(drop9 >= FADE_MIN_DROP_DB,
                  "g9: the song audibly fades — its last eighth is " +
                      std::to_string(int(FADE_MIN_DROP_DB)) + " dB or more below its first",
                  "drop is only " + std::to_string(drop9) + " dB");
        rep.check(drop7 < FADE_MIN_DROP_DB,
                  "g9: (control) the same measurement on the UNFADED g7 does not reach that drop",
                  "g7 drops " + std::to_string(drop7) + " dB with no ramp in it at all — so this "
                  "metric is reading how the material ends, not the fade");

    }

    // ⭐⭐ DOES THE MASTER FADER CARRY THE SEND RETURNS WITH IT?
    //
    // ⚠️ **THE FULL MIX CANNOT ANSWER THIS, AND TWO METRICS OVER IT BOTH PASSED ON THE BROKEN
    // ROUTING BEFORE THE CONTROL SAID SO.** The dry path obeys the fader either way, so in a mix it
    // is always most of what is being measured, and the returns' 18 dB hides inside it. Worse, g9's
    // per-voice `VOL FF → 20` sits UPSTREAM of the send tap, so it fades the wet under BOTH routings
    // and supplies most of whatever drop a mix-wide metric reads. Measured: opening-to-tail gave
    // 17.69 dB with the fader carrying the returns and 14.53 dB with them escaping it — 3 dB apart,
    // and green either way at any threshold worth setting.
    //
    // So render the REVERB STEM instead (`stemsMode = 9`): the dry path is excluded by construction
    // and what comes out is the return and nothing else. The confound is not out-measured, it is
    // removed — the fader's whole 18 dB is now the only thing between the two routings.
    //
    // The per-voice fade is still in the feed and still worth its own drop, so the number to beat is
    // not zero: g7's stem, same instruments and sends with a master that never moves, is the control
    // printed beside it, and the threshold sits between the two measured routings with room on both
    // sides.
    std::cout << "\n[the returns alone] the reverb STEM — where the dry path cannot hide the answer\n";
    {
        RenderOptions stemOpts;
        stemOpts.stemsMode = 9;   // 9 = reverb return; the dry tracks are not summed at all
        auto stem_fade = [&](const std::string& name, double& outDrop) -> bool {
            std::string blob;
            if (!read_file(testdata + "/" + name + ".ptp", blob) || !host.push_project(blob)) {
                rep.check(false, name + ": load for the reverb stem");
                return false;
            }
            host.load_media(testdata);
            const std::string path = outDir + "/" + name + ".reverb-stem.wav";
            const RenderStats st = host.render_song_to_wav(path, stemOpts);
            const Wav w = read_wav(path);
            if (!st.ok || !w.ok()) {
                rep.check(false, name + ": the reverb stem rendered", w.error);
                return false;
            }
            const int64_t e = st.songFrames / 8;
            const double first = rms_db_range(w, 0, e);
            const double last  = rms_db_range(w, st.songFrames - e, st.songFrames);
            std::printf("    %-14s reverb stem: opening %.2f dBFS → last eighth %.2f dBFS = %.2f dB\n",
                        name.c_str(), first, last, first - last);
            outDrop = first - last;
            // ⚠️ The stem must actually contain a reverb. An empty one would make every drop below
            // read as silence-to-silence and pass by having measured nothing.
            rep.check(first > SILENCE_FLOOR_DB, name + ": the reverb stem is not silent",
                      "the stem opens at " + std::to_string(first) + " dBFS — there is no return in "
                      "it to measure, so the fade check below would pass on an empty file");
            return true;
        };
        double stemDrop9 = 0.0, stemDrop7 = 0.0;
        const bool got9 = stem_fade("g9-automation", stemDrop9);
        const bool got7 = stem_fade("g7-audio", stemDrop7);
        if (got9 && got7) {
            std::printf("    the master fader is worth %+.2f dB of the RETURN\n", stemDrop9 - stemDrop7);
            rep.check(stemDrop9 - stemDrop7 >= MASTER_CARRIES_SENDS_DB,
                      "⭐ the master fader carries the SEND RETURNS with it — a fade takes the reverb "
                      "and delay down too",
                      "g9's reverb return only fades " + std::to_string(stemDrop9 - stemDrop7) +
                          " dB more than the same material with a still master, against the " +
                          std::to_string(int(MASTER_CARRIES_SENDS_DB)) + " dB a fade to 0x20 owes "
                          "it. The returns are being summed DOWNSTREAM of the master fader instead "
                          "of through it, so a fade to nothing ends in a blast of reverb "
                          "(audio-engine.cpp — the fader multiplies the summed bus, not each voice)");
        }
    }

    // ⚠️ The residue check, and the reason g9 moves the MASTER fader rather than a track's: the master
    // fader is one number the engine keeps on its own behalf, and a fade leaves it at 0x20. Render
    // g7 a third time — after the fade — and it must still be the file it was before it.
    //
    // The stem passes above now sit between the two g7 renders as well, so this also catches a stems
    // render leaking its mode — `render.h` puts `stemsMode` back to 0 itself, and this is what says so.
    std::cout << "\n[after the fade] g7-audio again — a ramp must leave nothing behind\n";
    RenderRun a3;
    const std::string wavA3 = outDir + "/g7-audio.3.wav";
    if (render_project(host, testdata, "g7-audio", wavA3, /*expectTail=*/true, a3, rep)) {
        std::string bytes3;
        if (!read_file(wavA3, bytes3) || bytes3.empty()) {
            rep.check(false, "g7-audio renders the same after an automated project",
                      "could not read " + wavA3 + " back");
        } else if (bytes3.size() == bytes1.size() && bytes3 == bytes1) {
            rep.check(true, "⭐ g7-audio renders the same after an automated project (" +
                                std::to_string(bytes3.size()) + " bytes) — the fade left the master "
                                "fader where it found it");
        } else {
            size_t at = 0;
            while (at < bytes3.size() && at < bytes1.size() && bytes3[at] == bytes1[at]) ++at;
            rep.check(false, "g7-audio renders the same after an automated project",
                      "first difference at byte " + std::to_string(at) +
                          " — a ramp moved engine state that the next render inherited. The master "
                          "fader ends g9 at 0x20; something has to put it back.");
        }
    }

    // And the equivalence again, on the moving parameter. A fade is scheduled as events on both paths,
    // so this is the check that says the two paths APPLY them the same way, at the same frames.
    std::string bytes9;
    if (read_file(wav9, bytes9) && !bytes9.empty()) {
        check_live_equals_render(host, *engine, testdata, "g9-automation",
                                 outDir + "/g9-automation.live.wav", bytes9, rep);
    } else {
        rep.check(false, "g9: could not read the automation render back for the live comparison");
    }

    // ── (iv) the fingerprints ──
    std::cout << "\n== fingerprints (tolerance-compared) ==\n";
    check_fingerprint(testdata + "/renders/g7-audio." + std::to_string(RENDER_SAMPLE_RATE) + ".txt",
                      "g7-audio", a1, rep);
    check_fingerprint(testdata + "/renders/g1-basics." + std::to_string(RENDER_SAMPLE_RATE) + ".txt",
                      "g1-basics", b, rep);
    // ⚠️ A fingerprint is written by the thing it later certifies, so it is a REGRESSION net and
    // nothing more. What says g9 fades is the drop measured above, derived from the cells.
    check_fingerprint(testdata + "/renders/g9-automation." + std::to_string(RENDER_SAMPLE_RATE) + ".txt",
                      "g9-automation", r9, rep);

    std::cout << "\n";
    if (rep.failures == 0) {
        std::cout << "ALL GREEN\n";
        return 0;
    }
    std::cout << rep.failures << " FAILED\n";
    return 1;
}
