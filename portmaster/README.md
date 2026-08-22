# PortMaster port

ytnative running as a PortMaster-style port on muOS handhelds. First verified
2026-08-22 on an Anbernic RG28XX (muOS 2601.0 JACARANDA, Allwinner H700,
640x480): search, thumbnails, GLES2 grid, and full mpv playback.

## Layout

- `ytnative.sh` — the launch script (portmaster.games/packaging.html layout).
  Installs to the CFW's ports scripts folder (muOS: `roms/Ports/`).
- `port/` — the assembled game directory, installed as `ports/ytnative/`:
  - `ytnative.aarch64` — stripped `yt_ui`
  - `config/clients.json` — Innertube client fingerprints
  - `data/` — gamecontrollerdb + bundled DejaVuSans.ttf (handhelds don't
    ship the Debian font path; `ui.cpp` falls back to `data/`)
  - `libs.aarch64/` — empty; kept as the standard LD_LIBRARY_PATH hook

## Build recipe (Orange Pi 5 build host, 192.168.86.243)

The goal is a binary whose only dynamic deps exist on the device:

```
NEEDED: libSDL2-2.0.so.0  libGLESv2.so.2  libmpv.so.2  libpthread/m/c
```

Everything else is linked statically from `/opt/pkhome-sysroot` (shared with
the other ports; built from source with `-O2 -fPIC`):

- **curl 8.5 + mbedtls 3.5.2, static** — Innertube is HTTPS, so unlike the
  other ports curl carries TLS. CA path baked at configure time:
  `--with-mbedtls=$P --with-ca-bundle=/etc/ssl/certs/ca-certificates.crt`
  (that exact path exists on both Debian and muOS; `http.cpp` sets no
  CAINFO and relies on the default).
- zlib static.

**libmpv is the device's own.** SONAMEs differ: the Pi's distro libmpv is
`.so.1` (mpv 0.32 — outdated, no hwdec, the reason a custom build was ever
considered), the device ships `.so.2` (mpv 0.35.1). The port links against a
copy of the device's library: `/opt/device-libs/lib/libmpv.so.2.0.0` (scp'd
off the handheld) + `libmpv.so`/`.so.2` symlinks + mpv 0.35.1 release headers
+ a hand-written `mpv.pc` (Version: 2.1.0). Link with
`-Wl,--allow-shlib-undefined` (the device lib's own deps aren't on the Pi)
and make sure `-L/opt/device-libs/lib` comes FIRST or the system `.so.1`
wins the `-lmpv` search — check `readelf -d` NEEDED after every relink.

Configure:

```
PKG_CONFIG_PATH=/opt/device-libs/lib/pkgconfig:/opt/pkhome-sysroot/lib/pkgconfig \
cmake -S . -B build-port -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_EXE_LINKER_FLAGS="-L/opt/device-libs/lib -L/opt/pkhome-sysroot/lib \
      -Wl,--allow-shlib-undefined -static-libstdc++ -static-libgcc" \
  -DCMAKE_CXX_STANDARD_LIBRARIES="<sysroot>/libmbedtls.a <sysroot>/libmbedx509.a \
      <sysroot>/libmbedcrypto.a <sysroot>/libz.a -lpthread" \
  -DBUILD_PLAYER=OFF
```

(Absolute .a paths because `CMAKE_CXX_STANDARD_LIBRARIES` flags don't reliably
see the -L dirs; `BUILD_PLAYER=OFF` skips the yt_play spike.)

## Device notes (RG28XX / muOS)

- muOS layout: script -> `/mnt/sdcard/roms/Ports/ytnative.sh`, game dir ->
  `/mnt/sdcard/ports/ytnative/`. `control.txt` derives `$directory` from the
  script path.
- The launcher exports `YTNATIVE_MAXHEIGHT=480`: 480p panel + software decode
  on 4xA53 (no hwdec on these devices; the custom mpv+ffmpeg build in
  PROGRESS.md's TODO is only worth it for hwdec or CFWs without libmpv).
- Built-in pad matches the bundled gamecontrollerdb
  ("Anbernic RG28XX Controller"); gptokeyb is exit-hotkey only.
- muOS SDL2 has NO dummy video/audio drivers — remote smoke tests must run
  the real kmsdrm path (works even while the muOS frontend is up, but the
  frontend bleeds overlays into the screen; only Ports-menu launches are
  clean). Verify remotely by `dd`-ing `/dev/fb0`.

## Still to do for a real PortMaster submission

- port.json, screenshot, licenses folder, README for the store.
- Test matrix beyond muOS (ArkOS, ROCKNIX, AmberELEC — libmpv presence/SONAME
  needs checking per CFW; bundling our own mpv+ffmpeg is the fallback).
- armhf decision (skip: aarch64-only is accepted).
