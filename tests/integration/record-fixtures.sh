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
# The query skiff_romm_list_roms() sends after platform_ids, limit and offset: ordered by name, the
# per-library extras RomM adds by default turned off (docs/development/architecture.md, "RomM
# integration"). Pages of one ROM, so the two seeded ROMs make two pages and an empty third.
readonly ROM_LIST_QUERY="order_by=name&order_dir=asc&with_char_index=false&with_filter_values=false&with_rom_id_index=false"
# What it adds for the player's favourites with each ROM's files (SKIFF_ROMM_LIST_FAVOURITES,
# with_files). The seed's only favourite is the payload: one page, then an empty one.
readonly FAVOURITES_QUERY="favorite=true&with_files=true"

TOKEN="$(jq -r .token "$SEED")"
PLATFORM_ID="$(jq -r .platform_id "$SEED")"
ROM_ID="$(jq -r .rom_id "$SEED")"
EXTRA_ROM_ID="$(jq -r .extra.rom_id "$SEED")"
FILE_NAME="$(jq -rn --arg n "$(jq -r .file_name "$SEED")" '$n | @uri')"
COLLECTIONS_TOKEN="$(jq -r .collections_token "$SEED")"
SHELF_ID="$(jq -r .collections.shelf "$SEED")"
SMART_SHELF_ID="$(jq -r .collections.smart "$SEED")"
readonly TOKEN PLATFORM_ID ROM_ID EXTRA_ROM_ID FILE_NAME COLLECTIONS_TOKEN SHELF_ID SMART_SHELF_ID
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
for offset in 0 1 2; do
  record "roms-page-$offset" \
    "/api/roms?platform_ids=$PLATFORM_ID&limit=1&offset=$offset&$ROM_LIST_QUERY" "${auth[@]}"
done
for offset in 0 1; do
  record "roms-favorites-page-$offset" \
    "/api/roms?platform_ids=$PLATFORM_ID&limit=1&offset=$offset&$ROM_LIST_QUERY&$FAVOURITES_QUERY" \
    "${auth[@]}"
done
record rom "/api/roms/$ROM_ID" "${auth[@]}"
record rom-extra "/api/roms/$EXTRA_ROM_ID" "${auth[@]}"
record rom-content "/api/roms/$ROM_ID/content/$FILE_NAME" "${auth[@]}"
record roms-unauthorized /api/roms
# The collections (tests/integration/seed.py): RomM's two lists, read with the token that holds
# collections.read and refused (403) to the one without it, and the first page of the shelf's and
# the smart shelf's games.
collections_auth=(-H "Authorization: Bearer $COLLECTIONS_TOKEN")
record collections /api/collections "${collections_auth[@]}"
record collections-smart /api/collections/smart "${collections_auth[@]}"
record collections-forbidden /api/collections "${auth[@]}"
record roms-collection-page-0 \
  "/api/roms?platform_ids=$PLATFORM_ID&limit=1&offset=0&$ROM_LIST_QUERY&collection_id=$SHELF_ID" \
  "${auth[@]}"
record roms-smart-collection-page-0 \
  "/api/roms?platform_ids=$PLATFORM_ID&limit=1&offset=0&$ROM_LIST_QUERY&smart_collection_id=$SMART_SHELF_ID" \
  "${auth[@]}"
# The pairing flow's answers, each secret replaced with a synthetic one.
"$(dirname "$0")/record-pairing-fixtures.sh" "$DIR" "$OUT"
# The save sync calls between two paired PSPs, with synthetic save units and device ids.
"$(dirname "$0")/record-saves-fixtures.sh" "$DIR" "$OUT"
# The install calls of a paired PSP, games sent to it by the admin, with synthetic device ids.
"$(dirname "$0")/record-installs-fixtures.sh" "$DIR" "$OUT"
