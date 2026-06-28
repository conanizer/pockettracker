#include "audio-decoders.h"
#include "audio-defs.h"   // LOGD/LOGE (portable shim)

#define DR_MP3_IMPLEMENTATION
#include "vendor/dr_mp3/dr_mp3.h"

#define DR_FLAC_IMPLEMENTATION
#include "vendor/dr_flac/dr_flac.h"

// stb_vorbis is compiled as its own C translation unit (see CMakeLists.txt). Here we only need its
// declarations — STB_VORBIS_HEADER_ONLY pulls in the public API without a second copy of the
// implementation. extern "C" so these C++ references resolve against the C-compiled symbols.
extern "C" {
#define STB_VORBIS_HEADER_ONLY
#include "vendor/stb_vorbis/stb_vorbis.c"
}

// opusfile.h carries its own extern "C" guards, so it's included directly (no manual wrapper).
#include <opusfile.h>

namespace ptdec {

namespace {
// Deinterleave a freshly-decoded float block into L (always) and R (only when channels >= 2).
// For >2 channels keep ch0/ch1 and drop the rest — same downmix the old Kotlin extractor used.
inline void appendBlock(const float* interleaved, int frames, int channels,
                        std::vector<float>& L, std::vector<float>& R) {
    for (int i = 0; i < frames; i++) {
        L.push_back(interleaved[(size_t)i * channels]);
        if (channels >= 2) R.push_back(interleaved[(size_t)i * channels + 1]);
    }
}
}  // namespace

bool decodeMp3File(const char* path, std::vector<float>& outL, std::vector<float>& outR, int& sampleRate) {
    drmp3 mp3;
    if (!drmp3_init_file(&mp3, path, nullptr)) {
        LOGE("decodeMp3File: drmp3_init_file failed: %s", path);
        return false;
    }
    const int channels = (int)mp3.channels;
    sampleRate = (int)mp3.sampleRate;
    if (channels < 1) { drmp3_uninit(&mp3); return false; }

    const drmp3_uint64 CHUNK = 8192;  // frames per read
    std::vector<float> block((size_t)CHUNK * channels);
    drmp3_uint64 got;
    while ((got = drmp3_read_pcm_frames_f32(&mp3, CHUNK, block.data())) > 0)
        appendBlock(block.data(), (int)got, channels, outL, outR);
    drmp3_uninit(&mp3);
    LOGD("decodeMp3File: ch=%d rate=%d frames=%zu", channels, sampleRate, outL.size());
    return !outL.empty();
}

bool decodeFlacFile(const char* path, std::vector<float>& outL, std::vector<float>& outR, int& sampleRate) {
    drflac* flac = drflac_open_file(path, nullptr);
    if (!flac) {
        LOGE("decodeFlacFile: drflac_open_file failed: %s", path);
        return false;
    }
    const int channels = (int)flac->channels;
    sampleRate = (int)flac->sampleRate;
    if (channels < 1) { drflac_close(flac); return false; }

    const drflac_uint64 CHUNK = 8192;
    std::vector<float> block((size_t)CHUNK * channels);
    drflac_uint64 got;
    while ((got = drflac_read_pcm_frames_f32(flac, CHUNK, block.data())) > 0)
        appendBlock(block.data(), (int)got, channels, outL, outR);
    drflac_close(flac);
    LOGD("decodeFlacFile: ch=%d rate=%d frames=%zu", channels, sampleRate, outL.size());
    return !outL.empty();
}

bool decodeOggFile(const char* path, std::vector<float>& outL, std::vector<float>& outR, int& sampleRate) {
    int err = 0;
    stb_vorbis* v = stb_vorbis_open_filename(path, &err, nullptr);
    if (!v) {
        // err is a STBVorbisError; common ones: 1=need_more_data, 2=invalid_api_mixing,
        // 33=ogg_skeleton_not_supported, 34=unexpected_eof. An Opus-in-Ogg file fails here (stb_vorbis
        // decodes Vorbis only, not Opus).
        LOGE("decodeOggFile: stb_vorbis_open_filename failed (err=%d): %s", err, path);
        return false;
    }
    stb_vorbis_info info = stb_vorbis_get_info(v);
    const int channels = info.channels;
    sampleRate = (int)info.sample_rate;
    if (channels < 1) { stb_vorbis_close(v); return false; }

    const int CHUNK = 4096;  // frames per read
    std::vector<float> block((size_t)CHUNK * channels);
    int got;
    // num_floats is the buffer capacity in floats; returns frames (samples per channel) written, 0 at EOF.
    while ((got = stb_vorbis_get_samples_float_interleaved(v, channels, block.data(), CHUNK * channels)) > 0)
        appendBlock(block.data(), got, channels, outL, outR);
    stb_vorbis_close(v);
    LOGD("decodeOggFile: ch=%d rate=%d frames=%zu", channels, sampleRate, outL.size());
    return !outL.empty();
}

bool decodeOpusFile(const char* path, std::vector<float>& outL, std::vector<float>& outR, int& sampleRate) {
    int err = 0;
    OggOpusFile* of = op_open_file(path, &err);
    if (!of) {
        LOGE("decodeOpusFile: op_open_file failed (err=%d): %s", err, path);
        return false;
    }
    const int channels = op_channel_count(of, -1);
    sampleRate = 48000;  // Opus always decodes at 48 kHz regardless of the original rate
    if (channels < 1) { op_free(of); return false; }

    // op_read_float wants room for >= 120 ms/channel (5760 frames at 48 kHz); use a generous chunk.
    const int CHUNK = 11520;  // frames
    std::vector<float> block((size_t)CHUNK * channels);
    int li = 0;
    int got;
    while ((got = op_read_float(of, block.data(), (int)block.size(), &li)) > 0)
        appendBlock(block.data(), got, channels, outL, outR);
    if (got < 0) LOGE("decodeOpusFile: op_read_float error %d (using %zu decoded frames): %s",
                      got, outL.size(), path);  // keep whatever decoded before the error
    op_free(of);
    LOGD("decodeOpusFile: ch=%d rate=48000 frames=%zu", channels, outL.size());
    return !outL.empty();
}

}  // namespace ptdec
