// ─── ptshot — a screen, rendered headlessly to a PNG ─────────────────────────────────────────────
//
// The eighth conformance tool, and the first that measures PIXELS rather than events, samples or
// bytes on disk. It takes a golden `.ptp`, puts the cursor somewhere, draws one screen through the
// SAME `TrackerLayout` the SDL shell draws through, and writes the 640×480 result to a PNG. No
// window, no audio device, no engine — `pt-ui` is platform-free by construction, and a green ptshot
// is the standing proof of it: the day the UI reaches for SDL, this tool stops linking.
//
// WHY IT EXISTS. The port plan's acceptance test for every screen is "a screenshot of screen X on
// Android at 1× and on the SDL build must match modulo theme — cheap to eyeball, automatable later"
// (§4.7). Eyeballing needs something to look AT, and the alternative — boot the SDL app, navigate to
// the screen, take a screenshot — is a manual loop per screen, on a machine with a display, which is
// exactly the kind of friction that makes a parity pass get skipped. This turns it into one command.
//
// It is deliberately NOT wired to ctest yet. There is nothing to compare against: the goldens for a
// pixel diff would be Android screenshots, and capturing those is a device job, not a CI one. What it
// gives today is the human loop — render, look, fix — which is what the grind actually needs. When a
// screen's pixels are settled, its PNG becomes the golden and a byte-compare test lands beside it.
//
//   ptshot <project.ptp> <out.png> [--screen=PHRASE] [--phrase=N] [--chain=N] [--table=N]
//                                  [--groove=N] [--instrument=N] [--scroll=N] [--cursor=ROW,COL]
//                                  [--theme=CLASSIC|AMBER|BLUE|MONO] [--viz=SCOPE|OCTA|SPECTRUM|…]
//                                  [--playing=ROW] [--source-column=N] [--from-pool] [--demo]
//                                  [--scale=N]

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "songcore/project_io.h"
#include "ui/app_state.h"
#include "ui/cursor_move.h"
#include "ui/layout.h"
#include "ui/modules/file_browser.h"
#include "ui/modules/oscilloscope.h"
#include "ui/std_filesystem.h"

/** "wav,mp3,flac" → {"wav","mp3","flac"} — for --browser-ext. */
static std::vector<std::string> split_csv(const std::string& s) {
    std::vector<std::string> out;
    std::stringstream        ss(s);
    std::string              item;
    while (std::getline(ss, item, ',')) {
        if (!item.empty()) out.push_back(item);
    }
    return out;
}

using namespace pt::ui;

namespace {

// ─── A PNG writer, in 60 lines ───────────────────────────────────────────────────────────────────
// Uncompressed ("stored") deflate blocks inside a valid zlib stream. A 640×480 frame is ~900 KB
// where a compressed one would be ~10 KB — irrelevant for a screenshot, and it buys us a writer with
// no dependency and no vendored library to license-sweep. stb_image_write would do the same job; the
// port plan already earmarks stb_image (the READER) for theme overlays, and that one will be
// vendored properly when it is needed.

uint32_t crc32_of(const uint8_t* data, size_t n, uint32_t crc = 0xFFFFFFFFu) {
    static uint32_t table[256];
    static bool     init = false;
    if (!init) {
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t c = i;
            for (int k = 0; k < 8; ++k) c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
            table[i] = c;
        }
        init = true;
    }
    for (size_t i = 0; i < n; ++i) crc = table[(crc ^ data[i]) & 0xFF] ^ (crc >> 8);
    return crc;
}

void put_be32(std::vector<uint8_t>& out, uint32_t v) {
    out.push_back(static_cast<uint8_t>(v >> 24));
    out.push_back(static_cast<uint8_t>(v >> 16));
    out.push_back(static_cast<uint8_t>(v >> 8));
    out.push_back(static_cast<uint8_t>(v));
}

void put_chunk(std::vector<uint8_t>& out, const char* type, const std::vector<uint8_t>& data) {
    put_be32(out, static_cast<uint32_t>(data.size()));
    const size_t crcStart = out.size();
    out.insert(out.end(), type, type + 4);
    out.insert(out.end(), data.begin(), data.end());
    const uint32_t crc = crc32_of(out.data() + crcStart, out.size() - crcStart) ^ 0xFFFFFFFFu;
    put_be32(out, crc);
}

