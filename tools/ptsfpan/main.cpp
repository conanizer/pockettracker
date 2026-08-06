// ptsfpan — does a MOD reach a SOUNDFONT voice's stereo position?
//
// ─── WHY THIS IS ITS OWN TOOL ────────────────────────────────────────────────────────────────────
//
// A SoundFont voice is modulated in a different place from a sampler voice, and the ladder never
// looked at the difference. The sampler pans in the mix loop, through `voice.panLeft/panRight`, in a
// block that walks `voices[]` only. A SoundFont voice has no such gains — TSF pans on its own channel
// — so its per-block pass has to say `tsf_channel_set_pan` out loud, and for a long time it did not:
// VOL, FILTER and PITCH were carried over to the SF path and PAN was not. Pan was written once at
// note-on and never again, so an LFO or envelope routed to PAN did nothing at all on a SoundFont
// instrument, silently, while doing the obvious thing on a sampler beside it.
//
// It is not in `ptdispatch` because it needs a REAL .sf2, and ptdispatch is deliberately the one tool
// with no /tools/testdata argument. It is not in `ptvoice` because that tool goldens the note DERIVATION
// (what the seam is handed), and this claim is about what happens to a voice per block AFTER the note
// exists — a different subject, one block below.
//
// ─── THE METRIC, AND WHY IT IS A SWING ───────────────────────────────────────────────────────────
//
// ⚠️ The measurement is the peak-to-peak SWING of the stereo balance over the note, not the balance
// itself. "Is it off-centre?" passes forever on an instrument that was merely panned at note-on, and
// on a stereo SF2 whose samples are not centred to begin with — neither of which is modulation. Only
// a value that MOVES over the life of one note can distinguish a live mod from a static offset.
//
// The control is the identical instrument with the PAN mod removed. It must come out flat: that is
// what proves the swing belongs to the mod rather than to the SoundFont, and what makes this check
// fail honestly if someone ever ships a wobbling fixture.
//
// ⚠️ The LFO shape is pinned below 8 on purpose — an `oscShape >= 8` RND/DRNK LFO is seeded from the
// wall clock and has no reproducible trajectory (see tools/ptnondet).

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#include "audio-engine.h"
#include "songcore/host.h"

namespace {

int checks = 0, failures = 0;

void ok(bool cond, const std::string& what) {
    ++checks;
    if (cond) {
        std::printf("  [ ok ] %s\n", what.c_str());
    } else {
        ++failures;
        std::printf("  [FAIL] %s\n", what.c_str());
    }
}

/**
 * Render one note of a SOUNDFONT instrument and report how far its stereo balance travels.
 *
 * Returns the peak-to-peak swing of (R−L)/(R+L), or −1.0 when the run produced no audio at all —
 * which is reported as a failure rather than as a swing of zero, because a silent render would
 * otherwise pass the control half of every comparison below.
 */
double balance_swing(bool withPanMod, const std::string& sf2, double& outMin, double& outMax) {
    auto engine = std::make_unique<AudioEngine>();   // ⚠️ HEAP: its scratch buffers blow a 1 MB stack
    engine->setDeviceSampleRate(44100);

    songcore::SongcoreHost host(engine.get(), 44100);
    songcore::Project&     p = host.edit_project();
    p = songcore::make_default_project();
    p.tempo = 60;                                    // a long step, so the LFO has room inside one note

    songcore::Instrument& ins = p.instruments[0];
    ins.instrumentType = songcore::InstrumentType::SOUNDFONT;
    ins.volume         = 0xFF;
    ins.pan            = 0x80;                       // dead centre: any offset must come from the mod

    if (withPanMod) {
        ins.modSlots[0].type     = songcore::ModType::LFO;
        ins.modSlots[0].dest     = songcore::ModDest::PAN;
        ins.modSlots[0].amount   = 0xFF;
        ins.modSlots[0].oscShape = 0x00;             // ⚠️ NOT >= 8 — see the header
        ins.modSlots[0].lfoFreq  = 0x40;
    }

    if (!host.load_soundfont(0, sf2)) {
        std::printf("  [FAIL] could not load the SoundFont: %s\n", sf2.c_str());
        ++checks; ++failures;
        return -1.0;
    }
    host.push_params();

    songcore::PhraseStep& n = p.phrases[0].steps[0];
    n.note = songcore::Note::C4();
    n.instrument = 0;
    n.volume = 0x7F;
    p.tracks[0].chainRefs.assign(256, -1);
    p.tracks[0].chainRefs[0]  = 0;
    p.chains[0].phraseRefs[0] = 0;

    host.play_song(0);

    constexpr int      BLK = 512;
    std::vector<float> buf(BLK * 2);

    outMin = 1e9;
    outMax = -1e9;
    bool anySound = false;

    for (int b = 0; b < 130; ++b) {                  // ~1.5 s
        host.poll();
        engine->processLiveBlock(buf.data(), BLK, 2, 44100.0f);

        double pl = 0.0, pr = 0.0;
        for (int i = 0; i < BLK; ++i) {
            pl = std::max(pl, std::fabs(static_cast<double>(buf[i * 2])));
            pr = std::max(pr, std::fabs(static_cast<double>(buf[i * 2 + 1])));
        }
        // A silent block has no balance — 0/0 is not "centred", it is "no reading".
        if (pl + pr < 1e-4) continue;
        anySound = true;
        const double bal = (pr - pl) / (pr + pl);
        outMin = std::min(outMin, bal);
        outMax = std::max(outMax, bal);
    }

    if (!anySound) {
        std::printf("  [FAIL] the render was SILENT - this check could not have failed\n");
        ++checks; ++failures;
        return -1.0;
    }
    return outMax - outMin;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::printf("usage: ptsfpan <path/to/test.sf2>\n");
        return 2;
    }
    // ⚠️ A MISSING FIXTURE IS A FAILURE, NEVER A SKIP. A compare-if-present check certifies anything
    // once the file is gone (tools/testdata/README.md's standing rule); load_soundfont reports it below.
    const std::string sf2 = argv[1];

    double onMin = 0, onMax = 0, offMin = 0, offMax = 0;
    const double swingOn  = balance_swing(true,  sf2, onMin,  onMax);
    const double swingOff = balance_swing(false, sf2, offMin, offMax);

    // ⭐ The numbers beside the verdicts, both derived from the rendered audio.
    std::printf("\n  LFO->PAN on : balance %+.3f .. %+.3f   swing = %.3f\n", onMin, onMax, swingOn);
    std::printf("  LFO->PAN off: balance %+.3f .. %+.3f   swing = %.3f   (control)\n\n",
                offMin, offMax, swingOff);

    ok(swingOn > 0.20,
       "a SOUNDFONT voice's PAN follows the mod (swing > 0.20)");
    ok(swingOff >= 0.0 && swingOff < 0.05,
       "(control) …and stays put without it, so the swing is the MOD's and not the SF2's");

    std::printf("\n%d checks, %d failure(s)\n", checks, failures);
    std::printf("%s\n", failures == 0 ? "ALL GREEN" : "RED");
    return failures == 0 ? 0 : 1;
}
