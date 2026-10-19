#pragma once
// Per-voice writes shared by the table walk (engine-tables.cpp) and the block's queue drain
// (engine-mix.cpp). Private to the engine: one overload per voice type, resolved at compile time.
#include "audio-engine.h"

// A table row's OFFSET. A SoundFont voice has no sample position.
static inline void tableOffset(Voice& v, uint8_t fxValue) {
    if (v.sampleLength > 0) v.position = fxValue / 255.0 * (v.sampleLength - 1);
}
static inline void tableOffset(SoundfontVoice&, uint8_t) {}

// A parameter's base plus its modulation, as the 0..`max` integer a module takes.
static inline int paramWithMod(const IAudioVoice& v, ParamId p, int max) {
    return std::max(0, std::min(max, (int)(v.params.base[p] + v.modDestValues[p])));
}

// The filter from the bus, modulation on top — so a CUT under an LFO moves the centre it swings around.
template <typename V> static inline void filterFromBus(V& v, int type, float sampleRate) {
    v.chain.filter.setParams(type, paramWithMod(v, PARAM_FILTER_CUT, 255),
                             paramWithMod(v, PARAM_FILTER_RES, 255), v.chain.filter.drive, sampleRate);
}

// CUT / RES. ⚠️ Inert while the filter type is OFF — they never switch one on. Per note.
template <typename V>
static inline void voiceSetFilter(V& v, int cut, int res, float sampleRate) {
    v.params.setBase(PARAM_FILTER_CUT, (float)cut);
    v.params.setBase(PARAM_FILTER_RES, (float)res);
    if (v.chain.filter.enabled()) filterFromBus(v, v.chain.filter.type, sampleRate);
}
// One filter write takes both, so each carries the other's current value through.
template <typename V> static inline void voiceSetFilterCut(V& v, int cut, float sr) {
    voiceSetFilter(v, cut, (int)v.params.base[PARAM_FILTER_RES], sr);
}
template <typename V> static inline void voiceSetFilterRes(V& v, int res, float sr) {
    voiceSetFilter(v, (int)v.params.base[PARAM_FILTER_CUT], res, sr);
}

// LPF / HPF / BPF — the one write that switches a filter on, so it has no enabled() guard.
// ⚠️ Resonance stays what the voice holds: a fresh 0 would undo a RES written on the step before.
template <typename V> static inline void voiceSetFilterMode(V& v, int type, int cut, float sr) {
    v.params.setBase(PARAM_FILTER_CUT, (float)cut);
    filterFromBus(v, type, sr);
}

// LPO. ⚠️ Adds where every other setter assigns — on a table row it walks a step per tic.
static inline void voiceSlideLoop(Voice& v, int byteValue) {
    v.loopSlideSixteenths += loopSlideSixteenthsOf(byteValue);
}
static inline void voiceSlideLoop(SoundfontVoice&, int) {}

// DRV / CRU: both are re-derived from `params.base` per piece.
static inline void voiceSetDrive(IAudioVoice& v, int drive) {
    v.params.setBase(PARAM_DRIVE, (float)drive);
}
// Bits high, downsample low, kept apart on the bus: the sampler's downsample is its read, not the module's.
static inline void voiceSetCrush(IAudioVoice& v, int packed) {
    v.params.setBase(PARAM_CRUSH,      (float)crushBitsOf(packed));
    v.params.setBase(PARAM_DOWNSAMPLE, (float)crushDownsampleOf(packed));
}

// REV / DEL
template <typename V> static inline void voiceSetSends(V& v, float rev, float dly) {
    v.reverbSend.set(rev, v.chain.started);
    v.delaySend.set(dly, v.chain.started);
}

// A sounding voice re-reads its instrument: a trigger copied it, so an edit is otherwise silent
// until the next note. ⚠️ It ends a live override an FX cell made on the same parameters.
template <typename V>
static inline void voiceReloadInstrument(V& v, const InstrumentParams& ip, float sampleRate) {
    // Mode before resonance: the mode write carries whatever resonance the voice holds.
    voiceSetFilterMode(v, ip.filterType, ip.filterCut, sampleRate);
    voiceSetFilterRes(v, ip.filterRes, sampleRate);
    voiceSetDrive(v, ip.drive);
    voiceSetCrush(v, ((ip.crush & 0x0F) << 4) | (ip.downsample & 0x0F));
    voiceSetSends(v, ip.reverbSend, ip.delaySend);
}

// A table PAN glides too, over twice the rule's length — at GLIDE_FRAMES a low note's swing still
// clicks. (A SoundFont moves in PAN_GLIDE_STEP stairs.)
inline constexpr int PAN_GLIDE_FRAMES = 256;
inline constexpr int PAN_GLIDE_STEP   = 64;

template <typename V> static inline void voiceGlidePan(V& v, float pan) {
    if (!v.chain.started) { v.setPan(pan); return; }   // the note's own set-up
    v.params.setBase(PARAM_PAN, pan);
    v.panGoal      = pan;
    v.panGlideLeft = PAN_GLIDE_FRAMES;
}

