#!/bin/sh
#
# PocketTracker — OnionOS (Miyoo Mini / Mini+) launch script.
#
# ⚠️ /bin/sh, NOT bash. OnionOS's userland is busybox ash; there is no bash on the device, and a
# `#!/bin/bash` here is an app that does nothing at all when you press A on it.
#
# =============================================================================================
#  This is NOT the PortMaster script and must not be made to look like one.
# =============================================================================================
#
# The two differ on the one thing that matters most, and in OPPOSITE directions:
#
#   PortMaster   links the DEVICE's libSDL2 — the copy its CFW patched for that hardware — and
#                deliberately sets no LD_LIBRARY_PATH.
#   here         there IS no system libSDL2. Every Miyoo port carries the `mmiyoo` fork itself, so
#                the libs/ directory beside this script is load-bearing rather than a mistake.
#
# The gptokeyb warning in the PortMaster script does not apply either: OnionOS has no gptokeyb and
# the buttons arrive as plain SDL keycodes, which is what config.json maps.

mydir=$(cd "$(dirname "$0")" && pwd)
cd "$mydir" || exit 1

# Everything this script and the app print, in one file, overwritten per launch — the run that just
# went wrong is the one anybody wants. ⚠️ THE APP LEAVES IT ALONE: its own session log only takes
# over a TERMINAL (main.cpp, open_session_log), so a launcher redirect stays the launcher's.
exec > "$mydir/log.txt" 2>&1

echo "PocketTracker launch  $(date)"

# ── The bundled SDL2 ─────────────────────────────────────────────────────────────────────────────
# `mmiyoo` is SigmaStar MI GFX for video and MI AO for audio. Nothing here touches /dev/fb0, and
# these three exports are the whole of the platform selection — every Miyoo port sets the same ones.
export LD_LIBRARY_PATH="$mydir/libs:$LD_LIBRARY_PATH"
export SDL_VIDEODRIVER=mmiyoo
export SDL_AUDIODRIVER=mmiyoo

# ── Where songs live ─────────────────────────────────────────────────────────────────────────────
# ⚠️⚠️ NOT this directory, and the reason is a NAME COLLISION rather than taste: Onion's app shelf
# reads `$mydir/config.json` (label, icon, launch) and PocketTracker reads `config.json` in its own
# root (folders, keyboard) — two different files that would be the same path.
#
# The SD card's top level is also simply where a user can find their work: plug the card into a PC
# and Projects/, Samples/, Soundfonts/, Instruments/, Renders/ and Themes/ are right there. The app
# creates all six at boot, so a card can be pulled, filled and put back.
PT_HOME=/mnt/SDCARD/PocketTracker
export POCKETTRACKER_HOME="$PT_HOME"
mkdir -p "$PT_HOME"

# ── The Miyoo button map, seeded ONCE ─────────────────────────────────────────────────────────────
# ⚠️⚠️ THIS IS THE ONLY THING THAT MAKES THE BUTTONS RIGHT, and it has to land before the FIRST
# launch. The app seeds its own starter config.json when none is present, and it never rewrites the
# file after that — so if it boots first, this device keeps the DESKTOP key map permanently and the
# collisions in miyoo-config.json's header are what the user gets.
#
# ⚠️ It prints which branch it took. A copy that silently does nothing is exactly how a config file
# looks applied and is not.
if [ -f "$PT_HOME/config.json" ]; then
    echo "config.json      : already present, left alone"
elif cp "$mydir/miyoo-config.json" "$PT_HOME/config.json" 2>/dev/null; then
    echo "config.json      : seeded from miyoo-config.json (Miyoo key map)"
else
    echo "config.json      : SEED FAILED - the buttons will use the desktop map and will be wrong"
fi

# A card unzipped on Windows arrives with no Unix mode bits, and the app then dies "Permission
# denied" on a file that is plainly there. Harmless where the mount hands out +x anyway.
chmod +x "$mydir/pockettracker" 2>/dev/null

./pockettracker
echo "exit code        : $?"
