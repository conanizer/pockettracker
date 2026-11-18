#pragma once

// One band in Hz/dB/Q, filled by setInstrumentEqSlot().
struct EqBandData {
    // ⚠️ 0 OFF 1 LOSHELF 2 LOWCUT 3 BELL 4 HISHELF 5 HICUT — the file's numbers, defined in
    // eq-module.h; append there, never insert.
    int   type   = 0;
    float freqHz = 1000.0f; // 20–20000 Hz
    float gainDb = 0.0f;    // −12..+12 dB
    float q      = 1.0f;    // 0.1–10.0
};

struct InstrumentParams {
    int startPoint = 0;     // 0-255 of the sample
    int endPoint   = 255;
    bool reverse   = false;
    // 3 (OSCILLATOR) retunes a forward loop so one trip is one cycle of the note: the loop's length
    // becomes timbre, not pitch.
    int loopMode   = 0;     // 0 off, 1 forward, 2 ping-pong, 3 oscillator
    int loopStart  = 0;     // 0-255
    int loopEnd    = 255;   // 255 = sample end

    int drive      = 0;     // 0-255
    int crush      = 0;     // 0-15: 0 off, 15 = 1-bit
    int downsample = 0;     // 0-15: 0 off, 1 = ÷2, 2 = ÷4 …

    int filterType  = 0;    // songcore::FilterType
    int filterCut   = 128;  // 0-255
    int filterRes   = 0;    // 0-255
    int filterDrive = 128;  // 0-255; 128 = DaisySP default

    EqBandData eqBands[3];  // set by setInstrumentEqSlot
    bool eqActive = false;  // any band not bypassed

    float reverbSend = 0.0f;   // 0-1
    float delaySend  = 0.0f;

    // In frames, replacing startPoint/endPoint when set (-1 = unset) — the sample editor's exact audition.
    // Cleared by every setInstrumentParams push; read at the trigger, so it changes the next note only.
    int startFrame = -1;
    int endFrame   = -1;
};

