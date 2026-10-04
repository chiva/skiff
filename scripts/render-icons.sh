#!/usr/bin/env bash
# Renders the committed PNGs from the SVG masters in assets/brand/. Runs in the host image (needs
# rsvg-convert): `scripts/dev.sh icons`. The PNGs are committed so PSP builds need no SVG renderer.
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
readonly REPO_ROOT
readonly BRAND_DIR="$REPO_ROOT/assets/brand"

# master SVG -> PNG, at the master's own size: ICON0 144x80 and PIC1 480x272 as the XMB expects
# them, GitHub's social preview 1280x640.
readonly RENDERS=(
  "skiff-icon0.svg:assets/psp/ICON0.PNG"
  "skiff-pic1.svg:assets/psp/PIC1.PNG"
  "skiff-social.svg:assets/github/social-preview.png"
)

for render in "${RENDERS[@]}"; do
  svg="$BRAND_DIR/${render%%:*}"
  png="$REPO_ROOT/${render#*:}"
  mkdir -p "$(dirname "$png")"
  rsvg-convert --format png --output "$png" "$svg"
  echo "${render#*:}"
done
