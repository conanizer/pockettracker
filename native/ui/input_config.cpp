#include "ui/input_config.h"

#include "vendor/nlohmann/json.hpp"

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
        // Name the accepted values. A user who typed "switch" or "x360" has to be told what to type
        // instead, or the next edit is another guess.
        warn(warnings, "config.json: \"controller.abxy\" = \"" + value +
                           "\" is not one of auto/xbox/nintendo — using auto");
        return;
    }
    cfg.abxy = layout;
}

/** The `keyboard` object. Each entry replaces that button's default key list. */
void read_keyboard(const json& j, InputConfig& cfg, std::vector<InputConfigWarning>& warnings) {
    const auto it = j.find("keyboard");
    if (it == j.end()) return;
    if (!it->is_object()) {
        warn(warnings, "config.json: \"keyboard\" is not an object — ignored");
        return;
    }

    for (auto entry = it->begin(); entry != it->end(); ++entry) {
        Button button{};
        if (!button_from_name(entry.key().c_str(), button)) {
            warn(warnings, "config.json: \"keyboard." + entry.key() +
                               "\" is not a button name — ignored");
            continue;
        }
        if (!entry->is_array()) {
            warn(warnings, "config.json: \"keyboard." + entry.key() +
                               "\" is not an array of key names — ignored");
            continue;
        }

        // A listed button REPLACES its defaults, so an empty array is meaningful: it unbinds. That is
        // why the vector is created before the loop rather than on the first accepted element — a
        // button whose every entry was rejected ends up UNBOUND, not silently back on its defaults,
        // which is the reading that matches what the file plainly says.
        std::vector<std::string> names;
        for (const json& k : *entry) {
            if (!k.is_string()) {
                warn(warnings, "config.json: \"keyboard." + entry.key() +
                                   "\" contains a non-string entry — that entry ignored");
                continue;
            }
            std::string name = k.get<std::string>();
            if (name.empty()) continue;
            names.push_back(std::move(name));
        }
        cfg.keyboard[button] = std::move(names);
    }
}

}  // namespace

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

bool load_input_config(FileSystem& fs, InputConfig& out, std::vector<InputConfigWarning>& warnings) {
    std::string blob;
    if (!fs.read_file(fs.config_path(), blob)) return false;   // no file: the common case, not an error

    const json j = json::parse(blob, nullptr, /*allow_exceptions=*/false);
    if (!j.is_object()) {
        warn(warnings, "config.json: not valid JSON — the whole file is ignored");
        return false;
    }

    read_controller(j, out, warnings);
    read_keyboard(j, out, warnings);
    return true;
}

}  // namespace pt::ui
