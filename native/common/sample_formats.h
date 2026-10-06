#pragma once

#include <cstring>

// ─── THE SAMPLE FORMATS ──────────────────────────────────────────────────────────────────────────
//
// The one list of file extensions a SAMPLER instrument loads, each with the decoder that reads it.
// The browser offers exactly these, songcore routes a load by them, and the engine picks the decoder
// from them — a new format is a row here plus its arm in `loadSampleFromCompressed`.
//
// Raw `.aac` (ADTS) stays out: a bare stream, not a container the MP4 demuxer can open.

namespace pt {

enum class SampleDecoder { NONE, WAV, MP3, FLAC, OGG, OPUS, MP4 };

struct SampleFormat {
    const char*   ext;   // lowercase, no dot
    SampleDecoder decoder;
};

inline constexpr SampleFormat SAMPLE_FORMATS[] = {
    {"wav",  SampleDecoder::WAV},
    {"mp3",  SampleDecoder::MP3},
    {"flac", SampleDecoder::FLAC},
    {"ogg",  SampleDecoder::OGG},    // Vorbis or Opus inside
    {"opus", SampleDecoder::OPUS},
    {"m4a",  SampleDecoder::MP4},    // the ISO box format, AAC inside
    {"mp4",  SampleDecoder::MP4},
    {"m4b",  SampleDecoder::MP4},
    {"mov",  SampleDecoder::MP4},
    {"3gp",  SampleDecoder::MP4},
};

/** The decoder for a LOWERCASE extension (no dot), or NONE. */
inline SampleDecoder sample_decoder_for(const char* ext) {
    for (const SampleFormat& f : SAMPLE_FORMATS)
        if (std::strcmp(f.ext, ext) == 0) return f.decoder;
    return SampleDecoder::NONE;
}

}  // namespace pt
