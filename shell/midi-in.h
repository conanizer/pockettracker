#ifndef POCKETTRACKER_SHELL_MIDI_IN_H
#define POCKETTRACKER_SHELL_MIDI_IN_H

// Which `songcore::IMidiIn` this build gets — the one #ifdef, so main.cpp has none. midi-out.h's twin,
// and deliberately the same shape: every backend derives from `MidiInBase`, so the console block in
// main.cpp is written once against that base and is identical on every platform.
//
// ⚠️ **ANDROID IS ABSENT ON PURPOSE**, exactly as it is from midi-out.h: it does not come through
// main.cpp at all, because its port lives on the Java side. android-main.cpp builds its own
// `AndroidMidiIn` (see midi-in-android.h).
//
// Since E5 the two platforms that SHIP both have a backend — ALSA rawmidi here, `MidiManager` there —
// so the "no backend on this platform yet" arm in app.cpp is now reachable only on a build that is
// neither Windows, Linux nor Android. It stays because a row that lists nothing is not a row that lies.

#include "midi-in-alsa.h"    // compiles to nothing off Linux
#include "midi-in-winmm.h"   // compiles to nothing off Windows

#if defined(_WIN32)
#define PT_HAS_PLATFORM_MIDI_IN 1
namespace ptshell { using PlatformMidiIn = WinmmMidiIn; }
#elif defined(__linux__) && !defined(__ANDROID__)
#define PT_HAS_PLATFORM_MIDI_IN 1
namespace ptshell { using PlatformMidiIn = AlsaMidiIn; }
#endif

#endif  // POCKETTRACKER_SHELL_MIDI_IN_H
