# PocketTracker — Miyoo Mini / Mini+ (OnionOS)

A music tracker, the same one as the Android app and built from the same C++. The sequencer, the
sample engine, the DSP and every screen are shared source; only the window, the audio device and the
input come from SDL.

The Mini's panel is 640×480, which is exactly the size this tracker draws at — so the picture is
one screen pixel per app pixel, with nothing scaled.

## Install

Extract the zip at the **root of your SD card**. It lands in `App/PocketTracker/` and appears on
Onion's **Apps** shelf.

⚠️ Extract with something that keeps Unix permissions (the CFW's own installer, or `unzip` on a Mac
or Linux box). Windows Explorer throws them away, and Onion then cannot start the app. If that
happens, the fix is one line over SSH or ADB: `chmod +x /mnt/SDCARD/App/PocketTracker/launch.sh`.

## Controls

| Button | What it does |
|---|---|
| D-pad | Move the cursor |
| A | Edit / confirm / open the thing under the cursor |
| A + D-pad | Change the value under the cursor (up/down = fine, left/right = coarse) |
| B | Back / close |
| A + B | Delete the value under the cursor |
| B + D-pad | Cycle the item under the cursor (chain, phrase, instrument…) |
| A, A | Double-tap to insert a new item |
| R + D-pad | Move between screens — this is how you get everywhere |
| L | Selection and clipboard modifier (L+A cut/paste, L+B+A clone) |
| SELECT | The screen's context action (SELECT+A renames a file in the browser) |
| START | Play / stop |

On a Mini+ the second shoulder row does the same as the first: L2 is another L, R2 another R.
X, Y and MENU are not used.

There is no exit hotkey: quit from **PROJECT → EXIT**. It asks first if the song has unsaved
changes. If the launcher or a flat battery kills the app instead, the work is autosaved and offered
back the next time you start.

## Your files

Everything lives at the top level of the card, so you can pull it and drag files straight in:

```
/mnt/SDCARD/PocketTracker/
├── Projects/      your songs (.ptp)
├── Samples/       .wav .mp3 .flac .ogg .opus
├── Soundfonts/    .sf2 .sf3
├── Instruments/   saved instrument presets (.pti)
├── Themes/        colour themes (.ptt)
└── Renders/       WAV mixes and stems you bounce
```

The folders are created the first time you launch. The layout is the same one the Android app uses
inside its home folder, so a folder copied off a phone works as-is.

`config.json` also appears there on the first launch: it holds the button map and, if you want them,
your own folder locations. It is yours — the app reads it and never rewrites it.

## Memory

The Mini has **128 MB of RAM in total**, with the firmware already in it. A loaded sample costs
about twice its file size while it is in memory, so a song built from long stereo samples can run
the device out of memory in a way a phone would not. Short samples, and a soundfont rather than
eight long one-shots, are what this hardware is comfortable with.

## Notes

Unlike the PortMaster build, this package **ships its own SDL2** — the `mmiyoo` fork, which talks to
the SigmaStar MI GFX and MI AO hardware. There is no system SDL2 on this device to link instead.
It is in `libs/`, with its licence in `licenses/`.

If something goes wrong, `App/PocketTracker/log.txt` has the whole of that session's output and is
overwritten each launch.
