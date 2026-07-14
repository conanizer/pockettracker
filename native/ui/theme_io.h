#pragma once

// ─── .ptt — a theme, as a file ───────────────────────────────────────────────────────────────────
//
// The reader and writer for the theme files the four built-in palettes have been standing in for
// since S1. It is the same job `songcore/project_io.h` does for `.ptp`, at a twentieth of the size,
// and it is done the same way and for the same reason: nlohmann parses (tolerant, like kotlinx's
// `ignoreUnknownKeys` + `coerceInputValues`), and a hand-rolled emitter writes bytes that are
// IDENTICAL to what `kotlinx.serialization` produces on Android.
//
// ⚠️ WHY BYTE-EXACT, when a theme is only eighteen colours and any JSON would "work"?
//
// Because byte-exactness is the only claim a golden can actually CHECK. "Semantically equivalent" is
// a promise a test cannot hold you to; "these 400 bytes match the 400 bytes Kotlin wrote" is a promise
// a `memcmp` holds you to. S2 bought the whole `.ptp` round trip with that argument and it has caught
// real drift since. The user-facing half is the same as it was there: a `PocketTracker/` folder copied
// off a phone onto an SD card must simply work, in both directions, with no conversion step.
//
// ⚠️ kotlinx's contract, reproduced exactly (this is the part that is easy to get almost right):
//
//   * `Json { prettyPrint = true }` — and `encodeDefaults` therefore defaults to **FALSE**. Every
//     field equal to its DECLARED DEFAULT is OMITTED. Since the field defaults ARE the CLASSIC
//     palette, saving an unmodified CLASSIC theme called "CLASSIC" writes `{}` — which is correct,
//     and reads back as CLASSIC.
//   * 4-space indent; `"key": value`; members separated by ",\n"; no trailing newline.
//   * Keys in @Serializable DECLARATION order, not alphabetical.
//   * A `Long` colour is a plain decimal number: 0xFF0A0A0A is written `4278518794`, not `"#0A0A0A"`.
//   * `visualizerType` is an enum → its DECLARED NAME, as a string.
//
// ⚠️ AND THE ENUM NAME IS **NOT** THE NAME THE SETTINGS SCREEN DRAWS. `SettingsModule::visualizer_names()`
// is a list of six LABELS sized to fit a settings cell — "OCTA.F", "SPECT", "SPCT.P" — while kotlinx
// writes the Kotlin identifier: OCTA_FULL, SPECTRUM, SPECTRUM_PEAKS. Three of the six differ. Writing
// the display label into a `.ptt` would produce a file Android parses without complaint and reads as
// SCOPE, because `coerceInputValues` turns an unknown enum into the field default rather than an
// error. A silent wrong answer, in a file that looks right. They are two vocabularies for one enum and
// they must never be confused — which is why the serial names live here and nowhere else.

#include <cstdint>
#include <string>

#include "songcore/project_io.h"   // JsonWriter — kotlinx-exact, and already goldened by ptroundtrip
#include "ui/filesystem.h"
#include "ui/theme.h"
#include "vendor/nlohmann/json.hpp"

