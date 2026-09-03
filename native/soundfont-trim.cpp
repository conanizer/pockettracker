#include "soundfont-trim.h"

#include <algorithm>
#include <cstring>

#include "byte_source.h"

namespace pt {
namespace {

// ─── the nine hydra tables, exactly as they sit in the file ──────────────────────────────────────
//
// Sizes are the record sizes RIFF declares, NOT sizeof() — every one of these is smaller on disk than
// a compiler would lay it out (phdr is 38 bytes, not 40). Fields are read little-endian by hand so
// the reader depends on no packing pragma and no host byte order.

constexpr int PHDR_SIZE = 38, PBAG_SIZE = 4, PMOD_SIZE = 10, PGEN_SIZE = 4;
constexpr int INST_SIZE = 22, IBAG_SIZE = 4, IMOD_SIZE = 10, IGEN_SIZE = 4, SHDR_SIZE = 46;

// A generator is a (u16 operator, u16 amount) pair. These are the two whose amount is an INDEX into
// another table, so they are the two that have to be renumbered when the tables shrink.
constexpr uint16_t GEN_INSTRUMENT = 41;   // in a preset zone: indexes `inst`
constexpr uint16_t GEN_SAMPLE_ID  = 53;   // in an instrument zone: indexes `shdr`

// ⚠️ A sample header with either bit set holds a compressed (Ogg Vorbis) stream, and its start/end
// are BYTE offsets into `smpl`. Without them they are FRAME offsets. Both kinds occur in one file,
// which is why every offset below carries its unit in the name.
constexpr uint16_t SAMPLE_TYPE_COMPRESSED = 0x30;

// The SF2 spec's gap between two uncompressed samples. Kept because a sample whose loop end runs past
// its declared end reads into it, and because interpolation reads one frame beyond a region's end.
constexpr uint32_t PCM_GAP_FRAMES = 46;

struct Phdr { char name[20]; uint16_t preset, bank, bagNdx; uint32_t library, genre, morphology; };
struct Bag  { uint16_t genNdx, modNdx; };
struct Mod  { uint16_t srcOper, destOper; int16_t amount; uint16_t amtSrcOper, transOper; };
struct Gen  { uint16_t oper, amount; };
struct Inst { char name[20]; uint16_t bagNdx; };
struct Shdr {
    char     name[20];
    uint32_t start, end, startLoop, endLoop, sampleRate;
    uint8_t  originalPitch;
    int8_t   pitchCorrection;
    uint16_t sampleLink, sampleType;
};

struct Hydra {
    std::vector<Phdr> phdr;
    std::vector<Bag>  pbag;
    std::vector<Mod>  pmod;
    std::vector<Gen>  pgen;
    std::vector<Inst> inst;
    std::vector<Bag>  ibag;
    std::vector<Mod>  imod;
    std::vector<Gen>  igen;
    std::vector<Shdr> shdr;

