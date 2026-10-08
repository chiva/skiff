#!/usr/bin/env bash
# Wraps a built EBOOT.PBP into the zip players download: unzip it at the root of the Memory Stick
# and the app lands where the XMB looks for it, with the CA bundle it trusts by default (cacert.pem,
# put next to the EBOOT by scripts/dev.sh) and every licence. The licences directory comes from
# scripts/collect-licenses.sh, which must run inside the Skiff toolchain image. The archive's
# contents are checked before its path is printed.
# Usage: scripts/package.sh <path/to/EBOOT.PBP> <path/to/cacert.pem> <path/to/third-party-licenses>
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
readonly REPO_ROOT
readonly USAGE="usage: scripts/package.sh <path/to/EBOOT.PBP> <path/to/cacert.pem> <path/to/third-party-licenses>"
readonly EBOOT="${1:?$USAGE}"
readonly CA_BUNDLE="${2:?$USAGE}"
readonly LICENSES_DIR="${3:?$USAGE}"
readonly APP_DIR="PSP/GAME/Skiff"
# The app opens this name in its folder when config.ini sets no ca_file (SKIFF_APP_DEFAULT_CA_FILE).
readonly CA_BUNDLE_NAME="cacert.pem"
readonly PEM_CERTIFICATE="-----BEGIN CERTIFICATE-----"
# Every archive must hold these (zip lists folders with a trailing slash); the licence folders are
# the ones always collected.
readonly REQUIRED_ENTRIES=(
  "$APP_DIR/EBOOT.PBP"
  "$APP_DIR/$CA_BUNDLE_NAME"
  "$APP_DIR/LICENSE.txt"
  "$APP_DIR/third-party-licenses/pspsdk/"
  "$APP_DIR/third-party-licenses/newlib/"
  "$APP_DIR/third-party-licenses/pthread-embedded/"
  "$APP_DIR/third-party-licenses/cacert/LICENSE"
)

version="$(sed -n 's/.*VERSION \([0-9][0-9.]*\) # x-release-please-version/\1/p' "$REPO_ROOT/CMakeLists.txt")"
if [[ -z "$version" ]]; then
  echo "error: could not read the project version from CMakeLists.txt" >&2
  exit 1
fi
if [[ ! -d "$LICENSES_DIR/pspsdk" ]]; then
  echo "error: $LICENSES_DIR has no pspsdk licence; run scripts/collect-licenses.sh first" >&2
  exit 1
fi
if ! grep -qF -- "$PEM_CERTIFICATE" "$CA_BUNDLE" 2>/dev/null; then
  echo "error: $CA_BUNDLE holds no certificate; build with scripts/dev.sh package" >&2
  exit 1
fi

# check_archive <zip>: fails unless every required entry is in it.
check_archive() {
  local archive="$1" entries entry missing=0
  entries="$(unzip -Z1 "$archive")"
  for entry in "${REQUIRED_ENTRIES[@]}"; do
    if ! grep -qxF -- "$entry" <<<"$entries"; then
      echo "error: $archive has no $entry" >&2
      missing=1
    fi
  done
  return "$missing"
}

staging="$(mktemp -d)"
trap 'rm -rf "$staging"' EXIT

mkdir -p "$staging/$APP_DIR"
cp "$EBOOT" "$staging/$APP_DIR/EBOOT.PBP"
cp "$CA_BUNDLE" "$staging/$APP_DIR/$CA_BUNDLE_NAME"
cp "$REPO_ROOT/LICENSE" "$staging/$APP_DIR/LICENSE.txt"
cp -R "$LICENSES_DIR" "$staging/$APP_DIR/third-party-licenses"

mkdir -p "$REPO_ROOT/dist"
archive="$REPO_ROOT/dist/skiff-$version.zip"
rm -f "$archive"
(cd "$staging" && zip -qr "$archive" PSP)
if ! check_archive "$archive"; then
  rm -f "$archive"
  exit 1
fi
echo "$archive"
