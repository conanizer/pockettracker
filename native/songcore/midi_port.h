#ifndef POCKETTRACKER_SONGCORE_MIDI_PORT_H
#define POCKETTRACKER_SONGCORE_MIDI_PORT_H

// What goes onto a MIDI cable: the platform's port, the message bytes and the bus-to-7-bit scale.

#include <cstdint>
#include <string>

#include "note_command.h"   // NOTE_COMMAND_BYTES

namespace songcore {

// A MIDI output port, one per platform. `send` gets a complete message and must not block for long —
// it runs on whichever thread pumps the queue.
struct IMidiOut {
    virtual ~IMidiOut() = default;

    // Re-enumerated on each call; hot-plug changes it.
    virtual int device_count() = 0;
    virtual std::string device_name(int index) = 0;

    virtual bool open(int index) = 0;
    virtual void close() = 0;
    virtual bool is_open() const = 0;

    virtual void send(const uint8_t* data, int len) = 0;

    // The open port failed in a way only a reopen cures (the cable was pulled).
    virtual bool broken() const { return false; }

    // A synth built into the OS — AUTO passes over it.
    virtual bool is_builtin_synth(int /*index*/) { return false; }
};

// A 0..1 bus value to 0–127 — the one place, so internal and external cannot drift.
inline int to7bit(float v01) {
    if (!(v01 > 0.0f)) return 0;      // also catches NaN
    if (v01 >= 1.0f) return 127;
    return static_cast<int>(v01 * 127.0f + 0.5f);
}

// An authored 0x00–0xFF byte to 0–127.
inline int byte_to_7bit(int b) {
    if (b <= 0) return 0;
    if (b >= 255) return 127;
    return (b * 127 + 127) / 255;
}

constexpr uint8_t MIDI_NOTE_OFF = 0x80, MIDI_NOTE_ON = 0x90, MIDI_CC = 0xB0,
                  MIDI_PROGRAM  = 0xC0, MIDI_PITCH_BEND = 0xE0;

constexpr uint8_t MIDI_CC_BANK_MSB = 0, MIDI_CC_BANK_LSB = 32, MIDI_CC_ALL_NOTES_OFF = 123;

// RPN 0, the bend range: the RPN's number goes to 101/100, its value to 6/38.
constexpr uint8_t MIDI_CC_RPN_MSB = 101, MIDI_CC_RPN_LSB = 100, MIDI_CC_DATA_MSB = 6, MIDI_CC_DATA_LSB = 38;

// One message and the frame it is due on. `seq` keeps same-frame messages in production order
// (bank → program → CC → note-on). `UNSCHEDULED` = a panic message; a lateness observer skips it.
struct MidiMessage {
    static constexpr int64_t UNSCHEDULED = INT64_MIN;

    int64_t  frame = 0;
    uint32_t seq   = 0;
    uint8_t  bytes[NOTE_COMMAND_BYTES] = {};   // the longest a note command
    uint8_t  len   = 0;
    // A wheel position: its BEND goes first when the channel holds another. A re-send equal to what
    // the channel holds is dropped, unless it is an MPB — a command, sent as written.
    uint8_t  bendRange  = 0;
    bool     bendAlways = false;
    uint8_t  track      = 0xFF;   // the sequencer track that queued it, for a LIVE take-back
};

// Told about every message as it is released — how lateness is measured; called with no port open too.
// ⚠️ Called under the lock: be quick, and never call back into `ExternalConsumer`.
struct IMidiSendObserver {
    virtual ~IMidiSendObserver() = default;
    virtual void on_released(const MidiMessage& m, int64_t nowFrame, bool sent) = 0;
};

}  // namespace songcore

#endif  // POCKETTRACKER_SONGCORE_MIDI_PORT_H
