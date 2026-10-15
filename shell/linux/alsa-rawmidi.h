#ifndef POCKETTRACKER_SHELL_ALSA_RAWMIDI_H
#define POCKETTRACKER_SHELL_ALSA_RAWMIDI_H

// The one libasound loader and rawmidi enumerator, shared by `AlsaMidiOut` and `AlsaMidiIn` (they
// differ by one integer). ⚠️ The prototypes are hand-copied for dlopen, so nothing checks them at build
// time: a wrong signature corrupts the stack at runtime. Check against <alsa/asoundlib.h> after an edit.

#include <cstddef>
#include <string>
#include <vector>

#if defined(__linux__) && !defined(__ANDROID__)

namespace ptshell {
namespace alsa_detail {

// The libasound entry points both backends use. ALSA's handles are pointers to incomplete types, so
// `void*` is the same ABI.
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

// alsa/rawmidi.h's `snd_rawmidi_stream_t`.
constexpr int STREAM_OUTPUT = 0;
constexpr int STREAM_INPUT  = 1;
// SND_RAWMIDI_NONBLOCK. OUT opens blocking (mode 0); IN with this.
constexpr int NONBLOCK = 0x0002;

struct RawmidiDevice {
    std::string name;   // shown on the MIDI screen and stored in settings.json
    std::string hw;     // "hw:C,D" to open — not stable across a replug; the name is
};

// dlopen libasound.so.2 and resolve `api`; null when absent or a symbol is missing (the row then shows
// `OFF  NO PORTS`). `who` is "OUT" or "IN". Always prints one line on success — "loaded, no devices"
// must differ from "not in this binary". ⚠️ Never dlclose'd: SDL's ALSA backend holds the same library.
void* load_alsa(AlsaApi& api, const char* who);

// Every rawmidi device with a stream in direction `stream`. ⚠️ The direction is the filter (a device
// without that stream answers -ENXIO); wrong, it hides every port and looks like "no devices".
void scan_rawmidi(const AlsaApi& api, int stream, std::vector<RawmidiDevice>& out);

}  // namespace alsa_detail
}  // namespace ptshell

#endif  // __linux__ && !__ANDROID__
#endif  // POCKETTRACKER_SHELL_ALSA_RAWMIDI_H
