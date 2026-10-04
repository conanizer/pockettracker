#ifndef POCKETTRACKER_SONGCORE_WAV_WRITER_H
#define POCKETTRACKER_SONGCORE_WAV_WRITER_H

// ─── PCM WAV: the writers, the cue chunk, and the reader ─────────────────────────────────────────
//
// Two writers for two jobs:
//   • `WavStreamWriter` — the RENDER, written chunk by chunk so peak memory is one chunk, always 16-bit.
//   • `write_wav()` — the SAMPLE EDITOR: a whole in-RAM buffer at the BIT cell's depth (8/16/24/32),
//     plus CUE POINTS, one per slice boundary.
// float→int16 is: clamp to ±1, × 32767, TRUNCATE toward zero — ⚠️ not rounding; existing renders must
// not move by an LSB.
//
// `read_cue_points` is the other half: a chopped WAV loaded into another slot or project carries its
// slices only in the file.
//
// Both writers write "<path>.tmp" and rename on success, so a failed or cancelled write leaves no
// half-written .wav. ⚠️ All three steps — open, drop the old target, rename — go through
// `byte_source.h` (`pt_fopen`, `pt_remove`, `pt_rename`); `std::remove` cannot see URI-addressed
// storage.

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "../common/byte_source.h"   // pt_fopen / pt_remove / pt_rename

namespace songcore {

// ─── float → int16, the conversion both writers share ───────────────────────────────────────────
//
// Clamp to ±1, × 32767, TRUNCATE toward zero. ⚠️ Rounding would shift half the samples of every
// existing file by one LSB.
inline int16_t float_to_int16(float v) {
    if (v < -1.0f) v = -1.0f;
    else if (v > 1.0f) v = 1.0f;
    return static_cast<int16_t>(static_cast<int>(v * 32767.0f));
}

class WavStreamWriter {
  public:
    WavStreamWriter(const std::string& path, int sampleRate, int channels = 2)
        : path_(path), tmpPath_(path + ".tmp"), sampleRate_(sampleRate), channels_(channels) {
        file_ = pt_fopen(tmpPath_.c_str(), "wb");
        if (!file_) return;
        uint8_t header[44];
        build_header(header, 0);
        // Placeholder sizes, patched in finish(). No room for 44 bytes fails here as "could not open".
        if (std::fwrite(header, 1, sizeof(header), file_) != sizeof(header)) close_and_remove();
    }

    ~WavStreamWriter() { abort(); }

    WavStreamWriter(const WavStreamWriter&) = delete;
    WavStreamWriter& operator=(const WavStreamWriter&) = delete;

    bool is_open() const { return file_ != nullptr; }

    // Append interleaved float frames ([L0, R0, L1, R1, …]), converted to 16-bit.
    void append_interleaved(const float* data, int frames) {
        if (!file_ || frames <= 0) return;
        const int samples = frames * channels_;
        // Little-endian bytes assembled explicitly (WAV is little-endian by spec, whatever the host).
        // One fwrite per chunk.
        buf_.resize(static_cast<size_t>(samples) * 2);
        for (int i = 0; i < samples; ++i) {
            const uint16_t u = static_cast<uint16_t>(float_to_int16(data[i]));
            buf_[static_cast<size_t>(i) * 2 + 0] = static_cast<uint8_t>(u & 0xFF);
            buf_[static_cast<size_t>(i) * 2 + 1] = static_cast<uint8_t>((u >> 8) & 0xFF);
        }
        // ⚠️ Checked, and the failure is STICKY: this returns void, and a card filling up mid-render
        // lands here. Otherwise finish() would publish a short file as a finished render.
        // `frames_written()` counts frames actually on disk.
        if (std::fwrite(buf_.data(), 1, buf_.size(), file_) != buf_.size()) {
            writeFailed_ = true;
            return;
        }
        framesWritten_ += frames;
    }

