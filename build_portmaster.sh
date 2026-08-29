#!/usr/bin/env bash
# Compile the PortMaster (handheld, aarch64 GLES2) build of YTC.
#
# Run from the repo root on a build host set up like the Orange Pi 5:
#   /opt/device-libs             the device's render-only libmpv (.so.2) + mpv.pc
#   $HOME/mpvbuild/prefix-clean  matched ffmpeg (libav*/libsw*) .pc for the download muxer
#
# Everything else — SDL2, GLESv2, curl (+ its TLS) — comes from the distro:
#   apt install libsdl2-dev libgles2-mesa-dev libcurl4-openssl-dev
# curl is linked DYNAMICALLY (no custom static sysroot); the packaging step bundles
# libcurl + its TLS deps into the port's libs.<arch>/ alongside libmpv/ffmpeg.
#
# Produces build-port/yt_ui. This only COMPILES — it does not assemble the port
# folder, copy libs.aarch64, or build the zip.
#
# Paths are overridable via env (DEVICE_LIBS, FFPREFIX, BUILD_DIR, JOBS).
set -euo pipefail

DEVICE_LIBS="${DEVICE_LIBS:-/opt/device-libs}"
FFPREFIX="${FFPREFIX:-$HOME/mpvbuild/prefix-clean}"
BUILD_DIR="${BUILD_DIR:-build-port}"
JOBS="${JOBS:-$(nproc)}"

# Device libmpv FIRST so its mpv.pc (render-only .so.2) wins over any system libmpv,
# then the ffmpeg prefix for the offline-download muxer; SDL2/GLESv2/curl come from
# the default pkg-config path after these.
export PKG_CONFIG_PATH="$DEVICE_LIBS/lib/pkgconfig:$FFPREFIX/lib/pkgconfig${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}"

# GLES2 path (YTC_GL_DESKTOP stays OFF); no yt_play spike. Static libstdc++/libgcc
# (compiler runtime, not a custom sysroot) so the binary doesn't depend on the
# device's C++ ABI version.
cmake -S . -B "$BUILD_DIR" \
  -DCMAKE_BUILD_TYPE=Release \
  -DBUILD_PLAYER=OFF \
  -DCMAKE_EXE_LINKER_FLAGS="-L$DEVICE_LIBS/lib -Wl,--allow-shlib-undefined -static-libstdc++ -static-libgcc"

cmake --build "$BUILD_DIR" -j"$JOBS"

echo "Built: $BUILD_DIR/yt_ui"
