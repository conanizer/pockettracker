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
//   ptshot <project.ptp> <out.png> [--screen=PHRASE] [--phrase=0] [--cursor=ROW,COL]
//                                  [--theme=CLASSIC|AMBER|BLUE|MONO] [--playing=ROW] [--scale=N]

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "songcore/project_io.h"
#include "ui/app_state.h"
#include "ui/layout.h"

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

/** "--key=value" → value, or nullptr. */
const char* opt(int argc, char** argv, const char* key) {
    const size_t klen = std::strlen(key);
    for (int i = 1; i < argc; ++i) {
        if (std::strncmp(argv[i], key, klen) == 0 && argv[i][klen] == '=') return argv[i] + klen + 1;
    }
    return nullptr;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 3) {
        std::fprintf(stderr,
                     "usage: ptshot <project.ptp> <out.png> [--screen=PHRASE] [--phrase=N]\n"
                     "              [--cursor=ROW,COL] [--theme=CLASSIC|AMBER|BLUE|MONO]\n"
                     "              [--playing=ROW] [--scale=N]\n");
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
    if (const char* v = opt(argc, argv, "--phrase")) state.currentPhrase = std::atoi(v);
    if (const char* v = opt(argc, argv, "--cursor")) {
        int row = 0, col = 1;
        if (std::sscanf(v, "%d,%d", &row, &col) == 2) {
            state.cursorRow    = row;
            state.cursorColumn = col;
        }
    }
    if (const char* v = opt(argc, argv, "--playing")) {
        state.isPlaying   = true;
        state.playbackRow = std::atoi(v);
    }
    const int scale = opt(argc, argv, "--scale") ? std::atoi(opt(argc, argv, "--scale")) : 1;

    Canvas       canvas;
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
