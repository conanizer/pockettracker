#pragma once

// A send's cells, as the project, a built-in preset and a `.ptr` / `.ptd` file all hold them.
// No includes: the effect modules' preset lists read this too.
// ⚠️ The defaults are what older projects play with, and match no built-in preset.

namespace songcore {

struct ReverbCells {
    int algo  = 0;      // which reverb reads the rest (`kReverbAlgo*`); stored — append, never insert
    int size  = 0x60;   // the room's dimensions; 0x60 is exactly the original room
    int decay = 0x60;   // DCAY, the tail's length (the `.ptp` key is `reverbFeedback`)
    int damp  = 0x80;   // how bright it stays
    int pre   = 0x00;   // the gap before it starts; 00 = none
    int width = 0x80;   // 80 = the stereo image untouched
    int mod   = 0x10;   // the wander; 40 = what the algorithm has always had

    bool operator==(const ReverbCells& o) const {
        return algo == o.algo && size == o.size && decay == o.decay && damp == o.damp &&
               pre == o.pre && width == o.width && mod == o.mod;
    }
    bool operator!=(const ReverbCells& o) const { return !(*this == o); }
};

struct DelayCells {
    bool pong       = false;   // repeats come back on the other side
    bool sync       = false;   // `time` is a note division (0..B), else 00-FF
    int  time       = 0x40;
    int  feedback   = 0x60;    // FF = the repeats never fade
    int  tone       = 0xFF;    // FF = no filter on the repeats at all
    int  wobble     = 0x00;    // 00 = the read head does not drift
    int  reverbSend = 0x00;    // how much of the delay goes on into the reverb

    bool operator==(const DelayCells& o) const {
        return pong == o.pong && sync == o.sync && time == o.time && feedback == o.feedback &&
               tone == o.tone && wobble == o.wobble && reverbSend == o.reverbSend;
    }
    bool operator!=(const DelayCells& o) const { return !(*this == o); }
};

}  // namespace songcore
