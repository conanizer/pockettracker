#ifndef POCKETTRACKER_SONGCORE_MIDI_MAP_H
#define POCKETTRACKER_SONGCORE_MIDI_MAP_H

// midi_map.h — a controller's knob moves a parameter in the song.
//
// ⭐⭐ **THE CATALOGUE BELOW IS THE FEATURE; THE SCREEN IS THE CHEAP PART.** Everywhere else in this
// app the cursor sitting on a cell is enough to say what is being edited — a module answers
// `cursor_context()` with "a hex byte, 0..255" and one generic handler does the rest. That makes a
// cursor position a SEAT, not a name, and a seat cannot be stored: `instrument_row_layout.h` is 16
// rows for a sampler and 15 for a SoundFont, so the same seat is a different knob after a type
// change. A mapping has to name a PARAMETER, so this file is where parameters get names.
//
// ⚠️⚠️ **AN ENTRY'S ID IS ITS IDENTITY AND IT IS WRITTEN INTO THE .ptp. APPEND, NEVER INSERT, NEVER
// REUSE, NEVER RENUMBER** — the rule `EFFECT_TYPES`, `SettingsRow` and `InstrumentType` already live
// under, for the same reason. Removing a parameter means leaving its id dead, not closing the gap.
// Lookup is BY ID rather than by array index so the table's order is free to change.
//
// ⚠️ A `scope` is what the id alone cannot say: which track's fader, which instrument's cutoff. It is
// resolved when the mapping is LEARNED and stored — "instrument 3's cutoff", never "the cutoff of
// whatever instrument is selected", which is the only reading that survives the cursor moving.
//
// ⚠️ **RANGES ARE IN THE DESTINATION'S OWN UNITS, never 0..127.** A volume is 0..255, a crush is
// 0..15. A user who sets a cutoff range has to type the numbers the cutoff row shows them, so the
// range a mapping carries is bounded by the catalogue entry's own min/max, and `scale_cc` below is
// the ONE place a 0..127 controller value becomes one of them.

#include <algorithm>
#include <cstdint>
#include <vector>

#include "model.h"

namespace songcore {

/** What a mapping's stored `scopeIndex` means. */
enum class MapScope : uint8_t {
    NONE,        // the master bus, the send buses — there is only one of it
    TRACK,       // 0-7
    INSTRUMENT,  // 0-127
};

/**
 * ⚠️⚠️ **APPEND ONLY. A NUMBER HERE IS A NUMBER IN SOMEONE'S SAVED SONG.**
 *
 * Which parameters are in the list at all is a decision, not a sweep of everything editable: a
 * parameter earns a place by being worth turning while the song plays.
 */
enum class MapDestId : uint8_t {
    NONE = 0,

    TRACK_VOL  = 1,
    MASTER_VOL = 2,

    REV_DCAY = 3,   // the tail's length — `reverbFeedback`, the DCAY cell's own field name
    REV_DAMP = 4,
    REV_WET  = 5,
    REV_SIZE = 6,
    REV_PRE  = 7,
    REV_WIDE = 8,
    REV_MOD  = 9,

    DLY_TIME = 10,
    DLY_FDBK = 11,
    DLY_WET  = 12,
    DLY_TONE = 13,
    DLY_WOBL = 14,
    DLY_SEND = 15,  // the delay's own feed into the reverb

    OTT_DEPTH  = 16,
    DUST_DEPTH = 17,
    LIMIT_PRE  = 18,

