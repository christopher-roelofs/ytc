#!/bin/bash
# PortMaster launch script for YTC.
# Uses the CFW's SDL2 + GLES2/EGL (device GPU driver) and libass/ALSA. libmpv +
# ffmpeg 6.1 are BUNDLED in libs.${DEVICE_ARCH} (render-only libmpv, software
# decode, GPU-agnostic) so playback works on CFWs that ship no libmpv (RockNIX)
# and avoids ffmpeg-version mismatches. curl/TLS static in the binary. Needs network.

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

# Optional hardware decode: offer the lib download once per manifest version
# (ytc_setup detects the decoder, asks Yes / Not now / Never, writes
# ./video_decode). "Never" and unsupported devices skip forever; "Not now"
# asks again next launch; a bumped data/hwdec_version re-offers to everyone
# else. Installed libs go FIRST on the path so the v4l2 ffmpeg wins.
VDFILE="$GAMEDIR/video_decode"
WANTVER=$(cat "$GAMEDIR/data/hwdec_version" 2>/dev/null)
if [ -x "$GAMEDIR/ytc_setup.${DEVICE_ARCH}" ] && [ -n "$WANTVER" ]; then
  if ! grep -q "^choice=never" "$VDFILE" 2>/dev/null && \
     ! grep -q "^manifest=$WANTVER" "$VDFILE" 2>/dev/null && \
     ! grep -q "^manifest=unsupported" "$VDFILE" 2>/dev/null; then
    "$GAMEDIR/ytc_setup.${DEVICE_ARCH}"
  fi
fi
if grep -q "^choice=installed" "$VDFILE" 2>/dev/null && [ -d "$GAMEDIR/libs.hwdec" ]; then
  export LD_LIBRARY_PATH="$GAMEDIR/libs.hwdec:$LD_LIBRARY_PATH"
fi

# Handheld-class defaults: cap the ladder at 480p (software decode on
# small ARM cores; the panel is 480p anyway). Font ships in data/.
export YTC_MAXHEIGHT=480

$GPTOKEYB "ytc.${DEVICE_ARCH}" &
pm_platform_helper "$GAMEDIR/ytc.${DEVICE_ARCH}"

./ytc.${DEVICE_ARCH}

pm_finish
