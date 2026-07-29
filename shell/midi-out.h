#ifndef POCKETTRACKER_SHELL_MIDI_OUT_H
#define POCKETTRACKER_SHELL_MIDI_OUT_H

// Which `songcore::IMidiOut` this build gets — the one #ifdef, so main.cpp has none.
//
// Every backend derives from `MidiOutBase`, so the console block in main.cpp (device list, env-var
// override, TEST note, trace) is written once against that base and is identical on every platform.
// Adding a backend means adding a header and an arm here, and touching nothing else.
//
// Android is absent on purpose: it does not come through main.cpp at all — android-main.cpp builds
// its own, because the port lives on the Java side (see midi-out-android.h).

#include "midi-out-alsa.h"    // compiles to nothing off Linux
#include "midi-out-winmm.h"   // compiles to nothing off Windows

#if defined(_WIN32)
#define PT_HAS_PLATFORM_MIDI_OUT 1
namespace ptshell { using PlatformMidiOut = WinmmMidiOut; }
#elif defined(__linux__) && !defined(__ANDROID__)
#define PT_HAS_PLATFORM_MIDI_OUT 1
namespace ptshell { using PlatformMidiOut = AlsaMidiOut; }
#endif

#endif  // POCKETTRACKER_SHELL_MIDI_OUT_H
