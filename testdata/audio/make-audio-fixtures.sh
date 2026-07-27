#!/usr/bin/env bash
# Regenerate the AAC decode fixtures for tools/ptaac (convergence media-unification).
#
# These prove the vendored minimp4 + FAAD2 decoder (native/audio-decoders.cpp decodeMp4File) actually
# decodes AAC — the same "a dependency vendored but never called is dead code" discipline ptdecode
# applies to the PNG reader. ffmpeg is the INDEPENDENT encoder (nothing to do with FAAD2), and the
# fixtures are pure sine tones of KNOWN frequency, so ptaac can assert the decoded energy sits at the
# right frequency in the right channel without a byte-golden (AAC is lossy). The tone frequencies are
# the invariant, hardcoded in ptaac/main.cpp — keep the two in sync if you change them here.
#
# Requires ffmpeg with the native 'aac' encoder (no libfdk needed). Run from anywhere:
#   testdata/audio/make-audio-fixtures.sh
#
# Committed binaries; this script never runs in CI (CI has no ffmpeg). Re-run it only on a deliberate
# change to the fixture set. See testdata/README.md §4 (`audio/`) and tools/ptaac/main.cpp's header.
set -euo pipefail
cd "$(dirname "$0")"

command -v ffmpeg >/dev/null 2>&1 || { echo "make-audio-fixtures: ffmpeg not found on PATH" >&2; exit 2; }

RATE=44100
DUR=2

# Stereo AAC-LC: L=440 Hz, R=660 Hz — proves channels, channel ORDER, and sample rate.
ffmpeg -hide_banner -loglevel error -y \
  -f lavfi -i "sine=frequency=440:sample_rate=$RATE:duration=$DUR" \
  -f lavfi -i "sine=frequency=660:sample_rate=$RATE:duration=$DUR" \
  -filter_complex "[0:a][1:a]join=inputs=2:channel_layout=stereo[a]" \
  -map "[a]" -c:a aac -b:a 128k tone_stereo.m4a

# Mono AAC-LC: 440 Hz — proves mono decodes to a single channel (FAAD2 upmixes mono to two identical
# channels; decodeMp4File collapses that back using the container's channel count, as MediaCodec did).
ffmpeg -hide_banner -loglevel error -y \
  -f lavfi -i "sine=frequency=440:sample_rate=$RATE:duration=$DUR" \
  -c:a aac -b:a 96k tone_mono.m4a

echo "wrote:"
ls -l tone_stereo.m4a tone_mono.m4a
