// macOS MIDI output (CoreMIDI). `MIDISend` is thread-safe and does not block, so no lock. An unplugged
// device drops out of the destination list, which is how the once-a-second scan notices it.
// ⚠️ The client is made on the main thread: CoreMIDI updates the device list through the run loop of
// the thread that made the first client, and SDL's event pump is what runs that loop.

#include "macos/midi-out-coremidi.h"

#ifdef __APPLE__

#include "macos/coremidi-name.h"

namespace ptshell {

CoreMidiOut::CoreMidiOut() {
    if (MIDIClientCreate(CFSTR("PocketTracker"), nullptr, nullptr, &client_) == noErr)
        MIDIOutputPortCreate(client_, CFSTR("PocketTracker out"), &port_);
}

CoreMidiOut::~CoreMidiOut() {
    close();
    if (client_) MIDIClientDispose(client_);   // and its port
}

int CoreMidiOut::device_count() { return static_cast<int>(MIDIGetNumberOfDestinations()); }

std::string CoreMidiOut::device_name(int index) {
    if (index < 0 || index >= device_count()) return std::string();
    return coremidi_endpoint_name(MIDIGetDestination(static_cast<ItemCount>(index)));
}

bool CoreMidiOut::open(int index) {
    close();
    if (!port_ || index < 0 || index >= device_count()) return false;
    const MIDIEndpointRef d = MIDIGetDestination(static_cast<ItemCount>(index));
    if (!d) return false;
    dest_      = d;
    openIndex_ = index;
    return true;
}

void CoreMidiOut::close() {
    if (!dest_) return;
    panic_all_channels();   // CoreMIDI has no reset: notes left on keep sounding
    dest_      = 0;
    openIndex_ = -1;
}

void CoreMidiOut::send(const uint8_t* data, int len) {
    if (!dest_ || len <= 0) return;
    // Timestamp 0 = now: the sender thread already releases each message on time.
    alignas(MIDIPacketList) Byte buf[256];
    auto*       list = reinterpret_cast<MIDIPacketList*>(buf);
    MIDIPacket* p    = MIDIPacketListInit(list);
    p = MIDIPacketListAdd(list, sizeof buf, p, 0, static_cast<ByteCount>(len), data);
    const bool bad = !p || MIDISend(port_, dest_, list) != noErr;
    if (bad) ++errors_;
    trace_message(data, len, bad);
}

}  // namespace ptshell

#endif  // __APPLE__