    // Patch the RIFF/data sizes, close, rename into place. False on I/O failure or a > 2 GB data size;
    // the temp is removed either way.
    bool finish() {
        if (!file_) return false;

        const int64_t dataSize = framesWritten_ * bytes_per_frame();
        // A failed append makes every count below a lie: drop the temp.
        if (writeFailed_ || dataSize > static_cast<int64_t>(INT32_MAX) - 44) {
            close_and_remove();
            return false;
        }

        uint8_t header[44];
        build_header(header, dataSize);
        if (std::fseek(file_, 0, SEEK_SET) != 0 ||
            std::fwrite(header, 1, sizeof(header), file_) != sizeof(header)) {
            close_and_remove();
            return false;
        }

        // ⚠️ `fclose` is a WRITE: it flushes stdio's buffer, and for a small render it is the ONLY place
        // a full card can report. Unchecked, the rename publishes a short file.
        const bool closed = (std::fclose(file_) == 0);
        file_ = nullptr;
        if (!closed) {
            pt_remove(tmpPath_.c_str());
            return false;
        }

        // ⚠️ The target goes FIRST: `rename()` fails on an existing target on Windows, and SAF's
        // `renameDocument` de-duplicates (leaving `song (1).wav` beside the old render).
        pt_remove(path_.c_str());
        if (pt_rename(tmpPath_.c_str(), path_.c_str()) != 0) {
            pt_remove(tmpPath_.c_str());
            return false;
        }
        return true;
    }

    // Discard everything written so far. Safe any time; the destructor calls it, so no early return
    // leaves a .tmp behind.
    void abort() {
        if (!file_) return;
        close_and_remove();
    }

    int64_t frames_written() const { return framesWritten_; }

  private:
    int bytes_per_frame() const { return channels_ * 2; }   // 16-bit

    void close_and_remove() {
        if (file_) {
            std::fclose(file_);
            file_ = nullptr;
        }
        pt_remove(tmpPath_.c_str());
    }

    // The standard 44-byte RIFF/fmt/data header.
    void build_header(uint8_t* h, int64_t dataSize) const {
        const uint32_t byteRate   = static_cast<uint32_t>(sampleRate_ * bytes_per_frame());
        const uint16_t blockAlign = static_cast<uint16_t>(bytes_per_frame());

        std::memcpy(h + 0, "RIFF", 4);
        put_u32(h + 4, static_cast<uint32_t>(36 + dataSize));
        std::memcpy(h + 8, "WAVE", 4);
        std::memcpy(h + 12, "fmt ", 4);
        put_u32(h + 16, 16);                                    // PCM fmt chunk size
        put_u16(h + 20, 1);                                     // AudioFormat = PCM
        put_u16(h + 22, static_cast<uint16_t>(channels_));
        put_u32(h + 24, static_cast<uint32_t>(sampleRate_));
        put_u32(h + 28, byteRate);
        put_u16(h + 32, blockAlign);
        put_u16(h + 34, 16);                                    // bits per sample
        std::memcpy(h + 36, "data", 4);
        put_u32(h + 40, static_cast<uint32_t>(dataSize));
    }

    static void put_u16(uint8_t* p, uint16_t v) {
        p[0] = static_cast<uint8_t>(v & 0xFF);
        p[1] = static_cast<uint8_t>((v >> 8) & 0xFF);
    }
    static void put_u32(uint8_t* p, uint32_t v) {
        p[0] = static_cast<uint8_t>(v & 0xFF);
        p[1] = static_cast<uint8_t>((v >> 8) & 0xFF);
        p[2] = static_cast<uint8_t>((v >> 16) & 0xFF);
        p[3] = static_cast<uint8_t>((v >> 24) & 0xFF);
    }

