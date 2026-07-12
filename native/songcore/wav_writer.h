#ifndef POCKETTRACKER_SONGCORE_WAV_WRITER_H
#define POCKETTRACKER_SONGCORE_WAV_WRITER_H

// ─── Streaming 16-bit PCM WAV writer ─────────────────────────────────────────────────────────────
//
// The C++ twin of Kotlin's WavStreamWriter, and its replacement: open → append_interleaved() per
// render chunk → finish(). Peak memory is one chunk, not one song, which is why the render is chunked
// at all (a full-song float render used to hold ~4 copies of the song in RAM at once and OOM-killed
// 1 GB devices).
//
// Byte-for-byte the same file the Kotlin writer produced — the same 44-byte RIFF/fmt/data header, and
// the same float→int16 conversion: clamp to ±1, scale by 32767, TRUNCATE toward zero (Kotlin's
// `.toInt()`, not a round). That is deliberate: an existing render must not change by a LSB just
// because the writer moved to C++.
//
// Writes to "<path>.tmp" and renames on finish, so a failed or cancelled render never leaves a
// half-written .wav behind (the same atomic pattern AndroidFileSystem uses).

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace songcore {

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
            float v = data[i];
            if (v < -1.0f) v = -1.0f;
            else if (v > 1.0f) v = 1.0f;
            const uint16_t u = static_cast<uint16_t>(
                static_cast<int16_t>(static_cast<int>(v * 32767.0f)));   // truncate, as Kotlin's .toInt()
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

}  // namespace songcore

#endif  // POCKETTRACKER_SONGCORE_WAV_WRITER_H
