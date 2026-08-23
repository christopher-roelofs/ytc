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
      sets YTNATIVE_HWDEC per device (or app auto-detects /dev/mpp_service). Allwinner
      (RG28XX) stays software (librockchip_mpp loads, decoders fail -> sw fallback).
- [ ] Verify on a real Rockchip handheld (RK3566/3588) and re-verify RG28XX still runs
      (software) with the bundled stack.
- [ ] Enumerate + bundle the libass chain; ldd the final yt_ui to confirm nothing
      outside the device-provided set is unresolved.
