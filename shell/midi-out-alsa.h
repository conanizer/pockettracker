#ifndef POCKETTRACKER_SHELL_MIDI_OUT_ALSA_H
#define POCKETTRACKER_SHELL_MIDI_OUT_ALSA_H

// The LINUX songcore::IMidiOut — ALSA rawmidi, reached through dlopen. MIDI plan phase B2b.
// See midi-out-alsa.cpp for why rawmidi and not seq, and why dlopen and not -lasound.
// Compiles to nothing off desktop/handheld Linux, so main.cpp can name the header unconditionally.

#include "midi-out-base.h"

#if defined(__linux__) && !defined(__ANDROID__)

#include <cstddef>
#include <string>
#include <vector>

namespace ptshell {

// ─── The libasound entry points this backend uses ────────────────────────────────────────────────
//
// ⚠️ THIS STRUCT IS IN THE HEADER ON PURPOSE, and it is the only reason it is not private to the .cpp.
// dlopen means these prototypes are HAND-COPIED from alsa/{control,rawmidi,error}.h, and a hand-copy
// of a C signature is checked by nothing: get one wrong and the call site still compiles, still
// links, and corrupts the stack at runtime on a device that is not on this desk. Because the struct
// is reachable from another translation unit, `tools/alsa-abi-check` can #include the REAL ALSA
// headers and assign the real functions into it — upstream's own declarations as the independent
// invariant, rather than this file agreeing with itself. Run that check when you touch this list.
//
// The opaque handles (snd_ctl_t*, snd_rawmidi_t*, snd_rawmidi_info_t*) are all pointer-to-incomplete
// in ALSA's own headers, so `void*` here is the same ABI and keeps the header dependency-free.
namespace alsa_detail {

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
    int         (*rawmidi_drain)(void* rmidi);
    const char* (*strerror_fn)(int errnum);
};

/** alsa/rawmidi.h's `snd_rawmidi_stream_t`. OUTPUT is 0 — see the note in the .cpp. */
constexpr int STREAM_OUTPUT = 0;
/** alsa/rawmidi.h's SND_RAWMIDI_NONBLOCK, for the record; this backend opens BLOCKING (mode 0). */
constexpr int NONBLOCK = 0x0002;

}  // namespace alsa_detail

class AlsaMidiOut : public MidiOutBase {
  public:
    AlsaMidiOut();
    ~AlsaMidiOut() override;

    int         device_count() override;
    std::string device_name(int index) override;
    bool        open(int index) override;
    void        close() override;
    bool        is_open() const override { return out_ != nullptr; }
    void        send(const uint8_t* data, int len) override;

    /**
     * False when libasound.so.2 is absent or a symbol is missing — MIDI is then simply unavailable
     * and `device_count()` answers 0, which the OUTPUT row already draws as `OFF  NO PORTS`. The app
     * itself still runs, which is the whole point of dlopen over a link-time dependency.
     */
    bool available() const { return lib_ != nullptr; }

  private:
    struct Device {
        std::string name;   // what the OUTPUT row shows, and what settings.json stores
        std::string hw;     // "hw:C,D" — the thing to open. NOT stable across a replug; the name is.
    };

    void scan();

    void*               lib_ = nullptr;
    alsa_detail::AlsaApi a_{};
    std::vector<Device> devices_;
    void*               out_ = nullptr;   // snd_rawmidi_t*
};

}  // namespace ptshell

#endif  // __linux__ && !__ANDROID__
#endif  // POCKETTRACKER_SHELL_MIDI_OUT_ALSA_H
