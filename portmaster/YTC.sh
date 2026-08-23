#!/bin/bash
# PortMaster launch script for ytc.
# Uses the CFW's SDL2, GLES2 and libmpv (SONAME libmpv.so.2, mpv 0.35+);
# curl/TLS and the rest are static in the binary. Needs network.

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

GAMEDIR="/$directory/ports/ytc"
cd "$GAMEDIR"

> "$GAMEDIR/log.txt" && exec > >(tee "$GAMEDIR/log.txt") 2>&1

export LD_LIBRARY_PATH="$GAMEDIR/libs.${DEVICE_ARCH}:$LD_LIBRARY_PATH"
export SDL_GAMECONTROLLERCONFIG="$sdl_controllerconfig"

# Handheld-class defaults: cap the ladder at 480p (software decode on
# small ARM cores; the panel is 480p anyway). Font ships in data/.
export YTC_MAXHEIGHT=480

$GPTOKEYB "ytc.${DEVICE_ARCH}" &
pm_platform_helper "$GAMEDIR/ytc.${DEVICE_ARCH}"

./ytc.${DEVICE_ARCH}

pm_finish
