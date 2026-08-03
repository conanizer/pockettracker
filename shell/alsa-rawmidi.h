#ifndef POCKETTRACKER_SHELL_ALSA_RAWMIDI_H
#define POCKETTRACKER_SHELL_ALSA_RAWMIDI_H

// alsa-rawmidi.{h,cpp} — the ONE libasound loader and the ONE rawmidi enumerator on Linux.
//
// Shared by `AlsaMidiOut` (MIDI plan B2b) and `AlsaMidiIn` (E5), and shared for the reason the
// guardrails keep charging this project for: the dlopen boilerplate and the card→device walk are
// IDENTICAL for the two directions — they differ by one integer, `STREAM_OUTPUT` vs `STREAM_INPUT` —
// so written twice they are two things that can disagree about which devices exist, and one of the
// two copies is the one nobody looks at again. Written once, the only per-direction fact left is the
// constant, which is exactly the fact `ptalsa` pins against ALSA's own headers.
//
// ⚠️ **THE PROTOTYPES BELOW ARE HAND-COPIED AND NOTHING IN A NORMAL BUILD CHECKS THEM.** That is the
// price of dlopen (midi-out-alsa.cpp argues why dlopen rather than `-lasound`: the PortMaster build
// container has no libasound-dev, and a link-time dependency turns a CFW without the library into an
// app that will not start). A mis-copied C signature still compiles, still links, and corrupts the
// stack at runtime on a device that is not on this desk — so `tools/ptalsa` includes the REAL
// <alsa/asoundlib.h>, assigns the real functions into this struct and static_asserts the rest.
// ⭐ **RUN IT AFTER TOUCHING THIS LIST**, and note that it also asserts the FIELD COUNT: a field added
// here and not covered there fails the check by construction rather than passing in silence.

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
    ptrdiff_t   (*rawmidi_read)(void* rmidi, void* buffer, size_t size);          // ssize_t, E5
    int         (*rawmidi_drain)(void* rmidi);
    const char* (*strerror_fn)(int errnum);
};

/** alsa/rawmidi.h's `snd_rawmidi_stream_t`. OUTPUT is 0, INPUT is 1 — see the note in scan(). */
constexpr int STREAM_OUTPUT = 0;
constexpr int STREAM_INPUT  = 1;
/** alsa/rawmidi.h's SND_RAWMIDI_NONBLOCK. OUT opens BLOCKING (mode 0); IN opens with this — see E5. */
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
 * `who` is "OUT" or "IN" and appears in the one line this prints. ⚠️ **IT PRINTS UNCONDITIONALLY ON
 * SUCCESS**, and that is not chattiness: a successful dlopen is silent by nature, and a component whose
 * correct behaviour is silence cannot be told from one that never ran. Without the line, "the ALSA
 * backend loaded and there are no devices" and "the ALSA backend is not in this binary at all" produce
 * identical console output — and on a handheld, where "no MIDI devices" is also what a bad cable looks
 * like, that is the whole afternoon.
 *
 * ⚠️ The handle is deliberately never `dlclose`d: these objects live for the process, and unloading
 * libasound while SDL's own ALSA audio backend still holds it is a teardown-order question with no
 * upside. (dlopen refcounts, so the two backends loading it separately is one library, twice counted.)
 */
void* load_alsa(AlsaApi& api, const char* who);

/**
 * Every rawmidi device on every card that has a stream in direction `stream`.
 *
 * ⚠️ **`stream` IS THE FILTER AND IT IS THE ONLY PER-DIRECTION FACT IN THIS FILE.**
 * `snd_ctl_rawmidi_info` answers -ENXIO when the device has no stream of the requested direction, so
 * this value is the ONE thing keeping input-only ports off the OUTPUT row and output-only ports off the
 * INPUT row — and, if it were wrong, the thing that would hide every port in the direction asked for.
 * The failure therefore looks like "no MIDI devices", not like a bug. tools/ptalsa static_asserts both
 * constants against the real header, and tools/ptalsain drives both directions against a fake card
 * whose three devices are in/out/duplex.
 */
void scan_rawmidi(const AlsaApi& api, int stream, std::vector<RawmidiDevice>& out);

}  // namespace alsa_detail
}  // namespace ptshell

#endif  // __linux__ && !__ANDROID__
#endif  // POCKETTRACKER_SHELL_ALSA_RAWMIDI_H
