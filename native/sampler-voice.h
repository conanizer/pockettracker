#pragma once
#include <cmath>
#include <cstdint>
#include <algorithm>
#include "rng.h"
#include "mods/mod-system.h"
#include "effects/instrument-chain.h"
#include "table-lanes.h"

// START and END are two free 0-255 cells, so an inverted pair is typeable. It means "from START to
// the end of the sample". ⚠️ Derived in ONE place: the window is computed at note-on and again every
// piece, and two repairs that disagree give a note that starts by one and stops by the other.
// The result is never empty, which the loop bounds and the mix loop assume.
inline void derive_sample_window(float startNorm, float endNorm, int length,
                                 int& outStart, int& outEnd) {
    if (length < 2) { outStart = 0; outEnd = std::max(0, length - 1); return; }
    // double: the endpoints carry a modulation offset, and float loses whole frames past 2^24.
    int s = (int)((double)startNorm * (double)length / 255.0);
    int e = (int)((double)endNorm   * (double)length / 255.0);
    s = std::max(0, std::min(s, length - 2));
    e = std::min(e, length - 1);
    if (e <= s) e = length - 1;
    outStart = s;
    outEnd   = e;
}

// Where one note is in its sample and how it moves on. ⚠️ A copy, not a view: the mix loop fills
// one with `playheadOf`, runs a piece on it and writes `position` and `loopingBack` back.
struct Playhead {
    enum Motion { FORWARD, BACKWARD, PING_PONG };

    const float* left;
    const float* right;      // null = mono
    double position;
    float  rate;             // frames per output sample, already modulated
    int    start, end;
    int    loopStart, loopEnd;
    Motion motion;
    bool   wraps;            // FORWARD / BACKWARD: the far end jumps to loopStart; false = the note ends there
    bool   loopingBack;      // PING_PONG: on the way down
    bool   oneShot;          // no loop mode: the last frames before the end fade
    bool   reverse;          // the instrument's direction — not always the motion, see playheadOf

    // The read at `idx` + `frac`. At `ds` > 0 the ADDRESS is quantized to 2^ds frames — the
    // sampler's downsample, a different sound from decimating the result.
    static inline float read(const float* data, int idx, float frac, int ds) {
        float a, b;
        if (ds > 0) {
            const int f = 1 << ds;
            a = b = data[(idx / f) * f];
        } else {
            a = data[idx];
            b = data[idx + 1];
        }
        return a + (b - a) * frac;
    }

    // A one-shot's distance to the frame it stops on.
    inline float framesLeft() const {
        return (float)(reverse ? (position - (double)start) : ((double)end - position));
    }

    // One output sample on. False when the note ran out, `position` parked on its last frame.
    inline bool advance() {
        switch (motion) {
            case PING_PONG:
                if (loopingBack) {
                    position -= rate;
                    if (position <= loopStart) {
                        loopingBack = false;
                        position = (double)loopStart;
                    }
                } else {
                    position += rate;
                    if (position >= loopEnd) {
                        loopingBack = true;
                        position = (double)loopEnd;
                    }
                }
                return true;
            case BACKWARD:
                position -= rate;
                if (position <= start) {
                    if (!wraps) {
                        position = (double)start;
                        return false;
                    }
                    position = (double)loopStart;
                }
                return true;
            default:
                position += rate;
                if (position >= (wraps ? loopEnd : end)) {
                    if (!wraps) {
                        position = (double)(end - 1);
                        return false;
                    }
                    position = (double)loopStart;
                }
                return true;
        }
    }
};

// One output sample's anti-click gain: the fade-in every note starts with (advanced here) and a
// one-shot's fade into its last frames.
inline float antiClickGain(int& fadeInRemaining, const Playhead& ph) {
    float fade = 1.0f;
    if (fadeInRemaining > 0) {
        fade = 1.0f - (float)fadeInRemaining / (float)DECLICK_SAMPLES;
        fadeInRemaining--;
    }
    if (ph.oneShot) {
        const float remaining = ph.framesLeft();
        if (remaining >= 0.0f && remaining < (float)DECLICK_SAMPLES)
            fade *= remaining / (float)DECLICK_SAMPLES;
    }
    return fade;
}

