/* The single translation unit that carries minimp4's implementation.
 *
 * minimp4 is a single-header library: exactly one .c must define MINIMP4_IMPLEMENTATION before the
 * include; everyone else includes the header for declarations only. We keep it in its OWN C TU rather
 * than inside audio-decoders.cpp — the same pattern as vendor/stb_vorbis/stb_vorbis.c — because the
 * implementation block defines short, generic macros (ERROR / MALLOC / FREE / TRACE) that would risk
 * colliding with dr_mp3 / dr_flac (which share audio-decoders.cpp) or the engine's own LOGE shim.
 * Isolating it here means audio-decoders.cpp includes minimp4.h purely for the extern "C" MP4D_*
 * declarations. Only the demuxer half is used; the muxer is dropped by --gc-sections at link time. */
#define MINIMP4_IMPLEMENTATION
#include "minimp4.h"
