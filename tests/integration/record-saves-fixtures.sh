#!/usr/bin/env bash
# Records RomM's answers to the save sync calls (skiff/romm_saves.h) for the fake transport, with two
# PSPs paired through the device-code flow with every scope Skiff asks for, as the app pairs:
# PSP A negotiates against an empty server (upload), uploads, closes its session; PSP B negotiates
# with nothing local (download), downloads, confirms it, negotiates again (no_op) and uploads its own
# version; A's upload without overwrite is then refused (409), and A reads B's save (its device). A
# third pairing that only browses is refused negotiate (403).
#
# The save units are synthetic stored zips (save-unit-1.zip, save-unit-2.zip, made folders under
# the game ID SKIF00001, never a real save), written next to the fixtures for the unit and
# integration tests, the same bytes on every run. Requests are built as Skiff builds them
# (src/romm/saves.c): the same query, JSON and multipart framing. Device ids are replaced with
# synthetic ones of the same length (Content-Length stays right) and the run fails if any secret
# remains; tokens never reach a response. Run by record-fixtures.sh, from a container on the compose
# network. Review before committing.
# Usage: tests/integration/record-saves-fixtures.sh <integration-dir> <fixtures-dir>
set -euo pipefail

readonly USAGE="usage: record-saves-fixtures.sh <integration-dir> <fixtures-dir>"
readonly DIR="${1:?$USAGE}"
readonly OUT="${2:?$USAGE}"
readonly SEED="$DIR/romm.json"
readonly BASE="https://proxy:8443"
readonly CONNECT_TIMEOUT_SECONDS=5
readonly REQUEST_TIMEOUT_SECONDS=30
readonly ALL_SCOPES='["platforms.read","roms.read","assets.read","assets.write","devices.read","devices.write","collections.read"]'
readonly BROWSE_SCOPES='["platforms.read","roms.read"]'
readonly SYNTHETIC_DEVICE_A="00000000-0000-4000-8000-00000000000a"
readonly SYNTHETIC_DEVICE_B="00000000-0000-4000-8000-00000000000b"
readonly SYNTHETIC_DEVICE_C="00000000-0000-4000-8000-00000000000c"
# What the app sends (include/skiff/romm_saves.h, the saves layer's choices).
readonly GAME="SKIF00001"
readonly FILE_NAME="$GAME.zip"
readonly SLOT="autosave"
readonly EMULATOR="psp_console"
readonly KEEP_VERSIONS=10
readonly UPDATED_AT="2026-10-11T18:00:00.000Z"

ROM_ID="$(jq -r .rom_id "$SEED")"
ADMIN_USER="$(grep '^SKIFF_ADMIN_USER=' "$DIR/romm.env" | cut -d= -f2-)"
ADMIN_PASSWORD="$(grep '^SKIFF_ADMIN_PASSWORD=' "$DIR/romm.env" | cut -d= -f2-)"
readonly ROM_ID ADMIN_USER ADMIN_PASSWORD
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