struct Voice : public IAudioVoice {
    bool isActive = false;
    int fadeInRemaining = 0;           // anti-click: DECLICK_SAMPLES down to 0 from the note's start
    float* sampleData = nullptr;
    float* sampleDataRight = nullptr;  // null = mono
    int sampleLength = 0;
    // double: float spacing reaches 1.0 at 2^24 frames (~6 min), where interpolation collapses.
    double position = 0;
    int trackId = -1;
    int instrId = -1;                  // = sampleId; names the instrument for its spectrum
    uint32_t sampleGen = 0;            // AudioEngine::sampleGen[instrId] when triggered; the mix ends a stale one
    float playbackRate = 1.0f;
    float basePlaybackRate = 1.0f;     // without table transpose
    float volume = 1.0f;
    // The channel gains, and what they were when the piece began: the mix ramps between the two.
    float panLeft = 0.707f, panRight = 0.707f;
    float prevPanLeft = 0.707f, prevPanRight = 0.707f;

    // The window and the loop, in frames.
    int actualStart = 0, actualEnd = 0;
    int actualLoopStart = 0, actualLoopEnd = 0;
    int loopEndNorm = 255;             // the instrument's 0-255 value; actualLoopEnd is re-derived per piece
    // The exact-frame window this note was triggered with, or -1/-1. ⚠️ Carried, because the mix
    // loop re-derives actualStart/End from the 0-255 pair, which cannot express it.
    int windowStartFrame = -1, windowEndFrame = -1;
    // ⚠️ LPO's position is a running COUNT of sixteenths of the loop's own length; the bounds above
    // are frames. The frame offset is derived from the count every piece, so sixteen steps of `01`
    // land where one step of `10` does. Clamped: the window stops at the sample's ends.
    int loopSlideSixteenths = 0;
    int loopSlideFrames = 0;           // the offset last applied: the playhead moves by the difference
    // The frequency a `playbackRate` of 1.0 sounds at. ⚠️ Read only by oscillator mode, and a
    // required argument of trigger() because forgetting it is silent in every other mode.
    float baseFrequency = 0.0f;
    bool reverse = false;
    int loopMode = 0;                  // 0=off, 1=forward, 2=ping-pong, 3=oscillator (note-queue.h)
    bool loopingBack = false;          // ping-pong: on the way down
    // An ADSR release began on a looping voice: the loop is abandoned and the tail plays out.
    bool loopReleasing = false;

    InstrumentChain chain;

    int tableId = -1;                  // -1 = no table
    // One cursor and one rate PER FX COLUMN — see table-lanes.h.
    TableLane lanes[TABLE_LANES];
    float tableTranspose = 0.0f;       // semitones
    float tableVolume = 1.0f;
    // Picked up passing through INS rows; every table pitch / volume write adds / multiplies it.
    float carrySemitones = 0.0f;
    float carryVolume    = 1.0f;
    // A table PAN glides to its value over PAN_GLIDE_FRAMES…
    float panNow       = 0.5f;
    float panGoal      = 0.5f;
    int   panGlideLeft = 0;
    // …and a table VOL blends from the gain the last sample used, over VOL_GLIDE_FRAMES.
    float volRouteLast = 0.0f;
    float volGlideFrom = 0.0f;
    int   volGlideLeft = 0;
    // A note on its way out keeps the track fader it had when a VTR on the next note changed it.
    bool  faderHeld      = false;
    float faderHeldStart = 1.0f, faderHeldEnd = 1.0f;
    // The sampler's downsample is a quantized read, not a chain module: a change on a sounding note
    // blends the old read into the new one over DOWNSAMPLE_FADE_FRAMES.
    int   dsLast = 0;
    int   dsPrev = 0;
    int   dsFadeLeft = 0;

