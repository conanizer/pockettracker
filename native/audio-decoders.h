#pragma once

#include <cstdint>

// Native compressed-audio file decoders (dr_mp3 / dr_flac / stb_vorbis). Each decodes a whole file
// into deinterleaved float channels in native memory — no Java-heap round trip, no MediaCodec. Used
// by AudioEngine::loadSampleFromCompressed to bring MP3/FLAC/OGG samples in exactly like a WAV.
//
// Convention (matches the rest of the engine): out is normalized float [-1, 1]; the left channel is
// always filled, the right only for >=2-channel sources (null for mono). For >2 channels, ch0→L and
// ch1→R, extras discarded — same downmix as the old Kotlin extractor.
//
// Returns true on success (non-empty output, sampleRate > 0); false on any open/decode failure.
namespace ptdec {

/**
 * Where a decoder puts its output: one plain buffer per channel, sized from the length the file
 * states and handed to the sample slot as it is — so a load holds the sample once, not twice.
 *
 * ⚠️ A stated length can be absent or wrong. `append` grows past it, holding the old and the new
 * buffer at once — the only moment a load holds more than its own size — and `finish` trims a large
 * overstatement.
 */
class PcmSink {
public:
    PcmSink() = default;
    PcmSink(const PcmSink&) = delete;
    PcmSink& operator=(const PcmSink&) = delete;
    ~PcmSink() { clear(); }

    // Size the buffers for `stated` frames (<= 0: the file did not say). Drops anything held before.
    // False = no room, and `out_of_memory()` says so.
    bool begin(int channels, int64_t stated);
    // Append `n` frames of `stride`-channel interleaved audio. False = no room.
    bool append(const float* interleaved, int n, int stride);
    void finish();
    void clear();

    int64_t      frames() const { return frames_; }
    bool         stereo() const { return stereo_; }
    const float* left()   const { return left_; }
    const float* right()  const { return right_; }
    bool         out_of_memory() const { return outOfMemory_; }

    // Hand both buffers over (`new[]`-allocated; the receiver `delete[]`s them). The sink is empty after.
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
// Ogg Opus (via libopus/opusfile). Opus always decodes at 48 kHz, so sampleRate is set to 48000.
// Handles both `.opus` files and Opus-in-`.ogg` (where decodeOggFile/Vorbis returns false first).
bool decodeOpusFile(const char* path, PcmSink& out, int& sampleRate);

// ISO-BMFF container holding AAC audio: `.m4a` / `.mp4` / `.m4b` / `.mov` / `.3gp` — they are all the
// same box format. minimp4 demuxes the container and hands us the first audio track's AAC access units
// plus its AudioSpecificConfig; FAAD2 decodes the AAC to normalized float. This is the native, in-place
// replacement for Android's MediaCodec/MediaExtractor path — same "container in, PCM out" job, no Java.
// sampleRate is taken from the decoded stream (post-SBR for HE-AAC, so it can exceed the container's
// stated rate). Decoder priming samples are NOT trimmed, matching the old MediaCodec extractor.
bool decodeMp4File(const char* path, PcmSink& out, int& sampleRate);

}  // namespace ptdec
