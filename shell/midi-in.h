#ifndef POCKETTRACKER_SHELL_MIDI_IN_H
#define POCKETTRACKER_SHELL_MIDI_IN_H

// Which `songcore::IMidiIn` this build gets — the one #ifdef, so main.cpp has none. Every backend
// derives from `MidiInBase`, so main.cpp's console block is written once against that base.
//
// Android is absent on purpose: its port lives on the Java side, and android-main.cpp builds its
// own `AndroidMidiIn`. The "no backend on this platform" arm in app.cpp remains for any other platform.

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
