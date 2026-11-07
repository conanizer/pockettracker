#!/bin/bash
#
# PocketTracker - PortMaster launch script.
#
# ⚠️ DO NOT ADD gptokeyb. The app reads the pad itself (and the keyboard), so gptokeyb's fake key
# presses arrive beside the real ones and mean other buttons: START also becomes A (inserts a chain,
# and playback will not stop), Y walks the cursor left, R1 holds SELECT, the stick moves the cursor.
# It has no mode that injects nothing.
#
# Quitting is the app's own: PROJECT -> EXIT, and a SIGTERM from the launcher autosaves.

XDG_DATA_HOME=${XDG_DATA_HOME:-$HOME/.local/share}

if [ -d "/opt/system/Tools/PortMaster/" ]; then
  controlfolder="/opt/system/Tools/PortMaster"
elif [ -d "/opt/tools/PortMaster/" ]; then
  controlfolder="/opt/tools/PortMaster"
elif [ -d "$XDG_DATA_HOME/PortMaster/" ]; then
  controlfolder="$XDG_DATA_HOME/PortMaster"
else
  controlfolder="/roms/ports/PortMaster"
fi

source $controlfolder/control.txt

[ -f "${controlfolder}/mod_${CFW_NAME}.txt" ] && source "${controlfolder}/mod_${CFW_NAME}.txt"

get_controls

GAMEDIR=/$directory/ports/pockettracker

cd "$GAMEDIR"

> "$GAMEDIR/log.txt" && exec > >(tee "$GAMEDIR/log.txt") 2>&1

# The app's folders, created at boot under $GAMEDIR so the card can be filled from a PC.
export POCKETTRACKER_HOME="$GAMEDIR/data"

# The pad's mapping comes from SDL_GAMECONTROLLERCONFIG_FILE, which control.txt exports. This inline
# one is empty on some CFWs (spruce) and harmless there; others fill it, and the porting guide asks for it.
export SDL_GAMECONTROLLERCONFIG="$sdl_controllerconfig"

# No LD_LIBRARY_PATH and no libs.aarch64: the port runs on the device's own SDL2 (2.0.18 or newer),
# patched for its display and audio.

# An unzip on Windows drops the binary's +x, and the port then dies with "Permission denied".
# Fails harmlessly on a vfat/exFAT card, which has no permission bits.
chmod +x "$GAMEDIR/pockettracker.${DEVICE_ARCH}" 2>/dev/null || true

pm_platform_helper "$GAMEDIR/pockettracker.${DEVICE_ARCH}"

./pockettracker.${DEVICE_ARCH}

pm_finish
