PocketTracker for Windows
=========================

A tracker for small screens. This is the same program that runs on Linux handhelds and
on Android, drawing the same 640x480 design and running the same audio engine.


RUNNING IT
----------

Double-click PocketTracker.exe. There is nothing to install and nothing to configure.

Windows 10 or later, 64-bit. If SmartScreen puts up a blue "Windows protected your PC"
box, that is because this download is not code-signed, not because anything is wrong
with it: click "More info", then "Run anyway".

A console window opens behind the tracker and stays there. That is deliberate, not a
bug - it prints what the audio device and display actually did, which folders it is
using, and whether your samples loaded. If something goes wrong, the text in that
window is the most useful thing you can send us. Closing it closes the app.


YOUR FILES
----------

Everything lives in your Documents folder:

    Documents\PocketTracker\Projects       songs (.ptp)
    Documents\PocketTracker\Samples        WAV, MP3, FLAC, OGG, Opus, M4A
    Documents\PocketTracker\Soundfonts     .sf2
    Documents\PocketTracker\Instruments    saved instruments
    Documents\PocketTracker\Renders        exported mixes and stems
    Documents\PocketTracker\Themes         colour themes

The folders are created the first time you run it. Drop samples straight into Samples\
and they will show up in the file browser.

These are the same folder names the Android version uses, so a PocketTracker folder
copied off a phone works here as-is, and vice versa.


CONTROLS
--------

The app is built around a handheld's buttons, so a gamepad works if you have one
plugged in. On the keyboard those buttons are:

    WASD or arrows   the D-pad
    K or Enter       A
    J or Escape      B
    U / I            L / R (the shoulder buttons)
    Left Shift       SELECT
    Space            START
    F10              quit

And the tracker itself:

    A+UP/DOWN            edit the value under the cursor
    A+LEFT/RIGHT         edit it in bigger steps
    A+B                  clear it
    A,A                  insert the next unused phrase/chain/instrument
    B+LEFT/RIGHT         change WHICH phrase/chain/table you are looking at
    B+UP/DOWN            page through the song
    R+D-pad              move between screens - SONG CHAIN PHRASE INSTRUMENT TABLE
                         MODS INST.POOL GROOVE MIXER EFFECTS PROJECT SETTINGS
    L+B                  start a selection (tap again to widen)
    B                    copy      L+A  cut / paste      L+R  deselect
    START                play / stop

The full list is printed in the console window every time the app starts.


SAVING
------

PROJECT > SAVE writes your song. The app also autosaves a few seconds after you stop
editing, so a crash or a flat battery costs you seconds rather than an evening - if an
autosave is left behind, you are asked whether to recover it the next time you start.

There are two ways out, and they mean different things:

    PROJECT > EXIT   the clean one. It asks before discarding unsaved work, and
                     answering yes leaves nothing behind.
    F10              the quick one. It does NOT save your song and does NOT ask -
                     but it does write the autosave on the way out, so the work is
                     still there when you come back.

Neither one writes your .ptp file for you. Use PROJECT > SAVE for that.


LICENCES
--------

PocketTracker is free software under the GNU General Public License v3 or later. The
full text is in licenses\LICENSE, and licenses\THIRD-PARTY-NOTICES.md lists every
third-party component compiled into the program, with its licence. licenses\SDL2-LICENSE.txt
is the licence of the SDL2 build linked into PocketTracker.exe.

You are free to use it, share it, and sell what you make with it. The songs you write
are yours.


SOMETHING WRONG?
----------------

https://github.com/conanizer/pockettracker/issues - and please paste the text from the
console window, it usually says what happened.
