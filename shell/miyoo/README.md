# PocketTracker — Miyoo Mini / Mini+ (OnionOS)

## Install

You need `pockettracker-miyoo.zip` and a way to write to the device's SD card. **Method A** works on
any Mini and is the one to use if you are unsure. **Method B** needs a Mini+ and Wi-Fi, and saves
opening the device.

Either way, what has to end up on the card is this exact path:

```
App/PocketTracker/launch.sh
```

⚠️ If it lands one folder deeper — `App/pockettracker-miyoo/PocketTracker/…` — the app does not
appear on the shelf at all, and nothing tells you why.

### A. With the card in a PC

1. **Switch the device off** and take the SD card out. Onion writes to the card while it is running,
   so pulling it live can damage what is already on there.
2. **Put the card in a PC.**
3. **Extract the zip onto the card**, at the top level — the drive letter or mount point on its own,
   nothing after it.
   - ⚠️ On Windows, *Extract All* fills the destination box in for you and adds a new folder named
     after the zip. Delete that last part so only the drive letter is left (`E:\`, not
     `E:\pockettracker-miyoo\`). If that goes wrong, extract anywhere you like and drag the `App`
     folder onto the card by hand — it merges with the `App` folder already there and adds
     `PocketTracker` beside the apps you have.
4. **Check the path.** Open the card and confirm `App\PocketTracker\launch.sh` is really there.
5. **Eject the card**, put it back in the device and switch it on.
6. PocketTracker is on Onion's **Apps** shelf. Press A on it.

### B. Over Wi-Fi (Mini+ only)

1. On the device, open **Tweaks → Network**, join your Wi-Fi, and turn on **Samba** (or **FTP**, if
   you would rather use an FTP client). The page shows the device's IP address — note it down.
2. **Unzip the package on the PC first.** You want the `App` folder that comes out of it, not the zip.
3. From the PC, open `\\<the IP address>` in Explorer, or point an FTP client at that address. What
   you are looking at is the top level of the card — the same view as method A.
4. **Copy the `App` folder across.** It merges into the card's existing `App` folder and adds
   `PocketTracker` beside your other apps. Copying takes a minute or two over Wi-Fi; let it finish.
5. **Restart the device.** The shelf is only read at boot, so the app does not appear until then.
6. PocketTracker is on the **Apps** shelf. Press A on it.

### If it does not start

**Not on the Apps shelf at all** — the files went in one folder too deep. Put the card back in the
PC and check for `App/PocketTracker/launch.sh` exactly; if what you find is
`App/pockettracker-miyoo/PocketTracker/…`, move the `PocketTracker` folder up one level.

**On the shelf, but pressing A does nothing** — `App/PocketTracker/log.txt` on the card holds the
whole of that session's output and is overwritten each launch. That file is what to send if you
report it. If there is no `log.txt` there at all, the launcher itself never ran, and saying so is
the useful half of the report.

**It starts and drops straight back to the shelf** — a message with an exit code is put up on
the way out, and `log.txt` holds the detail behind it.

You do not have to worry about file permissions on this device, whichever method you used: the card
is FAT32, which has no Unix permission bits for Windows to lose, and the device grants the execute
bit to everything on the mount. (The PortMaster port says the opposite for a real reason: its card is
a Linux filesystem where those bits do survive, and unzipping on a PC strips them.)

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
the device out of memory in a way a phone would not. Short samples and a soundfont are what this
hardware is comfortable with.

## Notes

Unlike the PortMaster build, this package **ships its own SDL2** — the `mmiyoo` fork, which talks to
the SigmaStar MI GFX and MI AO hardware. There is no system SDL2 on this device to link instead.
It is in `libs/`, with its licence in `licenses/`. The `libneonarmmiyoo.so` beside it is part of
PocketTracker: that fork looks for a library of that name at load time, and this is our own build of
what it asks for, under the same GPL-3.0 as the rest of the app.
