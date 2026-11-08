#include "ui/midi_ports.h"

#include "ui/modules/midi_settings.h"   // MIDI_PORT_OFF / AUTO, midi_offset_in_force

#include <algorithm>
#include <utility>

namespace pt::ui {

namespace {

// Resolve a saved choice against the current list: OFF 0, AUTO 1, a device its place, a missing name
// 0. The setting is a name because an index would silently point at whatever took its place.
int resolve_port_index(const std::vector<std::string>& names, const std::string& want) {
    if (want == MIDI_PORT_AUTO) return 1;
    for (size_t i = MIDI_FIRST_PORT; i < names.size(); ++i)
        if (names[i] == want) return static_cast<int>(i);
    return 0;
}

bool listed(const std::vector<std::string>& names, const std::string& name) {
    return std::find(names.begin(), names.end(), name) != names.end();
}

// Open the first port the choice accepts (the named device, or under AUTO any `skip` allows) and return
// its name, "" if none took. A port that refused is skipped until it leaves the list and comes back.
template <class Skip, class Open>
std::string open_first_port(const std::vector<std::string>& names, const std::string& choice,
                            std::vector<std::string>& refused, Skip skip, Open open) {
    if (choice == MIDI_PORT_OFF) return {};
    const bool autoPick = (choice == MIDI_PORT_AUTO);
    for (size_t i = MIDI_FIRST_PORT; i < names.size(); ++i) {
        const std::string& name = names[i];
        const int          port = static_cast<int>(i) - MIDI_FIRST_PORT;
        if (autoPick ? skip(port) : name != choice) continue;
        if (listed(refused, name)) continue;
        if (open(port)) return name;
        refused.push_back(name);
    }
    return {};
}

void forget_unlisted(std::vector<std::string>& refused, const std::vector<std::string>& names) {
    refused.erase(std::remove_if(refused.begin(), refused.end(),
                                 [&](const std::string& n) { return !listed(names, n); }),
                  refused.end());
}

}  // namespace

MidiPorts::MidiPorts(AppState& s, songcore::SongcoreHost& host)
    : s_(s), host_(host),
      out_{s.midiOut, s.midiOutDeviceNames, s.midiOutDeviceIndex, s.midiOutOpenName,
           s.settings.midiOutDevice, "OUT", "OUTPUT OFF", "PORT OPENED", "PORT BUSY"},
      in_{s.midiIn, s.midiInDeviceNames, s.midiInDeviceIndex, s.midiInOpenName,
          s.settings.midiInDevice, "IN", "INPUT OFF", "INPUT OPENED", "INPUT BUSY"} {}

// ─── Boot ────────────────────────────────────────────────────────────────────────────────────────

void MidiPorts::boot_out() {
    // OFFSET and SYNC first and unconditionally: they are on screen, and must not read ON and send nothing.
    host_.set_midi_offset_ms(midi_offset_in_force(s_.settings, s_.midiAutoOffsetMs));
    host_.set_midi_sync_out(s_.settings.midiSyncOut);

    list(out_);
    if (out_open()) {                    // the env override already opened this same device
        out_.openName = out_.choice;
        return;
    }
    if (out_.index != 0) apply(out_);
    s_.midiStatusText.clear();           // boot news is the console's job, not the screen's
}

void MidiPorts::boot_in() {
    // ⚠️ At boot, so knobs reach their mappings without a visit to the MIDI screen.
    host_.set_midi_control_channel(s_.settings.midiControlChannel);
    list(in_);
    if (in_.index != 0) apply(in_);
    // ⚠️ Unconditional, after the OUT port's boot: with no input the thru verdict is still decided.
    update_thru();
    s_.midiStatusText.clear();
}

// ─── Listing and picking ─────────────────────────────────────────────────────────────────────────

void MidiPorts::refresh() {
    list(out_);
    list(in_);
}

void MidiPorts::apply_out() { apply(out_); }
void MidiPorts::apply_in() { apply(in_); }

template <class Backend> void MidiPorts::list(Side<Backend>& d) {
    d.names.assign({MIDI_PORT_OFF, MIDI_PORT_AUTO});   // the module never handles "no device"
    for (int i = 0, n = d.backend ? d.backend->device_count() : 0; i < n; ++i)
        d.names.push_back(d.backend->device_name(i));
    d.index = resolve_port_index(d.names, d.choice);
}

template <class Backend> void MidiPorts::apply(Side<Backend>& d) {
    // The (just changed) choice's place in the drawn list.
    d.index = resolve_port_index(d.names, d.choice);

    if (!d.backend) { s_.midiStatusText = "NO MIDI BACKEND"; return; }

    close(d);
    d.refused.clear();
    const bool opened = open(d);
    if (d.choice == MIDI_PORT_OFF) s_.midiStatusText = d.offText;
    else if (opened)               s_.midiStatusText = d.openedText;
    else                           s_.midiStatusText = d.refused.empty() ? "NO DEVICE YET" : d.busyText;

    // Below every arm: the loopback check depends on which ports are open.
    update_thru();
}

// ─── Opening and closing ─────────────────────────────────────────────────────────────────────────

void MidiPorts::close(Side<songcore::IMidiOut>& d) {
    // ⚠️ Panic first: the device changes under the same `IMidiOut`, which panics only on a pointer
    // change, so the owed note-offs must go out before the old port closes.
    host_.midi_out().panic();
    d.backend->close();
    d.openName.clear();
}

bool MidiPorts::open(Side<songcore::IMidiOut>& d) {
    d.openName = open_first_port(
        d.names, d.choice, d.refused, [&](int port) { return d.backend->is_builtin_synth(port); },
        [&](int port) { return d.backend->open(port); });
    return !d.openName.empty();
}

// ⚠️ The input's order matters: unhook the sink before closing, reset the parser between (stale
// running status), hook the sink before opening.
void MidiPorts::close(Side<songcore::IMidiIn>& d) {
    d.backend->set_sink(nullptr);
    d.backend->close();
    host_.reset_midi_in();
    d.openName.clear();
}

bool MidiPorts::open(Side<songcore::IMidiIn>& d) {
    d.openName = open_first_port(d.names, d.choice, d.refused, [](int) { return false; }, [&](int port) {
        // Unwired again on a refusal, so a half-opened backend cannot deliver into a "closed" port.
        d.backend->set_sink(&host_.midi_in_sink());
        if (d.backend->open(port)) return true;
        d.backend->set_sink(nullptr);
        return false;
    });
    return !d.openName.empty();
}

// ─── Hot-plug and thru ───────────────────────────────────────────────────────────────────────────

void MidiPorts::run_hotplug(long long now_ms) {
    // Polled: winmm has no notification.
    if (now_ms < scanDueMs_) return;
    scanDueMs_ = now_ms + SCAN_MS;

    const bool outChanged = rescan(out_);
    const bool inChanged  = rescan(in_);
    if (outChanged || inChanged) update_thru();
}

template <class Backend> bool MidiPorts::rescan(Side<Backend>& d) {
    if (!d.backend) return false;
    list(d);
    forget_unlisted(d.refused, d.names);

    bool changed = false;
    if (!d.openName.empty() &&
        (d.backend->broken() || !d.backend->is_open() || !listed(d.names, d.openName))) {
        close(d);
        set_status(s_, std::string("MIDI ") + d.tag + " UNPLUGGED", false);
        changed = true;
    }
    if (d.openName.empty() && open(d)) {
        set_status(s_, std::string("MIDI ") + d.tag + ": " + d.openName, true);
        changed = true;
    }
    return changed;
}

void MidiPorts::update_thru() {
    // ⚠️ Compare the OPEN names, not the saved choices: a port that refused sends nothing. A loopback's
    // two directions carry the same display name on every backend seen.
    const bool loopback = !in_.openName.empty() && in_.openName == out_.openName;

    host_.set_midi_in_thru(!loopback);

    // ⚠️ On screen: a silent suppression looks like an EXTERNAL instrument ignoring the keyboard.
    if (loopback) s_.midiStatusText = "THRU OFF: LOOP";
}

}  // namespace pt::ui