// Advance a pan glide by `frames`; true when it moved, with the voice's new pan in `panNow`.
template <typename V> static inline bool voiceStepPanGlide(V& v, int frames) {
    if (v.panGlideLeft <= 0) return false;
    const float step = frames >= v.panGlideLeft ? 1.0f : static_cast<float>(frames) / static_cast<float>(v.panGlideLeft);
    v.panNow += (v.panGoal - v.panNow) * step;
    v.panGlideLeft = frames >= v.panGlideLeft ? 0 : v.panGlideLeft - frames;
    return true;
}

// A table VOL, by the glide rule (glide.h).
template <typename V> static inline void voiceGlideVol(V& v) {
    if (!v.chain.started) return;
    v.volGlideFrom = v.volRouteLast;
    v.volGlideLeft = GLIDE_FRAMES;
}

// FIN writes PARAM_PITCH's base (zeroed at trigger); the mod half carries transpose, slides and
// vibrato. ⚠️ Both voice types must read it — the SoundFont voice counts it as active pitch.
template <typename V> static inline void voiceSetFineTune(V& v, int byteValue) {
    v.params.setBase(PARAM_PITCH, fineTuneSemitonesOf(byteValue));
}

// A 0-1 CC value back to the 00-FF byte the author typed.
static inline int filterByteOf(float value) {
    return std::max(0, std::min(255, (int)(value * 255.0f + 0.5f)));
}

// One per-voice CC (songcore/event.h), `value` 0-1 — from a cell, a ramp, a mapped knob or another
// PocketTracker. A new live parameter is a CC id and a case here, and follows the glide rule
// (glide.h). PAN reaches only the current note (applyParamUpdate).
template <typename V>
static inline void applyVoiceCc(V& v, int cc, float value, float sampleRate) {
    using namespace songcore;
    const int byte = filterByteOf(value);
    switch (cc) {
        case CC_REVERB_SEND: v.reverbSend.set(value, v.chain.started); break;
        case CC_DELAY_SEND:  v.delaySend.set(value, v.chain.started);  break;
        case CC_FILTER_CUT:  voiceSetFilterCut(v, byte, sampleRate); break;
        case CC_FILTER_RES:  voiceSetFilterRes(v, byte, sampleRate); break;
        // The id is the filter type and the value the cutoff: one record, so both land on one frame.
        case CC_FILTER_LP:
        case CC_FILTER_HP:
        case CC_FILTER_BP:   voiceSetFilterMode(v, cc_filter_mode(cc), byte, sampleRate); break;
        case CC_DRIVE:       voiceSetDrive(v, byte); break;
        case CC_CRUSH:       voiceSetCrush(v, byte); break;   // two nibbles — never interpolated
        // Retunes a sounding note: 0x80 is in tune, the ends a semitone either way.
        case CC_FINE_TUNE:   voiceSetFineTune(v, byte); break;
        case CC_LOOP_SLIDE:  voiceSlideLoop(v, byte); break;  // ⚠️ accumulates: two records slide twice
        default: break;
    }
}

// BCK: sampler only. With a note it restarts at the end the new direction reads from; mid-note it
// keeps its position, so the flip is continuous.
static inline void voiceReverse(Voice& vo, bool rev, bool restart) {
    vo.reverse = rev;
    if (restart) vo.position = vo.firstFrame();
}
static inline void voiceReverse(SoundfontVoice&, bool, bool) {}

// Each way of ending a note, per voice type; a missing overload is a compile error.
static inline void voiceKeyRelease(Voice& v, int frame) { v.keyRelease(frame); }
static inline void voiceKeyRelease(SoundfontVoice& v, int frame) {
    v.noteOffAt(frame);   // TSF owns its release
}
// A declicked fade and gone: no envelope or SoundFont release outlives it. Also a table's KIL.
static inline void voiceCut(Voice& v, int frame)          { v.startFadeOut(KILL_FADE_SAMPLES, frame); }
static inline void voiceCut(SoundfontVoice& v, int frame) { v.startStopFade(KILL_FADE_SAMPLES, frame); }
static inline void voiceKill(Voice& v, int frame)          { v.startFadeOut(KILL_FADE_SAMPLES, frame); }
static inline void voiceKill(SoundfontVoice& v, int frame) {
    v.noteOffAt(frame);   // soft, so TSF's own release envelope can play out
}

// The row a TIC00 retrigger continues from: the next — ⚠️ unless the current row was never applied
// (just triggered, a HOP target, THO), then that one; skipping it drops a HOP's loop.
static inline int tic00RowAfter(int tableRow, int lastProcessedRow) {
    return (lastProcessedRow != tableRow) ? tableRow : (tableRow + 1) % 16;
}
static inline int tic00RowAfter(const TableLane& lane) {
    return tic00RowAfter(lane.row, lane.lastProcessed);
}
