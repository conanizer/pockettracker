#ifndef POCKETTRACKER_SHELL_MIDI_IN_H
#define POCKETTRACKER_SHELL_MIDI_IN_H

// Which `IMidiIn` this build gets — the one #ifdef, so main.cpp has none. Android builds its own
// `AndroidMidiIn` in android-main.cpp (the port is on the Java side).

#include "linux/midi-in-alsa.h"    // compiles to nothing off Linux
#include "windows/midi-in-winmm.h"   // compiles to nothing off Windows

#if defined(_WIN32)
#define PT_HAS_PLATFORM_MIDI_IN 1
namespace ptshell { using PlatformMidiIn = WinmmMidiIn; }
#elif defined(__linux__) && !defined(__ANDROID__)
#define PT_HAS_PLATFORM_MIDI_IN 1
namespace ptshell { using PlatformMidiIn = AlsaMidiIn; }
#endif

#endif  // POCKETTRACKER_SHELL_MIDI_IN_H
