#pragma once

#include <cstdint>

// Whole-file decoders into float channels in [-1, 1], for loadSampleFromCompressed. Right is null
// for mono; past two channels, ch0/ch1 are kept. False on any failure.
namespace ptdec {

// One buffer per channel, sized from the file's stated length and handed to the sample slot as is,
// so a load holds the sample once. A wrong length is grown past (`append`) or trimmed (`finish`).
class PcmSink {
public:
    PcmSink() = default;
    PcmSink(const PcmSink&) = delete;
    PcmSink& operator=(const PcmSink&) = delete;
    ~PcmSink() { clear(); }

    // For `stated` frames (<= 0 = unknown); drops what was held. False = no room.
    bool begin(int channels, int64_t stated);
    // `n` interleaved frames of `stride` channels. False = no room.
    bool append(const float* interleaved, int n, int stride);
    void finish();
    void clear();

    int64_t      frames() const { return frames_; }
    bool         stereo() const { return stereo_; }
    const float* left()   const { return left_; }
    const float* right()  const { return right_; }
    bool         out_of_memory() const { return outOfMemory_; }

    // Hands both `new[]` buffers over; the sink is empty after.
    void release(float*& left, float*& right);

private:
    bool reallocate(int64_t capacity);

    float*  left_        = nullptr;
    float*  right_       = nullptr;
    int64_t frames_      = 0;
    int64_t capacity_    = 0;
    bool    stereo_      = false;
    bool    outOfMemory_ = false;
};

bool decodeMp3File (const char* path, PcmSink& out, int& sampleRate);
bool decodeFlacFile(const char* path, PcmSink& out, int& sampleRate);
bool decodeOggFile (const char* path, PcmSink& out, int& sampleRate);
// `.opus` and Opus-in-`.ogg`; always 48 kHz.
bool decodeOpusFile(const char* path, PcmSink& out, int& sampleRate);

// AAC in `.m4a` / `.mp4` / `.m4b` / `.mov` / `.3gp`: the first audio track. The rate is the decoded
// one (after SBR). Priming samples are not trimmed.
bool decodeMp4File(const char* path, PcmSink& out, int& sampleRate);

}  // namespace ptdec