namespace pt::ui {

/** The enum's DECLARED name — what kotlinx writes. NOT the settings screen's label. See above. */
inline const char* visualizer_serial_name(VisualizerType v) {
    switch (v) {
        case VisualizerType::SCOPE:          return "SCOPE";
        case VisualizerType::FLAT:           return "FLAT";
        case VisualizerType::OCTA:           return "OCTA";
        case VisualizerType::OCTA_FULL:      return "OCTA_FULL";
        case VisualizerType::SPECTRUM:       return "SPECTRUM";
        case VisualizerType::SPECTRUM_PEAKS: return "SPECTRUM_PEAKS";
    }
    return "SCOPE";
}

/** The inverse. An unknown name reads as the field default — kotlinx's `coerceInputValues`. */
inline VisualizerType visualizer_from_serial_name(const std::string& s) {
    if (s == "FLAT")           return VisualizerType::FLAT;
    if (s == "OCTA")           return VisualizerType::OCTA;
    if (s == "OCTA_FULL")      return VisualizerType::OCTA_FULL;
    if (s == "SPECTRUM")       return VisualizerType::SPECTRUM;
    if (s == "SPECTRUM_PEAKS") return VisualizerType::SPECTRUM_PEAKS;
    return VisualizerType::SCOPE;   // incl. "SCOPE", and every value an older/newer build might write
}

/**
 * A theme → kotlinx-exact `.ptt` bytes.
 *
 * Every field is compared against `Theme{}` — the DECLARED default, which is the CLASSIC palette — and
 * omitted when equal. That is `encodeDefaults = false`, and it is why this takes a default-constructed
 * theme as its yardstick rather than the theme it is writing.
 */
inline std::string serialize_theme(const Theme& t) {
    const Theme d{};   // the field defaults — kotlinx's yardstick, not ours
    songcore::JsonWriter w;
    w.begin_object();

    if (t.name != d.name) w.field_string("name", t.name);

    // ⚠️ Spelled out, and deliberately NOT driven off `theme_color_rows()`, though the first seventeen
    // lines below are that table in that order. Two reasons, and the second is the real one:
    // `meterBorder` is a serialized field with no editor row, so the table is not the field list; and
    // more importantly THIS ORDER IS THE FILE FORMAT. The row table is a UI concern and someone may one
    // day reorder it to group the colours differently — which must not silently rewrite every `.ptt`
    // key order and break the byte-golden. The two lists look alike today and answer different
    // questions; the golden pins this one.
    auto color = [&](const char* key, Argb v, Argb dv) {
        if (v != dv) w.field_int(key, static_cast<long long>(v));
    };

    color("background",      t.background,      d.background);
    color("rowEvery4th",     t.rowEvery4th,     d.rowEvery4th);
    color("rowCursor",       t.rowCursor,       d.rowCursor);
    color("rowPlayback",     t.rowPlayback,     d.rowPlayback);
    color("rowSelection",    t.rowSelection,    d.rowSelection);
    color("textTitle",       t.textTitle,       d.textTitle);
    color("textParam",       t.textParam,       d.textParam);
    color("textValue",       t.textValue,       d.textValue);
    color("textCursor",      t.textCursor,      d.textCursor);
    color("textEmpty",       t.textEmpty,       d.textEmpty);
    color("vizBackground",   t.vizBackground,   d.vizBackground);
    color("vizCenterLine",   t.vizCenterLine,   d.vizCenterLine);
    color("vizWave",         t.vizWave,         d.vizWave);
    color("meterBackground", t.meterBackground, d.meterBackground);
    color("meterLow",        t.meterLow,        d.meterLow);
    color("meterMid",        t.meterMid,        d.meterMid);
    color("meterHigh",       t.meterHigh,       d.meterHigh);
    color("meterBorder",     t.meterBorder,     d.meterBorder);   // no editor row; still a field

    if (t.visualizerType != d.visualizerType)
        w.field_string("visualizerType", visualizer_serial_name(t.visualizerType));

    w.end_object();
    return std::move(w.out);
}

/**
 * `.ptt` bytes → a theme. Tolerant, exactly as Kotlin's reader is
 * (`Json { ignoreUnknownKeys = true; coerceInputValues = true }`):
 *
 *   * an unknown key is ignored (a theme from a newer build still loads),
 *   * a missing key keeps the FIELD DEFAULT (which is why `out` starts as a fresh CLASSIC),
 *   * a key of the wrong type, or an unknown enum name, keeps the field default too.
 *
 * Returns false only when the text is not a JSON object at all — the one case Kotlin throws on, and
 * the one the caller reports as "LOAD FAILED" rather than silently accepting a blank palette.
 */
inline bool parse_theme(const std::string& text, Theme& out) {
    const nlohmann::json j = nlohmann::json::parse(text, nullptr, /*allow_exceptions=*/false);
    if (!j.is_object()) return false;

    Theme t{};   // the field defaults fill in every key the file does not carry

    if (const auto it = j.find("name"); it != j.end() && it->is_string())
        t.name = it->get<std::string>();

    auto color = [&](const char* key, Argb& field) {
        const auto it = j.find(key);
        if (it == j.end() || !it->is_number_integer()) return;
        // Kotlin stores a colour as a Long. Read it as one and keep the low 32 bits, which is what
        // Compose's `Color(Long)` does with a hand-edited file carrying an out-of-range value.
        field = static_cast<Argb>(static_cast<uint64_t>(it->get<int64_t>()) & 0xFFFFFFFFu);
    };

    color("background",      t.background);
    color("rowEvery4th",     t.rowEvery4th);
    color("rowCursor",       t.rowCursor);
    color("rowPlayback",     t.rowPlayback);
    color("rowSelection",    t.rowSelection);
    color("textTitle",       t.textTitle);
    color("textParam",       t.textParam);
    color("textValue",       t.textValue);
    color("textCursor",      t.textCursor);
    color("textEmpty",       t.textEmpty);
    color("vizBackground",   t.vizBackground);
    color("vizCenterLine",   t.vizCenterLine);
    color("vizWave",         t.vizWave);
    color("meterBackground", t.meterBackground);
    color("meterLow",        t.meterLow);
    color("meterMid",        t.meterMid);
    color("meterHigh",       t.meterHigh);
    color("meterBorder",     t.meterBorder);

    if (const auto it = j.find("visualizerType"); it != j.end() && it->is_string())
        t.visualizerType = visualizer_from_serial_name(it->get<std::string>());

    out = t;
    return true;
}

/** Write `theme` to `path`. The bytes are kotlinx's — see serialize_theme. */
inline bool save_theme_file(FileSystem& fs, const std::string& path, const Theme& theme) {
    return fs.write_file(path, serialize_theme(theme));
}

/**
 * Read a theme from `path`.
 *
 * ⚠️ THE VISUALIZER IS *NOT* TAKEN FROM THE FILE, and that is the caller's call rather than the
 * parser's — `parse_theme` faithfully reads whatever the file says. Kotlin discards it at the call
 * site (`appTheme = loaded.copy(visualizerType = appTheme.visualizerType)`), because the palette
 * belongs to the theme but the visualizer belongs to the USER: loading a friend's palette should not
 * also switch your scope to their spectrum analyser. The field is still WRITTEN (kotlinx serializes
 * the whole object), so a `.ptt` carries a visualizer that nothing ever reads back. That asymmetry is
 * Kotlin's, it is harmless, and it is reproduced rather than tidied — a "fix" here would be a silent
 * behaviour divergence in a file format shared with the phone.
 */
inline bool load_theme_file(FileSystem& fs, const std::string& path, Theme& out) {
    std::string text;
    if (!fs.read_file(path, text)) return false;

    Theme loaded{};
    if (!parse_theme(text, loaded)) return false;

    loaded.visualizerType = out.visualizerType;   // the user's, kept across the load
    out = loaded;
    return true;
}

}  // namespace pt::ui
