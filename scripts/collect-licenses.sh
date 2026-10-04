#!/usr/bin/env bash
# Gathers the licences of everything compiled into the EBOOT, using pspdev's own
# psp-create-license-directory (always adds pspsdk, newlib and pthread-embedded, plus each listed
# package and its dependencies). Must run inside the pspdev image.
# Usage: scripts/collect-licenses.sh <output-dir>
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
readonly REPO_ROOT
readonly OUTPUT_DIR="${1:?usage: scripts/collect-licenses.sh <output-dir>}"
readonly PACKAGE_LIST="$REPO_ROOT/scripts/psp-packages.txt"
# The name psp-create-license-directory writes into, relative to its working directory.
readonly PSPDEV_LICENSE_DIR="third-party-licenses"

mapfile -t packages < <(grep -vE '^\s*(#|$)' "$PACKAGE_LIST" || true)

mkdir -p "$OUTPUT_DIR"
cd "$OUTPUT_DIR"
rm -rf "$PSPDEV_LICENSE_DIR"
psp-create-license-directory "${packages[@]}"
echo "$OUTPUT_DIR/$PSPDEV_LICENSE_DIR"
