#pragma once

#include <cstring>

// The file extensions a sampler instrument loads, and their decoders — the browser, songcore and the
// engine all read this. A new format is a row here plus its arm in `loadSampleFromCompressed`.
// Raw `.aac` is out: the MP4 demuxer cannot open a bare stream.

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

// The decoder for a lowercase extension (no dot), or NONE.
inline SampleDecoder sample_decoder_for(const char* ext) {
    for (const SampleFormat& f : SAMPLE_FORMATS)
        if (std::strcmp(f.ext, ext) == 0) return f.decoder;
    return SampleDecoder::NONE;
}

}  // namespace pt
