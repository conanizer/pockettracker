#include "audio-decoders.h"
#include "audio-defs.h"     // LOGD/LOGE (portable shim)
#include "byte_source.h"    // pt_fopen — every open below goes through it
#include "platform_memory.h"  // available_memory_bytes — the decode's memory guard
#include "load_progress.h"    // load_tick — a long decode reports itself, and can be cancelled

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

// Descriptor duplication for the Opus path below, which is the only decoder that takes an fd.
#if defined(_WIN32)
#include <io.h>
#else
#include <unistd.h>
#endif

namespace ptdec {

namespace {

#if defined(_WIN32)
inline int  ptFileno(FILE* f) { return _fileno(f); }   // MSVC deprecates the unprefixed spellings
inline int  ptDup(int fd)     { return _dup(fd); }
inline void ptClose(int fd)   { _close(fd); }
#else
inline int  ptFileno(FILE* f) { return fileno(f); }
inline int  ptDup(int fd)     { return dup(fd); }
inline void ptClose(int fd)   { close(fd); }
#endif

/**
 * Headroom left unclaimed, so that abandoning a decode leaves a machine that can still draw the
 * message saying so.
 */
constexpr int64_t DECODE_RESERVE_BYTES = 32ll * 1024 * 1024;

/**
 * Is there room to append `frames` more? **False means the machine ran out**, and the caller must
 * stop decoding and fail the load — a decode that keeps going here is the one that gets the process
 * killed with nothing on screen.
 *
 * ⚠️ The container's own frame count is NOT the thing to guard on: an MP3 without a Xing header does
 * not state one at all (`reserveOutput` is skipped entirely there), and a corrupt FLAC can state a
 * count in the hundreds of GB. Guarding the actual growth needs neither to be honest.
 *
 * ⭐ **The check rides on the REALLOCATION, not on the block.** `/proc/meminfo` costs ~50 us and a
 * long file is thousands of blocks; but a vector only takes memory when it outgrows its capacity, so
 * testing exactly there is O(log n) reads placed on precisely the events that can exhaust a machine.
 * A file that fits its reservation is never checked at all.
 *
 * ⚠️ `available <= 0` is "the platform cannot say", and must never refuse.
 */
inline bool decode_has_room(const std::vector<float>& L, const std::vector<float>& R, int frames) {
    if (L.size() + static_cast<size_t>(frames) <= L.capacity()) return true;
    const int64_t held      = static_cast<int64_t>(L.capacity() + R.capacity()) * sizeof(float);
    const int64_t available = pt::available_memory_bytes();
    // A regrow holds the old buffer and the new one at once and the new is about twice the old, so
    // what is about to be TAKEN on top of what is already resident is roughly 2x `held`.
    if (available <= 0) return true;
    return held * 2 <= available - DECODE_RESERVE_BYTES;
}

// Deinterleave a freshly-decoded float block into L (always) and R (only when channels >= 2).
// For >2 channels keep ch0/ch1 and drop the rest — same downmix the old Kotlin extractor used.
//
// **False means STOP**, for either of two reasons, and the caller asks `pt::load_cancelled()` which:
// the machine ran out (decode_has_room), or the user cancelled the load (load_tick). ⭐ One return
// value for both because the unwind is identical — the difference is only in the message at the end,
// and choosing it where the message is written is one question instead of five.
//
// `totalFrames` is what the container said its length was, or 0 when it did not say (an mp3 with no
// Xing header). Zero reports "unknown" rather than a made-up percentage.
inline bool appendBlock(const float* interleaved, int frames, int channels,
                        std::vector<float>& L, std::vector<float>& R, int64_t totalFrames) {
    if (!decode_has_room(L, R, frames)) return false;
    for (int i = 0; i < frames; i++) {
        L.push_back(interleaved[(size_t)i * channels]);
        if (channels >= 2) R.push_back(interleaved[(size_t)i * channels + 1]);
    }
    // Once per block, which is 4k-11k frames — often enough that a cancel lands within a frame or two
    // and rare enough that the report costs nothing against the decode it is reporting on.
    const float done = (totalFrames > 0)
                           ? static_cast<float>(L.size()) / static_cast<float>(totalFrames)
                           : -1.0f;
    return pt::load_tick(done > 1.0f ? 1.0f : done);
}

// Reserve the finished length before decoding, so a decode is ONE allocation per channel instead of a
// geometric growth series. It matters more than it looks: at every regrow the old and new buffers are
// both live, so the L channel alone peaks near 3x its final size — on top of the 2x the sample slot
// already costs when loadSampleStereo copies these out (audio-engine.cpp documents that 2x and accepts
// it; the growth churn is a third copy nobody costed).
//
// `frames <= 0` means the container did not say, and growth takes over exactly as before.
//
// ⚠️ THE CAP IS NOT A LENGTH LIMIT ON SAMPLES — nothing here refuses to decode a longer file, and the
// vector grows past it normally. It bounds what a *header* can make us allocate before a single byte
// of audio is decoded: FLAC's frame count is a 36-bit field, so a corrupt-but-parseable header can ask
// for hundreds of GB and turn a file that loads today into an uncaught bad_alloc. 30M frames is ~10
// minutes of 48 kHz stereo — already past what a 512 MB handheld can hold resident.
inline void reserveOutput(int64_t frames, int channels, std::vector<float>& L, std::vector<float>& R) {
    constexpr int64_t kMaxReserveFrames = 30'000'000;
    if (frames <= 0) return;
    const size_t n = static_cast<size_t>(frames < kMaxReserveFrames ? frames : kMaxReserveFrames);
    L.reserve(n);
    if (channels >= 2) R.reserve(n);
}

// ─── stdio over a FILE*, for the two decoders that have no FILE* entry point ─────────────────────
//
// dr_mp3 and dr_flac open a stream through a read/seek/tell triple; their `*_open_file` twins are
// the same triple plus an `fopen` they own. We need the open to be `pt_fopen`, so the triple comes
// here and the `_file` variants are unused.
//
// ⚠️ **`drmp3_uninit` / `drflac_close` will NOT close a handle they did not open** — both decide by
// comparing the read callback against their own stdio one, so with these installed the `fclose` is
// ours, on every path including the failed open.
//
// The two triples are byte-identical in body and cannot be shared: each library declares its own
// `bool32`, `int64` and seek-origin types, and a cast between the function-pointer types would be
// undefined behaviour rather than a saving.
int64_t stdioTell(FILE* f) {
#if defined(_MSC_VER)
    return _ftelli64(f);   // MSVC's ftell is a long, i.e. 32-bit even on x64
#else
    return (int64_t)ftello(f);
#endif
}
int stdioWhence(bool cur, bool end) { return cur ? SEEK_CUR : (end ? SEEK_END : SEEK_SET); }

size_t       mp3OnRead(void* ud, void* out, size_t n) { return std::fread(out, 1, n, (FILE*)ud); }
drmp3_bool32 mp3OnSeek(void* ud, int offset, drmp3_seek_origin origin) {
    return std::fseek((FILE*)ud, offset,
                      stdioWhence(origin == DRMP3_SEEK_CUR, origin == DRMP3_SEEK_END)) == 0;
}
drmp3_bool32 mp3OnTell(void* ud, drmp3_int64* cursor) {
    *cursor = (drmp3_int64)stdioTell((FILE*)ud);
    return DRMP3_TRUE;
}

size_t        flacOnRead(void* ud, void* out, size_t n) { return std::fread(out, 1, n, (FILE*)ud); }
drflac_bool32 flacOnSeek(void* ud, int offset, drflac_seek_origin origin) {
    return std::fseek((FILE*)ud, offset,
                      stdioWhence(origin == DRFLAC_SEEK_CUR, origin == DRFLAC_SEEK_END)) == 0;
}
drflac_bool32 flacOnTell(void* ud, drflac_int64* cursor) {
    *cursor = (drflac_int64)stdioTell((FILE*)ud);
    return DRFLAC_TRUE;
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
    FILE* f = pt_fopen(path, "rb");
    if (!f) { LOGE("decodeMp3File: cannot open %s", path); return false; }

    drmp3 mp3;
    if (!drmp3_init(&mp3, mp3OnRead, mp3OnSeek, mp3OnTell, nullptr, f, nullptr)) {
        LOGE("decodeMp3File: drmp3_init failed: %s", path);
        std::fclose(f);
        return false;
    }
    const int channels = (int)mp3.channels;
    sampleRate = (int)mp3.sampleRate;
    if (channels < 1) { drmp3_uninit(&mp3); std::fclose(f); return false; }

    // DRMP3_UINT64_MAX means "no Xing/LAME header, length unknown" — reserveOutput ignores it, and
    // so does the progress report: `total` stays 0 and the box shows a moving bar with no percentage.
    const int64_t total = (mp3.totalPCMFrameCount != DRMP3_UINT64_MAX)
                              ? (int64_t)mp3.totalPCMFrameCount : 0;
    if (total > 0) reserveOutput(total, channels, outL, outR);

    const drmp3_uint64 CHUNK = 8192;  // frames per read
    std::vector<float> block((size_t)CHUNK * channels);
    drmp3_uint64 got;
    bool room = true;
    while ((got = drmp3_read_pcm_frames_f32(&mp3, CHUNK, block.data())) > 0)
        if (!appendBlock(block.data(), (int)got, channels, outL, outR, total)) { room = false; break; }
    drmp3_uninit(&mp3);
    std::fclose(f);
    if (!room) { LOGE("decodeMp3File: %s at %zu frames: %s",
                      pt::load_cancelled() ? "cancelled" : "out of memory", outL.size(), path);
                 return false; }
    LOGD("decodeMp3File: ch=%d rate=%d frames=%zu", channels, sampleRate, outL.size());
    return !outL.empty();
}

bool decodeFlacFile(const char* path, std::vector<float>& outL, std::vector<float>& outR, int& sampleRate) {
    FILE* f = pt_fopen(path, "rb");
    if (!f) { LOGE("decodeFlacFile: cannot open %s", path); return false; }

    drflac* flac = drflac_open(flacOnRead, flacOnSeek, flacOnTell, f, nullptr);
    if (!flac) {
        LOGE("decodeFlacFile: drflac_open failed: %s", path);
        std::fclose(f);
        return false;
    }
    const int channels = (int)flac->channels;
    sampleRate = (int)flac->sampleRate;
    if (channels < 1) { drflac_close(flac); std::fclose(f); return false; }

    const int64_t total = (int64_t)flac->totalPCMFrameCount;
    reserveOutput(total, channels, outL, outR);

    const drflac_uint64 CHUNK = 8192;
    std::vector<float> block((size_t)CHUNK * channels);
    drflac_uint64 got;
    bool room = true;
    while ((got = drflac_read_pcm_frames_f32(flac, CHUNK, block.data())) > 0)
        if (!appendBlock(block.data(), (int)got, channels, outL, outR, total)) { room = false; break; }
    drflac_close(flac);
    std::fclose(f);
    if (!room) { LOGE("decodeFlacFile: %s at %zu frames: %s",
                      pt::load_cancelled() ? "cancelled" : "out of memory", outL.size(), path);
                 return false; }
    LOGD("decodeFlacFile: ch=%d rate=%d frames=%zu", channels, sampleRate, outL.size());
    return !outL.empty();
}

bool decodeOggFile(const char* path, std::vector<float>& outL, std::vector<float>& outR, int& sampleRate) {
    FILE* f = pt_fopen(path, "rb");
    if (!f) { LOGE("decodeOggFile: cannot open %s", path); return false; }

    // ⚠️ **The handle transfers exclusively.** With close_on_free set, stb_vorbis owns `f` from here
    // — it closes it in `stb_vorbis_close` AND on a failed open — and it is corrupted by a seek from
    // anyone else, so nothing below may touch `f` again. This is what `stb_vorbis_open_filename`
    // does with its own `fopen`, minus the `fopen`.
    int err = 0;
    stb_vorbis* v = stb_vorbis_open_file(f, /*close_handle_on_close=*/1, &err, nullptr);
    if (!v) {
        // err is a STBVorbisError; common ones: 1=need_more_data, 2=invalid_api_mixing,
        // 33=ogg_skeleton_not_supported, 34=unexpected_eof. An Opus-in-Ogg file fails here (stb_vorbis
        // decodes Vorbis only, not Opus).
        LOGE("decodeOggFile: stb_vorbis_open_file failed (err=%d): %s", err, path);
        return false;
    }
    stb_vorbis_info info = stb_vorbis_get_info(v);
    const int channels = info.channels;
    sampleRate = (int)info.sample_rate;
    if (channels < 1) { stb_vorbis_close(v); return false; }

    // 0 when the stream length is not derivable (stb_vorbis returns 0 rather than an error).
    const int64_t total = (int64_t)stb_vorbis_stream_length_in_samples(v);
    reserveOutput(total, channels, outL, outR);

    const int CHUNK = 4096;  // frames per read
    std::vector<float> block((size_t)CHUNK * channels);
    int got;
    // num_floats is the buffer capacity in floats; returns frames (samples per channel) written, 0 at EOF.
    bool room = true;
    while ((got = stb_vorbis_get_samples_float_interleaved(v, channels, block.data(), CHUNK * channels)) > 0)
        if (!appendBlock(block.data(), got, channels, outL, outR, total)) { room = false; break; }
    stb_vorbis_close(v);
    if (!room) { LOGE("decodeOggFile: %s at %zu frames: %s",
                      pt::load_cancelled() ? "cancelled" : "out of memory", outL.size(), path);
                 return false; }
    LOGD("decodeOggFile: ch=%d rate=%d frames=%zu", channels, sampleRate, outL.size());
    return !outL.empty();
}

bool decodeOpusFile(const char* path, std::vector<float>& outL, std::vector<float>& outR, int& sampleRate) {
    FILE* f = pt_fopen(path, "rb");
    if (!f) { LOGE("decodeOpusFile: cannot open %s", path); return false; }

    // ⚠️ **opusfile is the one consumer that wants a DESCRIPTOR, not a `FILE*`** — `op_fdopen` builds
    // its own stdio wrapper around the fd and owns it from then on. So the descriptor is duplicated
    // and our handle closed immediately: two `FILE*`s over one fd would each close it.
    const int fd = ptDup(ptFileno(f));
    std::fclose(f);
    if (fd < 0) { LOGE("decodeOpusFile: dup failed: %s", path); return false; }

    OpusFileCallbacks cb;
    void* stream = op_fdopen(&cb, fd, "rb");
    if (!stream) { ptClose(fd); LOGE("decodeOpusFile: op_fdopen failed: %s", path); return false; }

    int err = 0;
    // On failure opusfile does NOT take the stream — closing it is ours, through the callbacks it
    // just filled in (which is what `op_open_file` does internally on the same path).
    OggOpusFile* of = op_open_callbacks(stream, &cb, nullptr, 0, &err);
    if (!of) {
        cb.close(stream);
        LOGE("decodeOpusFile: op_open_callbacks failed (err=%d): %s", err, path);
        return false;
    }
    const int channels = op_channel_count(of, -1);
    sampleRate = 48000;  // Opus always decodes at 48 kHz regardless of the original rate
    if (channels < 1) { op_free(of); return false; }

    // Negative on error, or for a link-index total the stream cannot give — reserveOutput ignores it,
    // and so does the progress report (0 = "no percentage available").
    const int64_t pcmTotal = (int64_t)op_pcm_total(of, -1);
    const int64_t total    = pcmTotal > 0 ? pcmTotal : 0;
    reserveOutput(pcmTotal, channels, outL, outR);

    // op_read_float wants room for >= 120 ms/channel (5760 frames at 48 kHz); use a generous chunk.
    const int CHUNK = 11520;  // frames
    std::vector<float> block((size_t)CHUNK * channels);
    int li = 0;
    int got;
    bool room = true;
    while ((got = op_read_float(of, block.data(), (int)block.size(), &li)) > 0)
        if (!appendBlock(block.data(), got, channels, outL, outR, total)) { room = false; break; }
    if (got < 0) LOGE("decodeOpusFile: op_read_float error %d (using %zu decoded frames): %s",
                      got, outL.size(), path);  // keep whatever decoded before the error
    op_free(of);
    if (!room) { LOGE("decodeOpusFile: %s at %zu frames: %s",
                      pt::load_cancelled() ? "cancelled" : "out of memory", outL.size(), path);
                 return false; }
    LOGD("decodeOpusFile: ch=%d rate=48000 frames=%zu", channels, outL.size());
    return !outL.empty();
}

bool decodeMp4File(const char* path, std::vector<float>& outL, std::vector<float>& outR, int& sampleRate) {
    // Read the whole file into RAM. minimp4 reads sequentially and the index may sit at the end of the
    // stream, so buffering the file up front is both simplest and what its read callback wants; a
    // container sample is a few MB. (The convergence plan's OOM guard is a UI-level length warning on
    // the LOAD path, not this decoder's job — a container that fits in a sample is small.)
    FILE* f = pt_fopen(path, "rb");
    if (!f) { LOGE("decodeMp4File: cannot open %s", path); return false; }
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
    cfg->downMatrix   = 0;               // keep source channels; the append loop below takes ch0/ch1
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

        // Reserve on the FIRST decoded packet rather than up front, because the container gives a
        // packet count and not an output length: an AAC packet is 1024 frames, or 2048 once SBR
        // doubles it, and only a decoded frame says which this stream is. Every later packet is the
        // same size, so `sample_count x this one` is the whole file. Also the earliest point the L/R
        // decision (keepStereo) is known.
        if (outL.empty())
            reserveOutput((int64_t)tr->sample_count * frames, keepStereo ? 2 : 1, outL, outR);
        // ⚠️ This path appends INLINE rather than through appendBlock — the interleave stride and the
        // keepStereo rule are its own — so it needs the memory guard spelled out here. It is the one
        // decoder where "every append goes through one helper" is not true.
        if (!decode_has_room(outL, outR, frames)) {
            NeAACDecClose(dec);
            MP4D_close(&mp4);
            LOGE("decodeMp4File: out of memory at %zu frames: %s", outL.size(), path);
            return false;
        }
        for (int i = 0; i < frames; i++) {
            outL.push_back(p[(size_t)i * stride]);            // ch0 → L (a mono duplicate's ch0 == ch1)
            if (keepStereo) outR.push_back(p[(size_t)i * stride + 1]);  // ch1 → R only for real stereo
        }
        // ⚠️ The report is spelled out here for the same reason the guard above is: this path does not
        // go through appendBlock. Its fraction is the better one though — the container states a PACKET
        // count up front, so this is exact from the first packet rather than from a decoded length.
        if (!pt::load_tick((float)(s + 1) / (float)tr->sample_count)) {
            NeAACDecClose(dec);
            MP4D_close(&mp4);
            LOGE("decodeMp4File: cancelled at %zu frames: %s", outL.size(), path);
            return false;
        }
    }

    NeAACDecClose(dec);
    MP4D_close(&mp4);
    LOGD("decodeMp4File: %s rate=%d frames=%zu (%s)",
         outR.empty() ? "mono" : "stereo", sampleRate, outL.size(), path);
    return !outL.empty() && sampleRate > 0;
}

}  // namespace ptdec
