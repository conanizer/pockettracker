// midi-out-alsa.{h,cpp} — the LINUX implementation of songcore::IMidiOut (MIDI plan phase B2b).
//
// The second of the three backends (§4.3 / §4.5): Windows = winmm, **Linux = ALSA rawmidi**, Android
// = a JNI up-call into MidiManager. Nothing above `IMidiOut` is per-platform — the serializer, the
// note lifecycle, the release queue and the 0–255 → 0–127 scaling are native/songcore/midi_out.h and
// are shared by all three; the console half (device list, spec resolver, TEST, trace, counters) is
// midi-out-base.{h,cpp}. This file is the whole Linux difference, and it is small on purpose.
//
// ── WHY rawmidi AND NOT ALSA seq ─────────────────────────────────────────────────────────────────
//
// The plan (§4.5) settled this: ALSA *seq* — the API that gives you virtual ports and a routing
// graph — needs the `snd-seq` kernel module, and the PortMaster CFW kernels (muOS, ROCKNIX, ArkOS,
// Knulli, AmberELEC) do not guarantee it. rawmidi is the floor that works everywhere: a USB-OTG MIDI
// interface appears through the in-tree `snd-usb-audio` driver as /dev/snd/midiC*D*, which is exactly
// what this file enumerates. The cost is that there are no virtual/app-to-app ports on Linux — on a
// handheld there is nothing to route to anyway, and on a desktop the user can load `snd-virmidi`.
//
// ── WHY dlopen AND NOT -lasound ──────────────────────────────────────────────────────────────────
//
// ⚠️ Three reasons, and the first is the one that decides it:
//
//   1. **The PortMaster build container has no libasound-dev, and giving it one is not free.** The
//      container (shell/Dockerfile.portmaster) is ubuntu:20.04 — pinned there as the GLIBC FLOOR —
//      and it cross-compiles to aarch64. A link-time dependency would need `libasound2-dev:arm64`,
//      i.e. dpkg multiarch inside that image, for a library we call sixteen functions in.
//   2. **A missing libasound would otherwise stop the whole app from starting.** With `-lasound`,
//      a CFW without the library gives the user a dynamic-linker error instead of a tracker. With
//      dlopen, MIDI is simply unavailable: `device_count()` answers 0 and the OUTPUT row draws
//      `OFF  NO PORTS`, which is a state the screen already has a design for.
//   3. It keeps `shell/CMakeLists.txt` honest about the port's headline claim — SDL2 and nothing
//      else. libasound is on every CFW because SDL2's audio backend sits on it, so the dlopen
//      practically always succeeds; it is the *build* that must not require it.
//
// The price is that the prototypes in midi-out-alsa.h are hand-copied and nothing checks them. That
// is what `tools/alsa-abi-check` is for — see the header's comment on `AlsaApi`.
//
// ── THREADING AND BLOCKING ───────────────────────────────────────────────────────────────────────
//
// `send` is called from whichever thread pumps the queue — today the frame loop, and after phase B3
// a sender thread. The port is opened in BLOCKING mode (`snd_rawmidi_open` mode 0) rather than
// SND_RAWMIDI_NONBLOCK, and that is deliberate: non-blocking `snd_rawmidi_write` returns -EAGAIN
// when the driver buffer is full and the bytes are simply **lost**, and the byte most worth losing
// is never the one you lose — a dropped note-off is a note that sounds until the gear is
// power-cycled. Blocking cannot bite here: the rawmidi output buffer defaults to 4 KB against a
// worst case of a few hundred bytes per second, and a device unplugged mid-write fails with -ENODEV
// rather than waiting. It must NOT be called from an audio callback, and nothing does.

#include "midi-out-alsa.h"

#if defined(__linux__) && !defined(__ANDROID__)

#include <cstdio>
#include <cstring>
#include <dlfcn.h>