    INS_VOL    = 19,
    INS_PAN    = 20,
    INS_CUT    = 21,
    INS_RES    = 22,
    INS_DRIVE  = 23,
    INS_CRUSH  = 24,
    INS_DWN    = 25,
    INS_REV    = 26,
    INS_DLY    = 27,
    INS_DETUNE = 28,
};

struct MapDest {
    MapDestId   id;
    const char* name;   // what the list's DEST column prints
    MapScope    scope;
    int         min;    // the destination's own range, and the bound on a mapping's own
    int         max;
};

// ⚠️ The order of this array carries nothing — `map_dest()` searches by id. Keeping it grouped the
// way the screens are is for the reader only.
inline constexpr MapDest MAP_DESTS[] = {
    {MapDestId::TRACK_VOL,  "TRACK VOL",  MapScope::TRACK,      0, 255},
    {MapDestId::MASTER_VOL, "MASTER VOL", MapScope::NONE,       0, 255},

    {MapDestId::REV_DCAY,   "REV DCAY",   MapScope::NONE,       0, 255},
    {MapDestId::REV_DAMP,   "REV DAMP",   MapScope::NONE,       0, 255},
    {MapDestId::REV_WET,    "REV WET",    MapScope::NONE,       0, 255},
    {MapDestId::REV_SIZE,   "REV SIZE",   MapScope::NONE,       0, 255},
    {MapDestId::REV_PRE,    "REV PRE",    MapScope::NONE,       0, 255},
    {MapDestId::REV_WIDE,   "REV WIDE",   MapScope::NONE,       0, 255},
    {MapDestId::REV_MOD,    "REV MOD",    MapScope::NONE,       0, 255},

    {MapDestId::DLY_TIME,   "DLY TIME",   MapScope::NONE,       0, 255},
    {MapDestId::DLY_FDBK,   "DLY FDBK",   MapScope::NONE,       0, 255},
    {MapDestId::DLY_WET,    "DLY WET",    MapScope::NONE,       0, 255},
    {MapDestId::DLY_TONE,   "DLY TONE",   MapScope::NONE,       0, 255},
    {MapDestId::DLY_WOBL,   "DLY WOBL",   MapScope::NONE,       0, 255},
    {MapDestId::DLY_SEND,   "DLY>REV",    MapScope::NONE,       0, 255},

    {MapDestId::OTT_DEPTH,  "OTT",        MapScope::NONE,       0, 255},
    {MapDestId::DUST_DEPTH, "DUST",       MapScope::NONE,       0, 255},
    {MapDestId::LIMIT_PRE,  "LIMIT PRE",  MapScope::NONE,       0, 255},

    {MapDestId::INS_VOL,    "INS VOL",    MapScope::INSTRUMENT, 0, 255},
    {MapDestId::INS_PAN,    "INS PAN",    MapScope::INSTRUMENT, 0, 255},
    {MapDestId::INS_CUT,    "INS CUT",    MapScope::INSTRUMENT, 0, 255},
    {MapDestId::INS_RES,    "INS RES",    MapScope::INSTRUMENT, 0, 255},
    {MapDestId::INS_DRIVE,  "INS DRIVE",  MapScope::INSTRUMENT, 0, 255},
    {MapDestId::INS_CRUSH,  "INS CRUSH",  MapScope::INSTRUMENT, 0,  15},
    {MapDestId::INS_DWN,    "INS DWNSMP", MapScope::INSTRUMENT, 0,  15},
    {MapDestId::INS_REV,    "INS REV",    MapScope::INSTRUMENT, 0, 255},
    {MapDestId::INS_DLY,    "INS DLY",    MapScope::INSTRUMENT, 0, 255},
    {MapDestId::INS_DETUNE, "INS DETUNE", MapScope::INSTRUMENT, 0, 255},
};

inline constexpr int MAP_DEST_COUNT = static_cast<int>(sizeof(MAP_DESTS) / sizeof(MAP_DESTS[0]));

/** The catalogue entry for an id, or null — which is what an id from a NEWER version reads as. */
inline const MapDest* map_dest(MapDestId id) {
    for (const MapDest& d : MAP_DESTS)
        if (d.id == id) return &d;
    return nullptr;
}

inline const MapDest* map_dest(uint8_t id) { return map_dest(static_cast<MapDestId>(id)); }

/**
 * ⚠️ **A COUNT PLUS THAT MANY ENTRIES, NEVER A FIXED 128-SLOT ARRAY** (`Project::midiMappings`). The
 * list GROWS: a song with no mappings carries none, and the screen shows its headers and nothing
 * else. A fixed array would put 128 blank rows in every .ptp and leave the screen hiding them.
 *
 * ⚠️ **ONE DESTINATION TAKES ONE MAPPING; ONE CONTROLLER MAY DRIVE MANY** — M8's asymmetry, and the
 * interesting half: a knob fanning out to several destinations is a macro control, and it comes free
 * from the list being flat.
 */
inline constexpr int MIDI_MAP_MAX = 128;

/**
 * A 0-127 controller value into the destination's own units. ⭐ **THE ONE PLACE THAT CONVERSION
 * HAPPENS** — the MIDI plan's §11 rule, which exists because a second copy drifts from the first.
 *
 * ⚠️ `lo > hi` is deliberate and works: a range typed backwards is an INVERTED mapping, which the
 * range rows get for free rather than needing a flag of their own.
 */
inline int scale_cc(int cc, int lo, int hi) {
    const int c = cc < 0 ? 0 : (cc > 127 ? 127 : cc);
    const int span = hi - lo;
    // Rounded to nearest rather than truncated, in both directions: integer division truncates
    // TOWARDS ZERO, so the half has to carry the span's sign or an inverted range lands a step high.
    const int half = span >= 0 ? 63 : -63;
    return lo + static_cast<int>((static_cast<long long>(span) * c + half) / 127);
}

/**
 * Does this mapping still point at something? ⚠️ **A "NO" MUST NOT DELETE THE ROW** — the screen
 * greys it and says why, because a mapping silently dropped when a slot was cleared for a minute is
 * one the user has to notice is missing before they can make it again.
 */
inline bool map_dest_present(const Project& p, const MidiMapping& m) {
    const MapDest* d = map_dest(m.dest);
    if (!d) return false;
    switch (d->scope) {
        case MapScope::NONE:  return true;
        case MapScope::TRACK: return m.scopeIndex < p.tracks.size();
        case MapScope::INSTRUMENT:
            // The empty-slot convention, one place, as everywhere else: an instrument is empty iff it
            // has no sample path — not `sampleId < 0`, not an empty name.
            return m.scopeIndex < p.instruments.size() &&
                   (p.instruments[m.scopeIndex].sampleFilePath.has_value() ||
                    p.instruments[m.scopeIndex].soundfontPath.has_value());
    }
    return false;
}

/**
 * Write `value` (already in the destination's units) into the song. Returns false and writes nothing
 * if the destination is not there any more.
 *
 * ⚠️ **IT ONLY WRITES.** Making the engine hear it is `push_mapped_dest` (engine_setup.h), and
 * marking the song modified is the dispatcher's — the three are split because they have different
 * costs and, since a knob moves ~30 times a second, the cost is the whole design.
 */
inline bool write_mapped(Project& p, const MidiMapping& m, int value) {
    const MapDest* d = map_dest(m.dest);
    if (!d || !map_dest_present(p, m)) return false;

    const int v = std::clamp(value, std::min(d->min, d->max), std::max(d->min, d->max));
    const size_t at = m.scopeIndex;

    switch (d->id) {
        case MapDestId::NONE:       return false;

        case MapDestId::TRACK_VOL:  p.tracks[at].volume = v;   return true;
        case MapDestId::MASTER_VOL: p.masterVolume = v;        return true;

        case MapDestId::REV_DCAY:   p.reverbFeedback = v;      return true;
        case MapDestId::REV_DAMP:   p.reverbDamp = v;          return true;
        case MapDestId::REV_WET:    p.reverbWet = v;           return true;
        case MapDestId::REV_SIZE:   p.reverbSize = v;          return true;
        case MapDestId::REV_PRE:    p.reverbPreDelay = v;      return true;
        case MapDestId::REV_WIDE:   p.reverbWidth = v;         return true;
        case MapDestId::REV_MOD:    p.reverbMod = v;           return true;

        case MapDestId::DLY_TIME:   p.delayTime = v;           return true;
        case MapDestId::DLY_FDBK:   p.delayFeedback = v;       return true;
        case MapDestId::DLY_WET:    p.delayWet = v;            return true;
        case MapDestId::DLY_TONE:   p.delayTone = v;           return true;
        case MapDestId::DLY_WOBL:   p.delayWobble = v;         return true;
        case MapDestId::DLY_SEND:   p.delayReverbSend = v;     return true;

        case MapDestId::OTT_DEPTH:  p.ottDepth = v;            return true;
        case MapDestId::DUST_DEPTH: p.dustDepth = v;           return true;
        case MapDestId::LIMIT_PRE:  p.limiterPreGain = v;      return true;

        case MapDestId::INS_VOL:    p.instruments[at].volume = v;      return true;
        case MapDestId::INS_PAN:    p.instruments[at].pan = v;         return true;
        case MapDestId::INS_CUT:    p.instruments[at].filterCut = v;   return true;
        case MapDestId::INS_RES:    p.instruments[at].filterRes = v;   return true;
        case MapDestId::INS_DRIVE:  p.instruments[at].drive = v;       return true;
        case MapDestId::INS_CRUSH:  p.instruments[at].crush = v;       return true;
        case MapDestId::INS_DWN:    p.instruments[at].downsample = v;  return true;
        case MapDestId::INS_REV:    p.instruments[at].reverbSend = v;  return true;
        case MapDestId::INS_DLY:    p.instruments[at].delaySend = v;   return true;
        case MapDestId::INS_DETUNE: p.instruments[at].detune = v;      return true;
    }
    return false;
}

/** What the song currently reads on a mapping's destination — the list's live value, and what the
 *  learn gesture needs to seed a range with. Returns -1 when the destination is gone. */
inline int read_mapped(const Project& p, const MidiMapping& m) {
    const MapDest* d = map_dest(m.dest);
    if (!d || !map_dest_present(p, m)) return -1;
    const size_t at = m.scopeIndex;

    switch (d->id) {
        case MapDestId::NONE:       return -1;

        case MapDestId::TRACK_VOL:  return p.tracks[at].volume;
        case MapDestId::MASTER_VOL: return p.masterVolume;

        case MapDestId::REV_DCAY:   return p.reverbFeedback;
        case MapDestId::REV_DAMP:   return p.reverbDamp;
        case MapDestId::REV_WET:    return p.reverbWet;
        case MapDestId::REV_SIZE:   return p.reverbSize;
        case MapDestId::REV_PRE:    return p.reverbPreDelay;
        case MapDestId::REV_WIDE:   return p.reverbWidth;
        case MapDestId::REV_MOD:    return p.reverbMod;

        case MapDestId::DLY_TIME:   return p.delayTime;
        case MapDestId::DLY_FDBK:   return p.delayFeedback;
        case MapDestId::DLY_WET:    return p.delayWet;
        case MapDestId::DLY_TONE:   return p.delayTone;
        case MapDestId::DLY_WOBL:   return p.delayWobble;
        case MapDestId::DLY_SEND:   return p.delayReverbSend;

        case MapDestId::OTT_DEPTH:  return p.ottDepth;
        case MapDestId::DUST_DEPTH: return p.dustDepth;
        case MapDestId::LIMIT_PRE:  return p.limiterPreGain;

        case MapDestId::INS_VOL:    return p.instruments[at].volume;
        case MapDestId::INS_PAN:    return p.instruments[at].pan;
        case MapDestId::INS_CUT:    return p.instruments[at].filterCut;
        case MapDestId::INS_RES:    return p.instruments[at].filterRes;
        case MapDestId::INS_DRIVE:  return p.instruments[at].drive;
        case MapDestId::INS_CRUSH:  return p.instruments[at].crush;
        case MapDestId::INS_DWN:    return p.instruments[at].downsample;
        case MapDestId::INS_REV:    return p.instruments[at].reverbSend;
        case MapDestId::INS_DLY:    return p.instruments[at].delaySend;
        case MapDestId::INS_DETUNE: return p.instruments[at].detune;
    }
    return -1;
}

}  // namespace songcore

#endif  // POCKETTRACKER_SONGCORE_MIDI_MAP_H
