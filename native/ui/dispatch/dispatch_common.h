// The helpers more than one of the dispatcher's files use. Private to `input_dispatcher.cpp` and
// `ui/dispatch/`; nothing else includes it.
#pragma once

#include "ui/input_dispatcher.h"
#include "ui/std_filesystem.h"   // path_stem

#include <string>

namespace pt::ui {

using songcore::Chain;
using songcore::Instrument;
using songcore::Note;
using songcore::Phrase;
using songcore::Project;

/**
 * One load, opened and closed. ⚠️ RAII because load paths return early (four arms of the browser's
 * switch do); a missed `end_load()` would leave the LOADING layer up and the app deaf to input.
 */
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

/** The 20 characters a slot's name is cut to when it is taken from a file. One writer, two readers. */
inline std::string adopted_name(const std::string& stem) { return stem.substr(0, 20); }

/**
 * The name slot `id` would carry had it adopted its CURRENT source file's — "" with no source (EXTERNAL
 * has none). Compare the actual name against this to tell an adopted name from a typed one.
 */
inline std::string instrument_auto_name(const Project& p, int id) {
    if (id < 0 || static_cast<size_t>(id) >= p.instruments.size()) return {};
    const Instrument& ins = p.instruments[static_cast<size_t>(id)];
    const std::string source = (ins.instrumentType == songcore::InstrumentType::SOUNDFONT)
                                   ? ins.soundfontPath.value_or("")
                                   : ins.sampleFilePath.value_or("");
    return source.empty() ? std::string{} : adopted_name(path_stem(source));
}

}  // namespace pt::ui
