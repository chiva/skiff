#!/usr/bin/env bash
# Records the fake transport's fixtures for downloads RomM converts (?format=, RomM 5.4 with
# rom-converto), from the convert discs `python3 /seed.py convert-discs` added after the other
# fixtures were recorded (scripts/dev.sh romm-record), so those keep their two ROMs. Same format and
# rules as record-fixtures.sh: review before committing, synthetic data only, never a token.
# Usage: tests/integration/record-conversion-fixtures.sh <integration-dir> <fixtures-dir>
set -euo pipefail

readonly USAGE="usage: record-conversion-fixtures.sh <integration-dir> <fixtures-dir>"
readonly DIR="${1:?$USAGE}"
readonly OUT="${2:?$USAGE}"
readonly SEED="$DIR/romm.json"
readonly CONVERT="$DIR/romm-convert.json"
readonly BASE="https://proxy:8443"
readonly CONNECT_TIMEOUT_SECONDS=5
readonly REQUEST_TIMEOUT_SECONDS=30
# nginx's ETag holds the file's modification time in whole seconds, which RomM refreshes on every
# GET of a converted copy.
readonly ETAG_CLOCK_SECONDS=1.1
readonly RANGE_START=1000

TOKEN="$(jq -r .token "$SEED")"
DISC_ID="$(jq -r .convert.fast.rom_id "$CONVERT")"
DISC_NAME="$(jq -rn --arg n "$(jq -r .convert.fast.file_name "$CONVERT")" '$n | @uri')"
SLOW_ID="$(jq -r .convert.slow.rom_id "$CONVERT")"
SLOW_NAME="$(jq -rn --arg n "$(jq -r .convert.slow.file_name "$CONVERT")" '$n | @uri')"
readonly TOKEN DISC_ID DISC_NAME SLOW_ID SLOW_NAME
WORK="$(mktemp -d)"
readonly WORK
trap 'rm -rf "$WORK"' EXIT

# record <fixture-name> <path> [curl arguments...]
record() {
  local name="$1" path="$2"
  shift 2
  curl -sS --connect-timeout "$CONNECT_TIMEOUT_SECONDS" --max-time "$REQUEST_TIMEOUT_SECONDS" \
    --http1.1 --cacert "$DIR/certs/ca.crt" -D "$WORK/headers" -o "$WORK/body" "$@" "$BASE$path"
  grep -viE '^(date|set-cookie):' "$WORK/headers" >"$WORK/kept"
  cat "$WORK/kept" "$WORK/body" >"$OUT/$name.http"
  echo "recorded $name.http: $(head -n 1 "$WORK/headers" | tr -d '\r'), $(wc -c <"$WORK/body") body bytes"
}

mkdir -p "$OUT"
auth=(-H "Authorization: Bearer $TOKEN")
disc="/api/roms/$DISC_ID/content/$DISC_NAME"
record heartbeat-conversion /api/heartbeat
record rom-convert "/api/roms/$DISC_ID" "${auth[@]}"
record rom-content-convert-unavailable "$disc?format=rvz" "${auth[@]}"
record rom-content-convert "$disc?format=cso" "${auth[@]}"
etag="$(awk 'tolower($1) == "etag:" {print $2}' "$WORK/headers" | tr -d '\r')"
sleep "$ETAG_CLOCK_SECONDS"
record rom-content-convert-resume "$disc?format=cso" "${auth[@]}" \
  -H "Range: bytes=$RANGE_START-" -H "If-Range: $etag"
# Held back by rom-converto-delay.sh past RomM's 15 s hold.
record rom-content-convert-pending "/api/roms/$SLOW_ID/content/$SLOW_NAME?format=cso" "${auth[@]}"
