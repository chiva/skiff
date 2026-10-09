#!/bin/sh
# Builds Nayuki's QR Code generator (C), which draws the pairing address as a QR code
# (src/ui/qr.c), as a static library installed under <prefix>: include/qrcodegen/qrcodegen.h,
# lib/libqrcodegen.a and its licence in share/licenses/qrcodegen/ (where pspdev's
# psp-create-license-directory finds it, through scripts/psp-packages.txt). Both images run it:
# the toolchain image for the PSP (extra arguments name pspdev's CMake toolchain file) and the host
# image for unit tests (no extra arguments), so both compile the same pinned source.
# Usage: build-qrcodegen.sh <prefix> [extra cmake arguments...]
set -eu

QRCODEGEN_VERSION=1.8.0
QRCODEGEN_SHA256=2ec0a4d33d6f521c942eeaf473d42d5fe139abcfa57d2beffe10c5cf7d34ae60
# The licence is the comment that opens the header (the project ships no separate file).
LICENSE_FIRST_LINE=2
LICENSE_LAST_LINE=21

readonly PREFIX="${1:?usage: build-qrcodegen.sh <prefix> [extra cmake arguments...]}"
shift
WORK="$(mktemp -d)"
readonly WORK
trap 'rm -rf "$WORK"' EXIT

# pspdev's image has wget, the host image curl; either is fine because the checksum decides.
if command -v wget >/dev/null 2>&1; then
  wget -q -O "$WORK/qrcodegen.tar.gz" \
    "https://github.com/nayuki/QR-Code-generator/archive/refs/tags/v${QRCODEGEN_VERSION}.tar.gz"
else
  curl -fsSL --retry 3 -o "$WORK/qrcodegen.tar.gz" \
    "https://github.com/nayuki/QR-Code-generator/archive/refs/tags/v${QRCODEGEN_VERSION}.tar.gz"
fi
echo "${QRCODEGEN_SHA256}  $WORK/qrcodegen.tar.gz" | sha256sum -c -
tar -xzf "$WORK/qrcodegen.tar.gz" -C "$WORK"
readonly SRC="$WORK/QR-Code-generator-${QRCODEGEN_VERSION}/c"

# The project builds with a Makefile for the host only; this builds the one library file with
# whichever toolchain CMake is given.
cat >"$SRC/CMakeLists.txt" <<'CMAKE'
cmake_minimum_required(VERSION 3.16)
project(qrcodegen C)
add_library(qrcodegen STATIC qrcodegen.c)
set_target_properties(qrcodegen PROPERTIES C_STANDARD 99)
# The host's executables are position independent; the PSP's ABI has no such code.
if(NOT CMAKE_CROSSCOMPILING)
  set_target_properties(qrcodegen PROPERTIES POSITION_INDEPENDENT_CODE ON)
endif()
install(TARGETS qrcodegen ARCHIVE DESTINATION lib)
install(FILES qrcodegen.h DESTINATION include/qrcodegen)
CMAKE
cmake -S "$SRC" -B "$WORK/build" -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$PREFIX" "$@" \
  >/dev/null
cmake --build "$WORK/build" >/dev/null
cmake --install "$WORK/build" >/dev/null
mkdir -p "$PREFIX/share/licenses/qrcodegen"
sed -n "${LICENSE_FIRST_LINE},${LICENSE_LAST_LINE}p" "$SRC/qrcodegen.h" | sed -E 's/^ ?\* ?//' \
  >"$PREFIX/share/licenses/qrcodegen/LICENSE"
echo "qrcodegen ${QRCODEGEN_VERSION} installed in $PREFIX"
