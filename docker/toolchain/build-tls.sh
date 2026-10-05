#!/bin/sh
# Builds Skiff's TLS stack, Mbed TLS with Skiff's profile (configure-mbedtls.sh) and libcurl over it,
# as static libraries installed under <prefix>. Both images run it: the toolchain image for the PSP
# (extra arguments name pspdev's CMake toolchain file) and the host image for unit and integration
# tests (no extra arguments). One script means one copy of the versions, checksums, profile and curl
# options, so the host tests exercise the same TLS code the EBOOTs link.
#
# When bumping a version, verify the new archive first: mbedtls publishes SHA256 sums in its release
# notes; curl archives are signed by Daniel Stenberg (27EDEAF22F3ABCEB50DB9A125CC908FDB71E12C2), see
# docs/development/toolchain.md.
# Usage: build-tls.sh <prefix> [extra cmake arguments...]
set -eu

MBEDTLS_VERSION=4.1.1
MBEDTLS_SHA256=3359a349e23db3d5536fcee032ae7b2ecbfc08972fab643089b5cbf2a375c98c
CURL_VERSION=8.22.0
CURL_SHA256=5d956a6a22b3c279f50c421ee5d3c9e9d660cb6f115dcf881b579e952130549c

readonly PREFIX="${1:?usage: build-tls.sh <prefix> [extra cmake arguments...]}"
shift
readonly LICENSES="$PREFIX/share/licenses"
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
readonly SCRIPT_DIR
WORK="$(mktemp -d)"
readonly WORK
trap 'rm -rf "$WORK"' EXIT

# pspdev's image has wget, the host image curl; either is fine because the checksum decides.
fetch() {
  if command -v wget >/dev/null 2>&1; then
    wget -q -O "$2" "$1"
  else
    curl -fsSL -o "$2" "$1"
  fi
}

# fetch_verified <url> <sha256> <archive>: downloads and unpacks into $WORK, or stops the build.
fetch_verified() {
  fetch "$1" "$WORK/$3"
  echo "$2  $WORK/$3" | sha256sum -c -
  tar -xjf "$WORK/$3" -C "$WORK"
}

fetch_verified \
  "https://github.com/Mbed-TLS/mbedtls/releases/download/mbedtls-${MBEDTLS_VERSION}/mbedtls-${MBEDTLS_VERSION}.tar.bz2" \
  "$MBEDTLS_SHA256" mbedtls.tar.bz2
readonly MBEDTLS_SRC="$WORK/mbedtls-${MBEDTLS_VERSION}"
"$SCRIPT_DIR/configure-mbedtls.sh" "$MBEDTLS_SRC"
cmake -S "$MBEDTLS_SRC" -B "$WORK/mbedtls-build" -Wno-dev "$@" \
  -DCMAKE_INSTALL_PREFIX="$PREFIX" \
  -DCMAKE_BUILD_TYPE=Release \
  -DENABLE_TESTING=OFF \
  -DENABLE_PROGRAMS=OFF \
  -DUSE_SHARED_MBEDTLS_LIBRARY=OFF
cmake --build "$WORK/mbedtls-build" -j "$(nproc)"
cmake --install "$WORK/mbedtls-build"
install -D -m 644 "$MBEDTLS_SRC/LICENSE" "$LICENSES/mbedtls/LICENSE"
install -D -m 644 "$MBEDTLS_SRC/tf-psa-crypto/LICENSE" "$LICENSES/mbedtls/tf-psa-crypto/LICENSE"

# HTTPS only, IPv4 only, no resolver thread (the PSP resolver is synchronous), and every optional
# dependency off. CA bundle and path are unset because Skiff passes the CA file at runtime.
# zlib is off for now: RomM's JSON pages are small and decompression costs RAM; revisit with
# Phase 1 measurements.
fetch_verified "https://curl.se/download/curl-${CURL_VERSION}.tar.bz2" "$CURL_SHA256" curl.tar.bz2
readonly CURL_SRC="$WORK/curl-${CURL_VERSION}"
cmake -S "$CURL_SRC" -B "$WORK/curl-build" -Wno-dev "$@" \
  -DCMAKE_INSTALL_PREFIX="$PREFIX" \
  -DCMAKE_BUILD_TYPE=Release \
  -DBUILD_SHARED_LIBS=OFF \
  -DBUILD_STATIC_LIBS=ON \
  -DBUILD_CURL_EXE=OFF \
  -DBUILD_TESTING=OFF \
  -DBUILD_EXAMPLES=OFF \
  -DBUILD_LIBCURL_DOCS=OFF \
  -DBUILD_MISC_DOCS=OFF \
  -DENABLE_CURL_MANUAL=OFF \
  -DCURL_USE_PKGCONFIG=OFF \
  -DHTTP_ONLY=ON \
  -DCURL_USE_MBEDTLS=ON \
  -DCURL_USE_OPENSSL=OFF \
  -DENABLE_IPV6=OFF \
  -DENABLE_THREADED_RESOLVER=OFF \
  -DENABLE_UNIX_SOCKETS=OFF \
  -DCURL_DISABLE_SOCKETPAIR=ON \
  -DUSE_NGHTTP2=OFF \
  -DUSE_LIBIDN2=OFF \
  -DCURL_USE_LIBPSL=OFF \
  -DCURL_USE_LIBSSH2=OFF \
  -DCURL_ZLIB=OFF \
  -DCURL_BROTLI=OFF \
  -DCURL_ZSTD=OFF \
  -DCURL_DISABLE_ALTSVC=ON \
  -DCURL_DISABLE_COOKIES=ON \
  -DCURL_DISABLE_DOH=ON \
  -DCURL_DISABLE_HSTS=ON \
  -DCURL_DISABLE_NETRC=ON \
  -DCURL_DISABLE_WEBSOCKETS=ON \
  -DCURL_DISABLE_AWS=ON \
  -DCURL_DISABLE_KERBEROS_AUTH=ON \
  -DCURL_DISABLE_NEGOTIATE_AUTH=ON \
  -DCURL_CA_BUNDLE=none \
  -DCURL_CA_PATH=none
cmake --build "$WORK/curl-build" -j "$(nproc)"
cmake --install "$WORK/curl-build"
install -D -m 644 "$CURL_SRC/COPYING" "$LICENSES/curl/COPYING"

# Read by the CI toolchain check and by scripts that need to report what they built with.
printf 'mbedtls %s\ncurl %s\n' "$MBEDTLS_VERSION" "$CURL_VERSION" >"$PREFIX/share/skiff-toolchain-versions"
echo "TLS stack: mbedtls $MBEDTLS_VERSION, curl $CURL_VERSION installed in $PREFIX"
