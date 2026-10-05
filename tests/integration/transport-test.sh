#!/usr/bin/env bash
# Builds the host transport integration test and runs it against the integration RomM started by
# `scripts/dev.sh romm-up`, from a container on the compose network (`scripts/dev.sh romm-test`).
# The seeded file, the API token and the certificate paths reach the test as SKIFF_IT_* variables.
# Usage: tests/integration/transport-test.sh <integration-dir>
set -euo pipefail

readonly DIR="${1:?usage: transport-test.sh <integration-dir>}"
readonly SEED="$DIR/romm.json"
readonly BUILD_DIR="build/host"
readonly TARGET="test_transport_romm"

cmake --preset host >/dev/null
cmake --build "$BUILD_DIR" --target "$TARGET"

SKIFF_IT_CERTS="$DIR/certs" \
  SKIFF_IT_TOKEN="$(jq -r .token "$SEED")" \
  SKIFF_IT_ROM_ID="$(jq -r .rom_id "$SEED")" \
  SKIFF_IT_FILE_NAME="$(jq -rn --arg n "$(jq -r .file_name "$SEED")" '$n | @uri')" \
  SKIFF_IT_SIZE="$(jq -r .size "$SEED")" \
  "$BUILD_DIR/tests/$TARGET"
echo "SKIFF TRANSPORT INTEGRATION OK"
