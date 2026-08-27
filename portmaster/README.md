# PortMaster port

ytc running as a PortMaster-style port on muOS handhelds. First verified
2026-08-22 on an Anbernic RG28XX (muOS 2601.0 JACARANDA, Allwinner H700,
640x480): search, thumbnails, GLES2 grid, and full mpv playback.

2026-08-23: full feature build re-deployed + confirmed working on the RG28XX
(device libmpv, NEEDED = SDL2/GLESv2/libmpv.so.2/pthread/m/c). Carries channel &
Home tabs, Community posts, playlists, description overlays, Unicode text,
hold-to-seek, restricted/Shorts filters, paced-stream seek fixes.

## Layout

- `ytc.sh` — the launch script (portmaster.games/packaging.html layout).
  Installs to the CFW's ports scripts folder (muOS: `roms/Ports/`).
- `ytc/` — the assembled game directory, installed as `ports/ytc/`:
  - `ytc.aarch64` — stripped `yt_ui`
  - `config/clients.json` — Innertube client fingerprints
  - `data/` — gamecontrollerdb + bundled DejaVuSans.ttf (handhelds don't
    ship the Debian font path; `ui.cpp` falls back to `data/`)
  - `libs.aarch64/` — bundled render-only libmpv + FFmpeg (LD_LIBRARY_PATH)
  - `port.json`, `gameinfo.xml`, `screenshot.jpg` — PortMaster store metadata
  - `LICENSE`, `THIRD_PARTY_NOTICES.md` — shipped with the port

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

- muOS layout: script -> `/mnt/sdcard/roms/Ports/ytc.sh`, game dir ->
  `/mnt/sdcard/ports/ytc/`. `control.txt` derives `$directory` from the
  script path.
- The launcher exports `YTC_MAXHEIGHT=480`: 480p panel + software decode
  on 4xA53 (no hwdec on these devices; the custom mpv+ffmpeg build in
  PROGRESS.md's TODO is only worth it for hwdec or CFWs without libmpv).
- Built-in pad matches the bundled gamecontrollerdb
  ("Anbernic RG28XX Controller"); gptokeyb is exit-hotkey only.
- muOS SDL2 has NO dummy video/audio drivers — remote smoke tests must run
  the real kmsdrm path (works even while the muOS frontend is up, but the
  frontend bleeds overlays into the screen; only Ports-menu launches are
  clean). Verify remotely by `dd`-ing `/dev/fb0`.

## Bundling the custom rkmpp mpv — decision + findings (2026-08-23)

DECISION: do NOT bundle the custom mpv on Allwinner (H700) devices — they keep
using the device's own libmpv (current port, works). The custom rkmpp build is
reserved for RK3588/RK3566 targets, where it actually gives hwdec.

Why (measured on the RG28XX/H700 at 192.168.86.244):
- H700 has NO Rockchip VPU -> rkmpp = zero benefit (software fallback either way).
- The RK3588-built libmpv can't be dropped on it as-is: NEEDED libgbm.so.1 is
  ABSENT (and the Pi's copy is a Rockchip-Mali blob = wrong GPU vendor), and it
  NEEDs libjpeg.so.62 (device has only .8/.9). Those come from mpv's DRM/GBM VO
  backends, which we DON'T use (we drive mpv via the render API into SDL's GL ctx).
- Device already provides libmpv.so.2 (mpv 0.35.1), libEGL/GLESv2 -> libmali.so,
  libass.so.9, ffmpeg 4.x (avcodec.so.58 etc.).

When we DO bundle for RK3588 (the portable path):
1. Rebuild libmpv on the Pi RENDER-API-ONLY: disable the gpu-context/VO backends
   (drm, wayland, x11, gbm, egl) so libmpv drops libgbm/libEGL/libdrm NEEDEDs and
   only needs the ffmpeg chain + libass. `meson ... -Dgpl=true -Dlibmpv=true
   -Dvideo-output-drivers=[] ` (keep only what render API needs) — verify with
   `readelf -d libmpv.so.2 | grep NEEDED` afterward (should be just av*/ass/sys).
2. Bundle into ytc/libs.aarch64: libmpv.so.2 + ffmpeg 6.1 set (libavcodec.so.60,
   libavformat.so.60, libavutil.so.58, libswscale.so.7, libswresample.so.4,
   libavfilter.so.9, libavdevice.so.60) + librockchip_mpp.so.1 (HARD NEEDED of our
   libavcodec; loads + sw-falls-back on non-Rockchip) + libass.so.9 + libjpeg.so.62
   (bundle any the target CFW lacks; check per device).
   Source paths on the Pi (192.168.86.243): libmpv at
   ~/mpvbuild/prefix/lib/aarch64-linux-gnu/, ffmpeg 6.1 = the .so.60/.58/.4/.7 set
   in ~/mpvbuild/prefix/lib/ (NOT the .so.61/.59/.5/.8 ffmpeg-7 set that's also there),
   librockchip_mpp at /usr/lib/aarch64-linux-gnu/.
3. Link ytc against the custom mpv headers/pc (already in prefix), set
   YTC_HWDEC=rkmpp (or auto) in the RK3588 launcher variant, drop the 480p cap.

## License

YTC's own code is licensed under the **PolyForm Noncommercial License 1.0.0** —
free to use, modify, and share for any noncommercial purpose; selling it or
bundling it into a commercial product/service requires a separate license from
the copyright holder. See [`../LICENSE`](../LICENSE).

Bundled third-party components (libmpv and FFmpeg under LGPL-2.1-or-later,
DejaVu Sans, SDL_GameControllerDB, nlohmann/json, stb) keep their own licenses
and are **not** covered by YTC's noncommercial terms. See
[`../THIRD_PARTY_NOTICES.md`](../THIRD_PARTY_NOTICES.md). Both files also ship
inside the port (`ytc/LICENSE`, `ytc/THIRD_PARTY_NOTICES.md`) so they travel
with the distributed zip, as the LGPL and PolyForm notice terms require.

## Still to do for a real PortMaster submission

- ~~port.json, screenshot, licenses folder, README for the store.~~ Done:
  `ytc/port.json`, `ytc/gameinfo.xml`, `ytc/screenshot.jpg`, and shipped
  `LICENSE`/`THIRD_PARTY_NOTICES.md`.
- Test matrix beyond muOS (ArkOS, ROCKNIX, AmberELEC — libmpv presence/SONAME
  needs checking per CFW; the render-only custom mpv above is the fallback for
  CFWs that ship no libmpv).
- armhf decision (skip: aarch64-only is accepted).
