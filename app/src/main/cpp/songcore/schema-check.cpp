// Compile-time guard for songcore/event.h — the header has no other consumer until songcore
// Phase 1's router lands, and an uncompiled normative header rots. Any drift in the frozen tag
// values or the record shape fails the build here.

#include "event.h"

namespace songcore {

static_assert(SCHEMA_VERSION == 1, "schema bump requires regenerated goldens + doc update");

static_assert(EV_EXT_PITCH_RATE == 0x01 && EV_EXT_VIBRATO == 0x02 && EV_EXT_TABLE_ROW == 0x03 &&
              EV_EXT_REVERSE == 0x04 && EV_EXT_EQ_SLOT == 0x05 && EV_EXT_MASTER_EQ == 0x06,
              "EXT tags are frozen");
static_assert(EV_NOTE_OFF == 0x80 && EV_NOTE_ON == 0x90 && EV_CC == 0xB0 &&
              EV_PROGRAM == 0xC0 && EV_PITCH_BEND == 0xE0,
              "MIDI-status tags are frozen");

static_assert(TRACK_PREVIEW == 8 && TRACK_GLOBAL == 0xFF, "track domain is frozen");
static_assert(NOTE_OFF_RELEASE == 0 && NOTE_OFF_CUT == 1, "note-off modes are frozen");
static_assert(CC_VOLUME == 7 && CC_PAN == 10 && CC_REVERB_SEND == 91 && CC_DELAY_SEND == 93,
              "CC ids are frozen");

// Same-frame drain order: param-class → NoteOff → NoteOn
static_assert(sortRank(EV_NOTE_ON) == 2 && sortRank(EV_NOTE_OFF) == 1 &&
              sortRank(EV_CC) == 0 && sortRank(EV_EXT_MASTER_EQ) == 0,
              "canonical sort ranks are frozen");

// Payload shapes: note(1)+vel(1)+pad(2) + 15 × 4-byte fields = 64. A size change means a field
// was added/removed/reordered — that is a schema bump with regenerated goldens.
static_assert(sizeof(NoteOnPayload) == 64, "NoteOnPayload shape changed — schema bump required");
static_assert(sizeof(Event) - sizeof(NoteOnPayload) >= 12, "envelope shape changed");

}  // namespace songcore
