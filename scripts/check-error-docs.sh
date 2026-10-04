#!/usr/bin/env bash
# Fails when an error code shown to players has no row in the troubleshooting guide, so the guide
# cannot fall behind include/skiff/error.h. Exceptions: SKIFF_OK and the internal 1–99 group, which
# indicate bugs rather than something a player can fix.
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
readonly REPO_ROOT
readonly ERROR_HEADER="$REPO_ROOT/include/skiff/error.h"
readonly GUIDE="$REPO_ROOT/docs/guide/troubleshooting.md"
readonly FIRST_PLAYER_FACING_CODE=100

missing=0
while read -r name code; do
  if ((code < FIRST_PLAYER_FACING_CODE)); then
    continue
  fi
  if ! grep -qE "^\| ${code} \|" "$GUIDE"; then
    echo "error: $name ($code) has no row in docs/guide/troubleshooting.md" >&2
    missing=1
  fi
done < <(sed -nE 's/^ *X\((SKIFF_[A-Z_]+), ([0-9]+),.*/\1 \2/p' "$ERROR_HEADER")

exit "$missing"