# post_json <path> <json> [curl arguments...]
post_json() {
  local path="$1" json="$2"
  shift 2
  call POST "$path" -H "Content-Type: application/json" --data-binary "$json" "$@"
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

# keep_binary <name>: as keep, for a body that is not text (a save file), which bash strings cannot
# hold: nothing is replaced, so a secret anywhere fails the run.
keep_binary() {
  local name="$1"
  grep -viE '^(date|set-cookie):' "$WORK/headers" >"$OUT/$name.http"
  cat "$WORK/body" >>"$OUT/$name.http"
  for secret in "${SECRETS[@]}"; do
    if grep -qF -- "$secret" "$OUT/$name.http"; then
      rm -f "$OUT/$name.http"
      echo "error: a secret survived in $name.http" >&2
      exit 1
    fi
  done
  echo "recorded $name.http: $(head -n 1 "$WORK/headers" | tr -d '\r'), $(wc -c <"$WORK/body") body bytes"
}

# pair <identifier> <name> <scopes> <synthetic-device-id> <token-variable> <device-variable>
pair() {
  local identifier="$1" name="$2" scopes="$3" synthetic="$4" device_code user_code interval
  post_json /api/auth/device/init "{\"client_device_identifier\":\"$identifier\",\"name\":\"$name\",\"client\":\"skiff\",\"platform\":\"psp\",\"client_version\":\"0.0.0\",\"requested_scopes\":$ALL_SCOPES}"
  device_code="$(jq -r .device_code "$WORK/body")"
  user_code="$(jq -r .user_code "$WORK/body")"
  interval="$(jq -r .interval "$WORK/body")"
  SECRETS+=("$device_code")
  post_json /api/auth/device/approve "{\"user_code\":\"$user_code\",\"approved_scopes\":$scopes}" \
    -u "$ADMIN_USER:$ADMIN_PASSWORD" >/dev/null
  sleep "$((interval + 1))"
  post_json /api/auth/device/token "{\"device_code\":\"$device_code\"}"
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

# The synthetic units and their RomM content hashes (MD5 of the sorted "name:md5" lines): stored,
# files only, entries in byte order, dated 1980-01-01, as sigil writes a PSP unit.
make_units() {
  python3 - "$OUT/save-unit-1.zip" "$OUT/save-unit-2.zip" <<'PY'
import hashlib, sys, zipfile

GAME = "SKIF00001"
def unit(path, data):
    files = [
        (GAME + "DATA00/DATA.BIN", data),
        (GAME + "DATA00/PARAM.SFO", b"\x00PSF synthetic " + GAME.encode()),
        (GAME + "DATA01/DATA.BIN", b"synthetic slot one"),
        (GAME + "DATA01/PARAM.SFO", b"\x00PSF synthetic " + GAME.encode()),
    ]
    with zipfile.ZipFile(path, "w", zipfile.ZIP_STORED) as z:
        for name, content in sorted(files):
            info = zipfile.ZipInfo(name, date_time=(1980, 1, 1, 0, 0, 0))
            info.compress_type = zipfile.ZIP_STORED
            info.create_system = 0
            z.writestr(info, content)
    lines = sorted(f"{name}:{hashlib.md5(content).hexdigest()}" for name, content in files)
    print(hashlib.md5("\n".join(lines).encode()).hexdigest())

unit(sys.argv[1], b"A" * 1024)
unit(sys.argv[2], b"B" * 1024)
PY
}

# negotiate <token> <device> [<unit> <hash>]: what skiff_romm_saves_negotiate() sends, listing the
# unit as this PSP's save when given.
negotiate() {
  local token="$1" device="$2" saves="[]"
  if (($# > 2)); then
    saves="[{\"rom_id\":$ROM_ID,\"file_name\":\"$FILE_NAME\",\"slot\":\"$SLOT\",\"emulator\":\"$EMULATOR\",\"content_hash\":\"$4\",\"updated_at\":\"$UPDATED_AT\",\"file_size_bytes\":$(wc -c <"$3")}]"
  fi
  post_json /api/sync/negotiate "{\"device_id\":\"$device\",\"rom_ids\":[$ROM_ID],\"saves\":$saves}" \
    -H "Authorization: Bearer $token"
}

# upload <token> <device> <unit> <hash> <overwrite>: what skiff_romm_saves_upload() sends.
upload() {
  local token="$1" device="$2" unit="$3" hash="$4" overwrite="$5" boundary
  boundary="skiff-$hash"
  {
    printf -- '--%s\r\nContent-Disposition: form-data; name="saveFile"; filename="%s"\r\n' \
      "$boundary" "$FILE_NAME"
    printf 'Content-Type: application/zip\r\n\r\n'
    cat "$unit"
    printf '\r\n--%s--\r\n' "$boundary"
  } >"$WORK/multipart"
  call POST "/api/saves?rom_id=$ROM_ID&emulator=$EMULATOR&slot=$SLOT&device_id=$device&content_hash=$hash&overwrite=$overwrite&autocleanup=true&autocleanup_limit=$KEEP_VERSIONS" \
    -H "Authorization: Bearer $token" -H "Content-Type: multipart/form-data; boundary=$boundary" \
    --data-binary "@$WORK/multipart"
}

mkdir -p "$OUT"
mapfile -t HASHES < <(make_units)
readonly UNIT_1="$OUT/save-unit-1.zip" UNIT_2="$OUT/save-unit-2.zip"
readonly HASH_1="${HASHES[0]}" HASH_2="${HASHES[1]}"
echo "units: save-unit-1.zip $HASH_1, save-unit-2.zip $HASH_2"

pair 0000000000000000000000000000000a "Skiff fixture A" "$ALL_SCOPES" "$SYNTHETIC_DEVICE_A" TOKEN_A DEVICE_A
pair 0000000000000000000000000000000b "Skiff fixture B" "$ALL_SCOPES" "$SYNTHETIC_DEVICE_B" TOKEN_B DEVICE_B
pair 0000000000000000000000000000000c "Skiff fixture C" "$BROWSE_SCOPES" "$SYNTHETIC_DEVICE_C" TOKEN_C DEVICE_C

negotiate "$TOKEN_A" "$DEVICE_A" "$UNIT_1" "$HASH_1"
keep saves-negotiate-upload
SESSION_A="$(jq -r .session_id "$WORK/body")"
upload "$TOKEN_A" "$DEVICE_A" "$UNIT_1" "$HASH_1" false
keep saves-upload
SAVE_1="$(jq -r .id "$WORK/body")"
post_json "/api/sync/sessions/$SESSION_A/complete" '{"operations_completed":1,"operations_failed":0}' \
  -H "Authorization: Bearer $TOKEN_A"
keep saves-complete

negotiate "$TOKEN_B" "$DEVICE_B"
keep saves-negotiate-download
call GET "/api/saves/$SAVE_1/content?device_id=$DEVICE_B&optimistic=false" \
  -H "Authorization: Bearer $TOKEN_B"
keep_binary saves-content
post_json "/api/saves/$SAVE_1/downloaded" "{\"device_id\":\"$DEVICE_B\",\"content_hash\":\"$HASH_1\"}" \
  -H "Authorization: Bearer $TOKEN_B"
keep saves-downloaded
negotiate "$TOKEN_B" "$DEVICE_B" "$UNIT_1" "$HASH_1"
keep saves-negotiate-no-op
# RomM keeps times to the second and takes a slot's newest save as its head: B's must not tie A's.
sleep 1
upload "$TOKEN_B" "$DEVICE_B" "$UNIT_2" "$HASH_2" false
keep saves-upload-second-device
SAVE_2="$(jq -r .id "$WORK/body")"

upload "$TOKEN_A" "$DEVICE_A" "$UNIT_1" "$HASH_1" false
keep saves-upload-conflict
# What A's conflict screen shows of B's save: the device that uploaded it.
call GET "/api/saves/$SAVE_2?device_id=$DEVICE_A" -H "Authorization: Bearer $TOKEN_A"
keep saves-get

negotiate "$TOKEN_C" "$DEVICE_C"
keep saves-negotiate-forbidden
