#ifndef POCKETTRACKER_SONGCORE_WAV_WRITER_H
#define POCKETTRACKER_SONGCORE_WAV_WRITER_H

// ─── 16-bit PCM WAV: the writer, the cue chunk, and the reader ───────────────────────────────────
//
// TWO writers, because the app writes WAVs for two unrelated reasons and they want opposite things:
//
//   • `WavStreamWriter` — the RENDER. A song is far too big to hold in RAM, so it is written chunk by
//     chunk: open → append_interleaved() per render chunk → finish(). Peak memory is one chunk, not
//     one song, which is why the render is chunked at all (a full-song float render used to hold ~4
//     copies of the song in RAM at once and OOM-killed 1 GB devices). The C++ twin of Kotlin's
//     WavStreamWriter, and its replacement.
//
//   • `write_wav()` — the SAMPLE EDITOR (S6b). A sample is already in RAM in its entirety (the editor
//     has been drawing it), it is small, and it carries something a render never does: CUE POINTS,
//     one per slice boundary. The C++ twin of Kotlin's `core/storage/WavWriter.kt`.
//
// Byte-for-byte the same files the two Kotlin writers produced — the same RIFF/fmt/data header, the
// same `cue ` chunk, and the same float→int16 conversion: clamp to ±1, scale by 32767, TRUNCATE
// toward zero (Kotlin's `.toInt()`, not a round). That is deliberate: an existing render must not
// change by a LSB just because the writer moved to C++.
//
// ⚠️ **`read_cue_points` is the half S6a deferred, and it is here because S6b writes the other half.**
// A slice boundary survives a save/reload ONLY as a cue point in the file — `sliceMarkers` is written
// into the .ptp too, but a WAV chopped in the editor and loaded into a *different* slot (or a
// different project) has nothing but the file to carry them. S6a stated the gap plainly ("a sliced WAV
// loaded on Linux plays whole") because it had no WAV writer to pair a reader with. It has one now, so
// the round trip closes here rather than waiting for the Phase 1.5 media unification: writing markers
// that nothing can read back is not half a feature, it is a feature that silently loses data.
//
// Both writers write to "<path>.tmp" and rename on completion, so a failed or cancelled write never
// leaves a half-written .wav behind (the same atomic pattern AndroidFileSystem uses).

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace songcore {

// ─── float → int16, the one conversion both writers share ───────────────────────────────────────
//
// Clamp to ±1, scale by 32767, TRUNCATE toward zero. Kotlin writes `(clamped * 32767f).toInt()`, and
// `.toInt()` on a Float truncates — it does not round. A `std::lround` here would shift half the
// samples in every existing file by one LSB.
inline int16_t float_to_int16(float v) {
    if (v < -1.0f) v = -1.0f;
    else if (v > 1.0f) v = 1.0f;
    return static_cast<int16_t>(static_cast<int>(v * 32767.0f));
}

class WavStreamWriter {
  public:
    WavStreamWriter(const std::string& path, int sampleRate, int channels = 2)
        : path_(path), tmpPath_(path + ".tmp"), sampleRate_(sampleRate), channels_(channels) {
        file_ = std::fopen(tmpPath_.c_str(), "wb");
        if (!file_) return;
        uint8_t header[44];
        build_header(header, 0);
        std::fwrite(header, 1, sizeof(header), file_);   // placeholder sizes, patched in finish()
    }

    ~WavStreamWriter() { abort(); }

    WavStreamWriter(const WavStreamWriter&) = delete;
    WavStreamWriter& operator=(const WavStreamWriter&) = delete;

    bool is_open() const { return file_ != nullptr; }

    // Append interleaved float frames ([L0, R0, L1, R1, …]), converted to 16-bit.
    void append_interleaved(const float* data, int frames) {
        if (!file_ || frames <= 0) return;
        const int samples = frames * channels_;
        // Little-endian bytes, assembled explicitly rather than memcpy'd from int16: WAV is
        // little-endian by spec, and a big-endian host would otherwise write a silently byte-swapped
        // file. One fwrite per chunk (not per sample) keeps it fast.
        buf_.resize(static_cast<size_t>(samples) * 2);
        for (int i = 0; i < samples; ++i) {
            const uint16_t u = static_cast<uint16_t>(float_to_int16(data[i]));
            buf_[static_cast<size_t>(i) * 2 + 0] = static_cast<uint8_t>(u & 0xFF);
            buf_[static_cast<size_t>(i) * 2 + 1] = static_cast<uint8_t>((u >> 8) & 0xFF);
        }
        std::fwrite(buf_.data(), 1, buf_.size(), file_);
        framesWritten_ += frames;
    }

    // Patch the RIFF/data sizes, close, and rename to the final path. False on I/O failure or if the
    // data would overflow WAV's 32-bit size field (> 2 GB), with the temp file removed either way.
    bool finish() {
        if (!file_) return false;

        const int64_t dataSize = framesWritten_ * bytes_per_frame();
        if (dataSize > static_cast<int64_t>(INT32_MAX) - 44) {
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
        std::fclose(file_);
        file_ = nullptr;

        std::remove(path_.c_str());   // rename() fails on an existing target on Windows
        if (std::rename(tmpPath_.c_str(), path_.c_str()) != 0) {
            std::remove(tmpPath_.c_str());
            return false;
        }
        return true;
    }

    // Discard everything written so far (failed / cancelled render). Safe to call any time, and the
    // destructor calls it — so an early return can never leave the .tmp behind.
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
        std::remove(tmpPath_.c_str());
    }

    // The standard 44-byte RIFF/fmt/data header — the same layout Kotlin's WavStreamWriter wrote.
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

/** Write `bytes` to `path` via "<path>.tmp" + rename, so a failed write leaves no partial file. */
inline bool wav_write_atomic(const std::string& path, const std::vector<uint8_t>& bytes) {
    const std::string tmp = path + ".tmp";
    std::FILE* f = std::fopen(tmp.c_str(), "wb");
    if (!f) return false;
    const size_t written = bytes.empty() ? 0 : std::fwrite(bytes.data(), 1, bytes.size(), f);
    const bool   ok      = (written == bytes.size());
    std::fclose(f);
    if (!ok) {
        std::remove(tmp.c_str());
        return false;
    }
    std::remove(path.c_str());   // rename() fails on an existing target on Windows
    if (std::rename(tmp.c_str(), path.c_str()) != 0) {
        std::remove(tmp.c_str());
        return false;
    }
    return true;
}

}  // namespace detail

/**
 * Write a 16-bit PCM WAV, optionally with a `cue ` chunk marking slice boundaries.
 *
 * `left` and `right` must be the same length (frames). `channels` is 1 (write `left` only) or 2
 * (interleave both) — the CALLER decides which, because only it knows the editor's SOURCE mode:
 * a stereo sample saved as SOURCE=LEFT is a mono file, and one saved as SOURCE=STEREO is not.
 *
 * The chunk order is RIFF / fmt / data / cue, which is where Kotlin puts the cue chunk (after the
 * audio, not before it) — and a byte-compared golden pins it, so it is not free to drift.
 */
inline bool write_wav(const std::string& path, const std::vector<float>& left,
                      const std::vector<float>& right, int sampleRate,
                      const std::vector<int>& cuePoints = {}, int channels = 2) {
    if (left.size() != right.size()) return false;

    const int64_t frames        = static_cast<int64_t>(left.size());
    const int     numChannels   = (channels < 1) ? 1 : (channels > 2 ? 2 : channels);
    const int     blockAlign    = numChannels * 2;   // 16-bit
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
    detail::wav_put_u16(b, 1);                                                     // AudioFormat = PCM
    detail::wav_put_u16(b, static_cast<uint16_t>(numChannels));
    detail::wav_put_u32(b, static_cast<uint32_t>(sampleRate));
    detail::wav_put_u32(b, static_cast<uint32_t>(sampleRate * blockAlign));        // byte rate
    detail::wav_put_u16(b, static_cast<uint16_t>(blockAlign));
    detail::wav_put_u16(b, 16);                                                    // bits per sample

    // data chunk (8 + dataSize)
    detail::wav_put_tag(b, "data");
    detail::wav_put_u32(b, static_cast<uint32_t>(dataSize));
    for (int64_t i = 0; i < frames; ++i) {
        const size_t k = static_cast<size_t>(i);
        detail::wav_put_u16(b, static_cast<uint16_t>(float_to_int16(left[k])));
        if (numChannels == 2) detail::wav_put_u16(b, static_cast<uint16_t>(float_to_int16(right[k])));
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
 * ⚠️ Kotlin's `writeWavMono` writes a **STEREO** file with the mono data duplicated into both
 * channels — it forwards to `writeWav(…, samples, samples, rate, cues)` and takes the `channels = 2`
 * DEFAULT. Its own doc comment says so ("Write mono audio data to a stereo WAV file"), and CHOP is its
 * only caller, so every chop the app has ever written is a two-channel file of identical channels.
 *
 * Ported as it stands, deliberately: this is the twin of a shipped format, and "fixing" it here would
 * make the chops the SDL shell writes differ from the ones Android writes for the same sample. The
 * SAVE path is unaffected — it passes `channels` explicitly and writes a true mono file.
 */
inline bool write_wav_mono(const std::string& path, const std::vector<float>& samples, int sampleRate,
                           const std::vector<int>& cuePoints = {}) {
    return write_wav(path, samples, samples, sampleRate, cuePoints, /*channels=*/2);
}

/**
 * Read the frame positions out of a WAV's `cue ` chunk. Empty if it has none, or cannot be read.
 *
 * Frame 0 is EXCLUDED: it is the implicit start of the sample, not a slice boundary, and letting it
 * through would give every sliced file a zero-length slice 0.
 *
 * Chunk headers are walked with seeks, so only the small cue chunk is ever read into memory — this is
 * called on every sample load and once per instrument on project load, and reading a multi-MB WAV to
 * find 8 integers at the end of it is exactly the cost Kotlin's own comment says it removed.
 */
inline std::vector<int> read_cue_points(const std::string& path) {
    std::vector<int> frames;

    std::FILE* f = std::fopen(path.c_str(), "rb");
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

        // A size with the top bit set would be a >2 GB chunk (or garbage). Kotlin reads the field as a
        // SIGNED int and breaks when it goes negative; the same guard, so a malformed file cannot turn
        // the skip below into a backward seek and spin here forever.
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
