// The helpers more than one of the dispatcher's files use. Private to `input_dispatcher.cpp` and
// `ui/dispatch/`; nothing else includes it.
#pragma once

#include "ui/input_dispatcher.h"
#include "ui/filesystem.h"

#include <algorithm>
#include <climits>
#include <string>

namespace pt::ui {

using songcore::Chain;
using songcore::Instrument;
using songcore::Note;
using songcore::Phrase;
using songcore::Project;

// One load, opened and closed. ⚠️ RAII: a missed end_load() leaves the LOADING layer up and the app deaf.
struct LoadScope {
    LoadScope(InputDispatcher& d, long long now_ms, std::string detail) : d_(d) {
        d_.begin_load(now_ms, std::move(detail));
    }
    ~LoadScope() { d_.end_load(); }

    LoadScope(const LoadScope&)            = delete;
    LoadScope& operator=(const LoadScope&) = delete;

  private:
    InputDispatcher& d_;
};

/** B+LEFT/RIGHT through a pool of `count` items, wrapping at both ends. */
inline int step_wrapping(int value, int delta, int count) {
    return ((value + delta) % count + count) % count;
}

/** B+UP/DOWN through the same pool, 16 at a time. Stops at the ends, where a single step wraps. */
inline int page_clamping(int value, int direction, int count) {
    return std::clamp(value + direction * 16, 0, count - 1);
}

/** `base`, or the first free `base_2`, `base_3`, … — a WAV name in `dir` that nothing holds yet. */
inline std::string free_wav_stem(FileSystem& fs, const std::string& dir, const std::string& base) {
    const auto path = [&](int n) { return dir + "/" + numbered_name(base, n) + ".wav"; };
    return numbered_name(base, first_free(fs, 1, INT_MAX, path));
}

/** The 20 characters a slot's name is cut to when it is taken from a file. One writer, two readers. */
inline std::string adopted_name(const std::string& stem) { return stem.substr(0, 20); }

// The name a slot takes from what it plays — its file, or a MACRO's model ("" with nothing) — tells an
// adopted name from a typed one.
inline std::string instrument_auto_name(const Instrument& ins) {
    switch (ins.instrumentType) {
        case songcore::InstrumentType::SOUNDFONT:
            return ins.soundfontPath ? adopted_name(path_stem(*ins.soundfontPath)) : std::string{};
        case songcore::InstrumentType::SAMPLER:
            return ins.sampleFilePath ? adopted_name(path_stem(*ins.sampleFilePath)) : std::string{};
        case songcore::InstrumentType::MACRO:
            return songcore::macro_model_name(ins.macroModel);
        case songcore::InstrumentType::EXTERNAL:
            break;
    }
    return {};
}

inline std::string instrument_auto_name(const Project& p, int id) {
    if (id < 0 || static_cast<size_t>(id) >= p.instruments.size()) return {};
    return instrument_auto_name(p.instruments[static_cast<size_t>(id)]);
}

// The one rule for a slot's name when what it plays changes — its type or its MACRO model. Read BEFORE
// the change; `settle` after it. A name that was the default or came from the old source becomes the new
// source's, or the default; a typed one is put back.
// ⚠️ Read before ANY part of the change: a source cleared first makes an adopted name look typed, and it
// then sticks through every later change.
struct NameFollow {
    std::string before;
    bool        follows = false;

    void settle(Instrument& ins) const {
        if (!follows) {
            ins.name = before;
            return;
        }
        const std::string now = instrument_auto_name(ins);
        ins.name = now.empty() ? songcore::default_instrument_name(ins.id) : now;
    }
};

inline NameFollow name_follows(const Instrument& ins) {
    const std::string adopted = instrument_auto_name(ins);
    return {ins.name, songcore::instrument_has_default_name(ins) || (!adopted.empty() && ins.name == adopted)};
}

}  // namespace pt::ui
