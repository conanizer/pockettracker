#include "alsa-rawmidi.h"

#if defined(__linux__) && !defined(__ANDROID__)

#include <cstdio>
#include <dlfcn.h>

namespace ptshell {
namespace alsa_detail {

void* load_alsa(AlsaApi& api, const char* who) {
    // ".so.2" and not ".so": the unversioned symlink is in libasound2-dev, which no user has; the
    // SONAME is what is present at runtime everywhere.
    void* lib = ::dlopen("libasound.so.2", RTLD_NOW | RTLD_LOCAL);
    if (!lib) {
        // ⚠️ ONE call to dlerror(), into a variable. It CONSUMES the error — a second call returns
        // null — so the obvious `dlerror() ? dlerror() : "..."` prints "(null)" every single time and
        // throws away the only sentence that says why. Found by the control that unloads the library
        // on purpose, which is the only run in which this line is ever reached.
        const char* why = ::dlerror();
        std::printf("midi:    libasound.so.2 not available (%s) - MIDI %s disabled\n",
                    why ? why : "no error reported", who);
        return nullptr;
    }

    const char* missing = nullptr;
    auto        sym     = [&](const char* name) -> void* {
        void* p = ::dlsym(lib, name);
        if (!p && !missing) missing = name;
        return p;
    };

    // ⚠️ The casts are the unchecked part. See the header, and tools/ptalsa.
    api.card_next   = reinterpret_cast<int (*)(int*)>(sym("snd_card_next"));
    api.ctl_open    = reinterpret_cast<int (*)(void**, const char*, int)>(sym("snd_ctl_open"));
    api.ctl_close   = reinterpret_cast<int (*)(void*)>(sym("snd_ctl_close"));
    api.ctl_rawmidi_next_device =
            reinterpret_cast<int (*)(void*, int*)>(sym("snd_ctl_rawmidi_next_device"));
    api.ctl_rawmidi_info   = reinterpret_cast<int (*)(void*, void*)>(sym("snd_ctl_rawmidi_info"));
    api.rawmidi_info_malloc = reinterpret_cast<int (*)(void**)>(sym("snd_rawmidi_info_malloc"));
    api.rawmidi_info_free   = reinterpret_cast<void (*)(void*)>(sym("snd_rawmidi_info_free"));
    api.rawmidi_info_set_device =
            reinterpret_cast<void (*)(void*, unsigned)>(sym("snd_rawmidi_info_set_device"));
    api.rawmidi_info_set_subdevice =
            reinterpret_cast<void (*)(void*, unsigned)>(sym("snd_rawmidi_info_set_subdevice"));
    api.rawmidi_info_set_stream =
            reinterpret_cast<void (*)(void*, int)>(sym("snd_rawmidi_info_set_stream"));
    api.rawmidi_info_get_name =
            reinterpret_cast<const char* (*)(const void*)>(sym("snd_rawmidi_info_get_name"));
    api.rawmidi_open =
            reinterpret_cast<int (*)(void**, void**, const char*, int)>(sym("snd_rawmidi_open"));
    api.rawmidi_close = reinterpret_cast<int (*)(void*)>(sym("snd_rawmidi_close"));
    api.rawmidi_write =
            reinterpret_cast<ptrdiff_t (*)(void*, const void*, size_t)>(sym("snd_rawmidi_write"));
    api.rawmidi_read =
            reinterpret_cast<ptrdiff_t (*)(void*, void*, size_t)>(sym("snd_rawmidi_read"));
    api.rawmidi_drain = reinterpret_cast<int (*)(void*)>(sym("snd_rawmidi_drain"));
    api.strerror_fn   = reinterpret_cast<const char* (*)(int)>(sym("snd_strerror"));

    if (missing) {
        // A partial libasound is not a thing that happens, but a TYPO in a symbol name above is, and
        // it would otherwise show up as a null call through a function pointer at the first note.
        std::printf("midi:    libasound.so.2 is missing '%s' - MIDI %s disabled\n", missing, who);
        ::dlclose(lib);
        return nullptr;
    }

    // ⚠️ UNCONDITIONAL — the "I woke up" line. See the header for why it is not chattiness.
    std::printf("midi:    ALSA rawmidi backend ready for %s (libasound.so.2, %d entry points)\n", who,
                static_cast<int>(sizeof(AlsaApi) / sizeof(void (*)())));
    return lib;
}

void scan_rawmidi(const AlsaApi& api, int stream, std::vector<RawmidiDevice>& out) {
    out.clear();

    int card = -1;
    while (api.card_next(&card) == 0 && card >= 0) {
        char ctlName[32];
        std::snprintf(ctlName, sizeof ctlName, "hw:%d", card);

        void* ctl = nullptr;
        if (api.ctl_open(&ctl, ctlName, 0) < 0) continue;   // a card we cannot read is not an error

        int device = -1;
        while (api.ctl_rawmidi_next_device(ctl, &device) == 0 && device >= 0) {
            void* info = nullptr;
            if (api.rawmidi_info_malloc(&info) < 0) break;

            api.rawmidi_info_set_device(info, static_cast<unsigned>(device));
            api.rawmidi_info_set_subdevice(info, 0);
            api.rawmidi_info_set_stream(info, stream);

            // -ENXIO here means "this rawmidi device has no stream in that direction" — a port of the
            // other kind, correctly skipped. This is the filter; see the note on `stream` in the header.
            if (api.ctl_rawmidi_info(ctl, info) == 0) {
                const char*   n = api.rawmidi_info_get_name(info);
                RawmidiDevice d;
                d.name = (n && *n) ? n : ctlName;

                // Two identical USB interfaces produce two identical names, and the settings store a
                // NAME — so an un-suffixed duplicate would make the second one unselectable forever.
                // (Which of two identical devices you get after a replug is still undecidable; the
                // plan calls that acceptable for v1, §11.)
                int               dup  = 1;
                const std::string base = d.name;
                for (const RawmidiDevice& e : out)
                    if (e.name == d.name) d.name = base + " #" + std::to_string(++dup);

                char hw[40];
                std::snprintf(hw, sizeof hw, "hw:%d,%d", card, device);
                d.hw = hw;
                out.push_back(d);
            }
            api.rawmidi_info_free(info);
        }
        api.ctl_close(ctl);
    }
}

}  // namespace alsa_detail
}  // namespace ptshell

#endif  // __linux__ && !__ANDROID__
