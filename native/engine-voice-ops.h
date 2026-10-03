#pragma once
// Per-voice writes shared by the table walk (engine-tables.cpp) and the block's queue drain
// (engine-mix.cpp). Private to the engine: one overload per voice type, resolved at compile time.
#include "audio-engine.h"

// Per-voice-type table FX behaviour, resolved at compile time inside processTableTick:
//   KIL:    sampler = declicked kill fade; SF = noteOff (TSF plays its own release).
//   OFFSET: sampler repositions playback; SF voices have no sample position — ignored.
// `at` is the frame of the block the row plays on.
static inline void tableKill(Voice& v, int at)          { v.startFadeOut(KILL_FADE_SAMPLES, at); }
static inline void tableKill(SoundfontVoice& v, int at) { v.noteOffAt(at); }
static inline void tableOffset(Voice& v, uint8_t fxValue) {
    if (v.sampleLength > 0) {
        double normalizedPos = fxValue / 255.0;
        v.position = normalizedPos * (v.sampleLength - 1);
    }
}
static inline void tableOffset(SoundfontVoice&, uint8_t) {}

// ─── CUT / RES — the one place a live filter override lands ───────────────────────────────────────
//
// Shared by the param-queue arm (the FX column and an AUS/AUF ramp) and the table's own CUT/RES rows,
// for both voice types.
//
// ⚠️ **INERT ON A VOICE RUNNING NO FILTER** (`type == 0`, i.e. the instrument's FILTER TYPE is OFF).
// These move the filter the instrument declares; they do not switch one on.
//
// The live values are on the param bus, which every voice type's per-block recompute reads. The
// override is per-note by construction: a note-on rebuilds the chain from the instrument, so nothing
// has to be restored when the note ends.
static inline void filterStore(IAudioVoice& v, int cut, int res) {
    v.params.setBase(PARAM_FILTER_CUT, (float)cut);
    v.params.setBase(PARAM_FILTER_RES, (float)res);
}

template <typename V>
static inline void voiceSetFilter(V& v, int cut, int res, float sampleRate) {
    filterStore(v, cut, res);
    if (!v.chain.filter.enabled()) return;
    // Modulation still applies on top — the same sum the per-block recompute makes, so a CUT under an
    // LFO moves the centre the LFO swings around instead of fighting it for one block.
    int modCut = std::max(0, std::min(255, (int)(cut + v.modDestValues[PARAM_FILTER_CUT])));
    int modRes = std::max(0, std::min(255, (int)(res + v.modDestValues[PARAM_FILTER_RES])));
    v.chain.filter.setParams(v.chain.filter.type, modCut, modRes, v.chain.filter.drive, sampleRate);
}
// CUT and RES are one FilterModule call, so each carries the other's CURRENT value through. Read off
// the bus, which both voice types seed from the instrument at trigger.
template <typename V> static inline void voiceSetFilterCut(V& v, int cut, float sr) {
    voiceSetFilter(v, cut, (int)v.params.base[PARAM_FILTER_RES], sr);
}
template <typename V> static inline void voiceSetFilterRes(V& v, int res, float sr) {
    voiceSetFilter(v, (int)v.params.base[PARAM_FILTER_CUT], res, sr);
}

// LPF / HPF / BPF — the one write that switches a filter ON.
//
// ⚠️ **IT DELIBERATELY SKIPS `voiceSetFilter`'s `enabled()` GUARD**, which is the whole difference:
// that guard is what makes CUT and RES inert on an instrument whose FILTER TYPE is OFF, and turning
// the filter on is what these three are for. Resonance rides along at whatever the voice currently
// holds — the instrument's, or the last RES — because one FilterModule call takes all four numbers
// and passing a fresh 0 here would silently undo a RES written on the step before.
//
// Per-note like the other two: a note-on rebuilds the chain from the instrument, so a filter opened
// from a cell is gone by the next note with nothing to restore.
template <typename V> static inline void voiceSetFilterMode(V& v, int type, int cut, float sr) {
    const int res = (int)v.params.base[PARAM_FILTER_RES];
    filterStore(v, cut, res);
    int modCut = std::max(0, std::min(255, (int)(cut + v.modDestValues[PARAM_FILTER_CUT])));
    int modRes = std::max(0, std::min(255, (int)(res + v.modDestValues[PARAM_FILTER_RES])));
    v.chain.filter.setParams(type, modCut, modRes, v.chain.filter.drive, sr);
}

