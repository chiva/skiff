#!/usr/bin/env bash
# Gathers the licences of everything compiled into the EBOOT, using pspdev's own
# psp-create-license-directory (always adds pspsdk, newlib and pthread-embedded, plus each listed
# package and its dependencies), and the licence of the CA bundle that ships next to it. Must run
# inside the Skiff toolchain image.
# Usage: scripts/collect-licenses.sh <output-dir>
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
readonly REPO_ROOT
readonly OUTPUT_DIR="${1:?usage: scripts/collect-licenses.sh <output-dir>}"
readonly PACKAGE_LIST="$REPO_ROOT/scripts/psp-packages.txt"
# The name psp-create-license-directory writes into, relative to its working directory.
readonly PSPDEV_LICENSE_DIR="third-party-licenses"
# Mozilla's root certificates (MPL-2.0), installed by docker/ca-bundle/fetch-ca-bundle.sh: every
# release ships them as cacert.pem, so their licence goes in under the same name.
readonly CA_BUNDLE_LICENSE="${SKIFF_CA_BUNDLE_DIR:?run inside the Skiff toolchain image}/LICENSE"
readonly CA_BUNDLE_LICENSE_NAME="cacert"

mapfile -t packages < <(grep -vE '^\s*(#|$)' "$PACKAGE_LIST" || true)

mkdir -p "$OUTPUT_DIR"
cd "$OUTPUT_DIR"
rm -rf "$PSPDEV_LICENSE_DIR"
psp-create-license-directory "${packages[@]}"
install -D -m 644 "$CA_BUNDLE_LICENSE" "$PSPDEV_LICENSE_DIR/$CA_BUNDLE_LICENSE_NAME/LICENSE"
echo "$OUTPUT_DIR/$PSPDEV_LICENSE_DIR"