    // The note, for the note monitor: octave 0-9 (-1 = none) and pitch 0-11, C = 0.
    int noteOctave = -1, notePitch = 0;
    // The note TICFC / TICFE place a cursor from — the voice's, so every lane reads the same one.
    int triggerOctave = 4, triggerPitch = 0;

    // PSL / PBN: semitones off the base pitch, sliding toward the target at a rate per sample.
    float pitchOffset = 0.0f;
    float pitchSlideTarget = 0.0f;
    float pitchSlideRate = 0.0f;
    bool pitchSliding = false;

    // PVB / PVX: phase 0 to 2π, speed in Hz, depth in semitones.
    float vibratoPhase = 0.0f;
    float vibratoSpeed = 0.0f;
    float vibratoDepth = 0.0f;
    bool vibratoActive = false;

    // Mod sources fixed at note-on.
    float noteVelocity = 0.0f;  // 0.0–1.0 (note volume proxies velocity)
    float noteKeytrack = 0.0f;  // (midiNote − 60) / 12.0, bipolar
    float noteRandom   = 0.0f;  // random 0.0–1.0

    // Fade-out instead of a hard cut: DECLICK_SAMPLES for a steal, KILL_FADE_SAMPLES for a kill.
    int fadeOutRemaining = 0;   // fadeOutTotal down to 0
    int fadeOutTotal = 1;
    bool isFadingOut = false;

    // The frame of the block a note dispatched mid-block starts at. The mix loop skips to it, then
    // zeroes it.
    int startDelayFrames = 0;
    // The fade's twin: a KIL, note-off or steal dispatched at frame f starts fading at f. The
    // engine zeroes it after every block.
    int fadeStartFrame = 0;

    // Constant-power pan gains: 0 = left, 0.5 = centre, 1 = right.
    void setPanGains(float pan) {
        const float angle = pan * (float)M_PI * 0.5f;
        panLeft  = cosf(angle);
        panRight = sinf(angle);
    }

    // The frame a note starts on. Reverse starts at end - 1: the read needs idx + 1.
    double firstFrame() const {
        return (double)(reverse && actualEnd > actualStart ? actualEnd - 1 : actualStart);
    }

