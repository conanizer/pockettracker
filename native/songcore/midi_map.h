#ifndef POCKETTRACKER_SONGCORE_MIDI_MAP_H
#define POCKETTRACKER_SONGCORE_MIDI_MAP_H

// midi_map.h — a controller's knob moves a parameter in the song.
//
// A mapping must name a PARAMETER, not a cursor position: the same row index is a different knob on a
// sampler and a SoundFont instrument. This catalogue is where parameters get names.
//
// ⚠️ An entry's id is written into the .ptp: APPEND, never insert, reuse or renumber. A removed
// parameter leaves its id dead. Lookup is by id, so the array's order is free.
// ⚠️ `scope` (which track, which instrument) is resolved when the mapping is LEARNED and stored —
// "instrument 3's cutoff", never "the selected instrument's".
// ⚠️ Ranges are in the destination's own units (volume 0..255, crush 0..15); `scale_cc` is the one
// place a 0..127 controller value becomes one of them.

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
 * Which cluster a destination belongs to, for picking one. Not derivable from `MapScope` (MASTER,
 * REVERB and DELAY all have none). A reading aid, never stored, so regrouping is free.
 */
enum class MapGroup : uint8_t {
    TRACK,
    MASTER,
    REVERB,
    DELAY,
    INSTRUMENT,
};

inline constexpr int MAP_GROUP_COUNT = 5;

/** Three characters: the list row spends its width on the parameter name. */
inline const char* map_group_name(MapGroup g) {
    switch (g) {
        case MapGroup::TRACK:      return "TRK";
        case MapGroup::MASTER:     return "MIX";
        case MapGroup::REVERB:     return "REV";
        case MapGroup::DELAY:      return "DLY";
        case MapGroup::INSTRUMENT: return "INS";
    }
    return "---";
}

/**
 * ⚠️ APPEND ONLY — a number here is a number in someone's saved song. A parameter is listed only if
 * it is worth turning while the song plays.
 */
enum class MapDestId : uint8_t {
    NONE = 0,

    TRACK_VOL  = 1,
    MASTER_VOL = 2,

    REV_DCAY = 3,   // the tail's length — `reverbFeedback`
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
    /** The PICKER's name, read under its section heading — so the instrument's drop the group word,
     *  while `REV WET` / `DLY WET` keep it (they share a heading). */
    const char* name;
    MapScope    scope;
    int         min;    // the destination's own range, and the bound on a mapping's own
    int         max;
    MapGroup    group;
    /**
     * The list row's name, group word dropped (the group is in the next cell).
     * ⚠️ Five characters at most: the row's columns fill the panel's 28, and a sixth is clipped.
     */
    const char* cell;
};

