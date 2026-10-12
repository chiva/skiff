#!/usr/bin/env bash
# Records RomM's answers to the install calls (skiff/romm_installs.h) for the fake transport, with a
# PSP paired through the device-code flow with every scope Skiff asks for, as the app pairs: it
# registers remote_install, the admin sends it three games as RomM's web UI does (the payload, the
# extra ROM and the seed's game of another platform), and the PSP claims them (3 taken), claims again
# (the same 3), reports done, already_installed and failed with a reason, reports done again (404),
# reports a send it has not claimed yet (409), and claims once more (none left). It then turns
# remote_install off. A second pairing that only browses is refused the claim and the register (403).
#
# Requests are built as Skiff builds them (src/romm/installs.c): the same paths and JSON. Device ids
# are replaced with synthetic ones of the same length (Content-Length stays right) and the run fails
# if any secret remains; tokens never reach a response. Request ids are RomM's random UUIDs, which
# are no secret. Run by record-fixtures.sh, from a container on the compose network, with the seed's
# game of another platform (SKIFF_FOREIGN_ROM). Review before committing.
# Usage: tests/integration/record-installs-fixtures.sh <integration-dir> <fixtures-dir>
set -euo pipefail

readonly USAGE="usage: record-installs-fixtures.sh <integration-dir> <fixtures-dir>"
readonly DIR="${1:?$USAGE}"
readonly OUT="${2:?$USAGE}"
readonly SEED="$DIR/romm.json"
readonly BASE="https://proxy:8443"
readonly CONNECT_TIMEOUT_SECONDS=5
readonly REQUEST_TIMEOUT_SECONDS=30
readonly ALL_SCOPES='["platforms.read","roms.read","assets.read","assets.write","devices.read","devices.write","collections.read"]'
readonly BROWSE_SCOPES='["platforms.read","roms.read"]'
readonly SYNTHETIC_DEVICE_D="00000000-0000-4000-8000-00000000000d"
readonly SYNTHETIC_DEVICE_E="00000000-0000-4000-8000-00000000000e"
# The reason a PSP set to Spanish sends for a game of another platform (SKIFF_ERR_ROMM_SEND_NOT_PSP).
readonly NOT_PSP_REASON="Ese juego no es de PSP [219]"

ROM_ID="$(jq -r .rom_id "$SEED")"
EXTRA_ROM_ID="$(jq -r .extra.rom_id "$SEED")"
FOREIGN_ROM_ID="$(jq -er .foreign.rom_id "$SEED")"
ADMIN_USER="$(grep '^SKIFF_ADMIN_USER=' "$DIR/romm.env" | cut -d= -f2-)"
ADMIN_PASSWORD="$(grep '^SKIFF_ADMIN_PASSWORD=' "$DIR/romm.env" | cut -d= -f2-)"
readonly ROM_ID EXTRA_ROM_ID FOREIGN_ROM_ID ADMIN_USER ADMIN_PASSWORD
WORK="$(mktemp -d)"
readonly WORK
trap 'rm -rf "$WORK"' EXIT

# Every secret of the run, checked for in each kept file; device ids, which responses carry, are
# replaced first (REPLACEMENTS), and no response may hold a token or a device code at all.
declare -a SECRETS=()
declare -A REPLACEMENTS=()

# call <method> <path> [curl arguments...]: the response in $WORK/headers and $WORK/body.
call() {
  local method="$1" path="$2"
  shift 2
  curl -sS --connect-timeout "$CONNECT_TIMEOUT_SECONDS" --max-time "$REQUEST_TIMEOUT_SECONDS" \
    --http1.1 --cacert "$DIR/certs/ca.crt" -D "$WORK/headers" -o "$WORK/body" -H "Expect:" \
    -X "$method" "$@" "$BASE$path"
}

# send_json <method> <path> <json> [curl arguments...]
send_json() {
  local method="$1" path="$2" json="$3"
  shift 3
  call "$method" "$path" -H "Content-Type: application/json" --data-binary "$json" "$@"
}

# Replaces every secret in the last response, keeps it as <name>.http, and fails if one remains.
keep() {
  local name="$1" response
  response="$(grep -viE '^(date|set-cookie):' "$WORK/headers"; cat "$WORK/body")"
  for secret in "${!REPLACEMENTS[@]}"; do
    response="${response//"$secret"/"${REPLACEMENTS[$secret]}"}"
  done
  printf '%s' "$response" >"$OUT/$name.http"
  for secret in "${SECRETS[@]}"; do
    if grep -qF -- "$secret" "$OUT/$name.http"; then
      rm -f "$OUT/$name.http"
      echo "error: a secret survived in $name.http" >&2
      exit 1
    fi
  done
  echo "recorded $name.http: $(head -n 1 "$WORK/headers" | tr -d '\r'), $(wc -c <"$WORK/body") body bytes"
}

# expect <status>: fails the run when the last response had another status.
expect() {
  local got
  got="$(head -n 1 "$WORK/headers" | cut -d' ' -f2)"
  if [[ "$got" != "$1" ]]; then
    echo "error: expected HTTP $1, got $got: $(head -c 300 "$WORK/body")" >&2
    exit 1
  fi
}

