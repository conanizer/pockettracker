#include "ui/value_hint.h"

#include <algorithm>
#include <cstdio>

#include "effects/cell-curves.h"
#include "effects/modules/reverb-presets.h"
#include "songcore/chords.h"
#include "songcore/program.h"
#include "songcore/scales.h"
#include "songcore/timing.h"
#include "ui/app_state.h"
#include "ui/canvas.h"

namespace pt::ui {

std::string format_hz(float hz) {
    char buf[32];
    if (hz < 20.0f) {
        std::snprintf(buf, sizeof(buf), hz < 10.0f ? "%.2fHz" : "%.1fHz", static_cast<double>(hz));
        return buf;
    }
    const int rounded = static_cast<int>(hz);
    if (rounded < 1000) {
        std::snprintf(buf, sizeof(buf), "%dHz", rounded);
    } else if (rounded < 10000) {
        // ⚠️ Divided in float, then widened: double can round across a display boundary, and this
        // string is what the EQ's FREQ stepping stops on.
        std::snprintf(buf, sizeof(buf), "%.1fkHz", static_cast<double>(hz / 1000.0f));
    } else {
        std::snprintf(buf, sizeof(buf), "%dkHz", static_cast<int>(hz / 1000.0f + 0.5f));
    }
    return buf;
}

std::string format_ms(double ms) {
    char buf[32];
    if (ms <= 0.0)         std::snprintf(buf, sizeof(buf), "0ms");
    else if (ms < 9.95)    std::snprintf(buf, sizeof(buf), "%.1fms", ms);
    else if (ms < 999.5)   std::snprintf(buf, sizeof(buf), "%.0fms", ms);
    else if (ms < 9995.0)  std::snprintf(buf, sizeof(buf), "%.2fs", ms / 1000.0);
    else                   std::snprintf(buf, sizeof(buf), "%.1fs", ms / 1000.0);
    return buf;
}

std::string instrument_label(const Canvas& c, const songcore::Instrument& ins) {
    using songcore::InstrumentType;
    if ((ins.instrumentType == InstrumentType::SAMPLER && !ins.sampleFilePath) ||
        (ins.instrumentType == InstrumentType::SOUNDFONT && !ins.soundfontPath))
        return c.cased("EMPTY");
    if (songcore::instrument_has_default_name(ins)) return c.cased(songcore::instrument_type_name(ins.instrumentType));
    return ins.name;
}

ValueUnit fx_value_unit(int effect_code) {
    using namespace songcore;
    switch (effect_code) {
        case FX_CUT: case FX_LPF: case FX_HPF: case FX_BPF:    return ValueUnit::CUTOFF_HZ;
        case FX_TIM:                                           return ValueUnit::DELAY_TIME;
        case FX_LAT: case FX_KILL: case FX_PSL: case FX_LEG:   return ValueUnit::TICS;
        case FX_TIC:                                           return ValueUnit::TABLE_TICS;
        case FX_CHD:                                           return ValueUnit::CHORD;
        default:                                               return ValueUnit::NONE;
    }
}

namespace {

// "16 TICKS AT 120 BPM": what a tic-counted value is measured against.
std::string tics_at_tempo(const Canvas& c, int tics, int tempo) {
    return c.cased(std::to_string(tics) + (tics == 1 ? " TICK AT " : " TICKS AT ") +
                   std::to_string(tempo) + " BPM");
}

ValueHint tics_hint(const Canvas& c, int tics, int tempo, int tics_per_step = songcore::TICS_PER_STEP) {
    return {format_ms(songcore::tics_ms(tics, tempo, tics_per_step)), tics_at_tempo(c, tics, tempo)};
}

// The chord's name and its notes. In a PHRASE, made of this row's note in the track's scale as far as
// this phrase shows it; in a TABLE, on C-4 moved by the row's transpose, as the table plays it —
// unsnapped, and named as the note it lands on.
ValueHint chord_hint(const Canvas& c, int value, const AppState& s) {
    const songcore::Project& p = *s.project;
    int root = 60;
    const songcore::Scale* scale = nullptr;
    int key = 0;
    std::string where;   // the root's name, or why there is none
    if (s.currentScreen == ScreenType::PHRASE) {
        const songcore::Phrase&     ph   = p.phrases[static_cast<size_t>(s.currentPhrase)];
        const songcore::PhraseStep& step = ph.steps[static_cast<size_t>(s.phraseCursor.row)];
        if (!songcore::note_is_pitch(step.note)) {
            where = c.cased("NEEDS A NOTE ON THIS ROW");
        } else {
            const songcore::NoteScale ns = songcore::phrase_note_scale(p, ph, s.phraseCursor.row, step.instrument);
            root  = ns.snap(songcore::note_to_midi(step.note));
            scale = ns.scale;
            key   = ns.key;
        }
    } else if (s.currentScreen == ScreenType::TABLE) {
        const songcore::TableRow& r =
            p.tables[static_cast<size_t>(s.currentTable)].rows[static_cast<size_t>(s.tableCursor.row)];
        // The row's transpose moves the note the chord is built on: 00-7F up, 80-FF down.
        const int tsp = r.transpose < 0x80 ? r.transpose : r.transpose - 256;
        root = std::max(0, std::min(127, 60 + tsp));
    } else {
        return {};
    }

    // The chord's words in the set case; the root note, put in front below, as it is.
    std::string head  = c.cased(std::string(songcore::chord_name(value)) + "  " +
                                songcore::chord_layout_name(value));
    std::string notes = where;
    if (where.empty()) {
        if (s.currentScreen == ScreenType::PHRASE) {
            const char* name = songcore::NOTE_NAMES[songcore::scale_mod12(root)];
            head = std::string(name, name[1] == '-' ? 1 : 2) + " " + head;
        } else {
            notes = c.cased("ON ") + songcore::note_name(songcore::note_from_midi(root)) + ":";
        }
        int semis[songcore::CHORD_MAX_NOTES];
        const int n = songcore::chord_notes(value, root, scale, key, semis);
        for (int i = 0; i < n; ++i) {
            if (!notes.empty()) notes += ' ';
            notes += songcore::note_name(songcore::note_from_midi(root + semis[i]));
        }
    }
    return {head, notes};
}

ValueHint instrument_hint(const Canvas& c, int value, const AppState& s) {
    const auto& list = s.project->instruments;
    if (value < 0 || value >= static_cast<int>(list.size())) return {};
    const songcore::Instrument& ins  = list[static_cast<size_t>(value)];
    const std::string           type = c.cased(songcore::instrument_type_name(ins.instrumentType));
    const std::string           name = instrument_label(c, ins);
    if (name != type) return {name, type};
    return {type + " " + songcore::hex2(value), ""};
}

}  // namespace

ValueHint value_hint(const Canvas& c, ValueUnit unit, int value, const AppState& s, double exactMs) {
    if (!s.project) return {};
    const songcore::Project& p     = *s.project;
    const int                tempo = p.tempo;
    const bool               dflt  = p.reverb.algo == kReverbAlgoDefault;

    switch (unit) {
        case ValueUnit::NONE:
            return {};

        case ValueUnit::CUTOFF_HZ:
            return {format_hz(cutoff_hz(value)), ""};

        case ValueUnit::LFO_HZ: {
            const float hz = songcore::lfo_hz(value);
            return {format_hz(hz), c.cased("ONE CYCLE ") + format_ms(1000.0 / hz)};
        }

        case ValueUnit::REVERB_PRE:
            return {format_ms(reverb_pre_delay_seconds(value) * 1000.0), ""};

        case ValueUnit::REVERB_DECAY:
            if (dflt && reverb_decay_feedback(value, p.reverb.size) >= 1.0f) return {c.cased("ENDLESS"), ""};
            return {format_ms((dflt ? reverb_decay_seconds(value) : reverb_df_decay_seconds(value)) * 1000.0),
                    c.cased("TILL THE TAIL FADES")};

        case ValueUnit::REVERB_DAMP:
            return {format_hz(dflt ? reverb_damp_freq(value) : static_cast<float>(reverb_df_damp_hz(value))), ""};

        case ValueUnit::DELAY_TIME: {
            const double ms = delay_time_seconds(value) * 1000.0;
            if (value == 0) return {format_ms(ms), ""};
            char steps[48];
            std::snprintf(steps, sizeof(steps), "%.1f STEPS AT %d BPM",
                          ms / songcore::tics_ms(songcore::TICS_PER_STEP, tempo), tempo);
            return {format_ms(ms), c.cased(steps)};
        }

        case ValueUnit::DELAY_TONE:
            if (!delay_tone_filters(value)) return {c.cased("NO FILTER"), ""};
            return {format_hz(delay_tone_hz(value)), ""};

        case ValueUnit::TICS:
            return tics_hint(c, value, tempo);

        case ValueUnit::TABLE_TICS:
            // The four modes, as the table player reads them.
            if (value == 0x00) return {c.cased("ONE ROW PER NOTE"), ""};
            if (value == 0xFC) return {c.cased("ROW BY THE NOTE'S OCTAVE"), ""};
            if (value == 0xFE) return {c.cased("ROW BY THE NOTE"), ""};
            if (value == 0xFF) return {format_ms(1000.0 / 200.0) + c.cased(" A ROW"), c.cased("200 ROWS A SECOND, ANY TEMPO")};
            {
                ValueHint h = tics_hint(c, value, tempo);
                h.head += c.cased(" A ROW");
                return h;
            }

        case ValueUnit::GROOVE_TICS: {
            if (value == 0) return {c.cased("STEP SKIPPED"), ""};
            const songcore::Groove& g = p.grooves[static_cast<size_t>(s.currentGroove)];
            return tics_hint(c, value, tempo, songcore::groove_ticks_per_step(g));
        }

        case ValueUnit::MIDI_LENGTH:
            if (value == 0) return {c.cased("UNTIL THE NEXT NOTE"), ""};
            return tics_hint(c, value, tempo);

        case ValueUnit::CHORD:
            return chord_hint(c, value, s);

        case ValueUnit::ENV_TIME:
        case ValueUnit::ENV_TIME_LINEAR:
        case ValueUnit::ENV_TIME_FAST:
        case ValueUnit::ENV_TIME_ANALOG: {
            // Fixed in time; the tics are what it comes to at this tempo, to place it against the steps.
            const int scale = unit == ValueUnit::ENV_TIME ? songcore::ENV_SCALE_LOG
                            : static_cast<int>(unit) - static_cast<int>(ValueUnit::ENV_TIME_LINEAR);   // CURVE_*
            const double ms = exactMs >= 0.0 ? exactMs : songcore::env_ms(value, scale);
            if (ms <= 0.0) return {c.cased("INSTANT"), ""};
            char tics[48];
            std::snprintf(tics, sizeof(tics), "%.1f TICKS AT %d BPM", ms / songcore::tics_ms(1, tempo), tempo);
            return {format_ms(ms), c.cased(tics)};
        }

        case ValueUnit::INSTRUMENT:
            return instrument_hint(c, value, s);
    }
    return {};
}

ValueHint held_value_hint(const Canvas& c, const AppState& s) {
    if (!s.aHeld || s.selection.active || !s.cellUnderCursor) return {};
    const CursorContext k = s.cellUnderCursor();
    if (k.unit == ValueUnit::NONE || k.capabilities.isEmpty) return {};
    return value_hint(c, k.unit, k.currentValue, s, k.exactMs);
}

}  // namespace pt::ui
