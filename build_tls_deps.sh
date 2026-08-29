#!/usr/bin/env bash
# Build the static TLS stack (zlib + mbedTLS + curl) that the PortMaster build of
# YTC links IN, so Innertube HTTPS works with NO libcurl/OpenSSL present on the
# device and nothing extra to bundle in the port. curl 8.5 + mbedTLS 3.6 (LTS),
# CA bundle path baked at configure time.
#
# Native build: run on an aarch64 host (e.g. the Orange Pi) that matches the target
# handhelds. Output goes to TLS_PREFIX (default ~/ytc-tls); pass that same
# TLS_PREFIX to build_portmaster.sh, which bakes these .a's into the binary.
#
# Needs: gcc, make, cmake, curl (to fetch sources), tar/bzip2. One-time — re-run
# only to change versions. (Uses the mbedTLS *release* tarball so no Python needed.)
set -euo pipefail

PREFIX="${TLS_PREFIX:-$HOME/ytc-tls}"
JOBS="${JOBS:-$(nproc)}"
WORK="${WORK:-/tmp/ytc-tls-build}"
# Baked into curl as the default CA bundle; present on muOS/Debian/most CFWs. The
# app also falls back to a cacert.pem shipped next to the binary (see http.cpp).
CA_BUNDLE="${CA_BUNDLE:-/etc/ssl/certs/ca-certificates.crt}"

ZLIB=1.3.1
MBEDTLS=3.6.2
CURL=8.11.0     # must be >= curl's mbedTLS 3.6 support (8.5 fails: "ssl_init failed")

mkdir -p "$PREFIX" "$WORK"; cd "$WORK"
dl() { [ -f "$2" ] || curl -fSL "$1" -o "$2"; }

echo "=== zlib $ZLIB (static) ==="
dl "https://zlib.net/fossils/zlib-$ZLIB.tar.gz" zlib.tar.gz
rm -rf "zlib-$ZLIB"; tar xf zlib.tar.gz
( cd "zlib-$ZLIB" && ./configure --prefix="$PREFIX" --static && make -j"$JOBS" && make install )

echo "=== mbedTLS $MBEDTLS (static; release tarball, no codegen needed) ==="
dl "https://github.com/Mbed-TLS/mbedtls/releases/download/mbedtls-$MBEDTLS/mbedtls-$MBEDTLS.tar.bz2" mbedtls.tar.bz2
rm -rf "mbedtls-$MBEDTLS"; tar xf mbedtls.tar.bz2
cmake -S "mbedtls-$MBEDTLS" -B "mbedtls-$MBEDTLS/b" \
  -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$PREFIX" \
  -DUSE_SHARED_MBEDTLS_LIBRARY=OFF -DENABLE_TESTING=OFF -DENABLE_PROGRAMS=OFF
cmake --build "mbedtls-$MBEDTLS/b" -j"$JOBS"
cmake --install "mbedtls-$MBEDTLS/b"

echo "=== curl $CURL (static, mbedTLS backend, CA bundle baked) ==="
dl "https://curl.se/download/curl-$CURL.tar.gz" curl.tar.gz
rm -rf "curl-$CURL"; tar xf curl.tar.gz
( cd "curl-$CURL" && ./configure --prefix="$PREFIX" --disable-shared --enable-static \
    --with-mbedtls="$PREFIX" --with-zlib="$PREFIX" --with-ca-bundle="$CA_BUNDLE" \
    --without-libpsl --without-libidn2 --without-nghttp2 --without-brotli --without-zstd \
    --without-libssh2 --without-librtmp \
    --disable-ldap --disable-ldaps --disable-rtsp --disable-dict --disable-telnet \
    --disable-tftp --disable-pop3 --disable-imap --disable-smtp --disable-gopher \
    --disable-mqtt --disable-manual \
 && make -j"$JOBS" && make install )

rm -rf "$WORK"
echo ""
echo "Static TLS libs installed in $PREFIX/lib:"
ls "$PREFIX"/lib/lib{curl,mbedtls,mbedx509,mbedcrypto,z}.a
echo
echo "Next: TLS_PREFIX=$PREFIX ./build_portmaster.sh"
echo "  (or just ./build_portmaster.sh — it defaults TLS_PREFIX to ~/ytc-tls)"
