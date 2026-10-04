#!/usr/bin/env bash
# Wraps a built EBOOT.PBP into the zip players download: unzip it at the root of the Memory Stick
# and the app lands where the XMB looks for it. The licences directory comes from
# scripts/collect-licenses.sh, which must run inside the pspdev image.
# Usage: scripts/package.sh <path/to/EBOOT.PBP> <path/to/third-party-licenses>
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
readonly REPO_ROOT
readonly USAGE="usage: scripts/package.sh <path/to/EBOOT.PBP> <path/to/third-party-licenses>"
readonly EBOOT="${1:?$USAGE}"
readonly LICENSES_DIR="${2:?$USAGE}"
readonly APP_DIR="PSP/GAME/Skiff"

version="$(sed -n 's/.*VERSION \([0-9][0-9.]*\) # x-release-please-version/\1/p' "$REPO_ROOT/CMakeLists.txt")"
if [[ -z "$version" ]]; then
  echo "error: could not read the project version from CMakeLists.txt" >&2
  exit 1
fi

staging="$(mktemp -d)"
trap 'rm -rf "$staging"' EXIT

mkdir -p "$staging/$APP_DIR"
cp "$EBOOT" "$staging/$APP_DIR/EBOOT.PBP"
cp "$REPO_ROOT/LICENSE" "$staging/$APP_DIR/LICENSE.txt"
if [[ ! -d "$LICENSES_DIR/pspsdk" ]]; then
  echo "error: $LICENSES_DIR has no pspsdk licence; run scripts/collect-licenses.sh first" >&2
  exit 1
fi
cp -R "$LICENSES_DIR" "$staging/$APP_DIR/third-party-licenses"

mkdir -p "$REPO_ROOT/dist"
archive="$REPO_ROOT/dist/skiff-$version.zip"
rm -f "$archive"
(cd "$staging" && zip -qr "$archive" PSP)
echo "$archive"
