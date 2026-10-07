#!/usr/bin/env bash
# Builds the host integration tests (transport, resumable downloads, RomM client, pairing) and runs them against the
# integration RomM started by `scripts/dev.sh romm-up`, from a container on the compose network
# (`scripts/dev.sh romm-test`). The seeded file, its CRC-32, the API token and the certificate paths
# reach the tests as SKIFF_IT_* variables.
# Usage: tests/integration/transport-test.sh <integration-dir>
set -euo pipefail

readonly DIR="${1:?usage: transport-test.sh <integration-dir>}"
readonly SEED="$DIR/romm.json"
readonly BUILD_DIR="build/host"
readonly TARGETS=(test_transport_romm test_download_romm test_client_romm test_pairing_romm)

cmake --preset host >/dev/null
cmake --build "$BUILD_DIR" --target "${TARGETS[@]}"

SKIFF_IT_CERTS="$DIR/certs"
SKIFF_IT_TOKEN="$(jq -r .token "$SEED")"
SKIFF_IT_ROM_ID="$(jq -r .rom_id "$SEED")"
SKIFF_IT_FILE_NAME="$(jq -rn --arg n "$(jq -r .file_name "$SEED")" '$n | @uri')"
SKIFF_IT_SIZE="$(jq -r .size "$SEED")"
SKIFF_IT_CRC32="$(jq -r .crc32 "$SEED")"
# The RomM client test takes names as stored and builds its own URLs; the second seeded file's name
# needs percent-encoding.
SKIFF_IT_FILE_NAME_RAW="$(jq -r .file_name "$SEED")"
SKIFF_IT_EXTRA_ROM_ID="$(jq -r .extra.rom_id "$SEED")"
SKIFF_IT_EXTRA_FILE_NAME_RAW="$(jq -r .extra.file_name "$SEED")"
SKIFF_IT_EXTRA_SIZE="$(jq -r .extra.size "$SEED")"
SKIFF_IT_EXTRA_CRC32="$(jq -r .extra.crc32 "$SEED")"
export SKIFF_IT_CERTS SKIFF_IT_TOKEN SKIFF_IT_ROM_ID SKIFF_IT_FILE_NAME SKIFF_IT_SIZE SKIFF_IT_CRC32
export SKIFF_IT_FILE_NAME_RAW SKIFF_IT_EXTRA_ROM_ID SKIFF_IT_EXTRA_FILE_NAME_RAW SKIFF_IT_EXTRA_SIZE
export SKIFF_IT_EXTRA_CRC32
# The pairing test approves and refuses pairings as the admin, as RomM's web UI does.
ADMIN_USER="$(grep '^SKIFF_ADMIN_USER=' "$DIR/romm.env" | cut -d= -f2-)"
ADMIN_PASSWORD="$(grep '^SKIFF_ADMIN_PASSWORD=' "$DIR/romm.env" | cut -d= -f2-)"
SKIFF_IT_ADMIN_AUTHORIZATION="Basic $(printf '%s:%s' "$ADMIN_USER" "$ADMIN_PASSWORD" | base64 -w0)"
export SKIFF_IT_ADMIN_AUTHORIZATION

for target in "${TARGETS[@]}"; do
  "$BUILD_DIR/tests/$target"
done
echo "SKIFF TRANSPORT INTEGRATION OK"
