#!/bin/bash
#
# PocketTracker - PortMaster launch script.
#
# =============================================================================================
#  DO NOT ADD gptokeyb TO THIS FILE.
# =============================================================================================
#
# It looks like an omission. Every other port has it. It is deliberate, and adding it back
# reintroduces a bug that took a device session to find.
#
# gptokeyb exists to fake KEYBOARD presses for games that cannot read a gamepad. PocketTracker
# reads the pad itself, through SDL's GameController API (linux/sdl-input.cpp), and it ALSO reads
# the keyboard, because the same source builds the desktop dev binary. So with gptokeyb running,
# every physical press arrives TWICE by two different paths - and gptokeyb's built-in defaults do
# not agree with the app's keyboard map about what the buttons mean:
#
#     gptokeyb default   ->  sdl-input.cpp key_to_button  ->  what the user gets
#     start = enter          SDLK_RETURN  -> Button::A        START also inserts a chain, AND
#                                                             (A now counting as held) the !m.a
#                                                             guard in main.cpp swallows START,
#                                                             so playback will not stop
#     y = a                  SDLK_a       -> DPAD_LEFT        Y also walks the cursor left
#     r1 = leftshift         SDLK_LSHIFT  -> SELECT           R1 also holds SELECT, and R+DPAD is
#                                                             the nav-grid gesture
#     left_analog = wasd     w/a/s/d      -> the D-pad        a drifting stick moves the cursor
#
# The D-pad collision is invisible (SdlInput::press ignores a button already held, so the pad's
# own DPAD_UP absorbs the injected Up), which is why only the START symptom was ever reported.
# There is no gptokeyb mode that injects nothing - PortMaster's own docs confirm it - so the only
# fix is not to run it.
#
# Quitting is the app's own job and always was: SETTINGS -> EXIT (it asks first if the song is
# dirty), and a SIGTERM from the launcher is caught and autosaved either way (Phase 3 S10).

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

# The app's six folders (Projects/ Samples/ Soundfonts/ Instruments/ Renders/ Themes/) are created
# under this root at boot rather than lazily, so a user can pull the SD card, put samples in, and
# put it back. Keeping it under $GAMEDIR is what makes that reachable from a PC.
export POCKETTRACKER_HOME="$GAMEDIR/data"

# The shell opens SDL GameControllers. Without the CFW's mapping SDL sees an unrecognised joystick,
# SDL_IsGameController() is false, no controller is opened and NOTHING responds. get_controls sets
# this variable; exporting it is what makes the pad work.
export SDL_GAMECONTROLLERCONFIG="$sdl_controllerconfig"

# No LD_LIBRARY_PATH and no libs.aarch64: this port deliberately links the DEVICE's libSDL2, which
# is the copy patched for its display (KMSDRM) and audio (ALSA). Shipping our own would override
# the one that knows the hardware. The binary needs SDL >= 2.0.18 (it calls SDL_GetTicks64); every
# current Tier-1 CFW is well past that.

pm_platform_helper "$GAMEDIR/pockettracker.${DEVICE_ARCH}"

./pockettracker.${DEVICE_ARCH}

pm_finish
