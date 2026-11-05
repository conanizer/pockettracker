PocketTracker for macOS (beta)
==============================

A tracker for small screens. This is the same program that runs on Linux handhelds,
Windows and Android, drawing the same 640x480 design and running the same audio engine.

This is a BETA: nobody on the team owns a Mac, so this build has only been compiled,
never played. Reports of what works and what does not are very welcome - MIDI most of
all, see below.


RUNNING IT
----------

macOS 10.15 or later, on Apple Silicon or Intel. Move PocketTracker.app wherever you
like (Applications is fine) and open it.

The first time, macOS will refuse, because this download is not signed with a paid
Apple developer account - not because anything is wrong with it. To let it run:

    macOS 15 (Sequoia) and later
        1. Double-click PocketTracker.app, click "Done" on the warning.
        2. Open System Settings > Privacy & Security, scroll down, and click
           "Open Anyway" next to PocketTracker. Confirm with your password.

    macOS 14 and earlier
        Right-click (or Control-click) PocketTracker.app, choose Open, then Open again.

Or, in Terminal:

    xattr -dr com.apple.quarantine /path/to/PocketTracker.app

You only do this once per version. macOS will also ask whether PocketTracker may use
your Documents folder - say yes, that is where your songs live.


YOUR FILES
----------

    ~/Documents/PocketTracker/Projects       songs (.ptp)
    ~/Documents/PocketTracker/Samples        WAV, MP3, FLAC, OGG, Opus, M4A
    ~/Documents/PocketTracker/Soundfonts     .sf2
    ~/Documents/PocketTracker/Instruments    saved instruments
    ~/Documents/PocketTracker/Renders        exported mixes and stems
    ~/Documents/PocketTracker/Themes         colour themes

The folders are created the first time you run it. These are the same folder names the
Android and Windows versions use, so a PocketTracker folder copied from another device
works here as-is.


CONTROLS
--------

The app is built around a handheld's buttons, so a gamepad works if you have one
connected. On the keyboard those buttons are:

    WASD or arrows   the D-pad
    K or Return      A
    J or Escape      B
    U / I            L / R (the shoulder buttons)
    Left Shift       SELECT
    Space            START

And the tracker itself:

    A+UP/DOWN            edit the value under the cursor
    A+LEFT/RIGHT         edit it in bigger steps
    A+B                  clear it
    A,A                  insert the next unused phrase/chain/instrument
    B+LEFT/RIGHT         change WHICH phrase/chain/table you are looking at
    B+UP/DOWN            page through the song
    R+D-pad              move between screens
    START                play / stop


MIDI
----

PROJECT > MIDI is where the MIDI devices are chosen: OUTPUT for a synth, INPUT for a
keyboard. Both list every MIDI device macOS knows about.

No hardware? Every Mac has a built-in virtual cable, the IAC bus:

    1. Open Audio MIDI Setup (in Applications > Utilities), choose Window > Show MIDI
       Studio, double-click "IAC Driver" and tick "Device is online".
    2. In PocketTracker, PROJECT > MIDI, set OUTPUT to "IAC Driver Bus 1".
    3. Open GarageBand with a software instrument track - it listens to every MIDI
       device, the IAC bus included. TEST on the MIDI screen now plays a note there,
       and an instrument with TYPE set to EXTERNAL plays GarageBand's sound.

The same bus as PocketTracker's INPUT lets another app play PocketTracker.


SAVING AND QUITTING
-------------------

PROJECT > SAVE writes your song. The app also autosaves a few seconds after you stop
editing; if an autosave is left behind, you are asked whether to recover it the next
time you start.

PROJECT > EXIT is the clean way out: it asks before discarding unsaved work.
Cmd+Q or closing the window quits at once without saving your song - the autosave keeps
the work, and you are offered it back next time.


SOMETHING WRONG?
----------------

Every run writes a log to ~/Documents/PocketTracker/pockettracker-log.txt - what the
audio device and display actually did, and whether your samples loaded. Please attach
it to a report:

    https://github.com/conanizer/pockettracker/issues


LICENCES
--------

PocketTracker is free software under the GNU General Public License v3 or later. The
full text is in licenses/LICENSE, and licenses/THIRD-PARTY-NOTICES.md lists every
third-party component compiled into the program, with its licence. licenses/SDL2-LICENSE.txt
is the licence of the SDL2 build linked into the app.

You are free to use it, share it, and sell what you make with it. The songs you write
are yours.