/** `px` is ARGB (0xAARRGGBB); the PNG is written as 8-bit RGB, nearest-neighbour upscaled by `scale`. */
bool write_png(const std::string& path, const uint32_t* px, int w, int h, int scale) {
    const int ow = w * scale, oh = h * scale;

    // Raw scanlines: one filter byte (0 = None) then RGB triplets.
    std::vector<uint8_t> raw;
    raw.reserve(static_cast<size_t>(oh) * (1 + static_cast<size_t>(ow) * 3));
    for (int y = 0; y < oh; ++y) {
        raw.push_back(0);
        const uint32_t* srcRow = px + static_cast<size_t>(y / scale) * w;
        for (int x = 0; x < ow; ++x) {
            const uint32_t c = srcRow[x / scale];
            raw.push_back(static_cast<uint8_t>((c >> 16) & 0xFF));
            raw.push_back(static_cast<uint8_t>((c >> 8) & 0xFF));
            raw.push_back(static_cast<uint8_t>(c & 0xFF));
        }
    }

    // zlib: header, stored deflate blocks, adler32.
    std::vector<uint8_t> z;
    z.push_back(0x78);
    z.push_back(0x01);  // (0x7801 % 31 == 0, as the format demands)
    for (size_t off = 0; off < raw.size();) {
        const size_t n     = std::min<size_t>(65535, raw.size() - off);
        const bool   final = (off + n) >= raw.size();
        z.push_back(final ? 1 : 0);
        z.push_back(static_cast<uint8_t>(n & 0xFF));
        z.push_back(static_cast<uint8_t>((n >> 8) & 0xFF));
        z.push_back(static_cast<uint8_t>(~n & 0xFF));
        z.push_back(static_cast<uint8_t>((~n >> 8) & 0xFF));
        z.insert(z.end(), raw.begin() + static_cast<long>(off),
                 raw.begin() + static_cast<long>(off + n));
        off += n;
    }
    uint32_t a = 1, b = 0;
    for (uint8_t v : raw) {
        a = (a + v) % 65521;
        b = (b + a) % 65521;
    }
    put_be32(z, (b << 16) | a);

    std::vector<uint8_t> png = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};

    std::vector<uint8_t> ihdr;
    put_be32(ihdr, static_cast<uint32_t>(ow));
    put_be32(ihdr, static_cast<uint32_t>(oh));
    ihdr.push_back(8);  // bit depth
    ihdr.push_back(2);  // colour type: truecolour RGB
    ihdr.push_back(0);  // compression: deflate
    ihdr.push_back(0);  // filter: adaptive
    ihdr.push_back(0);  // interlace: none
    put_chunk(png, "IHDR", ihdr);
    put_chunk(png, "IDAT", z);
    put_chunk(png, "IEND", {});

    std::ofstream f(path, std::ios::binary);
    if (!f) return false;
    f.write(reinterpret_cast<const char*>(png.data()), static_cast<std::streamsize>(png.size()));
    return f.good();
}

// ─── args ────────────────────────────────────────────────────────────────────────────────────────

bool read_file(const std::string& path, std::string& out) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    std::ostringstream ss;
    ss << f.rdbuf();
    out = ss.str();
    return true;
}

bool screen_from_name(const std::string& n, ScreenType& out) {
    static const std::pair<const char*, ScreenType> NAMES[] = {
        {"SONG", ScreenType::SONG},           {"CHAIN", ScreenType::CHAIN},
        {"PHRASE", ScreenType::PHRASE},       {"INSTRUMENT", ScreenType::INSTRUMENT},
        {"TABLE", ScreenType::TABLE},         {"PROJECT", ScreenType::PROJECT},
        {"GROOVE", ScreenType::GROOVE},       {"SCALE", ScreenType::SCALE},
        {"MODS", ScreenType::MODS},           {"INST_POOL", ScreenType::INST_POOL},
        {"MIXER", ScreenType::MIXER},         {"EFFECTS", ScreenType::EFFECTS},
        {"FILE_BROWSER", ScreenType::FILE_BROWSER}, {"SETTINGS", ScreenType::SETTINGS},
        {"SAMPLE_EDITOR", ScreenType::SAMPLE_EDITOR},
    };
    for (const auto& [name, s] : NAMES) {
        if (n == name) {
            out = s;
            return true;
        }
    }
    return false;
}

