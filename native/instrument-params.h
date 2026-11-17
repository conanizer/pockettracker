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

    int filterType  = 0;    // 0 off, 1 lp, 2 hp, 3 bp, 4 notch, 5 peak
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

// One modulation slot; copied to VoiceModSlot at the trigger.
struct InstrumentModSlot {
    int type = 0;            // 0 NONE, 1 AHD, 2 ADSR, 3 LFO, 4 DRUM, 5 TRIG, 6 SCALAR
    int dest = 0;            // 0 NONE, 1 VOL, 2 PAN, 3 PITCH, 4 FINE_PITCH, 5 CUT, 6 RES, 7 STA, 8 MOD_AMT, 9 MOD_RATE, 10 MOD_BOTH, 11 TIMBRE, 12 COLOR
    float amount = 0.5f;     // 0-1
    int attackSamples = 0;
    int holdSamples = 0;     // AHD only
    int decaySamples = 0;
    float sustainLevel = 0.5f;  // ADSR, 0-1
    float lfoHz = 4.0f;
    int oscShape = 0;        // LFO: 0 TRI, 1 SIN, 2 RMP+, 3 RMP-, 4 EXP+, 5 EXP-, 6 SQU+, 7 SQU-, 8 RND, 9 DRNK
    int lfoTrigMode = 1;     // LFO: 0 FREE, 1 RETG, 2 HOLD, 3 ONCE
    int releaseSamples = 0;  // ADSR/TRIG; 0 = instant
};
