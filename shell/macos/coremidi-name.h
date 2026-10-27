#ifndef POCKETTRACKER_SHELL_COREMIDI_NAME_H
#define POCKETTRACKER_SHELL_COREMIDI_NAME_H

// A CoreMIDI endpoint's display name ("IAC Driver Bus 1"), as UTF-8; "" if it has none. macOS only.

#ifdef __APPLE__

#include <CoreMIDI/CoreMIDI.h>

#include <string>

namespace ptshell {

inline std::string coremidi_endpoint_name(MIDIEndpointRef ep) {
    CFStringRef name = nullptr;
    if (!ep || MIDIObjectGetStringProperty(ep, kMIDIPropertyDisplayName, &name) != noErr || !name)
        return std::string();
    char buf[256];
    const bool ok = CFStringGetCString(name, buf, sizeof buf, kCFStringEncodingUTF8);
    CFRelease(name);
    return ok ? std::string(buf) : std::string();
}

}  // namespace ptshell

#endif  // __APPLE__
#endif  // POCKETTRACKER_SHELL_COREMIDI_NAME_H