// The array order carries no identity, but it is the order a group's parameters are dialled in.
inline constexpr MapDest MAP_DESTS[] = {
    {MapDestId::TRACK_VOL,  "TRACK VOL",  MapScope::TRACK,      0, 255, MapGroup::TRACK,      "VOL"},

    // The array order is the PICKER's reading order (`ui/map_picker.h`), so the master fader follows
    // the two colour effects.
    {MapDestId::OTT_DEPTH,  "OTT",        MapScope::NONE,       0, 255, MapGroup::MASTER,     "OTT"},
    {MapDestId::DUST_DEPTH, "DUST",       MapScope::NONE,       0, 255, MapGroup::MASTER,     "DUST"},
    {MapDestId::MASTER_VOL, "MIX VOL",    MapScope::NONE,       0, 255, MapGroup::MASTER,     "VOL"},
    {MapDestId::LIMIT_PRE,  "LIMITER",    MapScope::NONE,       0, 255, MapGroup::MASTER,     "LIMIT"},

    {MapDestId::REV_DCAY,   "REV DCAY",   MapScope::NONE,       0, 255, MapGroup::REVERB,     "DCAY"},
    {MapDestId::REV_DAMP,   "REV DAMP",   MapScope::NONE,       0, 255, MapGroup::REVERB,     "DAMP"},
    {MapDestId::REV_WET,    "REV WET",    MapScope::NONE,       0, 255, MapGroup::REVERB,     "WET"},
    {MapDestId::REV_SIZE,   "REV SIZE",   MapScope::NONE,       0, 255, MapGroup::REVERB,     "SIZE"},
    {MapDestId::REV_PRE,    "REV PRE",    MapScope::NONE,       0, 255, MapGroup::REVERB,     "PRE"},
    {MapDestId::REV_WIDE,   "REV WIDE",   MapScope::NONE,       0, 255, MapGroup::REVERB,     "WIDE"},
    {MapDestId::REV_MOD,    "REV MOD",    MapScope::NONE,       0, 255, MapGroup::REVERB,     "MOD"},

    {MapDestId::DLY_TIME,   "DLY TIME",   MapScope::NONE,       0, 255, MapGroup::DELAY,      "TIME"},
    {MapDestId::DLY_FDBK,   "DLY FDBK",   MapScope::NONE,       0, 255, MapGroup::DELAY,      "FDBK"},
    {MapDestId::DLY_WET,    "DLY WET",    MapScope::NONE,       0, 255, MapGroup::DELAY,      "WET"},
    {MapDestId::DLY_TONE,   "DLY TONE",   MapScope::NONE,       0, 255, MapGroup::DELAY,      "TONE"},
    {MapDestId::DLY_WOBL,   "DLY WOBL",   MapScope::NONE,       0, 255, MapGroup::DELAY,      "WOBL"},
    {MapDestId::DLY_SEND,   "DLY>REV",    MapScope::NONE,       0, 255, MapGroup::DELAY,      ">REV"},

    {MapDestId::INS_VOL,    "VOL",        MapScope::INSTRUMENT, 0, 255, MapGroup::INSTRUMENT, "VOL"},
    {MapDestId::INS_PAN,    "PAN",        MapScope::INSTRUMENT, 0, 255, MapGroup::INSTRUMENT, "PAN"},
    {MapDestId::INS_CUT,    "FREQ",       MapScope::INSTRUMENT, 0, 255, MapGroup::INSTRUMENT, "FREQ"},
    {MapDestId::INS_RES,    "RES",        MapScope::INSTRUMENT, 0, 255, MapGroup::INSTRUMENT, "RES"},
    {MapDestId::INS_DRIVE,  "DRIVE",      MapScope::INSTRUMENT, 0, 255, MapGroup::INSTRUMENT, "DRIVE"},
    {MapDestId::INS_CRUSH,  "CRUSH",      MapScope::INSTRUMENT, 0,  15, MapGroup::INSTRUMENT, "CRUSH"},
    // Names match the INSTRUMENT screen's rows; only the five-character cell must abbreviate.
    {MapDestId::INS_DWN,    "DWNSMPL",    MapScope::INSTRUMENT, 0,  15, MapGroup::INSTRUMENT, "DWNSM"},
    {MapDestId::INS_REV,    "REV",        MapScope::INSTRUMENT, 0, 255, MapGroup::INSTRUMENT, "REV"},
    {MapDestId::INS_DLY,    "DEL",        MapScope::INSTRUMENT, 0, 255, MapGroup::INSTRUMENT, "DEL"},
    {MapDestId::INS_DETUNE, "DETUNE",     MapScope::INSTRUMENT, 0, 255, MapGroup::INSTRUMENT, "DTUNE"},
};

inline constexpr int MAP_DEST_COUNT = static_cast<int>(sizeof(MAP_DESTS) / sizeof(MAP_DESTS[0]));

/** The catalogue entry for an id, or null — as an id from a newer version reads. */
inline const MapDest* map_dest(MapDestId id) {
    for (const MapDest& d : MAP_DESTS)
        if (d.id == id) return &d;
    return nullptr;
}

inline const MapDest* map_dest(uint8_t id) { return map_dest(static_cast<MapDestId>(id)); }

// ─── Picking one out of twenty-eight ─────────────────────────────────────────────────────────────
//
// A destination is TWO cells — a group and a parameter within it — each cycled like any other cell.
// These walk MAP_DESTS in array order, the order the parameters are dialled in.

/** How many destinations `g` holds. */
inline int map_group_size(MapGroup g) {
    int n = 0;
    for (const MapDest& d : MAP_DESTS)
        if (d.group == g) ++n;
    return n;
}

/** The `index`-th destination of `g`, or null when the group is shorter than that. */
inline const MapDest* map_dest_in_group(MapGroup g, int index) {
    int n = 0;
    for (const MapDest& d : MAP_DESTS)
        if (d.group == g && n++ == index) return &d;
    return nullptr;
}

/** Where `id` sits inside its own group, or 0 for an id this build does not know. */
inline int map_index_in_group(MapDestId id) {
    const MapDest* self = map_dest(id);
    if (!self) return 0;
    int n = 0;
    for (const MapDest& d : MAP_DESTS) {
        if (d.group != self->group) continue;
        if (d.id == id) return n;
        ++n;
    }
    return 0;
}

/**
 * `Project::midiMappings` is a growing list (count plus entries), never a fixed 128 slots — a song
 * with no mappings carries none.
 * One destination takes one mapping; one controller may drive many (a macro knob).
 */
