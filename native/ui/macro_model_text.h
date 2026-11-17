#pragma once

// What the INSTRUMENT screen says about each MACRO model: what it is, and what TIMBRE and COLOR do in
// it, beside the three cells. In MACRO_MODEL_NAMES' order. Each fits the room right of a value
// (`MACRO_NOTE_CHARS`), and is drawn in capitals, as all text here is.

#include <iterator>

#include "songcore/model.h"

namespace pt::ui {

struct MacroModelText {
    const char* about;
    const char* timbre;
    const char* color;
};

// A note starts 235 in; 15 characters of 17 pixels end where the screen's other rows do, clear of the
// side panel.
inline constexpr int MACRO_NOTE_CHARS = 15;

inline constexpr MacroModelText MACRO_MODEL_TEXT[] = {
    /* CSAW */ {"CS-80 saw",         "notch width",      "notch polarity"},
    /* MRPH */ {"variable wave",     "waveshape",        "distort/filter"  },
    /* SWSQ */ {"saw / square",      "pulse width",      "saw to square"},
    /* FOLD */ {"wavefolder",        "fold amount",      "sine to tri"     },
    /* BUZZ */ {"harmonic combs",    "smoothness",       "detune"},
    // Below the middle COLOR brings in a sub two octaves down, above it one octave down.
    /* SUBQ */ {"square + sub",      "pulse width",      "sub -2 / -1 oct"},
    /* SUBS */ {"saw + sub",         "saw shape",        "sub -2 / -1 oct"},
    /* SYNQ */ {"synced square",     "frequency ratio",  "balance"},
    /* SYNS */ {"synced saw",        "frequency ratio",  "balance"},
    /* SAW3 */ {"triple saw",        "osc 2 detune",     "osc 3 detune"},
    /* SQR3 */ {"triple square",     "osc 2 detune",     "osc 3 detune"},
    /* TRI3 */ {"triple triangle",   "osc 2 detune",     "osc 3 detune"},
    /* SIN3 */ {"triple sine",       "osc 2 detune",     "osc 3 detune"},
    /* RING */ {"ring-mod sines",    "osc 2 ratio",      "osc 3 ratio"},
    /* SWRM */ {"saw swarm",         "detune",           "high-pass"       },
    /* COMB */ {"comb-filter saw",   "delay time",       "feedback -/+"},
    /* TOY* */ {"circuit-bent"    ,  "sample rate",      "bit toggling"},
    /* ZLPF */ {"LP filter wave"  ,  "cutoff",           "waveshape"},
    /* ZPKF */ {"PK filter wave"  ,  "cutoff",           "waveshape"},
    /* ZBPF */ {"BP filter wave"  ,  "cutoff",           "waveshape"},
    /* ZHPF */ {"HP filter wave"  ,  "cutoff",           "waveshape"},
    /* VOSM */ {"two formants",      "formant 1",        "formant 2"},
    /* VOWL */ {"lo-fi vowels",      "vowel a e i o u",  "gender"},
    /* VFOF */ {"hi-fi vowels",      "vowel a e i o u",  "gender"},
    /* HARM */ {"14 harmonics",      "harmonic number",  "peakedness"},
    /* FM   */ {"2-operator FM",     "mod index",        "frequency ratio"},
    /* FBFM */ {"feedback FM",       "mod index",        "frequency ratio"},
    /* WTFM */ {"chaotic FM",        "mod index",        "frequency ratio"},
    /* PLUK */ {"plucked string",    "decay",            "pluck position"},
    /* BOWD */ {"bowed string",      "friction",         "bow position"},
    /* BLOW */ {"reed",              "air pressure",     "geometry"},
    /* FLUT */ {"flute",             "air pressure",     "geometry"},
    /* BELL */ {"bell",              "decay",            "harmonicity"},
    /* DRUM */ {"metallic drum",     "decay",            "harmonicity"},
    /* KICK */ {"808 bass drum",     "decay",            "brightness"},
    /* CYMB */ {"cymbal noise",      "cutoff",           "noisiness"},
    /* SNAR */ {"808 snare drum",    "tone",             "noisiness/decay"},
    /* WTBL */ {"21 wavetables",     "position",         "table"},
    /* WMAP */ {"16x16 wave map",    "x position",       "y position"},
    /* WLIN */ {"wavetable scan",    "position",         "interpolation"},
    /* WTX4 */ {"wavetable chord" ,  "position",         "chord type"},
    /* NOIS */ {"tuned noise",       "resonance",        "LP to HP"},
    /* TWNQ */ {"twin-peak noise",   "resonance",        "peak spacing"},
    /* CLKN */ {"clocked noise",     "cycle length",     "quantization"},
    /* CLOU */ {"grain cloud",       "grain density",    "freq dispersion"},
    /* PRTC */ {"droplets",          "grain density",    "freq dispersion"},
    /* QPSK */ {"modem noise",       "bit rate",         "data"},
};

namespace detail {
constexpr bool macro_note_fits(const char* s) {
    int n = 0;
    while (s[n] != '\0') ++n;
    return n <= MACRO_NOTE_CHARS;
}
constexpr bool macro_model_text_fits() {
    for (const MacroModelText& t : MACRO_MODEL_TEXT)
        if (!macro_note_fits(t.about) || !macro_note_fits(t.timbre) || !macro_note_fits(t.color)) return false;
    return true;
}
}  // namespace detail

static_assert(std::size(MACRO_MODEL_TEXT) == static_cast<size_t>(songcore::MACRO_MODEL_COUNT),
              "a line of text for every model");
static_assert(detail::macro_model_text_fits(), "a model's text runs past the room beside its cell");

inline const MacroModelText& macro_model_text(int model) {
    const bool known = model >= 0 && model < static_cast<int>(std::size(MACRO_MODEL_TEXT));
    return MACRO_MODEL_TEXT[known ? model : 0];
}

}  // namespace pt::ui
