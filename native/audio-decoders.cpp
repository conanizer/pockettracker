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

// minimp4 (ISO-BMFF demux, declarations only — the implementation is vendor/minimp4/minimp4_impl.c)
// and FAAD2 (AAC decode). Both carry their own extern "C" guards.
#include "vendor/minimp4/minimp4.h"
#include <neaacdec.h>

#include <cstdint>
#include <cstdio>
#include <cstring>

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

// minimp4 reads the container sequentially through this callback. We hand it the whole file already in
// memory (a sample is a few MB), so the "read" is a bounds-checked memcpy. Return 0 on success, non-zero
// on failure — the convention minimp4 checks (`if (read_callback(...)) error`).
struct Mp4Buf { const uint8_t* data; size_t size; };
int mp4ReadCb(int64_t offset, void* buffer, size_t size, void* token) {
    const Mp4Buf* b = static_cast<const Mp4Buf*>(token);
    if (offset < 0 || (uint64_t)offset + size > b->size) return -1;
    std::memcpy(buffer, b->data + offset, size);
    return 0;
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

bool decodeMp4File(const char* path, std::vector<float>& outL, std::vector<float>& outR, int& sampleRate) {
    // Read the whole file into RAM. minimp4 reads sequentially and the index may sit at the end of the
    // stream, so buffering the file up front is both simplest and what its read callback wants; a
    // container sample is a few MB. (The convergence plan's OOM guard is a UI-level length warning on
    // the LOAD path, not this decoder's job — a container that fits in a sample is small.)
    FILE* f = std::fopen(path, "rb");
    if (!f) { LOGE("decodeMp4File: fopen failed: %s", path); return false; }
    std::fseek(f, 0, SEEK_END);
    long fsize = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    if (fsize <= 0) { std::fclose(f); LOGE("decodeMp4File: empty/unseekable file: %s", path); return false; }
    std::vector<uint8_t> buf((size_t)fsize);
    size_t rd = std::fread(buf.data(), 1, (size_t)fsize, f);
    std::fclose(f);
    if (rd != (size_t)fsize) { LOGE("decodeMp4File: short read (%zu/%ld): %s", rd, fsize, path); return false; }

    Mp4Buf token{ buf.data(), buf.size() };
    MP4D_demux_t mp4;
    // MP4D_open memset()s mp4 to zero even on its early arg-check failure, so `track` is NULL and there
    // is nothing to close on the failure path below.
    if (!MP4D_open(&mp4, mp4ReadCb, &token, (int64_t)fsize) || mp4.track_count == 0) {
        LOGE("decodeMp4File: MP4D_open failed or no tracks: %s", path);
        return false;
    }

    // First AAC audio track. handler_type 'soun' + object type 0x40 (MPEG-4 AAC). A container whose
    // audio is some other codec (e.g. ALAC, AC-3) is not ours to decode — fail cleanly, do not guess.
    int atrack = -1;
    for (unsigned t = 0; t < mp4.track_count; t++) {
        if (mp4.track[t].handler_type == MP4D_HANDLER_TYPE_SOUN &&
            mp4.track[t].object_type_indication == MP4_OBJECT_TYPE_AUDIO_ISO_IEC_14496_3) {
            atrack = (int)t;
            break;
        }
    }
    if (atrack < 0) { LOGE("decodeMp4File: no AAC audio track: %s", path); MP4D_close(&mp4); return false; }
    MP4D_track_t* tr = &mp4.track[atrack];
    if (!tr->dsi || tr->dsi_bytes == 0) {
        LOGE("decodeMp4File: AAC track has no AudioSpecificConfig: %s", path);
        MP4D_close(&mp4);
        return false;
    }

    NeAACDecHandle dec = NeAACDecOpen();
    if (!dec) { LOGE("decodeMp4File: NeAACDecOpen failed"); MP4D_close(&mp4); return false; }
    NeAACDecConfigurationPtr cfg = NeAACDecGetCurrentConfiguration(dec);
    cfg->outputFormat = FAAD_FMT_FLOAT;  // FAAD2's float output is FLOAT_SCALE (1/32768) — already [-1,1]
    cfg->downMatrix   = 0;               // keep source channels; appendBlock does the >2ch → L/R downmix
    NeAACDecSetConfiguration(dec, cfg);

    unsigned long initRate = 0;
    unsigned char initCh = 0;
    if (NeAACDecInit2(dec, tr->dsi, tr->dsi_bytes, &initRate, &initCh) < 0) {
        LOGE("decodeMp4File: NeAACDecInit2 failed: %s", path);
        NeAACDecClose(dec);
        MP4D_close(&mp4);
        return false;
    }

    // ⚠️ FAAD2 UPMIXES mono AAC to two identical channels (measured: a mono .m4a decodes with
    // fi.channels == 2 and L == R exactly). The CONTAINER's channel count is the truth — it is what the
    // old MediaCodec path read from KEY_CHANNEL_COUNT — so it, not FAAD2's per-frame count, decides mono
    // vs stereo. A mono container collapses FAAD2's duplicate to one channel (empty R, the engine's mono
    // convention); a source with no declared count falls back to FAAD2's. The interleave STRIDE is always
    // FAAD2's actual output channel count, whatever we then keep.
    const unsigned containerCh = tr->SampleDescription.audio.channelcount;

    sampleRate = 0;
    for (unsigned s = 0; s < tr->sample_count; s++) {
        unsigned fbytes = 0, ts = 0, dur = 0;
        MP4D_file_offset_t ofs = MP4D_frame_offset(&mp4, (unsigned)atrack, s, &fbytes, &ts, &dur);
        if (fbytes == 0 || ofs < 0 || (uint64_t)ofs + fbytes > (uint64_t)fsize) continue;

        NeAACDecFrameInfo fi;
        void* pcm = NeAACDecDecode(dec, &fi, buf.data() + ofs, fbytes);
        if (fi.error != 0) {
            // A first-frame error is fatal; a mid-stream glitch keeps whatever decoded (as the Opus path
            // does), so one bad packet late in a long sample does not throw the whole load away.
            LOGE("decodeMp4File: NeAACDecDecode error %d (%s) at sample %u/%u",
                 fi.error, NeAACDecGetErrorMessage(fi.error), s, tr->sample_count);
            if (outL.empty()) { NeAACDecClose(dec); MP4D_close(&mp4); return false; }
            break;
        }
        if (!pcm || fi.samples == 0 || fi.channels < 1) continue;
        if (sampleRate == 0) sampleRate = (int)fi.samplerate;  // actual output rate (post-SBR for HE-AAC)

        const int   stride     = (int)fi.channels;                                  // FAAD2's real layout
        const int   declaredCh = containerCh >= 1 ? (int)containerCh : stride;       // container wins
        const bool  keepStereo = declaredCh >= 2 && stride >= 2;
        const int   frames     = (int)(fi.samples / (unsigned)stride);
        const float* p         = (const float*)pcm;
        for (int i = 0; i < frames; i++) {
            outL.push_back(p[(size_t)i * stride]);            // ch0 → L (a mono duplicate's ch0 == ch1)
            if (keepStereo) outR.push_back(p[(size_t)i * stride + 1]);  // ch1 → R only for real stereo
        }
    }

    NeAACDecClose(dec);
    MP4D_close(&mp4);
    LOGD("decodeMp4File: %s rate=%d frames=%zu (%s)",
         outR.empty() ? "mono" : "stereo", sampleRate, outL.size(), path);
    return !outL.empty() && sampleRate > 0;
}

}  // namespace ptdec
