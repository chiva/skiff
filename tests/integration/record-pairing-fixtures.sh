#!/usr/bin/env bash
# Records RomM's answers to the device-code pairing flow (skiff/romm_pairing.h) for the fake
# transport: the start, a pending poll, a poll too soon (slow_down), the approved token, a poll after
# the token was handed out (expired_token) and a refused pairing (access_denied). The admin approves
# and refuses through RomM's own endpoints, as the web UI does.
#
# The codes, the token and the device id are secrets or tied to this server, so each is replaced
# with a synthetic value of the same length (Content-Length stays right). Review before committing.
# Run by record-fixtures.sh, from a container on the compose network.
# Usage: tests/integration/record-pairing-fixtures.sh <integration-dir> <fixtures-dir>
set -euo pipefail

readonly USAGE="usage: record-pairing-fixtures.sh <integration-dir> <fixtures-dir>"
readonly DIR="${1:?$USAGE}"
readonly OUT="${2:?$USAGE}"
readonly BASE="https://proxy:8443"
readonly CONNECT_TIMEOUT_SECONDS=5
readonly REQUEST_TIMEOUT_SECONDS=30
# What skiff_romm_pairing_start() sends (src/romm/pairing.c), with a synthetic identifier.
readonly INIT_BODY='{"client_device_identifier":"00112233445566778899aabbccddeeff","name":"Skiff on PSP","client":"skiff","platform":"psp","client_version":"0.0.0","requested_scopes":["platforms.read","roms.read"]}'
readonly SCOPES='["platforms.read","roms.read"]'
readonly SYNTHETIC_DEVICE_ID="00000000-0000-4000-8000-000000000001"

ADMIN_USER="$(grep '^SKIFF_ADMIN_USER=' "$DIR/romm.env" | cut -d= -f2-)"
ADMIN_PASSWORD="$(grep '^SKIFF_ADMIN_PASSWORD=' "$DIR/romm.env" | cut -d= -f2-)"
readonly ADMIN_USER ADMIN_PASSWORD
WORK="$(mktemp -d)"
readonly WORK
trap 'rm -rf "$WORK"' EXIT

# post <path> <json> [curl arguments...]: the response in $WORK/headers and $WORK/body.
post() {
  local path="$1" json="$2"
  shift 2
  curl -sS --connect-timeout "$CONNECT_TIMEOUT_SECONDS" --max-time "$REQUEST_TIMEOUT_SECONDS" \
    --http1.1 --cacert "$DIR/certs/ca.crt" -D "$WORK/headers" -o "$WORK/body" \
    -H "Content-Type: application/json" -H "Expect:" --data-binary "$json" "$@" "$BASE$path"
}

admin_post() {
  post "$1" "$2" -u "$ADMIN_USER:$ADMIN_PASSWORD" >/dev/null
}

# A synthetic stand-in as long as value: pattern repeated and cut.
same_length() {
  local value="$1" pattern="$2" out=""
  while ((${#out} < ${#value})); do out+="$pattern"; done
  printf '%s' "${out:0:${#value}}"
}

# Replaces every secret of this flow in the last response, headers included, keeps it as
# <name>.http, and fails if any secret is still in the file.
keep() {
  local name="$1" response
  response="$(grep -viE '^(date|set-cookie):' "$WORK/headers"; cat "$WORK/body")"
  for secret in "${SECRETS[@]}"; do
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

declare -a SECRETS=()
declare -A REPLACEMENTS=()
# secret <field>: reads field from the last response and registers it with a synthetic stand-in of
# the same length (pattern repeated, or fixed when given). A missing or empty field is an error: it
# would leave nothing to scrub.
secret() {
  local field="$1" pattern="$2" fixed="${3:-}" value
  value="$(jq -r --arg f "$field" '.[$f] // empty' "$WORK/body")"
  if [[ -z "$value" ]]; then
    echo "error: no $field in the response to scrub" >&2
    exit 1
  fi
  SECRETS+=("$value")
  if [[ -n "$fixed" ]]; then
    if ((${#fixed} != ${#value})); then
      echo "error: the stand-in for $field must be ${#value} characters" >&2
      exit 1
    fi
    REPLACEMENTS["$value"]="$fixed"
  else
    REPLACEMENTS["$value"]="$(same_length "$value" "$pattern")"
  fi
  printf -v "SECRET_${field^^}" '%s' "$value"
}

start() {
  post /api/auth/device/init "$INIT_BODY"
  secret device_code "skiff-device-code-"
  secret user_code "SKIFF234"
  DEVICE_CODE="$SECRET_DEVICE_CODE"
  USER_CODE="$SECRET_USER_CODE"
  INTERVAL="$(jq -r .interval "$WORK/body")"
}

poll() {
  post /api/auth/device/token "{\"device_code\":\"$DEVICE_CODE\"}"
}

mkdir -p "$OUT"
start
keep device-init
poll
keep device-pending
poll
keep device-slow-down
admin_post /api/auth/device/approve "{\"user_code\":\"$USER_CODE\",\"approved_scopes\":$SCOPES}"
sleep "$((INTERVAL + 1))"
poll
secret access_token "rmm_synthetic_pairing_token_"
secret device_id "" "$SYNTHETIC_DEVICE_ID"
keep device-token
poll
keep device-expired

start
admin_post /api/auth/device/deny "{\"user_code\":\"$USER_CODE\"}"
sleep "$((INTERVAL + 1))"
poll
keep device-denied