    void trigger(float* sample, float* sampleRight, int length, int track, float rate, float baseFreq,
                 float instrVol, float phraseVol, float pan,
                 const InstrumentParams& instrParams, float sampleRate, int startPointOverride = -1,
                 int endPointOverride = -1,
                 int tblId = -1,
                 const int (&tblTicRates)[TABLE_LANES] = TABLE_TICS_DEFAULT,
                 int octave = 4, int pitch = 0,
                 const int (&startRows)[TABLE_LANES] = TABLE_ROWS_TOP) {
        sampleData = sample;
        sampleDataRight = sampleRight;
        sampleLength = length;
        trackId = track;
        playbackRate = rate;
        basePlaybackRate = rate;
        // Neutral: instrVol is PARAM_VOL's base and phraseVol a mod source — the fixed VOL route
        // multiplies TABLE_VOL × phraseVol × instrVol.
        volume = 1.0f;

        setPan(pan);
        volGlideLeft = 0;
        dsFadeLeft = 0;

        // An override is an Offset effect or a slice start.
        int effectiveStartPoint = (startPointOverride >= 0) ? startPointOverride : instrParams.startPoint;
        int effectiveEndPoint   = (endPointOverride   >= 0) ? endPointOverride   : instrParams.endPoint;
        derive_sample_window((float)effectiveStartPoint, (float)effectiveEndPoint, length,
                             actualStart, actualEnd);
        // The exact-frame window replaces the pair when armed — unless this NOTE carries an override.
        const bool frameWindow = (startPointOverride < 0 && endPointOverride < 0 &&
                                  length >= 2 &&
                                  instrParams.startFrame >= 0 &&
                                  instrParams.endFrame > instrParams.startFrame);
        if (frameWindow) {
            actualStart = std::max(0, std::min(instrParams.startFrame, length - 2));
            actualEnd   = std::max(actualStart + 1, std::min(instrParams.endFrame, length - 1));
        }
        actualLoopStart = (int)(((int64_t)instrParams.loopStart * length) / 255);
        actualLoopEnd   = (int)(((int64_t)instrParams.loopEnd   * length) / 255);

        windowStartFrame = frameWindow ? actualStart : -1;
        windowEndFrame   = frameWindow ? actualEnd   : -1;

        // loopStart ∈ [start, end-1], loopEnd ∈ [loopStart+1, end]: never an empty loop.
        actualLoopStart = std::max(actualStart, std::min(actualLoopStart, actualEnd - 1));
        actualLoopEnd   = std::max(actualLoopStart + 1, std::min(actualLoopEnd, actualEnd));
        loopEndNorm     = instrParams.loopEnd;
        loopReleasing   = false;
        // The loop slide resets on every note: one LPO cell colours that note only.
        loopSlideSixteenths = 0;
        loopSlideFrames     = 0;
        baseFrequency       = baseFreq;

        reverse = instrParams.reverse;
        // A slice plays once to its boundary: no looping.
        loopMode = (endPointOverride >= 0) ? 0 : instrParams.loopMode;
        loopingBack = false;

        chain.reset(sampleRate);
        chain.filter.setParams(instrParams.filterType, instrParams.filterCut,
                               instrParams.filterRes, instrParams.filterDrive, sampleRate);
        chain.filter.snapshotCoeffs();   // or the first piece would ramp from the reset defaults
        if (instrParams.eqActive) {
            chain.eq.active = true;
            for (int i = 0; i < 3; i++) {
                chain.eq.bands[i].setParams(instrParams.eqBands[i].type,
                                            instrParams.eqBands[i].freqHz,
                                            instrParams.eqBands[i].gainDb,
                                            instrParams.eqBands[i].q);
            }
        }

        reverbSend = instrParams.reverbSend;
        delaySend  = instrParams.delaySend;

        tableId = tblId;
        tableTranspose = 0.0f;
        tableVolume = 1.0f;
        carrySemitones = 0.0f;
        carryVolume    = 1.0f;
        // The lanes themselves are placed below, once the note they may be mapped from is known.

        // A new note clears PSL, PBN, PVB, PVX.
        pitchOffset = 0.0f;
        pitchSlideTarget = 0.0f;
        pitchSlideRate = 0.0f;
        pitchSliding = false;
        vibratoPhase = 0.0f;
        vibratoSpeed = 0.0f;
        vibratoDepth = 0.0f;
        vibratoActive = false;
        params.setBase(PARAM_VOL,          instrVol);
        params.setBase(PARAM_PAN,          pan);
        params.setBase(PARAM_PITCH,        0.0f);
        params.setBase(PARAM_FILTER_CUT,   (float)instrParams.filterCut);
        params.setBase(PARAM_FILTER_RES,   (float)instrParams.filterRes);
        params.setBase(PARAM_DRIVE,        (float)instrParams.drive);
        params.setBase(PARAM_CRUSH,        (float)instrParams.crush);
        params.setBase(PARAM_DOWNSAMPLE,   (float)instrParams.downsample);
        params.setBase(PARAM_SAMPLE_START, (float)effectiveStartPoint);
        params.setBase(PARAM_SAMPLE_END,   (float)effectiveEndPoint);
        params.setBase(PARAM_LOOP_START,   (float)instrParams.loopStart);
        params.resetMods();

        noteVelocity  = instrVol;
        int midiNote  = (octave + 1) * 12 + pitch;
        noteKeytrack  = (float)(midiNote - 60) / 12.0f;
        // xorshift, not rand(): this is the audio callback, and glibc's rand() takes a lock.
        static thread_local uint32_t noteRngState = 0x6E624EB7u;
        noteRandom    = xorshift32Unit(noteRngState);

        memset(modSourceValues, 0, sizeof(modSourceValues));
        modSourceValues[MOD_SRC_VELOCITY]  = noteVelocity;
        modSourceValues[MOD_SRC_KEYTRACK]  = noteKeytrack;
        modSourceValues[MOD_SRC_RANDOM]    = noteRandom;
        modSourceValues[MOD_SRC_TABLE_VOL]  = 1.0f;
        modSourceValues[MOD_SRC_PHRASE_VOL] = phraseVol;
        // ⚠️ MOD_SRC_NONE must stay 0: processRoutes' via=NONE path reads it.

        memset(modDestValues,     0, sizeof(modDestValues));
        memset(prevModDestValues, 0, sizeof(prevModDestValues));

        // Seeded, so the first piece ramps from the note's own level rather than from 0.
        modDestValues[PARAM_VOL]     = instrVol * phraseVol;
        prevModDestValues[PARAM_VOL] = instrVol * phraseVol;

        noteOctave = std::max(0, std::min(octave, 9));
        notePitch  = std::max(0, std::min(pitch, 11));
        triggerOctave = noteOctave;
        triggerPitch  = notePitch;

        reset_table_lanes(lanes, tblTicRates, startRows, triggerOctave, triggerPitch);

        position = firstFrame();

        fadeInRemaining = DECLICK_SAMPLES;
        isFadingOut = false;
        fadeOutRemaining = 0;
        startDelayFrames = 0;   // the dispatch loop sets the real offset after trigger()
        fadeStartFrame = 0;
        isActive = true;
    }