# pair <identifier> <name> <scopes> <synthetic-device-id> <token-variable> <device-variable>
pair() {
  local identifier="$1" name="$2" scopes="$3" synthetic="$4" device_code user_code interval
  send_json POST /api/auth/device/init "{\"client_device_identifier\":\"$identifier\",\"name\":\"$name\",\"client\":\"skiff\",\"platform\":\"psp\",\"client_version\":\"0.0.0\",\"requested_scopes\":$ALL_SCOPES}"
  device_code="$(jq -r .device_code "$WORK/body")"
  user_code="$(jq -r .user_code "$WORK/body")"
  interval="$(jq -r .interval "$WORK/body")"
  SECRETS+=("$device_code")
  send_json POST /api/auth/device/approve "{\"user_code\":\"$user_code\",\"approved_scopes\":$scopes}" \
    -u "$ADMIN_USER:$ADMIN_PASSWORD" >/dev/null
  sleep "$((interval + 1))"
  send_json POST /api/auth/device/token "{\"device_code\":\"$device_code\"}"
  local token device
  token="$(jq -r '.access_token // empty' "$WORK/body")"
  device="$(jq -r '.device_id // empty' "$WORK/body")"
  if [[ -z "$token" || -z "$device" || ${#device} -ne ${#synthetic} ]]; then
    echo "error: pairing $name gave no token or an unexpected device id" >&2
    exit 1
  fi
  SECRETS+=("$token" "$device")
  REPLACEMENTS["$device"]="$synthetic"
  printf -v "$5" '%s' "$token"
  printf -v "$6" '%s' "$device"
}

# register <token> <device> <true|false>: what skiff_romm_installs_register() sends.
register() {
  send_json PUT "/api/devices/$2" "{\"capabilities\":{\"remote_install\":$3}}" \
    -H "Authorization: Bearer $1"
}

# claim <token> <device>: what skiff_romm_installs_claim() sends, a POST without a body.
claim() {
  call POST "/api/devices/$2/installs/claim" -H "Content-Length: 0" -H "Authorization: Bearer $1"
}

# report <token> <device> <request-id> <json>: what skiff_romm_installs_report() sends.
report() {
  send_json PUT "/api/devices/$2/installs/$3" "$4" -H "Authorization: Bearer $1"
}

# send_game <device> <rom-id>: what RomM's web UI sends when the player ticks the PSP; prints the
# request's id.
send_game() {
  send_json POST "/api/devices/$1/installs" "{\"rom_id\":$2}" -u "$ADMIN_USER:$ADMIN_PASSWORD"
  expect 201
  jq -r .id "$WORK/body"
}

# request_for <rom-id>: the id of the last claim's request for that ROM.
request_for() {
  jq -er --argjson rom "$1" '.[] | select(.rom_id == $rom) | .id' "$WORK/body"
}

mkdir -p "$OUT"
pair 0000000000000000000000000000000d "Skiff fixture D" "$ALL_SCOPES" "$SYNTHETIC_DEVICE_D" TOKEN_D DEVICE_D
pair 0000000000000000000000000000000e "Skiff fixture E" "$BROWSE_SCOPES" "$SYNTHETIC_DEVICE_E" TOKEN_E DEVICE_E

register "$TOKEN_D" "$DEVICE_D" true
expect 200
keep installs-register

# Oldest first: one at a time, so their creation times differ.
send_game "$DEVICE_D" "$ROM_ID" >/dev/null
send_game "$DEVICE_D" "$EXTRA_ROM_ID" >/dev/null
send_game "$DEVICE_D" "$FOREIGN_ROM_ID" >/dev/null

claim "$TOKEN_D" "$DEVICE_D"
expect 200
keep installs-claim
claim "$TOKEN_D" "$DEVICE_D"
expect 200
keep installs-claim-again
PAYLOAD_REQUEST="$(request_for "$ROM_ID")"
EXTRA_REQUEST="$(request_for "$EXTRA_ROM_ID")"
FOREIGN_REQUEST="$(request_for "$FOREIGN_ROM_ID")"

report "$TOKEN_D" "$DEVICE_D" "$PAYLOAD_REQUEST" '{"status":"done"}'
expect 200
keep installs-report-done
report "$TOKEN_D" "$DEVICE_D" "$EXTRA_REQUEST" '{"status":"already_installed"}'
expect 200
keep installs-report-already-installed
report "$TOKEN_D" "$DEVICE_D" "$FOREIGN_REQUEST" "{\"status\":\"failed\",\"reason\":\"$NOT_PSP_REASON\"}"
expect 200
keep installs-report-failed
report "$TOKEN_D" "$DEVICE_D" "$PAYLOAD_REQUEST" '{"status":"done"}'
expect 404
keep installs-report-repeat

# Sent again and not claimed yet: still pending. The web UI then cancels it, as unticking does.
PENDING_REQUEST="$(send_game "$DEVICE_D" "$ROM_ID")"
report "$TOKEN_D" "$DEVICE_D" "$PENDING_REQUEST" '{"status":"done"}'
expect 409
keep installs-report-pending
call DELETE "/api/devices/$DEVICE_D/installs/$PENDING_REQUEST" -u "$ADMIN_USER:$ADMIN_PASSWORD"
expect 200

claim "$TOKEN_D" "$DEVICE_D"
expect 200
keep installs-claim-empty

register "$TOKEN_D" "$DEVICE_D" false
expect 200
keep installs-register-off

claim "$TOKEN_E" "$DEVICE_E"
expect 403
keep installs-claim-forbidden
register "$TOKEN_E" "$DEVICE_E" true
expect 403
keep installs-register-forbidden