// LPO. ⚠️ **IT ADDS, WHERE EVERY OTHER SETTER ON THIS PAGE ASSIGNS** — the byte is a signed STEP in
// sixteenths of the loop's own length, and the voice keeps the running total. Written on a table row
// it therefore walks the window a step per tic, which is the shape the technique is actually used in.
// Sampler-only, silently: a SoundFont voice has no sample position to slide.
static inline void voiceSlideLoop(Voice& v, int byteValue) {
    v.loopSlideSixteenths += loopSlideSixteenthsOf(byteValue);
}
static inline void voiceSlideLoop(SoundfontVoice&, int) {}

// ─── DRV / CRU — writes onto the per-block recompute's own inputs ────────────────────────────────
//
// Every voice type re-derives drive and crush from `params.base` once per block, so the base write IS
// the command. Per-note by construction: a note-on reseeds the bus from the instrument.
static inline void voiceSetDrive(IAudioVoice& v, int drive) {
    v.params.setBase(PARAM_DRIVE, (float)drive);
}

// ⚠️ The cell is TWO numbers. The sampler passes 0 for downsample at the chain and quantizes the read
// address instead — a different effect from the module's, and the reason the base write must carry
// both halves rather than being handed to `crush.setParams` here.
static inline void voiceSetCrush(IAudioVoice& v, int packed) {
    v.params.setBase(PARAM_CRUSH,      (float)crushBitsOf(packed));
    v.params.setBase(PARAM_DOWNSAMPLE, (float)crushDownsampleOf(packed));
}

// ─── REV / DEL — the send levels ─────────────────────────────────────────────────────────────────
static inline void voiceSetSends(IAudioVoice& v, float rev, float dly) {
    v.reverbSend = rev;
    v.delaySend  = dly;
}

// ─── A SOUNDING VOICE RE-READS ITS INSTRUMENT ────────────────────────────────────────────────────
//
// ⚠️ **A TRIGGER COPIES THE INSTRUMENT INTO THE VOICE**, which is what makes an edit to the filter,
// the drive, the crush or the sends silent until the next note — the note you are hearing is running
// on the copy it took. This is the same write the FX arms above make, with the values coming from the
// instrument instead of a cell, and it is aimed at an INSTRUMENT rather than a track: a knob names a
// parameter of instrument 3, not of whatever track 3 happens to be playing.
//
// ⚠️ It therefore ENDS a live override an FX cell had made on those same parameters — the hand that
// just moved the instrument wins, exactly as it does everywhere else a press meets a running take.
template <typename V>
static inline void voiceReloadInstrument(V& v, const InstrumentParams& ip, float sampleRate) {
    // Mode before resonance: the mode call is the one that can switch the filter ON, and it carries
    // whatever resonance the voice holds — so the resonance write has to come after it, not before.
    voiceSetFilterMode(v, ip.filterType, ip.filterCut, sampleRate);
    voiceSetFilterRes(v, ip.filterRes, sampleRate);
    voiceSetDrive(v, ip.drive);
    voiceSetCrush(v, ((ip.crush & 0x0F) << 4) | (ip.downsample & 0x0F));
    voiceSetSends(v, ip.reverbSend, ip.delaySend);
}