namespace ptshell {

using alsa_detail::AlsaApi;

namespace {

/**
 * ⚠️ `SND_RAWMIDI_STREAM_OUTPUT` is 0 (alsa/rawmidi.h), and this constant is load-bearing in a way
 * that is easy to miss: `snd_ctl_rawmidi_info` fails with -ENXIO when the device has no stream of
 * the requested direction, so this value is the ONLY thing keeping input-only ports off the list —
 * and, if it were wrong, the thing that would hide every output-only port. tools/alsa-abi-check
 * static_asserts it against the real header.
 */
constexpr int STREAM_OUTPUT = alsa_detail::STREAM_OUTPUT;

}  // namespace

AlsaMidiOut::AlsaMidiOut() {
    // ".so.2" and not ".so": the unversioned symlink is in libasound2-dev, which no user has; the
    // SONAME is what is present at runtime everywhere.
    lib_ = ::dlopen("libasound.so.2", RTLD_NOW | RTLD_LOCAL);
    if (!lib_) {
        // ⚠️ ONE call to dlerror(), into a variable. It CONSUMES the error — a second call returns
        // null — so the obvious `dlerror() ? dlerror() : "..."` prints "(null)" every single time and
        // throws away the only sentence that says why. Found by the control that unloads the library
        // on purpose, which is the only run in which this line is ever reached.
        const char* why = ::dlerror();
        std::printf("midi:    libasound.so.2 not available (%s) - MIDI out disabled\n",
                    why ? why : "no error reported");
        return;
    }

    const char* missing = nullptr;
    auto sym = [&](const char* name) -> void* {
        void* p = ::dlsym(lib_, name);
        if (!p && !missing) missing = name;
        return p;
    };

    // ⚠️ The casts are the unchecked part. See the header.
    a_.card_next   = reinterpret_cast<int (*)(int*)>(sym("snd_card_next"));
    a_.ctl_open    = reinterpret_cast<int (*)(void**, const char*, int)>(sym("snd_ctl_open"));
    a_.ctl_close   = reinterpret_cast<int (*)(void*)>(sym("snd_ctl_close"));
    a_.ctl_rawmidi_next_device =
            reinterpret_cast<int (*)(void*, int*)>(sym("snd_ctl_rawmidi_next_device"));
    a_.ctl_rawmidi_info = reinterpret_cast<int (*)(void*, void*)>(sym("snd_ctl_rawmidi_info"));
    a_.rawmidi_info_malloc = reinterpret_cast<int (*)(void**)>(sym("snd_rawmidi_info_malloc"));
    a_.rawmidi_info_free   = reinterpret_cast<void (*)(void*)>(sym("snd_rawmidi_info_free"));
    a_.rawmidi_info_set_device =
            reinterpret_cast<void (*)(void*, unsigned)>(sym("snd_rawmidi_info_set_device"));
    a_.rawmidi_info_set_subdevice =
            reinterpret_cast<void (*)(void*, unsigned)>(sym("snd_rawmidi_info_set_subdevice"));
    a_.rawmidi_info_set_stream =
            reinterpret_cast<void (*)(void*, int)>(sym("snd_rawmidi_info_set_stream"));
    a_.rawmidi_info_get_name =
            reinterpret_cast<const char* (*)(const void*)>(sym("snd_rawmidi_info_get_name"));
    a_.rawmidi_open =
            reinterpret_cast<int (*)(void**, void**, const char*, int)>(sym("snd_rawmidi_open"));
    a_.rawmidi_close = reinterpret_cast<int (*)(void*)>(sym("snd_rawmidi_close"));
    a_.rawmidi_write =
            reinterpret_cast<ptrdiff_t (*)(void*, const void*, size_t)>(sym("snd_rawmidi_write"));
    a_.rawmidi_drain = reinterpret_cast<int (*)(void*)>(sym("snd_rawmidi_drain"));
    a_.strerror_fn   = reinterpret_cast<const char* (*)(int)>(sym("snd_strerror"));

    if (missing) {
        // A partial libasound is not a thing that happens, but a TYPO in a symbol name above is, and
        // it would otherwise show up as a null call through a function pointer at the first note.
        std::printf("midi:    libasound.so.2 is missing '%s' - MIDI out disabled\n", missing);
        ::dlclose(lib_);
        lib_ = nullptr;
        return;
    }

    // ⚠️ UNCONDITIONAL, and it is not chattiness. A successful dlopen is SILENT by nature, and the
    // guardrails have a name for that: a component whose correct behaviour is silence cannot be told
    // from one that never ran. Without this line, "the ALSA backend loaded and there are no devices"
    // and "the ALSA backend is not in this binary at all" produce identical console output — and on a
    // handheld, where "no MIDI devices" is also what a bad cable looks like, that is the whole
    // afternoon. One line, always, saying which of the two states we are in.
    std::printf("midi:    ALSA rawmidi backend ready (libasound.so.2, %d entry points)\n",
                static_cast<int>(sizeof(AlsaApi) / sizeof(void (*)())));
}

AlsaMidiOut::~AlsaMidiOut() {
    close();
    // ⚠️ The library is NOT dlclose()d here. This object outlives nothing — main.cpp owns it for the
    // process — and unloading libasound while SDL's ALSA audio backend still holds it is the kind of
    // teardown-order question with no upside. The process exit does it.
}

void AlsaMidiOut::scan() {
    devices_.clear();
    if (!lib_) return;

    int card = -1;
    while (a_.card_next(&card) == 0 && card >= 0) {
        char ctlName[32];
        std::snprintf(ctlName, sizeof ctlName, "hw:%d", card);

        void* ctl = nullptr;
        if (a_.ctl_open(&ctl, ctlName, 0) < 0) continue;   // a card we cannot read is not an error

        int device = -1;
        while (a_.ctl_rawmidi_next_device(ctl, &device) == 0 && device >= 0) {
            void* info = nullptr;
            if (a_.rawmidi_info_malloc(&info) < 0) break;

            a_.rawmidi_info_set_device(info, static_cast<unsigned>(device));
            a_.rawmidi_info_set_subdevice(info, 0);
            a_.rawmidi_info_set_stream(info, STREAM_OUTPUT);

            // -ENXIO here means "this rawmidi device has no OUTPUT stream" — an input-only port,
            // correctly skipped. This is the filter; see the note on STREAM_OUTPUT above.
            if (a_.ctl_rawmidi_info(ctl, info) == 0) {
                const char* n = a_.rawmidi_info_get_name(info);
                Device      d;
                d.name = (n && *n) ? n : ctlName;

                // Two identical USB interfaces produce two identical names, and the settings store a
                // NAME — so an un-suffixed duplicate would make the second one unselectable forever.
                // (Which of two identical devices you get after a replug is still undecidable; the
                // plan calls that acceptable for v1, §11.)
                int dup = 1;
                const std::string base = d.name;
                for (const Device& e : devices_)
                    if (e.name == d.name) d.name = base + " #" + std::to_string(++dup);

                char hw[40];
                std::snprintf(hw, sizeof hw, "hw:%d,%d", card, device);
                d.hw = hw;
                devices_.push_back(d);
            }
            a_.rawmidi_info_free(info);
        }
        a_.ctl_close(ctl);
    }
}

int AlsaMidiOut::device_count() {
    // Re-enumerates on every call, because MIDI is hot-pluggable and a port list is only true at the
    // moment it is read. InputDispatcher::refresh_midi_devices calls this on every entry to the MIDI
    // screen and then walks device_name(0..n-1), so the cache this fills is always the fresh one.
    scan();
    return static_cast<int>(devices_.size());
}

std::string AlsaMidiOut::device_name(int index) {
    if (index < 0 || index >= static_cast<int>(devices_.size())) return std::string();
    return devices_[index].name;
}

bool AlsaMidiOut::open(int index) {
    close();
    if (!lib_) return false;
    if (index < 0 || index >= static_cast<int>(devices_.size())) return false;

    void*     out = nullptr;
    const int rc  = a_.rawmidi_open(nullptr, &out, devices_[index].hw.c_str(), 0);   // BLOCKING
    if (rc < 0 || !out) {
        std::printf("midi:    open %s failed: %s\n", devices_[index].hw.c_str(),
                    a_.strerror_fn(rc));
        return false;
    }
    out_       = out;
    openIndex_ = index;
    return true;
}

void AlsaMidiOut::close() {
    if (!out_) return;

    // The equivalent of winmm's `midiOutReset`; ALSA has no such call, so the bytes are ours to write.
    // Shared with the Android backend — see MidiOutBase::panic_all_channels for why it must happen.
    panic_all_channels();
    a_.rawmidi_drain(out_);   // and WAIT for them: closing first would discard the buffer
    a_.rawmidi_close(out_);
    out_       = nullptr;
    openIndex_ = -1;
}

void AlsaMidiOut::send(const uint8_t* data, int len) {
    if (!out_ || len <= 0 || len > 3) return;
    ++sent_;
    const ptrdiff_t n   = a_.rawmidi_write(out_, data, static_cast<size_t>(len));
    const bool      bad = n != static_cast<ptrdiff_t>(len);
    if (bad) ++errors_;
    trace_message(data, len, bad);
}

}  // namespace ptshell

#endif  // __linux__ && !__ANDROID__