    // Where the sample audio sits in the file. `smplSize` is in bytes; zero means there was none.
    long     smplOffset = 0;
    uint32_t smplSize   = 0;
};

// ─── little-endian primitives ────────────────────────────────────────────────────────────────────

uint16_t rd_u16(const uint8_t* p) { return static_cast<uint16_t>(p[0] | (p[1] << 8)); }
uint32_t rd_u32(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}
void wr_u16(std::vector<uint8_t>& v, uint16_t x) {
    v.push_back(static_cast<uint8_t>(x & 0xFF));
    v.push_back(static_cast<uint8_t>(x >> 8));
}
void wr_u32(std::vector<uint8_t>& v, uint32_t x) {
    v.push_back(static_cast<uint8_t>(x & 0xFF));
    v.push_back(static_cast<uint8_t>((x >> 8) & 0xFF));
    v.push_back(static_cast<uint8_t>((x >> 16) & 0xFF));
    v.push_back(static_cast<uint8_t>(x >> 24));
}
void wr_fourcc(std::vector<uint8_t>& v, const char* id) { v.insert(v.end(), id, id + 4); }
void wr_name(std::vector<uint8_t>& v, const char* name) { v.insert(v.end(), name, name + 20); }

bool fourcc_is(const uint8_t* p, const char* id) { return std::memcmp(p, id, 4) == 0; }


// ─── reading the hydra ───────────────────────────────────────────────────────────────────────────

/**
 * Walk the RIFF tree, parse every `pdta` sub-chunk, and note where `smpl` begins.
 *
 * ⚠️ RIFF pads an odd-sized chunk with a byte that is NOT counted in its declared size. Skipping that
 * byte is what keeps the walk aligned on a file whose sample chunk is odd — which an SF3's Ogg data
 * can be.
 */
bool read_hydra(FILE* f, Hydra& h) {
    uint8_t hdr[12];
    if (std::fread(hdr, 1, 12, f) != 12) return false;
    if (!fourcc_is(hdr, "RIFF") || !fourcc_is(hdr + 8, "sfbk")) return false;

    for (;;) {
        uint8_t ch[8];
        if (std::fread(ch, 1, 8, f) != 8) break;   // clean end of file
        const uint32_t size = rd_u32(ch + 4);

        if (!fourcc_is(ch, "LIST")) {
            if (std::fseek(f, static_cast<long>(size + (size & 1)), SEEK_CUR) != 0) break;
            continue;
        }
        uint8_t listType[4];
        if (std::fread(listType, 1, 4, f) != 4) break;
        const long listEnd = std::ftell(f) + static_cast<long>(size) - 4;

        if (fourcc_is(listType, "pdta")) {
            while (std::ftell(f) + 8 <= listEnd) {
                uint8_t sub[8];
                if (std::fread(sub, 1, 8, f) != 8) break;
                const uint32_t subSize = rd_u32(sub + 4);
                const long     next    = std::ftell(f) + static_cast<long>(subSize + (subSize & 1));

                // One reader per record SHAPE rather than nine copies of "read N records of M bytes".
                std::vector<uint8_t> raw;
                auto slurp = [&](int recSize) -> int {
                    if (recSize <= 0 || subSize % static_cast<uint32_t>(recSize) != 0) return 0;
                    raw.resize(subSize);
                    if (subSize && std::fread(raw.data(), 1, subSize, f) != subSize) return 0;
                    return static_cast<int>(subSize / static_cast<uint32_t>(recSize));
                };
                auto read_bags = [&](std::vector<Bag>& dst) {
                    const int n = slurp(PBAG_SIZE);
                    dst.resize(static_cast<size_t>(n));
                    for (int i = 0; i < n; ++i) {
                        const uint8_t* p = raw.data() + i * PBAG_SIZE;
                        dst[static_cast<size_t>(i)] = { rd_u16(p), rd_u16(p + 2) };
                    }
                };
                auto read_mods = [&](std::vector<Mod>& dst) {
                    const int n = slurp(PMOD_SIZE);
                    dst.resize(static_cast<size_t>(n));
                    for (int i = 0; i < n; ++i) {
                        const uint8_t* p = raw.data() + i * PMOD_SIZE;
                        dst[static_cast<size_t>(i)] = { rd_u16(p), rd_u16(p + 2),
                                                       static_cast<int16_t>(rd_u16(p + 4)),
                                                       rd_u16(p + 6), rd_u16(p + 8) };
                    }
                };
                auto read_gens = [&](std::vector<Gen>& dst) {
                    const int n = slurp(PGEN_SIZE);
                    dst.resize(static_cast<size_t>(n));
                    for (int i = 0; i < n; ++i) {
                        const uint8_t* p = raw.data() + i * PGEN_SIZE;
                        dst[static_cast<size_t>(i)] = { rd_u16(p), rd_u16(p + 2) };
                    }
                };

                if (fourcc_is(sub, "phdr")) {
                    const int n = slurp(PHDR_SIZE);
                    h.phdr.resize(static_cast<size_t>(n));
                    for (int i = 0; i < n; ++i) {
                        const uint8_t* p = raw.data() + i * PHDR_SIZE;
                        Phdr& r = h.phdr[static_cast<size_t>(i)];
                        std::memcpy(r.name, p, 20);
                        r.preset = rd_u16(p + 20); r.bank = rd_u16(p + 22); r.bagNdx = rd_u16(p + 24);
                        r.library = rd_u32(p + 26); r.genre = rd_u32(p + 30);
                        r.morphology = rd_u32(p + 34);
                    }
                } else if (fourcc_is(sub, "pbag")) { read_bags(h.pbag);
                } else if (fourcc_is(sub, "ibag")) { read_bags(h.ibag);
                } else if (fourcc_is(sub, "pmod")) { read_mods(h.pmod);
                } else if (fourcc_is(sub, "imod")) { read_mods(h.imod);
                } else if (fourcc_is(sub, "pgen")) { read_gens(h.pgen);
                } else if (fourcc_is(sub, "igen")) { read_gens(h.igen);
                } else if (fourcc_is(sub, "inst")) {
                    const int n = slurp(INST_SIZE);
                    h.inst.resize(static_cast<size_t>(n));
                    for (int i = 0; i < n; ++i) {
                        const uint8_t* p = raw.data() + i * INST_SIZE;
                        std::memcpy(h.inst[static_cast<size_t>(i)].name, p, 20);
                        h.inst[static_cast<size_t>(i)].bagNdx = rd_u16(p + 20);
                    }
                } else if (fourcc_is(sub, "shdr")) {
                    const int n = slurp(SHDR_SIZE);
                    h.shdr.resize(static_cast<size_t>(n));
                    for (int i = 0; i < n; ++i) {
                        const uint8_t* p = raw.data() + i * SHDR_SIZE;
                        Shdr& r = h.shdr[static_cast<size_t>(i)];
                        std::memcpy(r.name, p, 20);
                        r.start = rd_u32(p + 20); r.end = rd_u32(p + 24);
                        r.startLoop = rd_u32(p + 28); r.endLoop = rd_u32(p + 32);
                        r.sampleRate = rd_u32(p + 36);
                        r.originalPitch   = p[40];
                        r.pitchCorrection = static_cast<int8_t>(p[41]);
                        r.sampleLink = rd_u16(p + 42); r.sampleType = rd_u16(p + 44);
                    }
                }
                if (std::fseek(f, next, SEEK_SET) != 0) break;
            }
        } else if (fourcc_is(listType, "sdta")) {
            while (std::ftell(f) + 8 <= listEnd) {
                uint8_t sub[8];
                if (std::fread(sub, 1, 8, f) != 8) break;
                const uint32_t subSize = rd_u32(sub + 4);
                // ⚠️ Only `smpl` is picked up. The `smpo` variant packs the WHOLE font into ONE Ogg
                // stream, so it has no per-sample byte range to copy and cannot be trimmed at all;
                // leaving smplSize at zero is what makes the caller fall back to a whole-bank load.
                if (fourcc_is(sub, "smpl")) {
                    h.smplOffset = std::ftell(f);
                    h.smplSize   = subSize;
                }
                if (std::fseek(f, static_cast<long>(subSize + (subSize & 1)), SEEK_CUR) != 0) break;
            }
        }
        if (std::fseek(f, listEnd, SEEK_SET) != 0) break;
    }
    // A hydra tsf would accept: all nine tables present, and phdr/inst/shdr carrying their terminal
    // record on top of at least one real one.
    return h.phdr.size() >= 2 && h.inst.size() >= 2 && h.shdr.size() >= 2 &&
           !h.pbag.empty() && !h.pgen.empty() && !h.pmod.empty() &&
           !h.ibag.empty() && !h.igen.empty() && !h.imod.empty();
}

/** A hydra name field is 20 bytes and is not guaranteed to be terminated. */
std::string name_of(const char* raw) {
    size_t n = 0;
    while (n < 20 && raw[n] != '\0') ++n;
    return std::string(raw, n);
}

/**
 * The presets in the order tsf reports them: by bank, then preset number, then position in the file.
 * Returns indices into `h.phdr`, excluding its terminal record.
 */
std::vector<int> sorted_preset_order(const Hydra& h) {
    const int real = static_cast<int>(h.phdr.size()) - 1;
    std::vector<int> order(static_cast<size_t>(real));
    for (int i = 0; i < real; ++i) order[static_cast<size_t>(i)] = i;
    std::stable_sort(order.begin(), order.end(), [&](int a, int b) {
        const Phdr& x = h.phdr[static_cast<size_t>(a)];
        const Phdr& y = h.phdr[static_cast<size_t>(b)];
        if (x.bank != y.bank) return x.bank < y.bank;
        return x.preset < y.preset;
    });
    return order;
}

/** Append one RIFF chunk, padding an odd payload up to the even boundary RIFF requires. */
void append_chunk(std::vector<uint8_t>& out, const char* id, const std::vector<uint8_t>& body) {
    wr_fourcc(out, id);
    wr_u32(out, static_cast<uint32_t>(body.size()));
    out.insert(out.end(), body.begin(), body.end());
    if (body.size() & 1) out.push_back(0);
}

}  // namespace

bool sf_read_preset_list(const char* path, std::vector<SfPreset>& out) {
    out.clear();
    if (!path) return false;
    FILE* f = pt_fopen(path, "rb");
    if (!f) return false;
    Hydra h;
    const bool ok = read_hydra(f, h);
    std::fclose(f);
    if (!ok) return false;

    for (int idx : sorted_preset_order(h)) {
        const Phdr& p = h.phdr[static_cast<size_t>(idx)];
        out.push_back({ static_cast<int>(p.bank), static_cast<int>(p.preset), name_of(p.name) });
    }
    return !out.empty();
}

bool sf_build_trimmed_font(const char* path, int bank, int preset, std::vector<uint8_t>& out) {
    out.clear();
    if (!path) return false;
    FILE* f = pt_fopen(path, "rb");
    if (!f) return false;
    Hydra h;
    if (!read_hydra(f, h) || h.smplSize == 0) { std::fclose(f); return false; }

    // Locate the preset. Its zone range runs up to the NEXT phdr's, which is why the terminal record
    // has to be there — and it is, because read_hydra required it.
    const int realPresets = static_cast<int>(h.phdr.size()) - 1;
    int target = -1;
    for (int i = 0; i < realPresets; ++i) {
        const Phdr& p = h.phdr[static_cast<size_t>(i)];
        if (p.bank == bank && p.preset == preset) { target = i; break; }
    }
    if (target < 0) { std::fclose(f); return false; }

    const size_t pbagBegin = h.phdr[static_cast<size_t>(target)].bagNdx;
    const size_t pbagEnd   = h.phdr[static_cast<size_t>(target) + 1].bagNdx;
    if (pbagBegin > pbagEnd || pbagEnd >= h.pbag.size()) { std::fclose(f); return false; }

    // ── which instruments this preset reaches ──
    std::vector<int> instUsed;
    for (size_t b = pbagBegin; b < pbagEnd; ++b) {
        const size_t g0 = h.pbag[b].genNdx, g1 = h.pbag[b + 1].genNdx;
        if (g0 > g1 || g1 >= h.pgen.size()) continue;
        for (size_t g = g0; g < g1; ++g) {
            if (h.pgen[g].oper != GEN_INSTRUMENT) continue;
            const int wi = h.pgen[g].amount;
            if (wi >= static_cast<int>(h.inst.size()) - 1) continue;
            if (std::find(instUsed.begin(), instUsed.end(), wi) == instUsed.end()) instUsed.push_back(wi);
        }
    }
    std::sort(instUsed.begin(), instUsed.end());

    // ── which sample headers those instruments reach ──
    std::vector<int> shdrUsed;
    for (int ii : instUsed) {
        const size_t ib0 = h.inst[static_cast<size_t>(ii)].bagNdx;
        const size_t ib1 = h.inst[static_cast<size_t>(ii) + 1].bagNdx;
        if (ib0 > ib1 || ib1 >= h.ibag.size()) continue;
        for (size_t b = ib0; b < ib1; ++b) {
            const size_t g0 = h.ibag[b].genNdx, g1 = h.ibag[b + 1].genNdx;
            if (g0 > g1 || g1 >= h.igen.size()) continue;
            for (size_t g = g0; g < g1; ++g) {
                if (h.igen[g].oper != GEN_SAMPLE_ID) continue;
                const int si = h.igen[g].amount;
                if (si >= static_cast<int>(h.shdr.size()) - 1) continue;
                if (std::find(shdrUsed.begin(), shdrUsed.end(), si) == shdrUsed.end()) shdrUsed.push_back(si);
            }
        }
    }
    if (shdrUsed.empty()) { std::fclose(f); return false; }

    // ⚠️⚠️ **WHETHER ANY SAMPLE IS COMPRESSED DECIDES THE WHOLE LAYOUT, SO IT IS ANSWERED FIRST.**
    // tsf runs one decoder over both formats and switches behaviour the moment it meets its first
    // compressed sample, which makes "is this font compressed at all" a property of the file rather
    // than of the sample in hand. The two layouts are written out at the copy loop.
    bool anyCompressed = false;
    for (int si : shdrUsed)
        if ((h.shdr[static_cast<size_t>(si)].sampleType & SAMPLE_TYPE_COMPRESSED) != 0) {
            anyCompressed = true;
            break;
        }

    // Ascending by source offset, so the emitted chunk has the same shape a real file does.
    //
    // ⚠️ **WHAT ACTUALLY HAS TO HOLD IS NARROWER THAN IT LOOKS.** tsf's uncompressed pass walks the
    // headers in table order and reads its input at its OUTPUT cursor, which reads as "the table's
    // order must be the chunk's order". It is not: in an all-PCM font that pass ends up copying the
    // sample chunk STRAIGHT THROUGH, one float per frame at the same index, and the terminal header is
    // extended to the end of the buffer unconditionally, which fills in whatever the earlier headers
    // did not reach. The offsets then index that copy correctly whatever order they were written in —
    // reversing this sort deliberately did not move a single sample on six real fonts.
    //
    // The three things that DO have to hold: every offset indexes the chunk emitted below, the terminal
    // header exists, and a compressed sample's header order matches the order its bytes were appended
    // — true by construction, one loop writes both.
    //
    // ⚠️⚠️ **IN A COMPRESSED FONT THE COMPRESSED SAMPLES MUST COME FIRST, AND THAT IS NOT COSMETIC.**
    // The straight-through copy above is what makes gaps between PCM samples harmless, and it stops
    // the moment tsf has decoded one Ogg stream: from then on a PCM header is read from its OWN
    // offset instead of the output cursor. A PCM sample sitting BEFORE the first compressed one would
    // be read the old way and land at the wrong index, silently. Putting every Ogg stream first means
    // the switch has already happened before any PCM header is reached, so there is one rule for all
    // of them.
    std::sort(shdrUsed.begin(), shdrUsed.end(), [&](int a, int b) {
        const Shdr& x = h.shdr[static_cast<size_t>(a)];
        const Shdr& y = h.shdr[static_cast<size_t>(b)];
        const bool cx = (x.sampleType & SAMPLE_TYPE_COMPRESSED) != 0;
        const bool cy = (y.sampleType & SAMPLE_TYPE_COMPRESSED) != 0;
        if (cx != cy) return cx;
        return x.start < y.start;
    });

    // ── copy the sample bytes, and rewrite the headers onto where they landed ──
    //
    // ⚠️⚠️ **THE SILENT TAIL IS THE WHOLE CORRECTNESS ARGUMENT, AND THE TWO LAYOUTS DELIVER IT
    // DIFFERENTLY.** A region routinely reads a little past its own data — a loop end a frame beyond
    // the sample, an `endAddrsOffset` generator, the interpolator's one-frame lookahead. In the whole
    // bank those frames belong to the NEXT sample and tsf plays them; in a font holding one preset
    // there is no next sample, so the read lands on whatever the buffer happens to hold. It has to
    // land on silence, and it has to do so in the DECODED buffer, which is the only thing a region
    // indexes.
    //
    //   all-PCM — the chunk is copied to floats straight through, so the SF2 spec's 46 zero frames
    //             written after each sample are already there. Byte-for-byte what it always did.
    //
    //   compressed — an Ogg stream cannot be padded from the inside, and its decoded length is not in
    //             the header. But tsf walks the sample TABLE in order and appends each sample to one
    //             growing float buffer, so a dummy UNCOMPRESSED header placed after a compressed one
    //             puts its zeros exactly where the overrun reads. Every real sample gets one.
    //
    // ⭐ One shared 46-frame block of zeros serves every pad — `start` is an offset and nothing stops
    // several headers naming the same one — so the pads cost 46 bytes of table each and no audio.
    //
    // ⚠️⚠️ **THAT BLOCK GOES AT THE FRONT OF THE CHUNK, AND PUTTING IT AT THE BACK IS A SEGFAULT.**
    // Once tsf has decoded one Ogg stream it rebases every later PCM header by `resNum - shdr->start`
    // — held in an UNSIGNED 32-bit local, and then subtracted from a pointer. A header whose offset
    // sits AHEAD of the decode cursor makes that difference negative, it wraps to about 4.29e9, and
    // the read pointer lands four gigabytes below the buffer. Ogg data expands about tenfold when it
    // decodes, so an offset at the front of the chunk is always behind the cursor and an offset at the
    // back never is.
    std::vector<uint8_t> smpl;
    // Frame 0, by construction, for the reason above.
    const uint32_t padFrame = 0;
    if (anyCompressed) smpl.resize(PCM_GAP_FRAMES * 2, 0);
    std::vector<Shdr>    newShdr;
    // ⚠️ PARALLEL TO `newShdr`, with -1 for a pad — the renumbering below maps position to position,
    // and a pad present in one vector but not the other shifts every sample id after it, silently.
    std::vector<int>     kept;
    newShdr.reserve(shdrUsed.size() * 2 + 1);
    kept.reserve(shdrUsed.size() * 2);

    // Positions in `newShdr` that are pads. Their offsets are patched after the loop, because where
    // the shared block sits is only known once every sample has been copied.
    std::vector<size_t> padPositions;

    for (int si : shdrUsed) {
        const Shdr& src = h.shdr[static_cast<size_t>(si)];
        Shdr s = src;

        if ((src.sampleType & SAMPLE_TYPE_COMPRESSED) != 0) {
            // ⚠️ BYTE offsets, not frames, and the range is one whole Ogg stream. It is copied
            // opaquely — nothing here decodes Vorbis, which is what makes trimming a compressed bank
            // cheap: the presets left out are never decoded at all, and decoding is the expensive half.
            if (src.start >= src.end || src.end > h.smplSize) continue;
            const size_t bytes = src.end - src.start;
            const size_t at    = smpl.size();
            smpl.resize(at + bytes);
            if (std::fseek(f, h.smplOffset + static_cast<long>(src.start), SEEK_SET) != 0 ||
                std::fread(smpl.data() + at, 1, bytes, f) != bytes) { std::fclose(f); return false; }

            s.start = static_cast<uint32_t>(at);
            s.end   = static_cast<uint32_t>(at + bytes);
            // ⚠️ `startLoop`/`endLoop` are left ALONE. On a compressed sample they are frame offsets
            // RELATIVE to the sample and tsf rebases them itself (`shdr->startLoop += resNum`) — the
            // one place the two formats disagree about what a loop point means.
        } else {
            // Frame offsets. Copy through whichever of `end` and `endLoop` reaches further: a loop
            // running past the declared end is legal, and tsf reads that far.
            const uint32_t totalFrames = h.smplSize / 2;
            const uint32_t reach       = std::max(src.end, src.endLoop);
            if (src.start >= reach || reach > totalFrames) continue;
            const uint32_t frames = reach - src.start;
            // ⚠️ A frame index has to land on an even byte. In an all-PCM font it always does; after
            // an Ogg stream of odd length it does not.
            if (smpl.size() & 1u) smpl.push_back(0);
            const size_t at    = smpl.size();
            const size_t bytes = static_cast<size_t>(frames) * 2;
            // The in-chunk gap is the all-PCM layout's silent tail. A compressed font never reads it —
            // its silence comes from the pad below — so it is not written there.
            smpl.resize(at + bytes + (anyCompressed ? 0u : PCM_GAP_FRAMES * 2), 0);
            if (std::fseek(f, h.smplOffset + static_cast<long>(src.start) * 2, SEEK_SET) != 0 ||
                std::fread(smpl.data() + at, 1, bytes, f) != bytes) { std::fclose(f); return false; }

            const uint32_t base = static_cast<uint32_t>(at / 2);
            s.start     = base;
            s.end       = base + (src.end - src.start);
            s.startLoop = base + (src.startLoop >= src.start ? src.startLoop - src.start : 0);
            s.endLoop   = base + (src.endLoop   >= src.start ? src.endLoop   - src.start : 0);
        }
        newShdr.push_back(s);
        kept.push_back(si);

        if (anyCompressed) {
            padPositions.push_back(newShdr.size());
            Shdr pad{};
            const char padName[20] = "PT-PAD";
            std::memcpy(pad.name, padName, 20);
            pad.sampleRate    = src.sampleRate;
            pad.originalPitch = 60;
            pad.sampleType    = 1;      // monoSample, uncompressed — its offsets are frames
            newShdr.push_back(pad);
            kept.push_back(-1);         // referenced by nothing; it exists to be decoded, not played
        }
    }
    std::fclose(f);
    if (newShdr.empty() || smpl.empty()) return false;

    // ── the pads, onto the block reserved at the front ──
    for (size_t pos : padPositions) {
        newShdr[pos].start     = padFrame;
        newShdr[pos].end       = padFrame + PCM_GAP_FRAMES;
        newShdr[pos].startLoop = padFrame;
        newShdr[pos].endLoop   = padFrame;
    }

    // ── the renumbering tables ──
    std::vector<int> instNew(h.inst.size(), -1);
    for (size_t i = 0; i < instUsed.size(); ++i) {
        instNew[static_cast<size_t>(instUsed[i])] = static_cast<int>(i);
    }
    std::vector<int> shdrNew(h.shdr.size(), -1);
    for (size_t i = 0; i < kept.size(); ++i) {
        if (kept[i] >= 0) shdrNew[static_cast<size_t>(kept[i])] = static_cast<int>(i);
    }

    // ── the preset's zones ──
    std::vector<Bag> nPbag; std::vector<Gen> nPgen; std::vector<Mod> nPmod;
    for (size_t b = pbagBegin; b < pbagEnd; ++b) {
        nPbag.push_back({ static_cast<uint16_t>(nPgen.size()), static_cast<uint16_t>(nPmod.size()) });
        const size_t g0 = h.pbag[b].genNdx, g1 = h.pbag[b + 1].genNdx;
        if (g0 <= g1 && g1 < h.pgen.size()) {
            for (size_t g = g0; g < g1; ++g) {
                Gen gen = h.pgen[g];
                if (gen.oper == GEN_INSTRUMENT) {
                    const int mapped = (gen.amount < instNew.size()) ? instNew[gen.amount] : -1;
                    if (mapped < 0) continue;
                    gen.amount = static_cast<uint16_t>(mapped);
                }
                nPgen.push_back(gen);
            }
        }
        const size_t m0 = h.pbag[b].modNdx, m1 = h.pbag[b + 1].modNdx;
        if (m0 <= m1 && m1 < h.pmod.size()) {
            for (size_t m = m0; m < m1; ++m) nPmod.push_back(h.pmod[m]);
        }
    }

    // ── the instruments' zones ──
    std::vector<Inst> nInst; std::vector<Bag> nIbag; std::vector<Gen> nIgen; std::vector<Mod> nImod;
    for (int ii : instUsed) {
        Inst in{};
        std::memcpy(in.name, h.inst[static_cast<size_t>(ii)].name, 20);
        in.bagNdx = static_cast<uint16_t>(nIbag.size());
        nInst.push_back(in);

        const size_t ib0 = h.inst[static_cast<size_t>(ii)].bagNdx;
        const size_t ib1 = h.inst[static_cast<size_t>(ii) + 1].bagNdx;
        if (ib0 > ib1 || ib1 >= h.ibag.size()) continue;
        for (size_t b = ib0; b < ib1; ++b) {
            nIbag.push_back({ static_cast<uint16_t>(nIgen.size()), static_cast<uint16_t>(nImod.size()) });
            const size_t g0 = h.ibag[b].genNdx, g1 = h.ibag[b + 1].genNdx;
            if (g0 <= g1 && g1 < h.igen.size()) {
                for (size_t g = g0; g < g1; ++g) {
                    Gen gen = h.igen[g];
                    if (gen.oper == GEN_SAMPLE_ID) {
                        const int mapped = (gen.amount < shdrNew.size()) ? shdrNew[gen.amount] : -1;
                        if (mapped < 0) continue;   // a zone whose sample could not be copied
                        gen.amount = static_cast<uint16_t>(mapped);
                    }
                    nIgen.push_back(gen);
                }
            }
            const size_t m0 = h.ibag[b].modNdx, m1 = h.ibag[b + 1].modNdx;
            if (m0 <= m1 && m1 < h.imod.size()) {
                for (size_t m = m0; m < m1; ++m) nImod.push_back(h.imod[m]);
            }
        }
    }
    if (nInst.empty() || nPbag.empty()) return false;

    // ── terminal records ──
    // Every table's last entry is a sentinel whose only job is to say where the previous entry's range
    // ends. tsf reads `[i + 1]` on phdr, pbag, inst and ibag without a bounds check, so a missing
    // terminal is a read past the end of the table rather than a parse failure.
    nPbag.push_back({ static_cast<uint16_t>(nPgen.size()), static_cast<uint16_t>(nPmod.size()) });
    nPgen.push_back({ 0, 0 });
    nPmod.push_back({ 0, 0, 0, 0, 0 });
    nIbag.push_back({ static_cast<uint16_t>(nIgen.size()), static_cast<uint16_t>(nImod.size()) });
    nIgen.push_back({ 0, 0 });
    nImod.push_back({ 0, 0, 0, 0, 0 });

    // ── emit ──
    std::vector<uint8_t> pdta;
    {
        std::vector<uint8_t> body;
        const Phdr& src = h.phdr[static_cast<size_t>(target)];
        wr_name(body, src.name);
        wr_u16(body, src.preset); wr_u16(body, src.bank); wr_u16(body, 0);
        wr_u32(body, src.library); wr_u32(body, src.genre); wr_u32(body, src.morphology);
        const char eop[20] = "EOP";
        wr_name(body, eop);
        wr_u16(body, 0); wr_u16(body, 0);
        wr_u16(body, static_cast<uint16_t>(nPbag.size() - 1));
        wr_u32(body, 0); wr_u32(body, 0); wr_u32(body, 0);
        append_chunk(pdta, "phdr", body);
    }
    auto emit_bags = [&](const char* id, const std::vector<Bag>& bags) {
        std::vector<uint8_t> body;
        for (const Bag& b : bags) { wr_u16(body, b.genNdx); wr_u16(body, b.modNdx); }
        append_chunk(pdta, id, body);
    };
    auto emit_mods = [&](const char* id, const std::vector<Mod>& mods) {
        std::vector<uint8_t> body;
        for (const Mod& m : mods) {
            wr_u16(body, m.srcOper); wr_u16(body, m.destOper);
            wr_u16(body, static_cast<uint16_t>(m.amount));
            wr_u16(body, m.amtSrcOper); wr_u16(body, m.transOper);
        }
        append_chunk(pdta, id, body);
    };
    auto emit_gens = [&](const char* id, const std::vector<Gen>& gens) {
        std::vector<uint8_t> body;
        for (const Gen& g : gens) { wr_u16(body, g.oper); wr_u16(body, g.amount); }
        append_chunk(pdta, id, body);
    };
    emit_bags("pbag", nPbag);
    emit_mods("pmod", nPmod);
    emit_gens("pgen", nPgen);
    {
        std::vector<uint8_t> body;
        for (const Inst& in : nInst) { wr_name(body, in.name); wr_u16(body, in.bagNdx); }
        const char eoi[20] = "EOI";
        wr_name(body, eoi);
        wr_u16(body, static_cast<uint16_t>(nIbag.size() - 1));
        append_chunk(pdta, "inst", body);
    }
    emit_bags("ibag", nIbag);
    emit_mods("imod", nImod);
    emit_gens("igen", nIgen);
    {
        std::vector<uint8_t> body;
        auto put = [&](const Shdr& s) {
            wr_name(body, s.name);
            wr_u32(body, s.start); wr_u32(body, s.end);
            wr_u32(body, s.startLoop); wr_u32(body, s.endLoop);
            wr_u32(body, s.sampleRate);
            body.push_back(s.originalPitch);
            body.push_back(static_cast<uint8_t>(s.pitchCorrection));
            wr_u16(body, s.sampleLink); wr_u16(body, s.sampleType);
        };
        for (const Shdr& s : newShdr) put(s);
        // ⚠️ The terminal sample header is not ceremony: tsf extends the LAST header in the table to
        // the end of the sample chunk whatever its offsets say. Without a sentinel here, the final
        // real sample would swallow everything after it.
        //
        // ⚠️⚠️ **AND IN A COMPRESSED FONT IT IS MARKED COMPRESSED, WHICH IS WHAT MAKES IT FREE.** Left
        // as a PCM sentinel, tsf's forced read-to-the-end appends the ENTIRE undecoded Ogg chunk
        // reinterpreted as 16-bit PCM — half the chunk's bytes again in floats, for nothing anything
        // will ever play. Marked compressed with an empty range, the decoder's own "this is not an Ogg
        // stream" arm zeroes it and moves on, so the sentinel still exists in the table and costs no
        // audio at all. ⚠️ An all-PCM font must NOT take this: there the read-to-the-end is what copies
        // the chunk straight through, and the earlier headers depend on it.
        Shdr eos{};
        const char eosName[20] = "EOS";
        std::memcpy(eos.name, eosName, 20);
        if (anyCompressed) eos.sampleType = SAMPLE_TYPE_COMPRESSED;
        put(eos);
        append_chunk(pdta, "shdr", body);
    }

    // ⚠️⚠️ **THE SAMPLE CHUNK MUST BE EVEN-SIZED, AND RIFF's OWN PAD BYTE IS NOT ENOUGH.** RIFF pads an
    // odd chunk to the next even boundary with a byte outside the declared size — and tsf's chunk
    // walker does not know that. It advances by exactly `8 + size`, so an odd chunk leaves the stream
    // sitting ON the pad byte; the next four bytes it reads as a chunk id begin with a NUL, its
    // `*id <= ' '` check fails, and the walk stops there. Everything AFTER the samples is then never
    // read — `pdta` most of all — and the font comes back as "incomplete" with all nine tables empty.
    //
    // It cannot come up in an all-PCM font: those sizes are frames × 2 and always even. Ogg streams
    // are whatever length they are, and roughly half of them are odd — which is exactly the shape the
    // failure had, 87 of one bank's 156 presets refusing to parse with nothing in common but parity.
    if (smpl.size() & 1u) smpl.push_back(0);

    std::vector<uint8_t> sdta;
    append_chunk(sdta, "smpl", smpl);

    // No INFO list: tsf skips it, and the only field a trimmed font could carry honestly is the
    // version, which nothing reads back.
    const size_t riffSize = 4 /* "sfbk" */ + 12 + sdta.size() + 12 + pdta.size();
    out.reserve(riffSize + 8);
    wr_fourcc(out, "RIFF");
    wr_u32(out, static_cast<uint32_t>(riffSize));
    wr_fourcc(out, "sfbk");
    wr_fourcc(out, "LIST");
    wr_u32(out, static_cast<uint32_t>(4 + sdta.size()));
    wr_fourcc(out, "sdta");
    out.insert(out.end(), sdta.begin(), sdta.end());
    wr_fourcc(out, "LIST");
    wr_u32(out, static_cast<uint32_t>(4 + pdta.size()));
    wr_fourcc(out, "pdta");
    out.insert(out.end(), pdta.begin(), pdta.end());

    return true;
}

}  // namespace pt
