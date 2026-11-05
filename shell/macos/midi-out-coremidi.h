#ifndef POCKETTRACKER_SHELL_MIDI_OUT_COREMIDI_H
#define POCKETTRACKER_SHELL_MIDI_OUT_COREMIDI_H

// macOS MIDI output (see the .cpp). Compiles to nothing elsewhere, so main.cpp can include it.

#include "midi/midi-out-base.h"

#ifdef __APPLE__

#include <CoreMIDI/CoreMIDI.h>

#include <string>

namespace ptshell {

class CoreMidiOut : public MidiOutBase {
  public:
    CoreMidiOut();
    ~CoreMidiOut() override;

    int         device_count() override;
    std::string device_name(int index) override;
    bool        open(int index) override;
    void        close() override;
    bool        is_open() const override { return dest_ != 0; }
    void        send(const uint8_t* data, int len) override;

  private:
    MIDIClientRef   client_ = 0;
    MIDIPortRef     port_   = 0;
    MIDIEndpointRef dest_   = 0;
};

}  // namespace ptshell

#endif  // __APPLE__
#endif  // POCKETTRACKER_SHELL_MIDI_OUT_COREMIDI_H
