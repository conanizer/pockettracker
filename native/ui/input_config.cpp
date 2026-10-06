#include "ui/input_config.h"

#include "vendor/nlohmann/json.hpp"

#include <algorithm>

namespace pt::ui {

namespace {

using nlohmann::json;

void warn(std::vector<InputConfigWarning>& out, std::string text) {
    out.push_back(InputConfigWarning{std::move(text)});
}

/** The `controller` object. Absent or malformed leaves `cfg.abxy` at its default. */
void read_controller(const json& j, InputConfig& cfg, std::vector<InputConfigWarning>& warnings) {
    const auto it = j.find("controller");
    if (it == j.end()) return;
    if (!it->is_object()) {
        warn(warnings, "config.json: \"controller\" is not an object — ignored");
        return;
    }

    const auto ait = it->find("abxy");
    if (ait == it->end()) return;
    if (!ait->is_string()) {
        warn(warnings, "config.json: \"controller.abxy\" is not a string — ignored");
        return;
    }

    const std::string value = ait->get<std::string>();
    AbxyLayout        layout{};
    if (!abxy_from_name(value, layout)) {
        // Name the accepted values, or the next edit is another guess.
        warn(warnings, "config.json: \"controller.abxy\" = \"" + value +
                           "\" is not one of auto/xbox/nintendo — using auto");
        return;
    }
    cfg.abxy = layout;
}

/** A `keyboard`- or `gamepad`-shaped object. Each entry replaces that button's default inputs. */
void read_bindings(const json& j, const char* section, ButtonBindings& out,
                   std::vector<InputConfigWarning>& warnings) {
    const auto it = j.find(section);
    if (it == j.end()) return;
    const std::string where = std::string("config.json: \"") + section;
    if (!it->is_object()) {
        warn(warnings, where + "\" is not an object — ignored");
        return;
    }

    std::string listedAs[static_cast<size_t>(Button::COUNT)];
    for (auto entry = it->begin(); entry != it->end(); ++entry) {
        Button button{};
        if (!button_from_name(entry.key().c_str(), button)) {
            warn(warnings, where + "." + entry.key() + "\" is not a button name — ignored");
            continue;
        }
        // "START" and "Start" are one button. ⚠️ The parser sorts keys, so which line comes "later" is
        // not the file's order: say which one was used, or the other looks applied.
        std::string& seen = listedAs[static_cast<size_t>(button)];
        if (!seen.empty()) {
            warn(warnings, where + "\": \"" + seen + "\" and \"" + entry.key() +
                               "\" are the same button — only \"" + entry.key() + "\" is used");
        }
        seen = entry.key();
        if (!entry->is_array()) {
            warn(warnings, where + "." + entry.key() + "\" is not an array of names — ignored");
            continue;
        }

        // A listed button REPLACES its defaults, so `[]` unbinds — and one whose every entry was
        // rejected ends up UNBOUND, not back on its defaults, matching what the file says.
        std::vector<std::string> names;
        for (const json& k : *entry) {
            if (!k.is_string()) {
                warn(warnings, where + "." + entry.key() +
                                   "\" contains a non-string entry — that entry ignored");
                continue;
            }
            std::string name = k.get<std::string>();
            if (name.empty()) continue;
            names.push_back(std::move(name));
        }
        out[button] = std::move(names);
    }
}

/** One `repeat` value: a whole number of ms, clamped into [lo, hi]. */
void read_repeat_ms(const json& r, const char* key, int lo, int hi, int& out,
                    std::vector<InputConfigWarning>& warnings) {
    const auto it = r.find(key);
    if (it == r.end()) return;
    const std::string where = std::string("config.json: \"repeat.") + key + "\"";
    if (!it->is_number()) {
        warn(warnings, where + " is not a number — ignored");
        return;
    }
    const double v = it->get<double>();
    const int    clamped = v < lo ? lo : v > hi ? hi : static_cast<int>(v);
    if (static_cast<double>(clamped) != v) {
        const bool inRange = v >= lo && v <= hi;
        warn(warnings, where + " = " + it->dump() +
                           (inRange ? std::string(" is not a whole number")
                                    : " is outside " + std::to_string(lo) + "-" + std::to_string(hi) +
                                          " ms") +
                           " — using " + std::to_string(clamped));
    }
    out = clamped;
}

void read_repeat(const json& j, RepeatConfig& out, std::vector<InputConfigWarning>& warnings) {
    const auto it = j.find("repeat");
    if (it == j.end()) return;
    if (!it->is_object()) {
        warn(warnings, "config.json: \"repeat\" is not an object — ignored");
        return;
    }
    read_repeat_ms(*it, "delay", RepeatConfig::MIN_DELAY, RepeatConfig::MAX_DELAY, out.delay, warnings);
    read_repeat_ms(*it, "interval", RepeatConfig::MIN_INTERVAL, RepeatConfig::MAX_INTERVAL,
                   out.interval, warnings);
}

/** What each Miyoo Mini button sends, as SDL key names. */
/**
 * One Miyoo Mini button: its printed name, the key its SDL sends for it in keyboard mode (what older
 * config files name), and the controller button(s) it arrives as.
 */
struct MiyooButton {
    const char* name;
    const char* key;
    const char* pad[2];
};

constexpr MiyooButton MIYOO_MINI_BUTTONS[] = {
    {"UP", "Up", {"dpup"}},           {"DOWN", "Down", {"dpdown"}},
    {"LEFT", "Left", {"dpleft"}},     {"RIGHT", "Right", {"dpright"}},
    {"A", "Space", {"a"}},            {"B", "Left Ctrl", {"b"}},
    {"X", "Left Shift", {"x"}},       {"Y", "Left Alt", {"y"}},
    {"L1", "E", {"leftshoulder"}},    {"L2", "Tab", {"lefttrigger"}},
    {"R1", "T", {"rightshoulder"}},   {"R2", "Backspace", {"righttrigger"}},
    {"SELECT", "Right Ctrl", {"back"}}, {"START", "Return", {"start"}},
    {"MENU", "Escape", {"guide"}},
    {"L", nullptr, {"leftshoulder", "lefttrigger"}},
    {"R", nullptr, {"rightshoulder", "righttrigger"}},
};

/** Parse; on failure `error` names where (", line 11 column 9"), else is empty. */
json parse_reporting_position(const std::string& text, std::string& error) {
    error.clear();
    try {
        return json::parse(text);
    } catch (const json::parse_error& e) {
        size_t line = 1, column = 1;
        const size_t end = e.byte > 0 ? std::min<size_t>(e.byte - 1, text.size()) : 0;
        for (size_t i = 0; i < end; ++i) {
            if (text[i] == '\n') { ++line; column = 1; }
            else { ++column; }
        }
        error = ", line " + std::to_string(line) + " column " + std::to_string(column);
        return json();
    }
}

}  // namespace

void miyoo_mini_to_pad_names(ButtonBindings& bindings) {
    for (auto& slot : bindings.keys) {
        if (!slot) continue;
        std::vector<std::string> out;
        for (const std::string& name : *slot) {
            const MiyooButton* hit = nullptr;
            for (const MiyooButton& d : MIYOO_MINI_BUTTONS) {
                if (same_name_any_case(name.c_str(), d.name) ||
                    (d.key && same_name_any_case(name.c_str(), d.key))) {
                    hit = &d;
                    break;
                }
            }
            if (!hit) { out.push_back(name); continue; }
            for (const char* k : hit->pad) {
                if (k) out.emplace_back(k);
            }
        }
        *slot = std::move(out);
    }
}

const char* abxy_name(AbxyLayout layout) {
    switch (layout) {
        case AbxyLayout::AUTO:     return "auto";
        case AbxyLayout::XBOX:     return "xbox";
        case AbxyLayout::NINTENDO: return "nintendo";
    }
    return "auto";
}

bool abxy_from_name(const std::string& name, AbxyLayout& out) {
    for (const AbxyLayout l : {AbxyLayout::AUTO, AbxyLayout::XBOX, AbxyLayout::NINTENDO}) {
        if (name == abxy_name(l)) { out = l; return true; }
    }
    return false;
}

bool straighten_typographic_punctuation(std::string& text) {
    static const struct { const char* from; const char* to; } SWAPS[] = {
        {"\xE2\x80\x9C", "\""},  // “
        {"\xE2\x80\x9D", "\""},  // ”
        {"\xE2\x80\x9E", "\""},  // „
        {"\xE2\x80\xB3", "\""},  // ″
        {"\xEF\xBC\x82", "\""},  // ＂
        {"\xEF\xBC\x8C", ","},   // ，
        {"\xEF\xBC\x9A", ":"},   // ：
    };
    bool changed = false;
    for (const auto& s : SWAPS) {
        const std::string from = s.from;
        for (size_t pos = text.find(from); pos != std::string::npos; pos = text.find(from, pos)) {
            text.replace(pos, from.size(), s.to);
            changed = true;
        }
    }
    return changed;
}

bool load_input_config(FileSystem& fs, InputConfig& out, std::vector<InputConfigWarning>& warnings) {
    std::string blob;
    if (!fs.read_file(fs.config_path(), blob)) return false;   // no file: the common case

    std::string error;
    json        j = parse_reporting_position(blob, error);
    if (!j.is_object() && straighten_typographic_punctuation(blob)) {
        j = parse_reporting_position(blob, error);
        if (j.is_object()) {
            warn(warnings, "config.json: curly quotes or full-width commas were read as plain ones - "
                           "a plain-text editor avoids them");
        }
    }
    if (!j.is_object()) {
        // The line number is the whole help: on a handheld this log is the only feedback there is.
        warn(warnings, "config.json: not valid JSON" + error + " — the whole file is ignored");
        return false;
    }

    read_controller(j, out, warnings);
    read_bindings(j, "keyboard", out.keyboard, warnings);
    read_bindings(j, "gamepad", out.gamepad, warnings);
    read_repeat(j, out.repeat, warnings);
    return true;
}

}  // namespace pt::ui
