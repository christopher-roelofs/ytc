#!/usr/bin/env bash
# Compile the PortMaster (handheld, aarch64 GLES2) build of YTC.
#
# Run from the repo root on a build host set up like the Orange Pi 5:
#   /opt/device-libs             the device's render-only libmpv (.so.2) + mpv.pc
#   /opt/pkhome-sysroot          static curl + mbedTLS + zlib (.a) for baked-in TLS
#   $HOME/mpvbuild/prefix-clean  matched ffmpeg (libav*/libsw*) .pc for the download muxer
#
# The device's SDL2/GLESv2 come from the system pkg-config path. Produces the
# stripped-able binary at build-port/yt_ui. This only COMPILES — it does not
# assemble the port folder, copy libs.aarch64, or build the zip.
#
# Paths are overridable via env (DEVICE_LIBS, SYSROOT, FFPREFIX, BUILD_DIR, JOBS).
set -euo pipefail

DEVICE_LIBS="${DEVICE_LIBS:-/opt/device-libs}"
SYSROOT="${SYSROOT:-/opt/pkhome-sysroot}"
FFPREFIX="${FFPREFIX:-$HOME/mpvbuild/prefix-clean}"
BUILD_DIR="${BUILD_DIR:-build-port}"
JOBS="${JOBS:-$(nproc)}"

# Device libmpv FIRST so its mpv.pc (render-only .so.2) wins over any system libmpv,
# then the ffmpeg prefix for the offline-download muxer; SDL2/GLESv2/etc. come from
# the default pkg-config path after these.
export PKG_CONFIG_PATH="$DEVICE_LIBS/lib/pkgconfig:$FFPREFIX/lib/pkgconfig${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}"

# GLES2 path (YTC_GL_DESKTOP stays OFF); no yt_play spike; curl/mbedTLS/zlib linked
# static so the binary needs only SDL2/GLESv2/libmpv/pthread on the device.
cmake -S . -B "$BUILD_DIR" \
  -DCMAKE_BUILD_TYPE=Release \
  -DBUILD_PLAYER=OFF \
  -DCMAKE_EXE_LINKER_FLAGS="-L$DEVICE_LIBS/lib -L$SYSROOT/lib -Wl,--allow-shlib-undefined -static-libstdc++ -static-libgcc" \
  -DCMAKE_CXX_STANDARD_LIBRARIES="$SYSROOT/lib/libcurl.a $SYSROOT/lib/libmbedtls.a $SYSROOT/lib/libmbedx509.a $SYSROOT/lib/libmbedcrypto.a $SYSROOT/lib/libz.a -lpthread"

cmake --build "$BUILD_DIR" -j"$JOBS"

echo "Built: $BUILD_DIR/yt_ui"