bool theme_from_name(const std::string& n, Theme& out) {
    if (n == "CLASSIC") { out = theme_classic(); return true; }
    if (n == "AMBER")   { out = theme_amber();   return true; }
    if (n == "BLUE")    { out = theme_blue();    return true; }
    if (n == "MONO")    { out = theme_mono();    return true; }
    return false;
}

bool viz_from_name(const std::string& n, VisualizerType& out) {
    if (n == "SCOPE")          { out = VisualizerType::SCOPE;          return true; }
    if (n == "FLAT")           { out = VisualizerType::FLAT;           return true; }
    if (n == "OCTA")           { out = VisualizerType::OCTA;           return true; }
    if (n == "OCTA_FULL")      { out = VisualizerType::OCTA_FULL;      return true; }
    if (n == "SPECTRUM")       { out = VisualizerType::SPECTRUM;       return true; }
    if (n == "SPECTRUM_PEAKS") { out = VisualizerType::SPECTRUM_PEAKS; return true; }
    return false;
}

// ─── --demo: a synthetic feed for the furniture ──────────────────────────────────────────────────
//
// The oscilloscope, the note monitor and (since S5) the MIXER's meters are the pieces of furniture
// whose GEOMETRY only exists when there is data in them: forty spectrum bars at amplitude zero draw
// nothing at all, eight OCTA lanes with no audio are one empty panel, and a mixer at silence is
// twenty-two empty troughs. ptshot has no engine and never will (that is what makes it proof the UI is
// portable), so `--demo` fills those buffers with a deterministic formula instead — the same reasoning
// that made S6b synthesize its golden media rather than sample it.
//
// It is a FIXTURE, not a golden: nothing here is compared against anything. Its whole job is to make
// the bars, lanes and note rows visible so a human can check they are where Android puts them.
struct DemoFeed {
    float waveform[WAVEFORM_SIZE]{};
    float trackWaveforms[TRACK_WAVEFORM_COUNT * WAVEFORM_SIZE]{};
    float spectrum[OscilloscopeModule::NUM_BARS]{};

    void fill(AppState& state) {
        for (int i = 0; i < WAVEFORM_SIZE; ++i) {
            const float phase = static_cast<float>(i) / WAVEFORM_SIZE;
            waveform[i]       = 0.25f * std::sin(phase * 6.2831853f * 3.0f);
        }
        // A different frequency and amplitude per lane, so a mislabelled or misplaced lane is obvious
        // rather than plausible.
        for (int lane = 0; lane < TRACK_WAVEFORM_COUNT; ++lane) {
            for (int i = 0; i < WAVEFORM_SIZE; ++i) {
                const float phase = static_cast<float>(i) / WAVEFORM_SIZE;
                trackWaveforms[lane * WAVEFORM_SIZE + i] =
                    (0.30f - 0.02f * lane) * std::sin(phase * 6.2831853f * (lane + 1));
            }
        }
        // A tilted comb: every bar a different height, so the 14px width and 1px gap are countable.
        for (int i = 0; i < OscilloscopeModule::NUM_BARS; ++i) {
            const float t = static_cast<float>(i) / (OscilloscopeModule::NUM_BARS - 1);
            spectrum[i]   = (1.0f - 0.7f * t) * (0.55f + 0.45f * std::sin(t * 18.0f));
        }

        state.waveform          = waveform;
        state.trackWaveforms    = trackWaveforms;
        state.spectrum          = spectrum;
        state.trackMask         = 0xFF;
        state.previewLaneActive = true;

        // A note on every track, an octave apart, and one track left silent — so "--" is on screen
        // beside real notes rather than only ever alone.
        for (int i = 0; i < 8; ++i) {
            state.trackNotes[i] = (i == 5) ? songcore::Note::EMPTY()
                                           : songcore::Note{(i * 2) % 12, 3 + (i % 4)};
        }

        // The mixer's 22 meter channels. A descending staircase across the tracks, with L and R
        // deliberately UNEQUAL on every one (a stereo meter that draws its two bars from one value is a
        // mono meter, and this is what would show it), and levels chosen to land in all three colour
        // zones: track 0 well into the red, the middle ones amber, the last ones green.
        for (int i = 0; i < 8; ++i) {
            state.trackPeaks[i * 2]     = 1.20f * std::pow(0.72f, static_cast<float>(i));
            state.trackPeaks[i * 2 + 1] = 0.95f * std::pow(0.72f, static_cast<float>(i));
        }
        state.masterPeaks[0] = 0.90f;   // just under 0 dBFS — the top of the amber zone
        state.masterPeaks[1] = 1.05f;   // …and just over it, so the red segments light on one side only
        state.sendPeaks[0] = 0.35f;  state.sendPeaks[1] = 0.28f;   // reverb return
        state.sendPeaks[2] = 0.12f;  state.sendPeaks[3] = 0.16f;   // delay return

        // The contract that comes with filling those arrays: whoever refreshes the peaks bumps the
        // version, because that is what the peak-HOLD counts rather than frames (ui/modules/mixer.h).
        // The module also treats its very first draw as a refresh, so ptshot's single frame would place
        // the markers either way — but a filler that did not say it had filled anything would be a trap
        // for the next one.
        state.peaksVersion++;

        fill_sample_editor(state);
    }