inline constexpr int MIDI_MAP_MAX = 128;

/**
 * The control channel's "any channel" value, and the default: most people do not know their knob's
 * channel. ⚠️ Safe only because a CC is CLAIMED, not reserved: a CC that drives a mapping is consumed,
 * any other routes to its track as before.
 */
inline constexpr int MIDI_CTL_CH_ALL = 16;

/**
 * Does the control-channel setting let a knob on `channel` drive a mapping? The ONE reading of the
 * setting, shared by the MIDI drain and the learn gesture.
 */
inline bool ctl_ch_covers(int setting, int channel) {
    return setting == MIDI_CTL_CH_ALL || (setting >= 0 && setting == channel);
}

/**
 * A 0-127 controller value into the destination's units — the one place this conversion happens.
 * `lo > hi` is an inverted mapping, and works.
 */
inline int scale_cc(int cc, int lo, int hi) {
    const int c = cc < 0 ? 0 : (cc > 127 ? 127 : cc);
    const int span = hi - lo;
    // Round to nearest both ways: division truncates toward zero, so the half carries the span's sign.
    const int half = span >= 0 ? 63 : -63;
    return lo + static_cast<int>((static_cast<long long>(span) * c + half) / 127);
}

/**
 * Does this mapping still point at something? ⚠️ A "no" must not delete the row — the screen greys it.
 * The slot test is `instrument_is_free`, not a path test: an EXTERNAL instrument has no file, yet its
 * VOL and PAN are real.
 */
inline bool map_dest_present(const Project& p, const MidiMapping& m) {
    const MapDest* d = map_dest(m.dest);
    if (!d) return false;
    switch (d->scope) {
        case MapScope::NONE:  return true;
        case MapScope::TRACK: return m.scopeIndex < p.tracks.size();
        case MapScope::INSTRUMENT:
            return m.scopeIndex < p.instruments.size() &&
                   !instrument_is_free(p.instruments[m.scopeIndex]);
    }
    return false;
}

/**
 * Write `value` (in the destination's units) into the song; false if the destination is gone.
 * ⚠️ It only writes. Hearing it is `push_mapped_dest` (engine_setup.h); marking the song modified is
 * the dispatcher's. Kept apart because a knob moves ~30 times a second.
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

/** What the song reads on a mapping's destination (the list's live value, the learn seed), or -1
 *  when it is gone. */
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

// ─── Learning one ────────────────────────────────────────────────────────────────────────────────

/**
 * What the cell under the cursor names — beside `cursor_context()` (what KIND of value), this says
 * what it is CALLED. `NONE` when the screen cannot name it. The scope is resolved here and stored.
 */
struct MapTarget {
    MapDestId id    = MapDestId::NONE;
    uint8_t   scope = 0;

    bool named() const { return id != MapDestId::NONE; }
};

/**
 * Point an existing mapping at `d`; false when it already pointed there.
 * ⚠️ The range and scope reset with the destination: 00..FF carried onto a crush (0..F) is unreachable.
 * Shared by the group cell, the parameter cell and the picker.
 */
inline bool take_dest(MidiMapping& m, const MapDest& d) {
    if (m.dest == static_cast<uint8_t>(d.id)) return false;
    m.dest       = static_cast<uint8_t>(d.id);
    m.rangeMin   = d.min;
    m.rangeMax   = d.max;
    m.scopeIndex = 0;
    return true;
}

/**
 * Point `controller` at `target`. Returns the row, or −1 when the list is full.
 * One destination takes one mapping, so learning an already-mapped destination RE-POINTS its row and
 * keeps its range (the user's work); a new row gets the destination's full range.
 */
inline int learn_mapping(Project& p, const MapTarget& target, int controller) {
    const MapDest* d = map_dest(target.id);
    if (!d) return -1;
    const uint8_t dest  = static_cast<uint8_t>(target.id);
    const uint8_t scope = d->scope == MapScope::NONE ? 0 : target.scope;
    const uint8_t cc    = static_cast<uint8_t>(std::clamp(controller, 0, 127));

    for (size_t i = 0; i < p.midiMappings.size(); ++i) {
        MidiMapping& m = p.midiMappings[i];
        if (m.dest != dest || m.scopeIndex != scope) continue;
        m.controller = cc;
        return static_cast<int>(i);
    }

    if (static_cast<int>(p.midiMappings.size()) >= MIDI_MAP_MAX) return -1;
    p.midiMappings.push_back({cc, dest, scope, d->min, d->max});
    return static_cast<int>(p.midiMappings.size()) - 1;
}

}  // namespace songcore

#endif  // POCKETTRACKER_SONGCORE_MIDI_MAP_H