    void stop() {
        isActive = false;
        isFadingOut = false;
        fadeOutRemaining = 0;
        startDelayFrames = 0;
        fadeStartFrame = 0;
    }

    // Fade out from `atFrame` of the current block instead of stopping. ⚠️ isActive and trackId
    // stay: the slot is reserved for the fade, and the allocator recycles it for the same track.
    void startFadeOut(int fadeSamples = DECLICK_SAMPLES, int atFrame = 0) {
        if (isFadingOut) return;
        fadeStartFrame = atFrame;
        fadeOutTotal = (fadeSamples > 0) ? fadeSamples : 1;
        fadeOutRemaining = fadeOutTotal;
        isFadingOut = true;
    }

    // ── IAudioVoice implementation ──────────────────────────────────────────

    bool active()      const override { return isActive; }
    int  getTrackId()  const override { return trackId; }

    void hardStop() override { stop(); }

    // Every live ADSR/TRIG VOL mod to its release stage. True when the voice has a release to run.
    bool releaseVolMods() {
        bool hasRelease = false;
        for (int m = 0; m < 4; m++) {
            VoiceModSlot& vmod = voiceMods[m];
            if (vmod.dest == 1 && (vmod.type == 2 || vmod.type == 5)) {
                if (vmod.stage >= 1 && vmod.stage <= 3 && vmod.releaseSamples > 0) {
                    vmod.stage = 4;   // release
                    vmod.stageCounter = 0;
                    hasRelease = true;
                } else if (vmod.stage == 4) {
                    hasRelease = true;
                }
            }
        }
        // A looping voice abandons its loop and runs out into the tail.
        if (hasRelease && loopMode != 0) loopReleasing = true;
        return hasRelease;
    }

    void noteOff() override { noteOffAt(0); }

    // What an OFF note calls: ADSR/TRIG VOL mods with a release go to their release stage; with
    // none, the declicked kill fade from `atFrame`. A release begins on a block edge.
    void noteOffAt(int atFrame) {
        if (!releaseVolMods()) startFadeOut(KILL_FADE_SAMPLES, atFrame);
    }