// ─── FIN — the one command that lands in a bus slot the voices ALREADY reset ─────────────────────
//
// ⭐ It needs no field of its own: `PARAM_PITCH`'s BASE has been written to zero by both voice types'
// trigger paths since the bus existed and read by neither, while the MOD half carries the table
// transpose, the slides and the vibrato. Putting the fine tune in the base is therefore per-note by
// construction — a trigger clears it — and it cannot collide with any of those.
//
// ⚠️ Both voice types have to READ it, and they read it in different places: the sampler folds it
// into the playback rate (`getModulatedPlaybackRate`), the SoundFont voice into the pitch wheel
// (`soundfont-voice.cpp`), which also has to count it as active pitch or it re-centres the wheel.
// ─── PAN from a table row: a glide, not a jump ──────────────────────────────────────────────────
//
// A pan that jumps is a step in both channels' gain, and on a low note that is a click — loud on a
// row-by-row auto-pan. So a table row only sets the GOAL; the voice's per-piece pan update walks to it
// over PAN_GLIDE_FRAMES, and the table tick cuts the pieces to PAN_GLIDE_STEP while it does, so the
// sampler's per-sample interpolation turns each step into a ramp (TSF pans per piece, so a SoundFont
// moves in PAN_GLIDE_STEP stairs instead — small ones).
inline constexpr int PAN_GLIDE_FRAMES = 256;
inline constexpr int PAN_GLIDE_STEP   = 64;

template <typename V> static inline void voiceGlidePan(V& v, float pan) {
    if (!v.chain.started) { v.setPan(pan); return; }   // the note's own set-up: no glide from anywhere
    v.params.setBase(PARAM_PAN, pan);
    v.panGoal      = pan;
    v.panGlideLeft = PAN_GLIDE_FRAMES;
}

/** Advance a pan glide by `frames`; true when it moved, with the voice's new pan in `panNow`. */
template <typename V> static inline bool voiceStepPanGlide(V& v, int frames) {
    if (v.panGlideLeft <= 0) return false;
    const float step = frames >= v.panGlideLeft ? 1.0f : static_cast<float>(frames) / static_cast<float>(v.panGlideLeft);
    v.panNow += (v.panGoal - v.panNow) * step;
    v.panGlideLeft = frames >= v.panGlideLeft ? 0 : v.panGlideLeft - frames;
    return true;
}

// A table VOL on a SOUNDING note: the table volume itself is written as before; this arms the blend
// that hides the step. Before the first sample the new volume is simply where the note starts.
inline constexpr int VOL_GLIDE_FRAMES = 128;
template <typename V> static inline void voiceGlideVol(V& v) {
    if (!v.chain.started) return;
    v.volGlideFrom = v.volRouteLast;
    v.volGlideLeft = VOL_GLIDE_FRAMES;
}

// The sampler's read at downsample `ds` — the mix loop's own fetch, for the read being faded OUT when
// the downsample changes on a sounding note. `r` repeats `l` for a mono sample.
inline constexpr int DOWNSAMPLE_FADE_FRAMES = 128;
static inline void fetchDownsampled(const Voice& v, int idx, float frac, int ds, float& l, float& r) {
    if (ds > 0) {
        const int f  = 1 << ds;
        const int qi = (idx / f) * f;
        l = v.sampleData[qi];
        r = v.sampleDataRight ? v.sampleDataRight[qi] : l;
        return;
    }
    l = v.sampleData[idx] + (v.sampleData[idx + 1] - v.sampleData[idx]) * frac;
    r = v.sampleDataRight ? v.sampleDataRight[idx] + (v.sampleDataRight[idx + 1] - v.sampleDataRight[idx]) * frac : l;
}

template <typename V> static inline void voiceSetFineTune(V& v, int byteValue) {
    v.params.setBase(PARAM_PITCH, fineTuneSemitonesOf(byteValue));
}

/** A 0-1 CC value back to the 00-FF byte the author typed. */
static inline int filterByteOf(float value) {
    return std::max(0, std::min(255, (int)(value * 255.0f + 0.5f)));
}

