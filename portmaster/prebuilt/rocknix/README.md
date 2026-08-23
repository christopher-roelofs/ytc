# RockNIX / Adreno-class prebuilt libmpv

`libmpv.so.2` here is a **render-API-only** libmpv (mpv 0.36) for CFWs/devices that do
NOT ship a usable libmpv (e.g. RockNIX on Snapdragon/Adreno — it has the `mpv` CLI and
ffmpeg 6.x, but no `libmpv.so`).

## Why it's separate from `portmaster/port/libs.aarch64/`
The port's launch script prepends `libs.aarch64` to `LD_LIBRARY_PATH`. This libmpv links
**ffmpeg 6.x** (libavcodec.so.60 …). muOS ships **ffmpeg 4.x** (libavcodec.so.58), so
dropping this into the shared port would shadow muOS's working system libmpv and break
it. Keep the shared `libs.aarch64` empty for muOS; deploy this file only to ffmpeg-6
targets (RockNIX/Adreno).

## What it needs (satisfied by RockNIX's own system libs)
`libass.so.9`, ffmpeg 6.x (`libav*`/`libsw*` .so.60/58/7/4/9), `libasound.so.2`,
plus the usual `libc/m/z/dl/pthread`. NO libmali/libgbm/libdrm/libEGL/libjpeg (VO
backends were disabled), so it's GPU-vendor agnostic — it renders through the app's own
SDL GLES2 context via the mpv render API.

## Build recipe (on the Orange Pi 5 build host, ~/mpvbuild/mpv)
Reconfigure the existing mpv 0.36 build render-only, then rebuild:
```
meson configure build -Ddrm=disabled -Dgbm=disabled -Degl=disabled \
                      -Djpeg=disabled -Dvulkan=disabled -Dvaapi=disabled
ninja -C build
# result: build/libmpv.so.2.1.0  (verify: readelf -d has NO libdrm/gbm/EGL/jpeg/mali)
```

## Deploy to a RockNIX-class device
Copy the port to `/roms/ports/ytc/` + `/roms/ports/ytc.sh`, and place this file at
`/roms/ports/ytc/libs.aarch64/libmpv.so.2`. The launch script's
`LD_LIBRARY_PATH=$GAMEDIR/libs.aarch64` then loads it; its ffmpeg/libass/alsa resolve
from the device's `/usr/lib`. Verified 2026-08-23 on a Retroid Pocket (SD865) at
runtime: app + libmpv load, controller detected, no missing libs.

## Not yet done: fully self-contained ffmpeg bundle
The Pi's own ffmpeg 6.1 is unsuitable to bundle as-is (its libavcodec hard-needs
`librockchip_mpp` and libavdevice drags in X11/DRM/xcb). A clean, no-rkmpp, no-X11
ffmpeg build would be required to make the media stack fully self-contained instead of
relying on the device's ffmpeg.
