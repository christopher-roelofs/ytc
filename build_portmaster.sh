#!/usr/bin/env bash
# Compile the PortMaster (handheld, aarch64 GLES2) build of YTC.
#
# Run from the repo root on a build host set up like the Orange Pi 5:
#   /opt/device-libs             the device's render-only libmpv (.so.2) + mpv.pc
#   $HOME/mpvbuild/prefix-clean  matched ffmpeg (libav*/libsw*) .pc for the download muxer
#   $HOME/ytc-tls                (optional) static curl + mbedTLS + zlib (.a),
#                                built by ./build_tls_deps.sh
#
# TLS handling:
#   - If the static TLS libs are present (TLS_PREFIX), curl + mbedTLS are BAKED
#     INTO the binary (self-contained: the port bundles only libmpv/ffmpeg, no TLS
#     libs). Smaller and simpler (~1.3 MB in the binary vs ~5 MB of bundled
#     libcurl+OpenSSL otherwise). Build them once with ./build_tls_deps.sh.
#   - Otherwise curl is linked DYNAMICALLY against the distro's libcurl
#     (apt install libcurl4-openssl-dev); packaging must then bundle libcurl + its
#     TLS chain into libs.<arch>/ (CA via the bundled data/cacert.pem).
#
# SDL2/GLESv2 come from the distro (apt install libsdl2-dev libgles2-mesa-dev).
# Produces build-port/yt_ui. This only COMPILES — it does not assemble the port
# folder, copy libs.aarch64, or build the zip.
#
# Paths overridable via env (DEVICE_LIBS, FFPREFIX, TLS_PREFIX, BUILD_DIR, JOBS).
set -euo pipefail

DEVICE_LIBS="${DEVICE_LIBS:-/opt/device-libs}"
FFPREFIX="${FFPREFIX:-$HOME/mpvbuild/prefix-clean}"
TLS_PREFIX="${TLS_PREFIX:-$HOME/ytc-tls}"
BUILD_DIR="${BUILD_DIR:-build-port}"
JOBS="${JOBS:-$(nproc)}"

# Device libmpv FIRST so its mpv.pc (render-only .so.2) wins over any system libmpv,
# then the ffmpeg prefix for the offline-download muxer; SDL2/GLESv2/curl come from
# the default pkg-config path after these.
export PKG_CONFIG_PATH="$DEVICE_LIBS/lib/pkgconfig:$FFPREFIX/lib/pkgconfig${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}"

# Static libstdc++/libgcc (compiler runtime) so the binary doesn't depend on the
# device's C++ ABI version.
LINK_FLAGS="-L$DEVICE_LIBS/lib -Wl,--allow-shlib-undefined -static-libstdc++ -static-libgcc"
CMAKE_ARGS=(-DCMAKE_BUILD_TYPE=Release -DBUILD_PLAYER=OFF)

if [ -f "$TLS_PREFIX/lib/libcurl.a" ] && [ -f "$TLS_PREFIX/lib/libmbedtls.a" ]; then
  echo ">> TLS: STATIC (baked in from $TLS_PREFIX) — binary is self-contained for networking"
  LINK_FLAGS="-L$TLS_PREFIX/lib $LINK_FLAGS"
  CMAKE_ARGS+=(-DCMAKE_CXX_STANDARD_LIBRARIES="$TLS_PREFIX/lib/libcurl.a $TLS_PREFIX/lib/libmbedtls.a $TLS_PREFIX/lib/libmbedx509.a $TLS_PREFIX/lib/libmbedcrypto.a $TLS_PREFIX/lib/libz.a -lpthread")
else
  echo ">> TLS: DYNAMIC (system libcurl; no static TLS libs at $TLS_PREFIX)"
  echo ">>      run ./build_tls_deps.sh to bake TLS in, or packaging must bundle libcurl + its TLS libs"
fi
CMAKE_ARGS+=(-DCMAKE_EXE_LINKER_FLAGS="$LINK_FLAGS")

cmake -S . -B "$BUILD_DIR" "${CMAKE_ARGS[@]}"
cmake --build "$BUILD_DIR" -j"$JOBS"

echo "Built: $BUILD_DIR/yt_ui"
