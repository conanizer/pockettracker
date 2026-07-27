#pragma once

#include <vector>

// Native compressed-audio file decoders (dr_mp3 / dr_flac / stb_vorbis). Each decodes a whole file
// into deinterleaved float channels in native memory — no Java-heap round trip, no MediaCodec. Used
// by AudioEngine::loadSampleFromCompressed to bring MP3/FLAC/OGG samples in exactly like a WAV.
//
// Convention (matches the rest of the engine): out is normalized float [-1, 1]; outL is always
// filled; outR is filled only for >=2-channel sources (left EMPTY for mono). For >2 channels, ch0→L
// and ch1→R, extras discarded — same downmix as the old Kotlin extractor.
//
// Returns true on success (non-empty output, sampleRate > 0); false on any open/decode failure.
namespace ptdec {

bool decodeMp3File (const char* path, std::vector<float>& outL, std::vector<float>& outR, int& sampleRate);
bool decodeFlacFile(const char* path, std::vector<float>& outL, std::vector<float>& outR, int& sampleRate);
bool decodeOggFile (const char* path, std::vector<float>& outL, std::vector<float>& outR, int& sampleRate);
// Ogg Opus (via libopus/opusfile). Opus always decodes at 48 kHz, so sampleRate is set to 48000.
// Handles both `.opus` files and Opus-in-`.ogg` (where decodeOggFile/Vorbis returns false first).
bool decodeOpusFile(const char* path, std::vector<float>& outL, std::vector<float>& outR, int& sampleRate);

// ISO-BMFF container holding AAC audio: `.m4a` / `.mp4` / `.m4b` / `.mov` / `.3gp` — they are all the
// same box format. minimp4 demuxes the container and hands us the first audio track's AAC access units
// plus its AudioSpecificConfig; FAAD2 decodes the AAC to normalized float. This is the native, in-place
// replacement for Android's MediaCodec/MediaExtractor path — same "container in, PCM out" job, no Java.
// sampleRate is taken from the decoded stream (post-SBR for HE-AAC, so it can exceed the container's
// stated rate). Decoder priming samples are NOT trimmed, matching the old MediaCodec extractor.
bool decodeMp4File(const char* path, std::vector<float>& outL, std::vector<float>& outR, int& sampleRate);

}  // namespace ptdec
