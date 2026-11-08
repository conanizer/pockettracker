#pragma once

// The MIDI ports: which device is open out and which in, hot-plug, and thru. The lists and the open
// names live in AppState, where the MIDI screen draws them.

#include "songcore/host.h"
#include "ui/app_state.h"

#include <string>
#include <vector>

namespace pt::ui {

class MidiPorts {
  public:
    MidiPorts(AppState& s, songcore::SongcoreHost& host);

    // Open the port the settings name, once at boot — the same calls the MIDI screen makes. A port
    // already open (the dev override) is left alone. ⚠️ IN after OUT: the thru verdict needs both.
    void boot_out();
    void boot_in();

    // Re-list both directions and re-resolve the saved names. A name not in the list shows OFF: the row
    // shows what is open, never what was once wanted.
    void refresh();

    // Make the port match its setting, after a pick. A pick is a retry: a port that refused is asked
    // again, and the choice is kept on a refusal.
    void apply_out();
    void apply_in();

    // Once a second: close a device that has gone, open one that has arrived. Settings are never
    // overwritten.
    void run_hotplug(long long now_ms);

    // A backend can exist with no port open; "TEST SENT" into a closed one would lie.
    bool out_open() const { return s_.midiOut != nullptr && s_.midiOut->is_open(); }

  private:
    // One direction: its backend, its list and open device on screen, its setting, the words it shows.
    template <class Backend> struct Side {
        Backend*&                 backend;   // null with no backend
        std::vector<std::string>& names;     // OFF, AUTO, then the ports
        int&                      index;     // where the setting sits in `names`
        std::string&              openName;  // "" for none — under AUTO it differs from the setting
        const std::string&        choice;    // the setting: a device's name, OFF or AUTO
        const char*               tag;       // "OUT" / "IN"
        const char *              offText, *openedText, *busyText;
        // Refused to open (another app holds it) — not retried until it leaves the list.
        std::vector<std::string>  refused{};
    };

    template <class Backend> void list(Side<Backend>& d);
    template <class Backend> void apply(Side<Backend>& d);
    // True when a device went or came.
    template <class Backend> bool rescan(Side<Backend>& d);

    // Close the port and forget the device; open what the setting asks for (false if nothing took).
    void close(Side<songcore::IMidiOut>& d);
    void close(Side<songcore::IMidiIn>& d);
    bool open(Side<songcore::IMidiOut>& d);
    bool open(Side<songcore::IMidiIn>& d);

    // MIDI thru on a loopback port (listening where we send) is a feedback loop.
    void update_thru();

    static constexpr long long SCAN_MS = 1000;

    AppState&                      s_;
    songcore::SongcoreHost&        host_;
    Side<songcore::IMidiOut>       out_;
    Side<songcore::IMidiIn>        in_;
    long long                      scanDueMs_ = 0;
};

}  // namespace pt::ui
