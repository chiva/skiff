#!/usr/bin/env bash
# Records the RomM responses the fake transport replays (tests/support/fake_transport.h) from the
# integration RomM: raw status line, headers and body, byte for byte, one file per exchange. Run
# from a container on the compose network by `scripts/dev.sh romm-record`, which seeds a small
# synthetic payload first.
#
# Headers that differ per run or per session (Date, Set-Cookie) are dropped. Nothing else is edited,
# so review the files before committing them: synthetic data only, never a token.
# Usage: tests/integration/record-fixtures.sh <integration-dir> <fixtures-dir>
set -euo pipefail

readonly USAGE="usage: record-fixtures.sh <integration-dir> <fixtures-dir>"
readonly DIR="${1:?$USAGE}"
readonly OUT="${2:?$USAGE}"
readonly SEED="$DIR/romm.json"
readonly BASE="https://proxy:8443"
readonly CONNECT_TIMEOUT_SECONDS=5
readonly REQUEST_TIMEOUT_SECONDS=30
# The page size the PSP will ask for, with the per-library extras RomM adds by default turned off
# (docs/development/architecture.md, "RomM integration").
readonly ROM_PAGE_QUERY="limit=10&offset=0&with_char_index=false&with_filter_values=false&with_rom_id_index=false"

TOKEN="$(jq -r .token "$SEED")"
PLATFORM_ID="$(jq -r .platform_id "$SEED")"
ROM_ID="$(jq -r .rom_id "$SEED")"
FILE_NAME="$(jq -rn --arg n "$(jq -r .file_name "$SEED")" '$n | @uri')"
readonly TOKEN PLATFORM_ID ROM_ID FILE_NAME
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
record heartbeat /api/heartbeat
record platforms /api/platforms "${auth[@]}"
record roms-page "/api/roms?platform_ids=$PLATFORM_ID&$ROM_PAGE_QUERY" "${auth[@]}"
record rom "/api/roms/$ROM_ID" "${auth[@]}"
record rom-content "/api/roms/$ROM_ID/content/$FILE_NAME" "${auth[@]}"
record roms-unauthorized /api/roms
