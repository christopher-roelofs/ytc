# Building our own mpv + ffmpeg (with rkmpp hwdec)

Per docs/BUNDLING_POLICY.md we ship our own media stack: portability across CFWs
(no dependence on the device's libmpv) + hardware decode + version control.

## Result (verified 2026-08-22, Orange Pi 5 / RK3588)

libmpv **2.1.0** (mpv 0.36.0) on ffmpeg **6.1** with rkmpp. Hardware decode ON:

| content            | stock device libmpv | our build (hwdec=rkmpp) |
|--------------------|---------------------|--------------------------|
| 1080p60 H264       | software, hwdec=no  | rkmpp, 60fps, ~9 drops   |
| 4K60 VP9           | software, 57 drops  | rkmpp, 60fps, **1 drop** |

`readelf -d libavcodec.so.60` shows `librockchip_mpp.so.1`; mpv reports
`hwdec=rkmpp`. h264/hevc/vp9 rkmpp decoders enabled.

## Build host

Orange Pi 5 (native aarch64, Debian 11 / glibc 2.31 — matches the muOS floor the
port already ships against). `192.168.86.243`. rockchip_mpp 1.3.8 + pkgconfig,
libdrm 2.4.104, /dev/mpp_service present (so hwdec is testable here).

Prereqs: `pip3 install --user "meson>=1.1"` (Debian 11 meson is too old); ninja,
gcc/g++ present; `apt install libass-dev` (mpv hard dep). No nasm needed (aarch64).

## Recipe

Prefix: `~/mpvbuild/prefix` (ffmpeg -> `lib/`, libmpv -> `lib/aarch64-linux-gnu/`).

### ffmpeg 6.1  (NOT 7.x — mpv 0.36 uses AV_OPT_TYPE_CHANNEL_LAYOUT, removed in 7.0)
```
git clone --depth 1 --branch n6.1 https://github.com/FFmpeg/FFmpeg ffmpeg
cd ffmpeg && ./configure --prefix=$PFX \
  --enable-shared --disable-static \
  --disable-programs --disable-doc --disable-debug \
  --enable-version3 --enable-rkmpp --enable-libdrm --enable-network
make -j8 && make install
```
(`--enable-rkmpp` needs `--enable-version3`; gives LGPLv3. Confirm
`CONFIG_H264_RKMPP_DECODER=yes` in ffbuild/config.mak.)

### mpv 0.36.0  (meson; 0.37+ makes libplacebo mandatory — avoid for now)
```
export PATH=$HOME/.local/bin:$PATH
export PKG_CONFIG_PATH=$PFX/lib/aarch64-linux-gnu/pkgconfig:$PFX/lib/pkgconfig:/usr/lib/aarch64-linux-gnu/pkgconfig
git clone --depth 1 --branch v0.36.0 https://github.com/mpv-player/mpv mpv
cd mpv && meson setup build --prefix=$PFX \
  -Dlibmpv=true -Dcplayer=false -Dgpl=false \
  -Dlua=disabled -Djavascript=disabled -Dlcms2=disabled -Dlibplacebo=disabled \
  -Dx11=disabled -Dwayland=disabled \
  -Degl=enabled -Dgbm=enabled -Ddrm=enabled -Dplain-gl=enabled \
  -Dalsa=enabled -Dpulse=disabled -Dsdl2=disabled
ninja -C build && ninja -C build install
```
`plain-gl=enabled` is the render-API GL backend (no windowing system needed — we
supply the GL context via SDL). `libmpv: YES, opengl: YES, libplacebo: disabled`.

## libmpv.so.2 dependency set (for bundling)

Bundle (ours / not-reliably-present / must-control):
- our ffmpeg: libavcodec.so.60 avformat.60 avutil.58 avfilter.9 swscale.7
  swresample.4 avdevice.60
- libmpv.so.2, **librockchip_mpp.so.1** (HARD NEEDED of our libavcodec — must ship it
  or libmpv won't load on non-Rockchip devices; it loads fine on Allwinner and just
  fails rkmpp init -> software fallback), libass.so.9 (+ its chain: freetype/fribidi/
  fontconfig/harfbuzz/expat/png — TODO enumerate & bundle).

Device-provided (GPU/system layer, don't bundle): libEGL, libgbm, libdrm, libasound,
libjpeg, libz, libc/m/dl/pthread.

## TODO to wire into the port

- [ ] Copy the bundle set into `portmaster/port/libs.aarch64/`; relink `yt_ui` for the
      port against this libmpv (PKG_CONFIG_PATH to the prefix).
- [ ] hwdec selection: `rkmpp` on Rockchip, software elsewhere. Simplest: launcher
      sets YTC_HWDEC per device (or app auto-detects /dev/mpp_service). Allwinner
      (RG28XX) stays software (librockchip_mpp loads, decoders fail -> sw fallback).
- [ ] Verify on a real Rockchip handheld (RK3566/3588) and re-verify RG28XX still runs
      (software) with the bundled stack.
- [ ] Enumerate + bundle the libass chain; ldd the final yt_ui to confirm nothing
      outside the device-provided set is unresolved.

## Portable render-only bundle — all devices (2026-08-23)

The port now BUNDLES a matched libmpv + ffmpeg in `portmaster/port/libs.aarch64/`,
loaded app-only via the launch script's `LD_LIBRARY_PATH` (system untouched). This
replaces relying on the device's libmpv — which failed on CFWs that ship none
(RockNIX/Adreno) and hit ffmpeg-version-mismatch aborts (`libavutil 58.29 -> 58.2`)
when relying on a device's older ffmpeg minor.

Design rules:
- Bundle ONLY the media stack: `libmpv.so.2` + ffmpeg 6.1 (`libavcodec.so.60`,
  `libavformat.so.60`, `libavutil.so.58`, `libswscale.so.7`, `libswresample.so.4`,
  `libavfilter.so.9`), named by SONAME. ~20 MB.
- NEVER bundle GPU/windowing libs (libEGL/GLESv2/mali/gbm/drm) — device-specific;
  the render-only libmpv needs none (renders via the app's SDL GL context).
- Rely on the device for universal libs: libass.so.9, libasound.so.2 (ALSA),
  libc/m/z/dl/pthread. (Both muOS and RockNIX ship libass; if a target lacks it the
  app won't launch — bundle the libass tree then.)
- Build on the OLDEST-glibc host (Orange Pi 5, Debian 11 / glibc 2.31) for forward
  compatibility with newer devices (RockNIX glibc 2.40).
- Tradeoff: plain ffmpeg = SOFTWARE decode everywhere (fine at 480p/720p panels).
  Hardware decode (RK3588 rkmpp) would be a separate device-specific variant.

### Clean ffmpeg (no rkmpp / no X11 / no lzma-bz2), in ~/mpvbuild/ffmpeg (n6.1):
```
./configure --prefix=$HOME/mpvbuild/prefix-clean --enable-shared --disable-static \
  --disable-programs --disable-doc --disable-avdevice \
  --disable-rkmpp --disable-vaapi --disable-vdpau \
  --disable-lzma --disable-bzlib --enable-v4l2-m2m
# v4l2-m2m has NO external deps (kernel headers only) and is what drives
# stateful v4l2 decoders (Qualcomm Venus/Iris, Amlogic vdec) via mpv
# hwdec=v4l2m2m-copy — so it ships in the base bundle since 2026-09-01.
# Devices without a stateful decoder just fail to open it and fall back.
make -j8 && make install
# verify: readelf -d prefix-clean/lib/libavcodec.so.60 | grep NEEDED  (no rkmpp/drm/lzma)
```

### ZERO-COPY variant (2026-09-04) — EXPERIMENT, NOT SHIPPED (crashes Venus; see HWDEC_SURVEY.md)
ffmpeg as above but ADD `--enable-libdrm` (needed for v4l2_m2m to export
DRM_PRIME dmabufs; adds libdrm.so.2 to libavutil's NEEDED). libmpv:
```
export PATH=$HOME/.local/bin:$PATH        # pip meson
PKG_CONFIG_PATH=$HOME/mpvbuild/prefix-v4l2/lib/pkgconfig meson setup build-zc \
  -Dlibmpv=true -Dcplayer=false -Degl=enabled -Ddrm=enabled -Dplain-gl=enabled \
  -Dgbm=disabled -Djpeg=disabled -Dvulkan=disabled -Dvaapi=disabled \
  -Dlibavdevice=disabled -Dlua=disabled -Dpulse=disabled -Dcaca=disabled \
  -Djack=disabled -Dopenal=disabled -Dsndio=disabled -Doss-audio=disabled \
  -Dwayland=disabled -Dx11=disabled --buildtype=release
ninja -C build-zc
# features must include: dmabuf-interop-gl drm egl   (mpv's drmprime interop)
# NEEDED gains libdrm.so.2 + libEGL.so.1 — both present on muOS (libmali EGL)
# and ROCKNIX (mesa); still NOT bundled (GPU-side libs stay the device's).
```
Plus LibreELEC's `v4l2-drmprime` patch on ffmpeg 6.0.1 (mainline v4l2_m2m has
no DRM_PRIME output). Result on the RP5: Venus firmware exception / in-kernel
hang -> device reboot. The shipped bundle stays the mainline 6.1 v4l2m2m-copy
set with the render-only libmpv; the app requests `hwdec=v4l2m2m-copy` only.

### Render-only libmpv against that ffmpeg, in ~/mpvbuild/mpv (0.36) — the PRE-zero-copy recipe:
```
PKG_CONFIG_PATH=$HOME/mpvbuild/prefix-clean/lib/pkgconfig meson setup build-clean \
  -Dlibmpv=true -Dcplayer=false \
  -Ddrm=disabled -Dgbm=disabled -Degl=disabled -Djpeg=disabled \
  -Dvulkan=disabled -Dvaapi=disabled -Dlibavdevice=disabled \
  -Dlua=disabled -Dpulse=disabled -Dcaca=disabled -Djack=disabled \
  -Dopenal=disabled -Dsndio=disabled -Doss-audio=disabled --buildtype=release
ninja -C build-clean
# verify NEEDED = libass + libav*/libsw* + libasound + libdl/m/z/pthread/c ONLY
# (no drm/gbm/egl/mali/jpeg/avdevice/lua/pulse/caca)
```
Bundle `build-clean/libmpv.so.2.1.0` as `libmpv.so.2`, and the six ffmpeg libs from
`prefix-clean/lib/<name>.so.<ver>` named by their SONAME.

Verified 2026-08-23: `mpv_initialize()=0` (no abort) with the bundle on BOTH muOS
(.248, ffmpeg-4 device) and RockNIX (.249, Adreno, no system libmpv).
