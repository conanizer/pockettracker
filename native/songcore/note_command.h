#ifndef POCKETTRACKER_SONGCORE_NOTE_COMMAND_H
#define POCKETTRACKER_SONGCORE_NOTE_COMMAND_H

// What a phrase's REV, CUT, DRV… did to one note, for another PocketTracker to do to the same note.
// They shape this app's own voice and mean nothing to other gear, so they travel as a message only
// PocketTracker reads: a SysEx under the non-commercial ID (7D) and "PT", which other gear ignores.
//
//   F0 7D 50 54  kind  channel  note  id(hi lo)  value(hi lo)  F7
//
// WITH_NOTE goes out just ahead of its note-on, so the receiver plays the note with it from its first
// sample; the next note starts from the receiving instrument again. NOW is for the note sounding.

#include <cstdint>

#include "event.h"

namespace songcore {

constexpr uint8_t MIDI_SYSEX = 0xF0, MIDI_SYSEX_END = 0xF7;
constexpr int     NOTE_COMMAND_BYTES = 12;

enum NoteCommandKind : uint8_t { NOTE_COMMAND_WITH_NOTE = 1, NOTE_COMMAND_NOW = 2 };

struct NoteCommand {
    uint8_t kind    = NOTE_COMMAND_NOW;
    uint8_t channel = 0;
    uint8_t note    = 0;
    uint8_t id      = 0;   // an EV_CC id (event.h)
    uint8_t value   = 0;   // the byte the author typed, 00-FF
};

// The commands that travel this way: every one that shapes the app's own voice. PAN, VOL and the
// pitch commands are not among them — they have MIDI's own form.
constexpr bool travels_as_note_command(uint8_t id) {
    switch (id) {
        case CC_REVERB_SEND: case CC_DELAY_SEND:
        case CC_FILTER_CUT:  case CC_FILTER_RES:
        case CC_FILTER_LP:   case CC_FILTER_HP:   case CC_FILTER_BP:
        case CC_DRIVE:       case CC_CRUSH:       case CC_LOOP_SLIDE:
            return true;
        default:
            return false;
    }
}

inline void encode_note_command(const NoteCommand& c, uint8_t out[NOTE_COMMAND_BYTES]) {
    const uint8_t bytes[NOTE_COMMAND_BYTES] = {
        MIDI_SYSEX, 0x7D, 'P', 'T', c.kind, static_cast<uint8_t>(c.channel & 0x0F),
        static_cast<uint8_t>(c.note & 0x7F),
        static_cast<uint8_t>(c.id >> 7),    static_cast<uint8_t>(c.id & 0x7F),
        static_cast<uint8_t>(c.value >> 7), static_cast<uint8_t>(c.value & 0x7F), MIDI_SYSEX_END};
    for (int i = 0; i < NOTE_COMMAND_BYTES; ++i) out[i] = bytes[i];
}

// A SysEx's bytes between F0 and F7. False for anyone else's.
inline bool decode_note_command(const uint8_t* body, int len, NoteCommand& c) {
    if (len != NOTE_COMMAND_BYTES - 2 || body[0] != 0x7D || body[1] != 'P' || body[2] != 'T') return false;
    if (body[3] != NOTE_COMMAND_WITH_NOTE && body[3] != NOTE_COMMAND_NOW) return false;
    if (body[4] > 0x0F || body[6] > 1 || body[8] > 1) return false;   // past a channel, or 8 bits
    c.kind    = body[3];
    c.channel = body[4];
    c.note    = body[5];
    c.id      = static_cast<uint8_t>((body[6] << 7) | body[7]);
    c.value   = static_cast<uint8_t>((body[8] << 7) | body[9]);
    return true;
}

}  // namespace songcore

#endif  // POCKETTRACKER_SONGCORE_NOTE_COMMAND_H
