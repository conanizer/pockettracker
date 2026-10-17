// macOS MIDI input (CoreMIDI). The IAC bus (Audio MIDI Setup, built into every Mac) lets the app's OUT
// feed its own IN. ⚠️ `read_proc` runs on CoreMIDI's own high-priority thread: it calls `deliver` only —
// no printing, allocating or locking.

#include "macos/midi-in-coremidi.h"

#ifdef __APPLE__

#include "macos/coremidi-name.h"

namespace ptshell {

CoreMidiIn::CoreMidiIn() {
    if (MIDIClientCreate(CFSTR("PocketTracker"), nullptr, nullptr, &client_) == noErr)
        MIDIInputPortCreate(client_, CFSTR("PocketTracker in"), &read_proc, this, &port_);
}

CoreMidiIn::~CoreMidiIn() {
    close();
    if (client_) MIDIClientDispose(client_);   // and its port
}

int CoreMidiIn::device_count() { return static_cast<int>(MIDIGetNumberOfSources()); }

std::string CoreMidiIn::device_name(int index) {
    if (index < 0 || index >= device_count()) return std::string();
    return coremidi_endpoint_name(MIDIGetSource(static_cast<ItemCount>(index)));
}

// Whole messages, status byte included (CoreMIDI expands running status); the parser takes them as a
// byte stream either way.
void CoreMidiIn::read_proc(const MIDIPacketList* list, void* self, void*) {
    auto* in = static_cast<CoreMidiIn*>(self);
    const MIDIPacket* p = &list->packet[0];
    for (UInt32 i = 0; i < list->numPackets; ++i) {
        if (p->length > 0) in->deliver(p->data, static_cast<int>(p->length));
        p = MIDIPacketNext(p);
    }
}

bool CoreMidiIn::open(int index) {
    close();
    if (!port_ || index < 0 || index >= device_count()) return false;
    const MIDIEndpointRef s = MIDIGetSource(static_cast<ItemCount>(index));
    if (!s || MIDIPortConnectSource(port_, s, nullptr) != noErr) return false;
    src_       = s;
    openIndex_ = index;
    return true;
}

void CoreMidiIn::close() {
    if (!src_) return;
    MIDIPortDisconnectSource(port_, src_);
    src_       = 0;
    openIndex_ = -1;
}

}  // namespace ptshell

#endif  // __APPLE__
