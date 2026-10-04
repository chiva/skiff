#!/usr/bin/env bash
# Acceptance checks for the integration RomM started by `scripts/dev.sh romm-up`: what the PSP
# client will rely on, checked with curl from a container on the compose network. Prints one line per
# check and stops at the first failure.
# Usage: tests/integration/check.sh <integration-dir>   (scripts/dev.sh romm-check runs it)
set -euo pipefail

readonly DIR="${1:?usage: check.sh <integration-dir>}"
readonly CERTS="$DIR/certs"
readonly SEED="$DIR/romm.json"
readonly PROXY="proxy"
# The RomM tag pinned in compose.yaml, so a version bump needs no edit here.
EXPECTED_ROMM_VERSION="$(sed -n 's|.*image: rommapp/romm:\([^@]*\)@.*|\1|p' \
  "$(dirname "$0")/compose.yaml")"
readonly EXPECTED_ROMM_VERSION
readonly RANGE_START=1000
# Bounds for every network call, so a stalled proxy fails the run instead of hanging it.
readonly CONNECT_TIMEOUT_SECONDS=5
readonly REQUEST_TIMEOUT_SECONDS=30
readonly CURL_TLS_VERIFY_FAILED=60
readonly HTTP_OK=200
readonly HTTP_PARTIAL=206
readonly HTTP_UNAUTHORIZED=401
readonly HTTP_FORBIDDEN=403

TOKEN="$(jq -r .token "$SEED")"
ROM_ID="$(jq -r .rom_id "$SEED")"
FILE_NAME="$(jq -r .file_name "$SEED")"
SIZE="$(jq -r .size "$SEED")"
SHA1="$(jq -r .sha1 "$SEED")"
readonly TOKEN ROM_ID FILE_NAME SIZE SHA1
WORK="$(mktemp -d)"
readonly WORK
trap 'rm -rf "$WORK"' EXIT

curl() {
  command curl --connect-timeout "$CONNECT_TIMEOUT_SECONDS" --max-time "$REQUEST_TIMEOUT_SECONDS" "$@"
}

fail() {
  echo "FAIL: $*" >&2
  exit 1
}

ok() {
  echo "check: $* ... ok"
}

# HTTP status of a GET over the TLS site; extra curl arguments follow the path.
tls_status() {
  local path="$1"
  shift
  curl -sS -o "$WORK/body" -w '%{http_code}' --cacert "$CERTS/ca.crt" "$@" "https://$PROXY:8443$path"
}

heartbeat_version() {
  jq -r .SYSTEM.VERSION "$WORK/body"
}

