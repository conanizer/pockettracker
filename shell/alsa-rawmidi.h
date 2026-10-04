#ifndef POCKETTRACKER_SHELL_ALSA_RAWMIDI_H
#define POCKETTRACKER_SHELL_ALSA_RAWMIDI_H

// alsa-rawmidi.{h,cpp} — the ONE libasound loader and the ONE rawmidi enumerator on Linux, shared by
// `AlsaMidiOut` and `AlsaMidiIn`: the two directions differ by one integer, so two copies could
// only disagree about which devices exist.
//
// ⚠️ THE PROTOTYPES BELOW ARE HAND-COPIED, AND A NORMAL BUILD CHECKS NOTHING. dlopen rather than
// `-lasound` (midi-out-alsa.cpp says why), so a mis-copied signature compiles, links, and corrupts
// the stack at runtime on someone else's device. Check every field against <alsa/asoundlib.h>
// after touching the list.

#include <cstddef>
#include <string>
#include <vector>

#if defined(__linux__) && !defined(__ANDROID__)

namespace ptshell {
namespace alsa_detail {

/**
 * The libasound entry points the two rawmidi backends use, between them.
 *
 * The opaque handles (snd_ctl_t*, snd_rawmidi_t*, snd_rawmidi_info_t*) are all pointer-to-incomplete
 * in ALSA's own headers, so `void*` here is the same ABI and keeps this header dependency-free.
 */
struct AlsaApi {
    int         (*card_next)(int* card);
    int         (*ctl_open)(void** ctl, const char* name, int mode);
    int         (*ctl_close)(void* ctl);
    int         (*ctl_rawmidi_next_device)(void* ctl, int* device);
    int         (*ctl_rawmidi_info)(void* ctl, void* info);
    int         (*rawmidi_info_malloc)(void** info);
    void        (*rawmidi_info_free)(void* info);
    void        (*rawmidi_info_set_device)(void* info, unsigned int val);
    void        (*rawmidi_info_set_subdevice)(void* info, unsigned int val);
    void        (*rawmidi_info_set_stream)(void* info, int val);   // snd_rawmidi_stream_t
    const char* (*rawmidi_info_get_name)(const void* info);
    int         (*rawmidi_open)(void** in, void** out, const char* name, int mode);
    int         (*rawmidi_close)(void* rmidi);
    ptrdiff_t   (*rawmidi_write)(void* rmidi, const void* buffer, size_t size);   // ssize_t
    ptrdiff_t   (*rawmidi_read)(void* rmidi, void* buffer, size_t size);          // ssize_t
    int         (*rawmidi_drain)(void* rmidi);
    const char* (*strerror_fn)(int errnum);
};

/** alsa/rawmidi.h's `snd_rawmidi_stream_t`. OUTPUT is 0, INPUT is 1 — see the note in scan(). */
constexpr int STREAM_OUTPUT = 0;
constexpr int STREAM_INPUT  = 1;
/** alsa/rawmidi.h's SND_RAWMIDI_NONBLOCK. OUT opens BLOCKING (mode 0); IN opens with this. */
constexpr int NONBLOCK = 0x0002;

/** One enumerated rawmidi port, in whichever direction was asked for. */
struct RawmidiDevice {
    std::string name;   // what the MIDI screen shows, and what settings.json stores
    std::string hw;     // "hw:C,D" — the thing to open. NOT stable across a replug; the name is.
};

/**
 * dlopen libasound.so.2 and resolve every field of `api`. Returns the library handle, or nullptr when
 * the library is absent or a symbol is missing — in which case MIDI in that direction is simply
 * unavailable and the screen's row draws `OFF  NO PORTS`, which is a state it already has a design for.
 *
 * `who` is "OUT" or "IN". ⚠️ IT PRINTS ONE LINE UNCONDITIONALLY ON SUCCESS: a successful dlopen is
 * silent, and "loaded, no devices" must be distinguishable from "not in this binary" — on a
 * handheld, no devices is also what a bad cable looks like.
 *
 * ⚠️ Never `dlclose`d: SDL's own ALSA audio backend holds the same library (dlopen refcounts).
 */
void* load_alsa(AlsaApi& api, const char* who);

/**
 * Every rawmidi device on every card that has a stream in direction `stream`.
 *
 * ⚠️ `stream` IS THE FILTER AND THE ONLY PER-DIRECTION FACT: `snd_ctl_rawmidi_info` answers -ENXIO
 * for a device with no stream in that direction. Wrong, it would hide every port — which looks like
 * "no MIDI devices", not a bug.
 */
void scan_rawmidi(const AlsaApi& api, int stream, std::vector<RawmidiDevice>& out);

}  // namespace alsa_detail
}  // namespace ptshell

#endif  // __linux__ && !__ANDROID__
#endif  // POCKETTRACKER_SHELL_ALSA_RAWMIDI_H
