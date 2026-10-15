// The helpers more than one of the dispatcher's files use. Private to `input_dispatcher.cpp` and
// `ui/dispatch/`; nothing else includes it.
#pragma once

#include "ui/input_dispatcher.h"
#include "ui/std_filesystem.h"   // path_stem

#include <algorithm>
#include <cstdio>
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

/** A status line — the app's or the browser's own — green when `ok`, red otherwise. */
template <class S> void set_status(S& s, std::string message, bool ok) {
    s.statusMessage = std::move(message);
    s.statusSuccess = ok;
}

/** B+LEFT/RIGHT through a pool of `count` items, wrapping at both ends. */
inline int step_wrapping(int value, int delta, int count) {
    return ((value + delta) % count + count) % count;
}

/** B+UP/DOWN through the same pool, 16 at a time. Stops at the ends, where a single step wraps. */
inline int page_clamping(int value, int direction, int count) {
    return std::clamp(value + direction * 16, 0, count - 1);
}

/** `base`, or the first free `base_0001`, `base_0002`, … — a WAV name in `dir` that nothing holds yet. */
inline std::string free_wav_stem(FileSystem& fs, const std::string& dir, const std::string& base) {
    std::string stem = base;
    for (int n = 1; fs.file_exists(dir + "/" + stem + ".wav"); ++n) {
        char suffix[16];   // "_%04d" of an int can be 12 bytes (-Wformat-truncation)
        std::snprintf(suffix, sizeof(suffix), "_%04d", n);
        stem = base + suffix;
    }
    return stem;
}

/** The 20 characters a slot's name is cut to when it is taken from a file. One writer, two readers. */
inline std::string adopted_name(const std::string& stem) { return stem.substr(0, 20); }

// The name slot `id` would carry had it adopted its current source file's ("" with no source) — tells an
// adopted name from a typed one.
inline std::string instrument_auto_name(const Project& p, int id) {
    if (id < 0 || static_cast<size_t>(id) >= p.instruments.size()) return {};
    const Instrument& ins = p.instruments[static_cast<size_t>(id)];
    const std::string source = (ins.instrumentType == songcore::InstrumentType::SOUNDFONT)
                                   ? ins.soundfontPath.value_or("")
                                   : ins.sampleFilePath.value_or("");
    return source.empty() ? std::string{} : adopted_name(path_stem(source));
}

}  // namespace pt::ui
