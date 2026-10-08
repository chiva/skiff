#!/bin/sh
# Fetches the CA bundle Skiff trusts by default: Mozilla's root certificates as curl publishes them
# (https://curl.se/docs/caextract.html), a dated file pinned by SHA256. The toolchain image runs it,
# so every PSP build has the bundle without a network step of its own: scripts/dev.sh copies it next
# to the app's EBOOT, the release zip ships it, and scripts/collect-licenses.sh ships its licence
# (MPL-2.0, the text beside this script).
#
# When bumping, take the newest dated file from that page, compare its SHA256 with the one curl.se
# publishes beside it (cacert-<date>.pem.sha256), and update both values below.
# Usage: fetch-ca-bundle.sh <directory>
set -eu

CA_BUNDLE_DATE=2026-09-25
CA_BUNDLE_SHA256=a41b5d356aea97a529fe27e0f7316d2f9d946d75927476cf9cf1b90637d00505

readonly DEST="${1:?usage: fetch-ca-bundle.sh <directory>}"
# The name the app opens (SKIFF_APP_DEFAULT_CA_FILE in include/skiff/app.h).
readonly BUNDLE_NAME="cacert.pem"
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
readonly SCRIPT_DIR
WORK="$(mktemp -d)"
readonly WORK
trap 'rm -rf "$WORK"' EXIT

# pspdev's image has wget; the checksum decides either way.
if command -v wget >/dev/null 2>&1; then
  wget -q -O "$WORK/$BUNDLE_NAME" "https://curl.se/ca/cacert-${CA_BUNDLE_DATE}.pem"
else
  curl -fsSL -o "$WORK/$BUNDLE_NAME" "https://curl.se/ca/cacert-${CA_BUNDLE_DATE}.pem"
fi
echo "$CA_BUNDLE_SHA256  $WORK/$BUNDLE_NAME" | sha256sum -c -

install -D -m 644 "$WORK/$BUNDLE_NAME" "$DEST/$BUNDLE_NAME"
install -D -m 644 "$SCRIPT_DIR/MPL-2.0.txt" "$DEST/LICENSE"
echo "CA bundle: Mozilla roots of $CA_BUNDLE_DATE installed in $DEST"
