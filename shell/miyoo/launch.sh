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

# ── The demo song, seeded ONCE ────────────────────────────────────────────────────────────────────
# Optional: `demo/` is only in a package built from a tree that had one, so the first branch below is
# the normal public build rather than an error.
#
# ⚠️⚠️ GUARDED BY A MARKER, NOT BY "IS THE PROJECT THERE" — and the difference is the user's own
# work. Seeding on a missing file would restore a demo they deleted on purpose, and worse, would
# overwrite the copy of it they had spent an evening editing. The marker means the seed happens on a
# fresh card and never again; wipe $PT_HOME and it seeds afresh, which is the recovery.
#
# ⚠️ `cp -r <dir> "$PT_HOME/"` per top-level folder, NOT a glob over the files: the sample names
# contain spaces, and an unquoted glob hands `cp` "808" and "Bass.wav" as two arguments. Copying the
# directories merges them into the ones the app makes, and carries any sub-folder with it.
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
        # Deliberately NOT marked seeded on a failure, so the next launch tries again. A part-copied
        # demo is the one state worth retrying: a full card fails here and an emptied one recovers.
        echo "demo song        : SEED FAILED - the card may be full or read-only"
    fi
fi

# ── The audio device, taken back from Onion ───────────────────────────────────────────────────────
# ⚠️⚠️ WITHOUT THIS THE APP EXITS DURING START-UP ON A REAL DEVICE, and it looks like a launcher
# problem rather than an audio one: Onion runs a resident `audioserver` that holds the SigmaStar AO
# device open and ENABLED, the chip will not let a second owner set the output attributes while it
# is, and so MI_AO_SetPubAttr answers 0xa0052009 — module AO, "operation not permitted".
# SDL_OpenAudioDevice then fails and shell/main.cpp treats no audio as fatal, which is the whole of
# the "starts and closes again" report.
#
# This is Onion's own `KillAudioserver` flag from its ports collection, and the script below is what
# implements it there: it stops the server AND re-applies the user's volume to whichever process
# claims the device next — which is us. Prefer it over killing the process ourselves for exactly
# that second half.
#
# ⚠️ NOTHING HERE PUTS THE SERVER BACK. Onion restarts it on the way to the menu (`start_audioserver`
# in .tmp_update/runtime.sh), and a restart from here would take the device away from the app we are
# about to hand it to. The volume KEYS are unaffected either way — Onion sets the level by ioctl
# straight on /dev/mi_ao, which is below the server; what the server holds is the level itself, and
# Onion's script is what carries that across to whoever takes the device next.
#
# ⚠️ `miyoodir` IS DEFAULTED FIRST, and it is not cosmetic. One branch of Onion's script restarts
# `wpa_supplicant` and `udhcpc` from `$miyoodir/app/…` when it finds `libpadsp.so` preloaded into
# them — with the variable unset it would kill the two and fail to start them again, and the
# tester loses Wi-Fi until a reboot. Onion exports it from init_env.sh, so this only ever fires
# where the environment did not reach us.
: "${miyoodir:=/mnt/SDCARD/miyoo}"
export miyoodir
ONION_STOP_AUDIO=/mnt/SDCARD/.tmp_update/script/stop_audioserver.sh
audio_stopped=0
if [ -f "$ONION_STOP_AUDIO" ]; then
    sh "$ONION_STOP_AUDIO"
    audio_stopped=1
    echo "audioserver      : stopped via Onion's stop_audioserver.sh"
elif pgrep audioserver > /dev/null 2>&1; then
    # No such script: not Onion, or an Onion older than it. The kill is the half that unblocks us;
    # the volume the server was holding is NOT re-applied, so a quiet app on such a firmware is a
    # known consequence rather than a mystery.
    killall -q -9 audioserver audioserver.mod 2> /dev/null
    audio_stopped=1
    echo "audioserver      : killed directly (this firmware has no stop_audioserver.sh)"
else
    echo "audioserver      : not running"
fi

# ⚠️ THE RELEASE IS ASYNCHRONOUS. The driver only tears the AO device down when the dead process's
# handle is closed, and asking a fraction of a second too early fails exactly as it did before, with
# nothing on screen to say why. Bounded: a firmware that never releases it costs two seconds here
# rather than hanging. `pgrep audioserver` matches the process NAME, so it does not see the shell
# that stop_audioserver.sh leaves in the background re-applying the volume.
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

# ⚠️ MainUI hands LD_PRELOAD=libpadsp.so down to what it launches — Miyoo's shim that routes an app's
# audio calls into the server we have just stopped. Left set, it is a shim in front of nothing. The
# app talks to the device itself, which is the point of the handover above.
unset LD_PRELOAD

# A card unzipped on Windows arrives with no Unix mode bits, and the app then dies "Permission
# denied" on a file that is plainly there. Harmless where the mount hands out +x anyway.
chmod +x "$mydir/pockettracker" 2>/dev/null

./pockettracker
rc=$?
echo "exit code        : $rc"

# ⚠️ A START-UP FAILURE ON THIS DEVICE IS A FLASH OF BLACK AND THE SHELF AGAIN — there is no console
# and no window left to say anything in, and that is exactly what the first field report looked like
# from the user's side. Onion's own message panel is the one screen still reachable from here, so a
# failure names itself and points at the log instead of looking like the app "just closes".
#
# ⚠️ Its OWN LD_LIBRARY_PATH: the panel is a system binary and must not load the SDL2 we ship beside
# this script. Guarded on the binary existing, output discarded, and the exit code stays the app's
# either way — a firmware without the panel fails exactly as it did before.
INFO_PANEL=/mnt/SDCARD/.tmp_update/bin/infoPanel
if [ "$rc" != 0 ] && [ -x "$INFO_PANEL" ]; then
    LD_LIBRARY_PATH="/mnt/SDCARD/miyoo/lib:/config/lib:/lib" "$INFO_PANEL" \
        --title "PocketTracker" \
        --message "PocketTracker could not start (exit $rc). The reason is in App/PocketTracker/log.txt on the card." \
        > /dev/null 2>&1
fi

exit $rc