    // A live key was let go of. ⚠️ An OFF in every arm but one: with no release envelope a looping
    // voice still gets the kill fade (nothing else would end it), but a one-shot plays out.
    void keyRelease(int atFrame = 0) {
        if (!releaseVolMods() && loopMode != 0) startFadeOut(KILL_FADE_SAMPLES, atFrame);
    }

    void setVolume(float v) override { volume = v; params.setBase(PARAM_VOL, v); }

    void setPan(float pan) override {
        params.setBase(PARAM_PAN, pan);
        panNow = pan;
        panGlideLeft = 0;
        setPanGains(pan);
        prevPanLeft  = panLeft;
        prevPanRight = panRight;
    }

    void retrigger(int startPoint) override {
        if (!isActive || !sampleData) return;
        if (startPoint >= 0 && startPoint <= 255 && sampleLength > 0) {
            // int64: the product overflows an int on a long sample
            position = (double)(((int64_t)startPoint * sampleLength) / 255);
            position = std::max((double)actualStart, std::min(position, (double)(actualEnd - 1)));
        } else {
            position = (double)actualStart;
        }
        fadeInRemaining = DECLICK_SAMPLES;
    }

    void setMidiNote(int midiNote) override {
        // Relative to the note it was triggered at, taken from noteOctave / notePitch.
        int originalMidi = (noteOctave + 1) * 12 + notePitch;
        float semitones = (float)(midiNote - originalMidi);
        playbackRate = basePlaybackRate * powf(2.0f, semitones / 12.0f);
    }

    // Unused: the engine mixes sampler voices inline.
    float render(float* /*buf*/, int /*numFrames*/) override { return 0.0f; }

    // ── Pitch effect interface (IAudioVoice) ────────────────────────────────
    void setPitchBendRaw(float ratePerFrame) override {
        if (fabsf(ratePerFrame) < 0.000001f) {
            pitchSliding   = false;
            pitchSlideRate = 0.0f;
        } else {
            pitchSlideRate   = ratePerFrame;
            pitchSlideTarget = (ratePerFrame > 0) ? 127.0f : -127.0f;
            pitchSliding     = true;
        }
    }
    void setVibratoRaw(float speed, float depth) override {
        if (depth < 0.01f) {
            vibratoActive = false;
            vibratoDepth  = 0.0f;
        } else {
            vibratoSpeed  = speed;
            vibratoDepth  = depth;
            vibratoActive = true;
        }
    }
    // ── end IAudioVoice ─────────────────────────────────────────────────────
};

// The voice's playhead for one piece, moving at `rate`. Once loopReleasing is set the loop is
// abandoned: every mode runs FORWARD to `end`, so the tail plays out under the release.
inline Playhead playheadOf(const Voice& v, float rate) {
    Playhead ph;
    ph.left        = v.sampleData;
    ph.right       = v.sampleDataRight;
    ph.position    = v.position;
    ph.rate        = rate;
    ph.start       = v.actualStart;
    ph.end         = v.actualEnd;
    ph.loopStart   = v.actualLoopStart;
    ph.loopEnd     = v.actualLoopEnd;
    ph.loopingBack = v.loopingBack;
    ph.oneShot     = v.loopMode == 0;
    ph.reverse     = v.reverse;
    if (v.loopMode == LOOP_MODE_PINGPONG && !v.loopReleasing) {
        ph.motion = Playhead::PING_PONG;
        ph.wraps  = true;
    } else if (v.reverse && !v.loopReleasing) {
        ph.motion = Playhead::BACKWARD;
        ph.wraps  = isForwardLoopMode(v.loopMode);
    } else {
        ph.motion = Playhead::FORWARD;
        ph.wraps  = isForwardLoopMode(v.loopMode) && !v.loopReleasing;
    }
    return ph;
}