    std::string path_;
    std::string tmpPath_;
    int      sampleRate_ = 44100;
    int      channels_   = 2;
    std::FILE* file_     = nullptr;
    int64_t  framesWritten_ = 0;
    std::vector<uint8_t> buf_;   // reused across chunks — no per-chunk allocation
    // Sticky: set by any short write, read by finish() — the only way a mid-render disk failure is
    // reported.
    bool     writeFailed_ = false;
};

// ─── The sample editor's writer: whole buffers, plus the `cue ` chunk ────────────────────────────

namespace detail {

inline void wav_put_u16(std::vector<uint8_t>& b, uint16_t v) {
    b.push_back(static_cast<uint8_t>(v & 0xFF));
    b.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
}
inline void wav_put_u32(std::vector<uint8_t>& b, uint32_t v) {
    b.push_back(static_cast<uint8_t>(v & 0xFF));
    b.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
    b.push_back(static_cast<uint8_t>((v >> 16) & 0xFF));
    b.push_back(static_cast<uint8_t>((v >> 24) & 0xFF));
}
inline void wav_put_tag(std::vector<uint8_t>& b, const char* tag) {
    b.insert(b.end(), tag, tag + 4);
}

/**
 * One sample at the file's depth. 16 goes through `float_to_int16` so 16-bit saves stay byte-identical.
 * Others ROUND to their full scale (2^(bits−1)), which the loader divides by, so a buffer already on an
 * 8- or 24-bit grid round-trips exactly. 8-bit WAV is UNSIGNED, centred on 128.
 */
inline void wav_put_sample(std::vector<uint8_t>& b, float v, int bits, bool isFloat) {
    if (bits == 32 && isFloat) {
        uint32_t u = 0;
        std::memcpy(&u, &v, sizeof(u));
        wav_put_u32(b, u);
        return;
    }
    if (bits == 16) {
        wav_put_u16(b, static_cast<uint16_t>(float_to_int16(v)));
        return;
    }
    if (v < -1.0f) v = -1.0f;
    else if (v > 1.0f) v = 1.0f;
    const double  scale = static_cast<double>(int64_t{1} << (bits - 1));
    const int64_t top   = static_cast<int64_t>(scale) - 1;
    int64_t q = static_cast<int64_t>(std::floor(static_cast<double>(v) * scale + 0.5));
    if (q > top) q = top;
    if (q < -static_cast<int64_t>(scale)) q = -static_cast<int64_t>(scale);
    if (bits == 8) {
        b.push_back(static_cast<uint8_t>(q + 128));
        return;
    }
    const uint32_t u = static_cast<uint32_t>(static_cast<int32_t>(q));
    b.push_back(static_cast<uint8_t>(u & 0xFF));
    b.push_back(static_cast<uint8_t>((u >> 8) & 0xFF));
    b.push_back(static_cast<uint8_t>((u >> 16) & 0xFF));
    if (bits == 32) b.push_back(static_cast<uint8_t>((u >> 24) & 0xFF));
}

/** Write `bytes` to `path` via "<path>.tmp" + rename, so a failed write leaves no partial file. */
inline bool wav_write_atomic(const std::string& path, const std::vector<uint8_t>& bytes) {
    const std::string tmp = path + ".tmp";
    std::FILE* f = pt_fopen(tmp.c_str(), "wb");
    if (!f) return false;
    const size_t written = bytes.empty() ? 0 : std::fwrite(bytes.data(), 1, bytes.size(), f);
    // ⚠️ Both checked, and `fclose` first, unconditionally: a small sample reaches the disk only in
    // this flush, so a full card surfaces here or nowhere. On its own line so a short write cannot
    // short-circuit past it and leak the handle.
    const bool   flushed = (std::fclose(f) == 0);
    const bool   ok      = (written == bytes.size()) && flushed;
    if (!ok) {
        pt_remove(tmp.c_str());
        return false;
    }
    pt_remove(path.c_str());   // see WavStreamWriter::finish — the target must go before the rename
    if (pt_rename(tmp.c_str(), path.c_str()) != 0) {
        pt_remove(tmp.c_str());
        return false;
    }
    return true;
}

}  // namespace detail

/**
 * Write a PCM WAV at `bits` (8, 16, 24 or 32 — 32 as IEEE float when `isFloat`; anything else writes
 * 16), optionally with a `cue ` chunk of slice boundaries.
 * `channels` is 1 (`left` only) or 2 (interleaved) — the caller knows the SOURCE mode.
 * ⚠️ `right` is read, and length-checked, only when `channels == 2`; a mono caller passes it empty.
 * Chunk order RIFF / fmt / data / cue is pinned by a byte-compared golden.
 */
inline bool write_wav(const std::string& path, const std::vector<float>& left,
                      const std::vector<float>& right, int sampleRate,
                      const std::vector<int>& cuePoints = {}, int channels = 2, int bits = 16,
                      bool isFloat = false) {
    if (channels == 2 && left.size() != right.size()) return false;
    if (bits != 8 && bits != 16 && bits != 24 && bits != 32) bits = 16;
    if (bits != 32) isFloat = false;

    const int64_t frames        = static_cast<int64_t>(left.size());
    const int     numChannels   = (channels < 1) ? 1 : (channels > 2 ? 2 : channels);
    const int     blockAlign    = numChannels * (bits / 8);
    const int64_t dataSize      = frames * blockAlign;

    // WAV's size fields are 32-bit. A sample big enough to overflow one is not a sample.
    if (dataSize > static_cast<int64_t>(INT32_MAX)) return false;

    const size_t  numCue           = cuePoints.size();
    const int64_t cueChunkDataSize = numCue > 0 ? static_cast<int64_t>(4 + numCue * 24) : 0;
    const int64_t cueChunkBytes    = numCue > 0 ? 8 + cueChunkDataSize : 0;
    const int64_t riffContentSize  = 36 + dataSize + cueChunkBytes;   // everything after "RIFF" + size

    std::vector<uint8_t> b;
    b.reserve(static_cast<size_t>(8 + riffContentSize));

    // RIFF header (12 bytes)
    detail::wav_put_tag(b, "RIFF");
    detail::wav_put_u32(b, static_cast<uint32_t>(riffContentSize));
    detail::wav_put_tag(b, "WAVE");

    // fmt chunk (24 bytes)
    detail::wav_put_tag(b, "fmt ");
    detail::wav_put_u32(b, 16);                                                    // PCM fmt size
    detail::wav_put_u16(b, isFloat ? 3 : 1);                                       // AudioFormat: PCM / float
    detail::wav_put_u16(b, static_cast<uint16_t>(numChannels));
    detail::wav_put_u32(b, static_cast<uint32_t>(sampleRate));
    detail::wav_put_u32(b, static_cast<uint32_t>(sampleRate * blockAlign));        // byte rate
    detail::wav_put_u16(b, static_cast<uint16_t>(blockAlign));
    detail::wav_put_u16(b, static_cast<uint16_t>(bits));                           // bits per sample

    // data chunk (8 + dataSize)
    detail::wav_put_tag(b, "data");
    detail::wav_put_u32(b, static_cast<uint32_t>(dataSize));
    for (int64_t i = 0; i < frames; ++i) {
        const size_t k = static_cast<size_t>(i);
        detail::wav_put_sample(b, left[k], bits, isFloat);
        if (numChannels == 2) detail::wav_put_sample(b, right[k], bits, isFloat);
    }

    // cue chunk (8 + 4 + n*24), one point per slice boundary
    if (numCue > 0) {
        detail::wav_put_tag(b, "cue ");
        detail::wav_put_u32(b, static_cast<uint32_t>(cueChunkDataSize));
        detail::wav_put_u32(b, static_cast<uint32_t>(numCue));
        for (size_t i = 0; i < numCue; ++i) {
            const uint32_t frame = static_cast<uint32_t>(cuePoints[i]);
            detail::wav_put_u32(b, static_cast<uint32_t>(i + 1));   // ID (1-based)
            detail::wav_put_u32(b, frame);                          // position (play order)
            detail::wav_put_tag(b, "data");                         // the chunk it points into
            detail::wav_put_u32(b, 0);                              // chunk start (0 = unknown)
            detail::wav_put_u32(b, 0);                              // block start
            detail::wav_put_u32(b, frame);                          // sample offset within `data`
        }
    }

    return detail::wav_write_atomic(path, b);
}

/**
 * ⚠️ Writes a STEREO file with the mono data in both channels — the format every chop has always had.
 * Kept deliberately; SAVE passes `channels` explicitly and writes true mono.
 */
inline bool write_wav_mono(const std::string& path, const std::vector<float>& samples, int sampleRate,
                           const std::vector<int>& cuePoints = {}, int bits = 16, bool isFloat = false) {
    return write_wav(path, samples, samples, sampleRate, cuePoints, /*channels=*/2, bits, isFloat);
}

/**
 * The frame positions in a WAV's `cue ` chunk; empty if none or unreadable. Frame 0 is EXCLUDED —
 * it is the sample's start, not a boundary, and would make a zero-length slice 0.
 * Chunk headers are walked with seeks, so only the small cue chunk is read: this runs on every
 * sample load.
 */
inline std::vector<int> read_cue_points(const std::string& path) {
    std::vector<int> frames;

    std::FILE* f = pt_fopen(path.c_str(), "rb");
    if (!f) return frames;

    auto close_and_return = [&](std::vector<int> out) {
        std::fclose(f);
        return out;
    };

    if (std::fseek(f, 0, SEEK_END) != 0) return close_and_return({});
    const long fileLen = std::ftell(f);
    if (fileLen < 12 || std::fseek(f, 0, SEEK_SET) != 0) return close_and_return({});

    auto read_u32 = [&](uint32_t& out) -> bool {
        uint8_t p[4];
        if (std::fread(p, 1, 4, f) != 4) return false;
        out = static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
              (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
        return true;
    };
    auto read_tag = [&](char* tag) -> bool { return std::fread(tag, 1, 4, f) == 4; };

    char     riff[4], wave[4];
    uint32_t riffSize = 0;
    if (!read_tag(riff) || !read_u32(riffSize) || !read_tag(wave)) return close_and_return({});
    if (std::memcmp(riff, "RIFF", 4) != 0 || std::memcmp(wave, "WAVE", 4) != 0)
        return close_and_return({});

    // Walk the 8-byte chunk headers, reading only the body of "cue ".
    while (std::ftell(f) + 8 <= fileLen) {
        char     id[4];
        uint32_t chunkSize = 0;
        if (!read_tag(id) || !read_u32(chunkSize)) break;

        // A size with the top bit set is a > 2 GB chunk or garbage; stop, or the skip below could seek
        // backwards and loop forever.
        if (chunkSize > static_cast<uint32_t>(INT32_MAX)) break;

        if (std::memcmp(id, "cue ", 4) == 0) {
            const long remaining = fileLen - std::ftell(f);
            if (remaining < 4) break;
            uint32_t count = 0;
            if (!read_u32(count)) break;

            // Trust the chunk, not the count field: a truncated file can claim more points than it holds.
            const long  bodyLeft = (static_cast<long>(chunkSize) < remaining ? static_cast<long>(chunkSize)
                                                                             : remaining) - 4;
            const uint32_t maxPoints = static_cast<uint32_t>(bodyLeft / 24);
            if (count > maxPoints) count = maxPoints;

            for (uint32_t i = 0; i < count; ++i) {
                uint32_t cueId = 0, position = 0, dataTag = 0, chunkStart = 0, blockStart = 0, offset = 0;
                if (!read_u32(cueId) || !read_u32(position) || !read_u32(dataTag) ||
                    !read_u32(chunkStart) || !read_u32(blockStart) || !read_u32(offset))
                    break;
                if (position > 0) frames.push_back(static_cast<int>(position));
            }
            return close_and_return(frames);
        }

        // Skip the body, padded to an even boundary (the RIFF spec's word alignment).
        const long skip = static_cast<long>(chunkSize) + (chunkSize & 1u);
        if (std::ftell(f) + skip > fileLen) break;
        if (std::fseek(f, skip, SEEK_CUR) != 0) break;
    }

    return close_and_return(frames);
}

}  // namespace songcore

#endif  // POCKETTRACKER_SONGCORE_WAV_WRITER_H
