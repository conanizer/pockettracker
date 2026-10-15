#ifndef POCKETTRACKER_SHELL_MIDI_OUT_H
#define POCKETTRACKER_SHELL_MIDI_OUT_H

// Which `IMidiOut` this build gets — the one #ifdef, so main.cpp has none. Every backend derives from
// `MidiOutBase`. Android builds its own in android-main.cpp (the port is on the Java side).

#include "linux/midi-out-alsa.h"    // compiles to nothing off Linux
#include "windows/midi-out-winmm.h"   // compiles to nothing off Windows

#if defined(_WIN32)
#define PT_HAS_PLATFORM_MIDI_OUT 1
namespace ptshell { using PlatformMidiOut = WinmmMidiOut; }
#elif defined(__linux__) && !defined(__ANDROID__)
#define PT_HAS_PLATFORM_MIDI_OUT 1
namespace ptshell { using PlatformMidiOut = AlsaMidiOut; }
#endif

#endif  // POCKETTRACKER_SHELL_MIDI_OUT_H
