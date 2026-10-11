#!/bin/sh
# Fetches argosy-sigil and the two submodules it builds with, each at a pinned commit and checked by
# SHA256, into <dir>/argosy-sigil, for the sigil cross-check (tests/sigil/check.sh). Test-only:
# nothing of sigil is linked into an EBOOT.
#
# The pin is sigil v1.0.0, the first commit with the PSP save unit Skiff copies (psp_console layout,
# sigil_collect and sigil_restore, src/zip_mem.c). RomM 5.4.0's Dockerfile pins an older commit,
# 8a3b0089676e07f74da2d9ad08979dedf5cae7c0, which it uses only to read disc ids: that commit has
# no PSP save layout, collect or restore, so it cannot cross-check a unit.
# Usage: fetch-sigil.sh <dir>
set -eu

SIGIL_COMMIT=ebce9e898a5ac51a5c301c61a743e2b7881b06d2
SIGIL_SHA256=141064de6dc9ade485c3e283c54065d7328cf0ee9731e0dc2f108d37b3ade11e
LIBCHDR_COMMIT=7891cbee2e8d7967bf8b6c24f091d2c5a53d8cfc
LIBCHDR_SHA256=0f7fa9b98e40264c19ff347ff7fc893c2fbadaf01aa42c6a64891ecc9dfddce7
TINY_AES_COMMIT=23856752fbd139da0b8ca6e471a13d5bcc99a08d
TINY_AES_SHA256=b91c219c4ff2dd4f24750130c27045e264b52d763d3b5b2cd24230334176f820

readonly DIR="${1:?usage: fetch-sigil.sh <dir>}"
readonly SOURCE="$DIR/argosy-sigil"
readonly STAMP="$SOURCE/.skiff-pin"

if [ -f "$STAMP" ] && [ "$(cat "$STAMP")" = "$SIGIL_COMMIT" ]; then
  exit 0
fi
rm -rf "$SOURCE"
mkdir -p "$DIR"
WORK="$(mktemp -d)"
readonly WORK
trap 'rm -rf "$WORK"' EXIT

# fetch <owner/repo> <commit> <sha256> <destination>
fetch() {
  curl -fsSL --retry 3 -o "$WORK/archive.tar.gz" "https://github.com/$1/archive/$2.tar.gz"
  echo "$3  $WORK/archive.tar.gz" | sha256sum -c - >/dev/null
  mkdir -p "$4"
  tar -xzf "$WORK/archive.tar.gz" -C "$4" --strip-components=1
  rm "$WORK/archive.tar.gz"
}

fetch rommapp/argosy-sigil "$SIGIL_COMMIT" "$SIGIL_SHA256" "$WORK/argosy-sigil"
rm -rf "$WORK/argosy-sigil/third_party/libchdr" "$WORK/argosy-sigil/third_party/tiny-AES-c"
fetch rtissera/libchdr "$LIBCHDR_COMMIT" "$LIBCHDR_SHA256" "$WORK/argosy-sigil/third_party/libchdr"
fetch kokke/tiny-AES-c "$TINY_AES_COMMIT" "$TINY_AES_SHA256" "$WORK/argosy-sigil/third_party/tiny-AES-c"
mv "$WORK/argosy-sigil" "$SOURCE"
echo "$SIGIL_COMMIT" >"$STAMP"
echo "argosy-sigil ${SIGIL_COMMIT} fetched into $SOURCE"