# Plain HTTP and TLS reach the same RomM.
[[ "$(curl -sS -o "$WORK/body" -w '%{http_code}' "http://$PROXY:8080/api/heartbeat")" == "$HTTP_OK" ]] ||
  fail "plain HTTP heartbeat"
ok "plain HTTP heartbeat answers"

[[ "$(tls_status /api/heartbeat)" == "$HTTP_OK" ]] || fail "HTTPS heartbeat with the test CA"
[[ "$(heartbeat_version)" == "$EXPECTED_ROMM_VERSION" ]] ||
  fail "RomM is $(heartbeat_version), compose.yaml pins $EXPECTED_ROMM_VERSION"
ok "HTTPS heartbeat trusted through the test CA, RomM $EXPECTED_ROMM_VERSION"

# The PSP links Mbed TLS 4.1 with TLS 1.2 and 1.3; both must work through the proxy.
for version in 1.2 1.3; do
  [[ "$(tls_status /api/heartbeat --tlsv"$version" --tls-max "$version" --http1.1)" == "$HTTP_OK" ]] ||
    fail "TLS $version handshake"
  ok "TLS $version over HTTP/1.1"
done

# A PSP connecting by IP address sends no SNI; the proxy must still present the test certificate.
timeout "$REQUEST_TIMEOUT_SECONDS" openssl s_client -connect "$PROXY:8443" -noservername -CAfile "$CERTS/ca.crt" -verify_return_error \
  </dev/null >/dev/null 2>&1 || fail "a TLS client that sends no SNI must get the test certificate"
ok "a TLS client that sends no SNI (a PSP connecting by IP) gets the test certificate"

status=0
curl -sS -o /dev/null "https://$PROXY:8443/api/heartbeat" 2>/dev/null || status=$?
[[ "$status" == "$CURL_TLS_VERIFY_FAILED" ]] ||
  fail "a client without the test CA must fail verification" \
    "(curl exit $CURL_TLS_VERIFY_FAILED, got $status)"
ok "a client without the test CA is refused (certificate not trusted)"

# mTLS site: only client certificates from the test CA get through.
mtls() {
  curl -sS -o /dev/null -w '%{http_code}' --cacert "$CERTS/ca.crt" "$@" \
    "https://$PROXY:8444/api/heartbeat" 2>/dev/null || true
}
[[ "$(mtls)" != "$HTTP_OK" ]] || fail "the mTLS site accepted a request without a client certificate"
ok "mTLS site refuses a request without a client certificate"
status="$(mtls --cert "$CERTS/client-wrong-ca.crt" --key "$CERTS/client-wrong-ca.key")"
[[ "$status" != "$HTTP_OK" ]] ||
  fail "the mTLS site accepted a client certificate from an untrusted CA"
ok "mTLS site refuses a client certificate from an untrusted CA"
for type in ecdsa rsa; do
  [[ "$(mtls --cert "$CERTS/client-$type.crt" --key "$CERTS/client-$type.key")" == "$HTTP_OK" ]] ||
    fail "the mTLS site refused the $type client certificate"
  ok "mTLS site accepts the $type client certificate"
done

# API token: required, and enough to browse and download.
status="$(tls_status /api/roms)"
[[ "$status" == "$HTTP_UNAUTHORIZED" || "$status" == "$HTTP_FORBIDDEN" ]] ||
  fail "listing ROMs without a token returned HTTP $status"
ok "listing ROMs without a token is refused (HTTP $status)"

auth=(-H "Authorization: Bearer $TOKEN")
[[ "$(tls_status "/api/roms/$ROM_ID" "${auth[@]}")" == "$HTTP_OK" ]] ||
  fail "reading ROM $ROM_ID with the token"
for field in fs_size_bytes:"$SIZE" sha1_hash:"$SHA1" crc_hash:"$(jq -r .crc32 "$SEED")" \
  md5_hash:"$(jq -r .md5 "$SEED")"; do
  [[ "$(jq -r ".${field%%:*}" "$WORK/body")" == "${field#*:}" ]] ||
    fail "ROM $ROM_ID ${field%%:*} is $(jq -r ".${field%%:*}" "$WORK/body"), expected ${field#*:}"
done
ok "ROM $ROM_ID reports the seeded size and CRC32, MD5 and SHA-1"

content="/api/roms/$ROM_ID/content/$(jq -rn --arg n "$FILE_NAME" '$n | @uri')"
[[ "$(tls_status "$content" "${auth[@]}" -D "$WORK/headers")" == "$HTTP_OK" ]] ||
  fail "downloading the file"
[[ "$(sha1sum <"$WORK/body" | cut -d' ' -f1)" == "$SHA1" ]] ||
  fail "downloaded bytes do not match the seed"
etag="$(awk 'tolower($1) == "etag:" {print $2}' "$WORK/headers" | tr -d '\r')"
[[ -n "$etag" ]] || fail "the download has no ETag, so resuming could not use If-Range"
ok "download matches the seeded SHA-1, ETag $etag"

# Resume: a Range with the matching ETag continues; a stale ETag restarts from byte 0.
cp "$WORK/body" "$WORK/full"
range=(-H "Range: bytes=$RANGE_START-")
[[ "$(tls_status "$content" "${auth[@]}" "${range[@]}" -H "If-Range: $etag")" == "$HTTP_PARTIAL" ]] ||
  fail "a Range request with the current ETag must return $HTTP_PARTIAL"
cmp -s "$WORK/body" <(tail -c +"$((RANGE_START + 1))" "$WORK/full") ||
  fail "the ranged bytes do not match"
ok "Range from byte $RANGE_START with If-Range (current ETag) resumes ($HTTP_PARTIAL)"
[[ "$(tls_status "$content" "${auth[@]}" "${range[@]}" -H 'If-Range: "stale"' -D "$WORK/headers")" == \
  "$HTTP_OK" ]] || fail "a Range request with a stale ETag must return the whole file ($HTTP_OK)"
cmp -s "$WORK/body" "$WORK/full" || fail "the restarted download is not the whole file"
! grep -qi '^content-range:' "$WORK/headers" || fail "the restarted download is marked partial"
ok "Range with a stale If-Range ETag restarts with the whole file ($HTTP_OK)"

echo "SKIFF INTEGRATION SERVER OK"