    /**
     * The SAMPLE EDITOR (S6b). A sample editor with no sample is a 620×155 empty box and a "WAVEFORM"
     * placeholder — true, and useless for checking that the S/E markers, the slice boundaries and the
     * playhead land where Android puts them. So the waveform is SYNTHESIZED.
     *
     * Four decaying bursts, so the transient markers have something to point AT and the selection band
     * has a visible edge. The 620 bins are the panel's own width — one per pixel column.
     */
    void fill_sample_editor(AppState& state) {
        SampleEditorState& se = state.sampleEditor;

        constexpr int kBins   = SampleEditorModule::WAVEFORM_W;   // 620
        constexpr int kFrames = 132300;                           // 3 s at 44.1 kHz
        const int     onsets[4] = {0, 33000, 62000, 99000};

        se.waveformData.resize(static_cast<size_t>(kBins) * 2);
        for (int i = 0; i < kBins; ++i) {
            const int frame = static_cast<int>((static_cast<int64_t>(i) * kFrames) / kBins);

            // Envelope: an exponential decay from the most recent onset at or before this frame.
            float amp = 0.0f;
            for (const int o : onsets)
                if (frame >= o) amp = std::exp(-static_cast<float>(frame - o) / 9000.0f);

            const float osc = std::sin(static_cast<float>(i) * 0.55f);
            const float v   = 0.92f * amp * osc;
            se.waveformData[static_cast<size_t>(i) * 2]     = -std::fabs(v);   // min
            se.waveformData[static_cast<size_t>(i) * 2 + 1] = std::fabs(v);    // max
        }

        se.totalFrames   = kFrames;
        se.sampleRate    = 44100;
        se.hasStereoData = true;
        se.sampleName    = "BREAK";

        // A selection that is NOT the whole sample, so the lit/dim split across the bins is visible and
        // the S and E rules are somewhere you can see both of them.
        se.selectionStart = 24000;
        se.selectionEnd   = 96000;

        // One marker per onset after the first — N markers make N + 1 slices, so the first slice starts
        // at 0 and needs no marker of its own.
        se.transientMarkers = {onsets[1], onsets[2], onsets[3]};

        se.playbackPosition = 0.62f;   // the playhead, over on the right of the selection
    }
};

/** "--key=value" → value, or nullptr. */
const char* opt(int argc, char** argv, const char* key) {
    const size_t klen = std::strlen(key);
    for (int i = 1; i < argc; ++i) {
        if (std::strncmp(argv[i], key, klen) == 0 && argv[i][klen] == '=') return argv[i] + klen + 1;
    }
    return nullptr;
}

