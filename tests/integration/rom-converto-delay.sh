#!/bin/bash
# Stands in for rom-converto on the test RomM's PATH: compose.yaml mounts it at
# /usr/local/bin/rom-converto, ahead of the real /usr/bin/rom-converto. Anything RomM runs on the
# slow synthetic disc (seed.py, "Skiff Convert Slow") waits DELAY_SECONDS first: longer than the 15 s
# RomM holds a download that started a conversion, so that download is answered 202 with
# Retry-After every time, as on a real server converting a whole UMD. Everything else runs the real
# binary at once.
set -euo pipefail

readonly REAL=/usr/bin/rom-converto
readonly SLOW_NAME="Skiff Convert Slow"
readonly DELAY_SECONDS=20

for arg in "$@"; do
  if [[ "$arg" == *"$SLOW_NAME"* ]]; then
    sleep "$DELAY_SECONDS"
    break
  fi
done
exec "$REAL" "$@"
