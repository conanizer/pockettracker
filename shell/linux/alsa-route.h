#ifndef POCKETTRACKER_SHELL_ALSA_ROUTE_H
#define POCKETTRACKER_SHELL_ALSA_ROUTE_H

// What stands between ALSA's "default" and the sound chip, read from ALSA's own configuration; no
// device is opened. ⚠️ Hand-copied dlopen prototypes, as in alsa-rawmidi.h — check after an edit.

#include <string>

#if defined(__linux__) && !defined(__ANDROID__)

namespace ptshell {

namespace alsa_detail {

// The libasound config entry points the route walk uses; opaque handles as `void*`.
struct AlsaConfigApi {
    int  (*config_update_ref)(void** top);
    void (*config_unref)(void* top);
    int  (*config_search_definition)(void* config, const char* base, const char* key, void** result);
    int  (*config_search)(void* config, const char* key, void** result);
    int  (*config_get_type)(const void* config);   // snd_config_type_t
    int  (*config_get_string)(const void* config, const char** value);
    int  (*config_get_ascii)(const void* config, char** value);
    int  (*config_delete)(void* config);
    int  (*card_get_index)(const char* name);
};

// alsa/conf.h's SND_CONFIG_TYPE_STRING and SND_CONFIG_TYPE_COMPOUND.
constexpr int CONFIG_TYPE_STRING   = 3;
constexpr int CONFIG_TYPE_COMPOUND = 1024;

}  // namespace alsa_detail

enum class AlsaRoute {
    DIRECT,   // only mixing/format plugins above one chip: `hw` names it
    SERVER,   // a sound server (PipeWire, PulseAudio) — it owns volume and routing
    LEAVE,    // anything else, or unreadable: a volume layer, Bluetooth, hooks, an unknown plugin
};

struct DefaultRoute {
    AlsaRoute   route = AlsaRoute::LEAVE;
    std::string chain;   // the plugin types walked, "plug > dmix > hw"
    std::string hw;      // "hw:C,D" when DIRECT
};

// Follow `pcm.default` to the chip as `snd_pcm_open` would. ⚠️ A whitelist that fails closed: only
// `plug`, `dmix`, `empty`, `asym` (playback side) and `hw` are DIRECT — none can hold a volume, a route
// or a device switch. Anything else (softvol, route, hooks, bluealsa, unknown) is LEAVE: going around
// it could mean full volume or the wrong speaker.
DefaultRoute inspect_default_route();

}  // namespace ptshell

#endif  // __linux__ && !__ANDROID__
#endif  // POCKETTRACKER_SHELL_ALSA_ROUTE_H