/** A bare "--flag", no value. */
bool flag(int argc, char** argv, const char* key) {
    for (int i = 1; i < argc; ++i)
        if (std::strcmp(argv[i], key) == 0) return true;
    return false;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 3) {
        std::fprintf(stderr,
                     "usage: ptshot <project.ptp> <out.png> [--screen=PHRASE]\n"
                     "              [--phrase=N] [--chain=N] [--table=N] [--groove=N]\n"
                     "              [--instrument=N] [--scroll=N] [--cursor=ROW,COL]\n"
                     "              [--theme=CLASSIC|AMBER|BLUE|MONO]\n"
                     "              [--viz=SCOPE|FLAT|OCTA|OCTA_FULL|SPECTRUM|SPECTRUM_PEAKS]\n"
                     "              [--playing=ROW] [--source-column=N] [--from-pool]\n"
                     "              [--fx-helper=N] [--selection=r1,c1,r2,c2]\n"
                     "              [--browser=DIR] [--browser-ext=wav,mp3] [--browser-cursor=N]\n"
                     "              [--browser-mode=delete] [--browser-select=N] [--browser-clip=N]\n"
                     "              [--qwerty=TEXT] [--qwerty-label=L] [--qwerty-key=ROW,COL]\n"
                     "              [--qwerty-layout=0|1] [--qwerty-cursor=N]\n"
                     "              [--mod-cursor=PAIR,SIDE,ROW] [--sf-presets=COUNT,INDEX]\n"
                     "              [--se-slice=TRANSIENT|DIVIDE|OFF] [--se-slice-index=N]\n"
                     "              [--se-fx=N] [--se-zoom=N] [--se-source=N] [--se-confirm]\n"
                     "              [--demo] [--scale=N]\n"
                     "\n"
                     "  --demo         synthesise the visualizer, the note monitor and the MIXER's\n"
                     "                 meters, which are otherwise empty (ptshot has no engine).\n"
                     "                 A fixture for geometry, not data.\n"
                     "  --sf-presets   likewise for the INSTRUMENT screen's PRESET row: only an engine\n"
                     "                 that has opened the .sf2 can answer it.\n"
                     "  --mod-cursor   MODS has no columns — its cursor is (pair, side, row).\n"
                     "  --cursor       on MIXER this is (masterRow, column): rows 2 and 3 exist only\n"
                     "                 in column 8. On EFFECTS only the row is used (0..7).\n");
        return 2;
    }
    const std::string projectPath = argv[1];
    const std::string outPath     = argv[2];

    std::string blob;
    if (!read_file(projectPath, blob)) {
        std::fprintf(stderr, "cannot read %s\n", projectPath.c_str());
        return 1;
    }

    songcore::json j = songcore::json::parse(blob, nullptr, /*allow_exceptions=*/false);
    if (j.is_discarded() || !j.is_object()) {
        std::fprintf(stderr, "%s did not parse as a .ptp\n", projectPath.c_str());
        return 1;
    }

    // ptshot owns its Project outright — there is no SongcoreHost here, and that is the point: the UI
    // needs a document, not an engine, to draw a screen.
    songcore::Project project = songcore::parse_project(j);
    songcore::normalize_and_migrate(project);

    AppState state;
    state.project = &project;

    if (const char* v = opt(argc, argv, "--screen")) {
        if (!screen_from_name(v, state.currentScreen)) {
            std::fprintf(stderr, "unknown screen: %s\n", v);
            return 2;
        }
    }
    if (const char* v = opt(argc, argv, "--theme")) {
        if (!theme_from_name(v, state.theme)) {
            std::fprintf(stderr, "unknown theme: %s\n", v);
            return 2;
        }
    }
    if (const char* v = opt(argc, argv, "--viz")) {
        if (!viz_from_name(v, state.theme.visualizerType)) {
            std::fprintf(stderr, "unknown visualizer: %s\n", v);
            return 2;
        }
    }

    // Every slot index is clamped to its pool: the layout indexes those pools directly (as it must —
    // the running app can only ever hold a valid slot), so a typo on the command line would otherwise
    // be an out-of-bounds read rather than an error message.
    const auto clamp = [](int v, int hi) { return v < 0 ? 0 : (v > hi ? hi : v); };

    using namespace songcore;
    if (const char* v = opt(argc, argv, "--phrase"))
        state.currentPhrase = clamp(std::atoi(v), POOL_PHRASES - 1);
    if (const char* v = opt(argc, argv, "--chain"))
        state.currentChain = clamp(std::atoi(v), POOL_CHAINS - 1);
    if (const char* v = opt(argc, argv, "--table"))
        state.currentTable = clamp(std::atoi(v), POOL_TABLES - 1);
    if (const char* v = opt(argc, argv, "--groove"))
        state.currentGroove = clamp(std::atoi(v), POOL_GROOVES - 1);
    if (const char* v = opt(argc, argv, "--instrument"))
        state.currentInstrument = clamp(std::atoi(v), POOL_INSTRUMENTS - 1);
    if (const char* v = opt(argc, argv, "--scroll"))
        state.songScrollPosition = clamp(std::atoi(v), 240);  // 256 rows − a 16-row window
    if (const char* v = opt(argc, argv, "--source-column"))
        state.previousColumn = clamp(std::atoi(v), 4);

    state.instrumentFromPool = flag(argc, argv, "--from-pool");

    // ── The SAMPLE EDITOR ────────────────────────────────────────────────────────────────────────
    //
    // Every field here would normally come from the ENGINE (the waveform, the length, the rate, the
    // detected transients) — and ptshot has none, by design. So they are set on the command line, for
    // the same reason --sf-presets and --demo exist: a fixture that makes the geometry visible, compared
    // against nothing.
    if (const char* v = opt(argc, argv, "--se-slice")) {
        const std::string n(v);
        if (n == "TRANSIENT")   state.sampleEditor.sliceMethod = SampleEditorModule::SLICE_TRANSIENT;
        else if (n == "DIVIDE") state.sampleEditor.sliceMethod = SampleEditorModule::SLICE_DIVIDE;
        else                    state.sampleEditor.sliceMethod = SampleEditorModule::SLICE_OFF;
    }
    if (const char* v = opt(argc, argv, "--se-fx"))
        state.sampleEditor.fxType = clamp(std::atoi(v), 4);
    if (const char* v = opt(argc, argv, "--se-zoom"))
        state.sampleEditor.zoomLevel = clamp(std::atoi(v), 4);
    if (const char* v = opt(argc, argv, "--se-source"))
        state.sampleEditor.sourceMode = clamp(std::atoi(v), 3);
    if (const char* v = opt(argc, argv, "--se-slice-index"))
        state.sampleEditor.sliceIndex = clamp(std::atoi(v), 255);
    state.sampleEditor.showConfirmClose = flag(argc, argv, "--se-confirm");

    // --sf-presets=COUNT,INDEX: the SF2's preset list, which only an ENGINE can answer for — and ptshot
    // has none, by design. Supplying it here is what lets the INSTRUMENT screen's PRESET row be drawn
    // and eyeballed without opening a SoundFont. Same reasoning as --demo: a fixture, not a golden.
    if (const char* v = opt(argc, argv, "--sf-presets")) {
        int count = 0, index = 0;
        if (std::sscanf(v, "%d,%d", &count, &index) == 2) {
            state.sfPresetCount = count;
            state.sfPresetIndex = index;
            state.sfPresetName  = "ACOUSTIC GRAND";
        }
    }

    // --mod-cursor=PAIR,SIDE,ROW: the MODS cursor is a triple, not a (row, column) pair — there are no
    // columns on that screen, only two slots side by side (ui/modules/modulation.h).
    if (const char* v = opt(argc, argv, "--mod-cursor")) {
        int pair = 0, side = 0, row = 0;
        if (std::sscanf(v, "%d,%d,%d", &pair, &side, &row) == 3) {
            state.modCursorPair = clamp(pair, 1);
            state.modCursorSide = clamp(side, 1);
            // Clamped to the slot's own depth, exactly as the D-pad clamps it: a NONE slot is one row.
            const ModSlot& slot =
                project.instruments[static_cast<size_t>(state.currentInstrument)]
                    .modSlots[static_cast<size_t>(state.modCursorPair * 2 + state.modCursorSide)];
            state.modCursorRow = clamp(row, mod_slot_row_count(slot) - 1);
        }
    }

    // One --cursor, routed to whichever cursor the screen on show actually reads — TABLE, GROOVE,
    // INSTRUMENT and the pool carry their own, exactly as TrackerController does. Clamped to the same
    // bounds the D-pad obeys (ui/cursor_move.h), so the tool cannot put the cursor somewhere the app
    // never could.
    if (const char* v = opt(argc, argv, "--cursor")) {
        int row = 0, col = 1;
        if (std::sscanf(v, "%d,%d", &row, &col) == 2) {
            const int maxRow = (state.currentScreen == ScreenType::SONG) ? 255 : 15;

            switch (state.currentScreen) {
                case ScreenType::TABLE:
                    state.tableCursorRow    = clamp(row, maxRow);
                    state.tableCursorColumn = col < 1 ? 1 : (col > 8 ? 8 : col);
                    break;
                case ScreenType::GROOVE:
                    state.grooveCursorRow = clamp(row, maxRow);
                    break;

                case ScreenType::INSTRUMENT: {
                    // Its rows are a TABLE, not a range: how many there are depends on the instrument
                    // type, and the reachable columns depend on the row's kind. Clamp through the same
                    // functions the D-pad uses rather than re-deriving the bounds here.
                    // (Fully qualified: `songcore::detail` and `pt::ui::detail` are both in scope here.)
                    const bool sf = state.project->instruments[static_cast<size_t>(state.currentInstrument)]
                                        .instrumentType == InstrumentType::SOUNDFONT;
                    state.instrumentCursorRow = clamp(row, instrument_row_count(sf) - 1);
                    const int lo = pt::ui::detail::instrument_left_column(sf, state.instrumentCursorRow, col);
                    const int hi = pt::ui::detail::instrument_right_column(sf, state.instrumentCursorRow, col);
                    state.instrumentCursorColumn = col < lo ? lo : (col > hi ? hi : col);
                    break;
                }

                case ScreenType::INST_POOL:
                    // The pool's ROW is the selected instrument — so --cursor moves the selection, and
                    // --instrument and --cursor's row are the same knob. Last one on the line wins.
                    state.currentInstrument = clamp(row, POOL_INSTRUMENTS - 1);
                    state.poolCursorColumn  = clamp(col, 4);
                    break;

                case ScreenType::MIXER:
                    // (row, column) here means (mixerMasterRow, mixerCursorColumn). Rows 2 and 3 exist
                    // ONLY in column 8, and the tool deliberately does not enforce that: a screenshot of
                    // an unreachable cursor state is exactly how you check that the module answers
                    // `none()` there and draws nothing highlighted.
                    state.mixerMasterRow    = clamp(row, 3);
                    state.mixerCursorColumn = clamp(col, 8);
                    break;

                case ScreenType::EFFECTS:
                    state.effectsCursorRow = clamp(row, EffectModule::MAX_CURSOR_ROW);
                    break;

                case ScreenType::SAMPLE_EDITOR:
                    // A SPARSE row map (1, 2, 8, 10, 11, 13, 14, 16, 18, 19) — the rest are the waveform
                    // and the spacers. Not clamped to a range, deliberately: a screenshot of the cursor on
                    // a spacer row is how you check that nothing highlights there.
                    state.sampleEditor.cursorRow = clamp(row, 19);
                    state.sampleEditor.cursorCol = clamp(
                        col, SampleEditorModule::max_col_for_row(state.sampleEditor.cursorRow,
                                                                 state.sampleEditor.sliceMethod));
                    break;

                default: {
                    const int lo = min_cursor_column(state.currentScreen);
                    const int hi = max_cursor_column(state.currentScreen);
                    state.cursorRow    = clamp(row, maxRow);
                    state.cursorColumn = col < lo ? lo : (col > hi ? hi : col);
                    break;
                }
            }
        }
    }

    // SONG's 16-row window must contain its cursor — the same invariant the D-pad maintains. An
    // explicit --scroll wins only if it is consistent with the cursor.
    if (state.currentScreen == ScreenType::SONG) scroll_song_to_row(state, state.cursorRow);

    // One --playing, likewise: each screen watches a different playhead.
    if (const char* v = opt(argc, argv, "--playing")) {
        const int row   = std::atoi(v);
        state.isPlaying = true;
        switch (state.currentScreen) {
            case ScreenType::SONG:   state.playbackSongRow  = row; break;
            case ScreenType::CHAIN:  state.playbackChainRow = row; break;
            case ScreenType::TABLE:  state.tablePlaybackRow = row; break;
            case ScreenType::GROOVE: break;  // the groove editor draws no playhead
            default:                 state.playbackRow      = row; break;
        }
    }

    // --fx-helper=N: open the effect picker on effect index N (0..27). The overlay is modal and paints
    // over the finished frame, so this composes with every other option — it is drawn on top of
    // whatever screen and cursor the flags below asked for.
    if (const char* v = opt(argc, argv, "--fx-helper")) {
        state.fxHelper = fx_helper_opened_at(std::atoi(v));
    }
    // --selection=r1,c1,r2,c2: paint a selection region, so the row-highlight colours can be eyeballed.
    if (const char* v = opt(argc, argv, "--selection")) {
        int r1 = 0, c1 = 1, r2 = 0, c2 = 1;
        if (std::sscanf(v, "%d,%d,%d,%d", &r1, &c1, &r2, &c2) == 4) {
            state.selection.active = true;
            state.selection.scope  = SelectionScope::CELL;
            state.selection.start  = CursorPosition{r1, c1};
            state.selection.end    = CursorPosition{r2, c2};
        }
    }

    // ── --browser=DIR: the FILE BROWSER, over a real directory (S6a) ────────────────────────────
    //
    // The one screen ptshot points at the DISK, and it has to: the browser's whole content is a
    // listing, and a synthetic one (the `--demo` approach the mixer and the visualizer take) would be
    // drawing a picture of a fixture rather than of the module. Point it at `testdata/golden` and it
    // draws the three files S6b synthesised; point it at a folder of your own samples and it draws
    // those. `--browser-mode=delete` arms the confirm; `--browser-select=N` paints a live multi-select.
    StdFileSystem browserFs(".");
    if (const char* v = opt(argc, argv, "--browser")) {
        state.currentScreen                = ScreenType::FILE_BROWSER;
        state.fileBrowser.fileExtensions   = {};   // no filter: show everything that is there
        if (const char* e = opt(argc, argv, "--browser-ext")) {
            state.fileBrowser.fileExtensions = split_csv(e);
        }
        navigate_to_folder(state.fileBrowser, browserFs, v);

        if (const char* c = opt(argc, argv, "--browser-cursor")) {
            state.fileBrowser.cursor = std::atoi(c);
        }
        if (const char* m = opt(argc, argv, "--browser-mode")) {
            if (std::string(m) == "delete") state.fileBrowser.mode = BrowserMode::DELETE;
        }
        if (const char* n = opt(argc, argv, "--browser-select")) {
            state.fileBrowser.selectionMode   = true;
            state.fileBrowser.selectionAnchor = std::atoi(n);
        }
        if (const char* cb = opt(argc, argv, "--browser-clip")) {
            // A fake clipboard, purely so the "L+A=PASTE  CPY 3 FILES" top bar can be eyeballed.
            const int n = std::atoi(cb);
            for (int i = 0; i < n; ++i) state.fileBrowser.fileClipboard.push_back("x.wav");
        }
    }

    // ── --qwerty=TEXT: the KEYBOARD overlay, on top of whatever screen is showing ────────────────
    // Modal, drawn last, so it composes with every other flag — including --browser.
    if (const char* v = opt(argc, argv, "--qwerty")) {
        state.qwerty.isOpen     = true;
        state.qwerty.text       = v;
        state.qwerty.textCursor = static_cast<int>(state.qwerty.text.size());
        state.qwerty.fieldLabel = opt(argc, argv, "--qwerty-label")
                                      ? opt(argc, argv, "--qwerty-label")
                                      : "SAMPLE NAME:";
        if (const char* k = opt(argc, argv, "--qwerty-key")) {
            std::sscanf(k, "%d,%d", &state.qwerty.keyCursorRow, &state.qwerty.keyCursorCol);
        }
        if (const char* l = opt(argc, argv, "--qwerty-layout")) {
            state.qwerty.layout = std::atoi(l);
        }
        if (const char* c = opt(argc, argv, "--qwerty-cursor")) {
            state.qwerty.textCursor = std::atoi(c);
        }
    }

    DemoFeed demo;
    if (flag(argc, argv, "--demo")) demo.fill(state);

    const int scale = opt(argc, argv, "--scale") ? std::atoi(opt(argc, argv, "--scale")) : 1;

    Canvas        canvas;
    TrackerLayout layout;
    layout.draw(canvas, state);

    if (!write_png(outPath, canvas.pixels(), DESIGN_W, DESIGN_H, scale < 1 ? 1 : scale)) {
        std::fprintf(stderr, "cannot write %s\n", outPath.c_str());
        return 1;
    }

    std::printf("%s  %s  %dx%d\n", outPath.c_str(), screen_label(state.currentScreen),
                DESIGN_W * scale, DESIGN_H * scale);
    return 0;
}
