#!/bin/sh
#
# PocketTracker — OnionOS (Miyoo Mini / Mini+) launch script.
#
# ⚠️ /bin/sh: the device has busybox ash and no bash.
# Run via PocketTracker.port -> Emu/PORTS/launch_standalone.sh, which already sets libs/ and clears
# LD_PRELOAD; repeated here so the script also runs alone over SSH.
#
# ⚠️ Unlike PortMaster, the device has no SDL2 of its own: the `mmiyoo` fork in libs/ is the one.
# ⚠️ The buttons arrive as a GAMEPAD, not keys: the bundled SDL sends keys only to an app that opens no
# controllers.

mydir=$(cd "$(dirname "$0")" && pwd)
cd "$mydir" || exit 1

# The script's and the app's output, overwritten per launch.
exec > "$mydir/log.txt" 2>&1

echo "PocketTracker launch  $(date)"

# ── The bundled SDL2 ─────────────────────────────────────────────────────────────────────────────
# `mmiyoo`: the chip's own video and audio. These three exports are the whole platform choice.
export LD_LIBRARY_PATH="$mydir/libs:$LD_LIBRARY_PATH"
export SDL_VIDEODRIVER=mmiyoo
export SDL_AUDIODRIVER=mmiyoo

# ── Where songs live ─────────────────────────────────────────────────────────────────────────────
# ⚠️ At the card's top level, not in the port's folder, so the user's work is easy to reach from a PC.
# ⚠️ Also the older App package's path: replacing it keeps every song.
PT_HOME=/mnt/SDCARD/PocketTracker
export POCKETTRACKER_HOME="$PT_HOME"
mkdir -p "$PT_HOME"

# ── The Miyoo button map, seeded ONCE ─────────────────────────────────────────────────────────────
# ⚠️ Before the first launch: otherwise the app writes its own desktop config.json and never rewrites
# it, and L2/R2 do nothing. Each branch prints, so a failed copy cannot pass for an applied one.
if [ -f "$PT_HOME/config.json" ]; then
    echo "config.json      : already present, left alone"
elif cp "$mydir/miyoo-config.json" "$PT_HOME/config.json" 2>/dev/null; then
    echo "config.json      : seeded from miyoo-config.json (Miyoo key map)"
else
    echo "config.json      : SEED FAILED - the buttons will use the desktop map and will be wrong"
fi

# Refreshed as config.example.json on every launch, where newer settings show. The app writes no
# example here: its own would name the desktop keys.
if cp -f "$mydir/miyoo-config.json" "$PT_HOME/config.example.json" 2>/dev/null; then
    echo "config.example   : refreshed from miyoo-config.json"
else
    echo "config.example   : COPY FAILED"
fi

# ── The demo song, seeded ONCE ────────────────────────────────────────────────────────────────────
# Only in a package built from a tree that had `demo/`.
# ⚠️ Guarded by a marker, not by "is the song there": that would bring back a deleted demo and
# overwrite an edited one. Wiping $PT_HOME seeds it again.
# ⚠️ Whole folders, not a file glob: the sample names contain spaces.
DEMO_SEEDED="$PT_HOME/.demo-seeded"
if [ ! -d "$mydir/demo" ]; then
    echo "demo song        : not in this package"
elif [ -f "$DEMO_SEEDED" ]; then
    echo "demo song        : already seeded, left alone"
else
    demo_ok=1
    for D in "$mydir"/demo/*; do
        [ -d "$D" ] || continue
        cp -rf "$D" "$PT_HOME/" || demo_ok=0
    done
    if [ "$demo_ok" = 1 ] && touch "$DEMO_SEEDED" 2>/dev/null; then
        echo "demo song        : seeded into $PT_HOME"
    else
        # Not marked, so the next launch tries again.
        echo "demo song        : SEED FAILED - the card may be full or read-only"
    fi
fi

# ── The audio device, taken back from Onion ───────────────────────────────────────────────────────
# ⚠️ Without this the app closes during start-up: Onion's `audioserver` holds the audio device, the
# chip refuses a second owner, and no audio is fatal. Onion's own stop script also carries the user's
# volume over to the next owner, so it is preferred to a kill. The .port leaves Onion's flag off, so
# this block (and its wait) is what stops it. Onion restarts the server on the way back to the menu.
# ⚠️ `miyoodir` defaulted first: with it unset, one branch of Onion's script kills wpa_supplicant and
# udhcpc and cannot restart them — no Wi-Fi until a reboot.
: "${miyoodir:=/mnt/SDCARD/miyoo}"
export miyoodir
ONION_STOP_AUDIO=/mnt/SDCARD/.tmp_update/script/stop_audioserver.sh
audio_stopped=0
if [ -f "$ONION_STOP_AUDIO" ]; then
    sh "$ONION_STOP_AUDIO"
    audio_stopped=1
    echo "audioserver      : stopped via Onion's stop_audioserver.sh"
elif pgrep audioserver > /dev/null 2>&1; then
    # Not Onion, or an older one: the kill frees the device, but the volume is not carried over.
    killall -q -9 audioserver audioserver.mod 2> /dev/null
    audio_stopped=1
    echo "audioserver      : killed directly (this firmware has no stop_audioserver.sh)"
else
    echo "audioserver      : not running"
fi

# ⚠️ The release is asynchronous: opening a moment too early fails the same way. At most two seconds.
# `pgrep` matches the process name, so the stop script's background volume shell does not count.
if [ "$audio_stopped" = 1 ]; then
    waited=0
    while [ "$waited" -lt 10 ] && pgrep audioserver > /dev/null 2>&1; do
        sleep 0.2
        waited=$((waited + 1))
    done
    sleep 0.2
    if [ -e /proc/mi_modules/mi_ao/mi_ao0 ]; then MI_AO_NODE=present; else MI_AO_NODE=gone; fi
    echo "audio handover   : waited ${waited}x0.2s, mi_ao node $MI_AO_NODE"
fi

# MainUI's LD_PRELOAD routes audio into the server just stopped.
unset LD_PRELOAD

# A card unzipped on Windows loses +x ("Permission denied"). Harmless where the mount grants it.
chmod +x "$mydir/pockettracker" 2>/dev/null

./pockettracker
rc=$?
echo "exit code        : $rc"

# A failed start is otherwise a flash of black and the menu again: Onion's message panel points at the log.
# ⚠️ Its own LD_LIBRARY_PATH: the panel must not load our SDL2. The exit code stays the app's.
INFO_PANEL=/mnt/SDCARD/.tmp_update/bin/infoPanel
if [ "$rc" != 0 ] && [ -x "$INFO_PANEL" ]; then
    LD_LIBRARY_PATH="/mnt/SDCARD/miyoo/lib:/config/lib:/lib" "$INFO_PANEL" \
        --title "PocketTracker" \
        --message "PocketTracker could not start (exit $rc). The reason is in log.txt, in the port's own folder: Roms/PORTS/Games/PocketTracker on the card." \
        > /dev/null 2>&1
fi

exit $rc
