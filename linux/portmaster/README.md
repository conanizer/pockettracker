# PocketTracker

A music tracker for handhelds — the same tracker as the Android app, built from the same C++.
The sequencer, the sample engine, the DSP and every screen are shared source; only the window,
the audio device and the input come from SDL.

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

There is no exit hotkey: quit from **SETTINGS → EXIT**. It asks first if the song has unsaved
changes. If the launcher or a flat battery kills the app instead, the work is autosaved and
offered back the next time you start.

## Your files

Everything lives in the port's own `data/` folder, on the SD card:

```
ports/pockettracker/data/
├── Projects/      your songs (.ptp)
├── Samples/       .wav .mp3 .flac .ogg .opus
├── Soundfonts/    .sf2
├── Instruments/   saved instrument presets (.pti)
├── Themes/        colour themes (.ptt)
└── Renders/       WAV mixes and stems you bounce
```

The folders are created the first time you launch, so you can pull the card and drop files
straight in. The layout matches the Android app's `Documents/PocketTracker/`, so a folder copied
off a phone works as-is.

## Requirements

`aarch64` CFW with SDL2 **2.0.18 or newer**. No BIOS, no runtime, no extra downloads.

## Notes

The port uses the device's own SDL2 rather than shipping one, so it renders and plays through the
libraries your CFW patched for its screen and audio hardware.
