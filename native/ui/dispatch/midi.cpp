// The MIDI ports: choosing one out and one in, hot-plug, and thru.

#include "ui/dispatch/dispatch_common.h"


#include <algorithm>
#include <string>
#include <vector>

namespace pt::ui {

// ─── MIDI OUT ────────────────────────────────────────────────────────────────────────────────────

void InputDispatcher::boot_midi_port() {
    // The OFFSET and SYNC first and unconditionally: both live in settings.json and are drawn on screen,
    // so leaving them unapplied would show ON and send nothing.
    host_.set_midi_offset_ms(midi_offset_in_force(s_.settings, s_.midiAutoOffsetMs));
    host_.set_midi_sync_out(s_.settings.midiSyncOut);

    refresh_midi_devices();
    if (port_open()) {                             // the env override already opened this same device
        s_.midiOutOpenName = s_.settings.midiOutDevice;
        return;
    }
    if (s_.midiDeviceIndex != 0) apply_midi_device();
    s_.midiStatusText.clear();                     // boot news is the console's job, not the screen's
}

// Resolve a saved choice against the current list: OFF 0, AUTO 1, a device its place, a missing name
// 0. The setting is a name because an index would silently point at whatever took its place.
static int resolve_port_index(const std::vector<std::string>& names, const std::string& want) {
    if (want == MIDI_PORT_AUTO) return 1;
    for (size_t i = MIDI_FIRST_PORT; i < names.size(); ++i)
        if (names[i] == want) return static_cast<int>(i);
    return 0;
}

static bool listed(const std::vector<std::string>& names, const std::string& name) {
    return std::find(names.begin(), names.end(), name) != names.end();
}

// Open the first port the choice accepts (the named device, or under AUTO any `skip` allows) and return
// its name, "" if none took. A port that refused is skipped until it leaves the list and comes back.
template <class Skip, class Open>
static std::string open_first_port(const std::vector<std::string>& names, const std::string& choice,
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

static void forget_unlisted(std::vector<std::string>& refused, const std::vector<std::string>& names) {
    refused.erase(std::remove_if(refused.begin(), refused.end(),
                                 [&](const std::string& n) { return !listed(names, n); }),
                  refused.end());
}

static std::string port_status(const std::string& choice, bool opened, bool anyRefused,
                               const char* off, const char* opened_text, const char* busy) {
    if (choice == MIDI_PORT_OFF) return off;
    if (opened) return opened_text;
    return anyRefused ? busy : "NO DEVICE YET";
}

void InputDispatcher::refresh_midi_devices() {
    s_.midiDeviceNames.assign({MIDI_PORT_OFF, MIDI_PORT_AUTO});   // the module never handles "no device"

    if (s_.midiOut) {
        const int n = s_.midiOut->device_count();
        for (int i = 0; i < n; ++i) s_.midiDeviceNames.push_back(s_.midiOut->device_name(i));
    }

    s_.midiDeviceIndex = resolve_port_index(s_.midiDeviceNames, s_.settings.midiOutDevice);
}

void InputDispatcher::close_midi_out() {
    // ⚠️ Panic here: `set_out` panics only on a pointer change, and only the device behind it is
    // changing. Skip this and notes on the closing port hang until the hardware is power-cycled.
    host_.midi_out().panic();
    s_.midiOut->close();
    s_.midiOutOpenName.clear();
}

bool InputDispatcher::open_midi_out() {
    s_.midiOutOpenName = open_first_port(
        s_.midiDeviceNames, s_.settings.midiOutDevice, midiOutRefused_,
        [&](int port) { return s_.midiOut->is_builtin_synth(port); },
        [&](int port) { return s_.midiOut->open(port); });
    return !s_.midiOutOpenName.empty();
}

void InputDispatcher::apply_midi_device() {
    // `midiDeviceIndex` is where the (just changed) choice sits in the drawn list.
    s_.midiDeviceIndex = resolve_port_index(s_.midiDeviceNames, s_.settings.midiOutDevice);

    if (!s_.midiOut) { s_.midiStatusText = "NO MIDI BACKEND"; return; }

    close_midi_out();
    // A pick is a retry: a port that refused before is asked again. The choice is kept on refusal.
    midiOutRefused_.clear();
    const bool opened = open_midi_out();
    s_.midiStatusText = port_status(s_.settings.midiOutDevice, opened, !midiOutRefused_.empty(),
                                    "OUTPUT OFF", "PORT OPENED", "PORT BUSY");

    // ⭐ One call below every arm: the loopback check depends on which port is OPEN, and each arm
    // (including OUTPUT OFF, which turns thru back on) leaves that different.
    update_midi_thru();
}

// ─── MIDI IN ─────────────────────────────────────────────────────────────────────────────────────

void InputDispatcher::boot_midi_in_port() {
    // ⚠️ At boot, not only when the row is touched: a session that never opens the MIDI screen must
    // still have its knobs reaching their mappings.
    host_.set_midi_control_channel(s_.settings.midiControlChannel);
    refresh_midi_in_devices();
    if (s_.midiInDeviceIndex != 0) apply_midi_in_device();
    // ⚠️ Unconditional, after the OUT port's boot: with no input the thru verdict is still decided.
    update_midi_thru();
    // Boot news belongs on the console, not on the MIDI screen minutes later.
    s_.midiStatusText.clear();
}

void InputDispatcher::refresh_midi_in_devices() {
    s_.midiInDeviceNames.assign({MIDI_PORT_OFF, MIDI_PORT_AUTO});

    if (s_.midiIn) {
        const int n = s_.midiIn->device_count();
        for (int i = 0; i < n; ++i) s_.midiInDeviceNames.push_back(s_.midiIn->device_name(i));
    }

    s_.midiInDeviceIndex = resolve_port_index(s_.midiInDeviceNames, s_.settings.midiInDevice);
}

void InputDispatcher::close_midi_in() {
    // ⚠️ The teardown order matters — see the header.
    s_.midiIn->set_sink(nullptr);
    s_.midiIn->close();
    host_.reset_midi_in();
    s_.midiInOpenName.clear();
}

bool InputDispatcher::open_midi_in() {
    s_.midiInOpenName = open_first_port(
        s_.midiInDeviceNames, s_.settings.midiInDevice, midiInRefused_,
        [](int) { return false; },
        [&](int port) {
            // The sink BEFORE the open, or bytes arriving in between are dropped; unwired again on a
            // refusal, so a half-opened backend cannot deliver into a "closed" port.
            s_.midiIn->set_sink(&host_.midi_in_sink());
            if (s_.midiIn->open(port)) return true;
            s_.midiIn->set_sink(nullptr);
            return false;
        });
    return !s_.midiInOpenName.empty();
}

void InputDispatcher::apply_midi_in_device() {
    s_.midiInDeviceIndex = resolve_port_index(s_.midiInDeviceNames, s_.settings.midiInDevice);

    if (!s_.midiIn) { s_.midiStatusText = "NO MIDI BACKEND"; return; }

    close_midi_in();
    midiInRefused_.clear();   // a pick is a retry, as on the OUTPUT side
    const bool opened = open_midi_in();
    s_.midiStatusText = port_status(s_.settings.midiInDevice, opened, !midiInRefused_.empty(),
                                    "INPUT OFF", "INPUT OPENED", "INPUT BUSY");

    update_midi_thru();   // the IN half of the loopback check
}

// ─── Hot-plug ────────────────────────────────────────────────────────────────────────────────────

void InputDispatcher::run_midi_hotplug() {
    // Once a second, rescan both lists: a device that has gone is closed, and one the choice names
    // (under AUTO, the first plugged in) is opened. Settings are never overwritten. Polled, because
    // winmm has no notification.
    if (now_ms_ < midiScanDueMs_) return;
    midiScanDueMs_ = now_ms_ + MIDI_SCAN_MS;

    bool changed = false;

    if (s_.midiOut) {
        refresh_midi_devices();
        forget_unlisted(midiOutRefused_, s_.midiDeviceNames);
        const std::string was = s_.midiOutOpenName;
        if (!was.empty() &&
            (s_.midiOut->broken() || !s_.midiOut->is_open() || !listed(s_.midiDeviceNames, was))) {
            close_midi_out();
            s_.statusMessage = "MIDI OUT UNPLUGGED";
            s_.statusSuccess = false;
            changed = true;
        }
        if (s_.midiOutOpenName.empty() && open_midi_out()) {
            s_.statusMessage = "MIDI OUT: " + s_.midiOutOpenName;
            s_.statusSuccess = true;
            changed = true;
        }
    }

    if (s_.midiIn) {
        refresh_midi_in_devices();
        forget_unlisted(midiInRefused_, s_.midiInDeviceNames);
        const std::string was = s_.midiInOpenName;
        if (!was.empty() &&
            (s_.midiIn->broken() || !s_.midiIn->is_open() || !listed(s_.midiInDeviceNames, was))) {
            close_midi_in();
            s_.statusMessage = "MIDI IN UNPLUGGED";
            s_.statusSuccess = false;
            changed = true;
        }
        if (s_.midiInOpenName.empty() && open_midi_in()) {
            s_.statusMessage = "MIDI IN: " + s_.midiInOpenName;
            s_.statusSuccess = true;
            changed = true;
        }
    }

    if (changed) update_midi_thru();
}

void InputDispatcher::update_midi_thru() {
    // ⚠️ Compare the OPEN names, not the saved choices: a port that refused sends nothing. A loopback's
    // two directions carry the same display name on every backend seen.
    const bool loopback = !s_.midiInOpenName.empty() && s_.midiInOpenName == s_.midiOutOpenName;

    host_.set_midi_in_thru(!loopback);

    // ⚠️ Said on screen, over "INPUT OPENED": a silent suppression looks like an EXTERNAL instrument
    // that ignores the keyboard.
    if (loopback) s_.midiStatusText = "THRU OFF: LOOP";
}

}  // namespace pt::ui
