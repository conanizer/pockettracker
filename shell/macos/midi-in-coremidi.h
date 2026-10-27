#ifndef POCKETTRACKER_SHELL_MIDI_IN_COREMIDI_H
#define POCKETTRACKER_SHELL_MIDI_IN_COREMIDI_H

// macOS MIDI input (see the .cpp). Compiles to nothing elsewhere, so main.cpp can include it.

#include "midi/midi-in-base.h"

#ifdef __APPLE__

#include <CoreMIDI/CoreMIDI.h>

#include <string>

namespace ptshell {

class CoreMidiIn : public MidiInBase {
  public:
    CoreMidiIn();
    ~CoreMidiIn() override;

    int         device_count() override;
    std::string device_name(int index) override;
    bool        open(int index) override;
    void        close() override;
    bool        is_open() const override { return src_ != 0; }

  private:
    static void read_proc(const MIDIPacketList* list, void* self, void* srcRef);

    MIDIClientRef   client_ = 0;
    MIDIPortRef     port_   = 0;
    MIDIEndpointRef src_    = 0;
};

}  // namespace ptshell

#endif  // __APPLE__
#endif  // POCKETTRACKER_SHELL_MIDI_IN_COREMIDI_H
