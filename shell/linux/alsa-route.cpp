#include "linux/alsa-route.h"

#if defined(__linux__) && !defined(__ANDROID__)

#include <cstdio>
#include <cstdlib>
#include <dlfcn.h>

namespace ptshell {

using alsa_detail::AlsaConfigApi;

namespace {

bool load_config_api(AlsaConfigApi& api) {
    // Never dlclosed, as in alsa-rawmidi.cpp: SDL's ALSA backend holds the same library.
    void* lib = ::dlopen("libasound.so.2", RTLD_NOW | RTLD_LOCAL);
    if (!lib) return false;

    bool ok  = true;
    auto sym = [&](const char* name) -> void* {
        void* p = ::dlsym(lib, name);
        if (!p) ok = false;
        return p;
    };
    api.config_update_ref = reinterpret_cast<int (*)(void**)>(sym("snd_config_update_ref"));
    api.config_unref      = reinterpret_cast<void (*)(void*)>(sym("snd_config_unref"));
    api.config_search_definition = reinterpret_cast<int (*)(void*, const char*, const char*, void**)>(
            sym("snd_config_search_definition"));
    api.config_search = reinterpret_cast<int (*)(void*, const char*, void**)>(sym("snd_config_search"));
    api.config_get_type = reinterpret_cast<int (*)(const void*)>(sym("snd_config_get_type"));
    api.config_get_string =
            reinterpret_cast<int (*)(const void*, const char**)>(sym("snd_config_get_string"));
    api.config_get_ascii =
            reinterpret_cast<int (*)(const void*, char**)>(sym("snd_config_get_ascii"));
    api.config_delete  = reinterpret_cast<int (*)(void*)>(sym("snd_config_delete"));
    api.card_get_index = reinterpret_cast<int (*)(const char*)>(sym("snd_card_get_index"));
    return ok;
}

struct Walk {
    const AlsaConfigApi& api;
    void*                top;
    DefaultRoute&        out;

    std::string string_at(void* node, const char* key) {
        void*       n = nullptr;
        const char* s = nullptr;
        if (api.config_search(node, key, &n) < 0 || api.config_get_string(n, &s) < 0 || !s) return {};
        return s;
    }

    // A number or a name ("0", "rockchiprk817"); -1 when absent.
    long ascii_at(void* node, const char* key, bool card) {
        void* n = nullptr;
        char* s = nullptr;
        if (api.config_search(node, key, &n) < 0 || api.config_get_ascii(n, &s) < 0 || !s) return -1;
        const long v = card ? api.card_get_index(s) : std::strtol(s, nullptr, 10);
        std::free(s);
        return v;
    }

    // `pcm` is a definition's name (looked up and expanded like snd_pcm_open does) or an inline one.
    AlsaRoute follow(void* pcm, int depth) {
        if (api.config_get_type(pcm) == alsa_detail::CONFIG_TYPE_STRING) {
            const char* name = nullptr;
            void*       def  = nullptr;
            if (api.config_get_string(pcm, &name) < 0 || !name ||
                api.config_search_definition(top, "pcm", name, &def) < 0)
                return AlsaRoute::LEAVE;
            const AlsaRoute r = node(def, depth);
            api.config_delete(def);
            return r;
        }
        if (api.config_get_type(pcm) == alsa_detail::CONFIG_TYPE_COMPOUND) return node(pcm, depth);
        return AlsaRoute::LEAVE;
    }

    AlsaRoute node(void* def, int depth) {
        if (depth > 8) return AlsaRoute::LEAVE;
        const std::string type = string_at(def, "type");
        out.chain += out.chain.empty() ? type : " > " + type;

        if (type == "hw") {
            const long card   = ascii_at(def, "card", true);
            const long device = ascii_at(def, "device", false);
            if (card < 0) return AlsaRoute::LEAVE;
            out.hw = "hw:" + std::to_string(card) + "," + std::to_string(device < 0 ? 0 : device);
            return AlsaRoute::DIRECT;
        }
        if (type == "pipewire" || type == "pulse") return AlsaRoute::SERVER;

        void* next = nullptr;
        if (type == "asym") {
            if (api.config_search(def, "playback.pcm", &next) < 0) return AlsaRoute::LEAVE;
        } else if (type == "plug" || type == "dmix" || type == "empty") {
            // `slave "name"` or `slave { pcm "name" | { ... } }`.
            void* slave = nullptr;
            if (api.config_search(def, "slave", &slave) < 0) return AlsaRoute::LEAVE;
            if (api.config_get_type(slave) == alsa_detail::CONFIG_TYPE_STRING)
                next = slave;
            else if (api.config_search(slave, "pcm", &next) < 0)
                return AlsaRoute::LEAVE;
        } else {
            return AlsaRoute::LEAVE;
        }
        return follow(next, depth + 1);
    }
};

}  // namespace

DefaultRoute inspect_default_route() {
    DefaultRoute  out;
    AlsaConfigApi api{};
    if (!load_config_api(api)) {
        out.chain = "(libasound config API unavailable)";
        return out;
    }

    void* top = nullptr;
    if (api.config_update_ref(&top) < 0 || !top) {
        out.chain = "(ALSA config unreadable)";
        return out;
    }
    void* def = nullptr;
    if (api.config_search_definition(top, "pcm", "default", &def) >= 0) {
        Walk walk{api, top, out};
        out.route = walk.node(def, 0);
        api.config_delete(def);
    } else {
        out.chain = "(no pcm.default)";
    }
    api.config_unref(top);
    if (out.route != AlsaRoute::DIRECT) out.hw.clear();
    return out;
}

}  // namespace ptshell

#endif  // __linux__ && !__ANDROID__