// ─── ONE PER-VOICE CONTROLLER ONTO ONE VOICE ─────────────────────────────────────────────────────
//
// The whole meaning of a per-voice CC (songcore/event.h), for a phrase cell, an AUS/AUF ramp or a
// mapped knob — every record the param queue carries for one. `value` is the 0-1 CC value. A new live
// parameter is a CC id and a case here; a table row reaches the same `voiceSet…` helpers by its FX
// code (processTableRow). PAN is not here: it reaches only the track's current note, through
// IAudioVoice::setPan (processAudioBlock).
template <typename V>
static inline void applyVoiceCc(V& v, int cc, float value, float sampleRate) {
    using namespace songcore;
    const int byte = filterByteOf(value);
    switch (cc) {
        case CC_REVERB_SEND: v.reverbSend = value; break;
        case CC_DELAY_SEND:  v.delaySend  = value; break;
        // ⚠️ Inert on a voice whose FILTER TYPE is OFF: these move the filter the instrument declares.
        case CC_FILTER_CUT:  voiceSetFilterCut(v, byte, sampleRate); break;
        case CC_FILTER_RES:  voiceSetFilterRes(v, byte, sampleRate); break;
        // The id IS the filter type and the value the cutoff, so one record switches the filter on
        // and places it in the same frame — two records would be two blocks, and a click.
        case CC_FILTER_LP:
        case CC_FILTER_HP:
        case CC_FILTER_BP:   voiceSetFilterMode(v, cc_filter_mode(cc), byte, sampleRate); break;
        case CC_DRIVE:       voiceSetDrive(v, byte); break;
        // ⚠️ The byte carries TWO nibbles, bits crushed high and downsample low — never interpolated.
        case CC_CRUSH:       voiceSetCrush(v, byte); break;
        // Retunes a sounding note: 0x80 is in tune, the ends a semitone either way.
        case CC_FINE_TUNE:   voiceSetFineTune(v, byte); break;
        // ⚠️ ACCUMULATES — two records slide the window twice.
        case CC_LOOP_SLIDE:  voiceSlideLoop(v, byte); break;
        default: break;
    }
}

// BCK: sampler only — a SoundFont voice has no playback direction.
static inline void voiceReverse(Voice& vo, bool rev, bool restart) {
    vo.reverse = rev;
    if (restart) {
        // With-note BCK: (re)start at the boundary the new direction reads FROM, so a "play backwards"
        // note begins at the sample's end instead of instantly hitting actualStart and fading out.
        // Mid-note BCK (restart=false) keeps the live position so direction flips are continuous.
        vo.position = rev ? (double)(vo.actualEnd > vo.actualStart ? vo.actualEnd - 1 : vo.actualStart)
                          : (double)vo.actualStart;
    }
}
static inline void voiceReverse(SoundfontVoice&, bool, bool) {}

// What each way of ending a note does to one voice. A new voice type adds its overloads here — the
// kill arms in processAudioBlock reach every pool through forEachVoiceOnTrack, so a missing one is a
// compile error rather than a note that does not stop.
static inline void voiceKeyRelease(Voice& v, int frame) { v.keyRelease(frame); }
static inline void voiceKeyRelease(SoundfontVoice& v, int frame) {
    v.noteOffAt(frame);   // TSF owns its release; a one-shot with no envelope is a sampler-only shape
}
static inline void voiceCut(Voice& v, int frame) { v.startFadeOut(KILL_FADE_SAMPLES, frame); }
static inline void voiceCut(SoundfontVoice& v, int frame) {
    v.startStopFade(KILL_FADE_SAMPLES, frame);   // ends in hardStop: no TSF or ADSR release outlives it
}
static inline void voiceKill(Voice& v, int frame) {
    v.startFadeOut(KILL_FADE_SAMPLES, frame);   // a deliberate cut, not a steal
}
static inline void voiceKill(SoundfontVoice& v, int frame) {
    v.noteOffAt(frame);   // soft, so TSF's own release envelope can play out
}

// The row a TIC00 retrigger continues from.
//
// ⚠️ The NEXT row — unless the one the voice is standing on has not been applied yet, in which case
// it is that row again. `lastProcessedRow` is the record of consumption, and it can disagree with
// `tableRow` three ways: a voice triggered but not yet ticked, a HOP target, a THO write. Stepping
// past a row in any of them drops the row entirely — and a dropped HOP row lets the table walk on
// past its loop point. processTableTick consumes at most one row per voice per audio BLOCK while
// notes arrive per FRAME, so two triggers inside one block reach this with the row still pending.
static inline int tic00RowAfter(int tableRow, int lastProcessedRow) {
    return (lastProcessedRow != tableRow) ? tableRow : (tableRow + 1) % 16;
}
static inline int tic00RowAfter(const TableLane& lane) {
    return tic00RowAfter(lane.row, lane.lastProcessed);
}
