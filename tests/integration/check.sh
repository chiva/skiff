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
readonly HTTP_ACCEPTED=202
readonly HTTP_PARTIAL=206
readonly HTTP_UNAUTHORIZED=401
readonly HTTP_FORBIDDEN=403
readonly HTTP_NOT_ACCEPTABLE=406
# Download conversion (seed.py's convert discs): what RomM 5.4 answers while it converts, and how
# long the slow disc's conversion may take in all (rom-converto-delay.sh holds it back 20 s).
readonly RETRY_AFTER_SECONDS=30
readonly CONVERT_POLL_SECONDS=2
readonly CONVERT_POLLS_MAX=30
# A CSO (CISO v1) header: the magic, then the uncompressed size as 8 little-endian bytes at 8.
readonly CSO_MAGIC="CISO"
readonly CSO_SIZE_AT=8
# nginx's ETag holds the file's modification time in whole seconds.
readonly ETAG_CLOCK_SECONDS=1.1

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

# A response header's value (the last one of that name), from a curl -D file.
header() {
  awk -v name="$(tr '[:upper:]' '[:lower:]' <<<"$1"):" 'tolower($1) == name {sub(/^[^:]*: */, ""); value = $0} END {print value}' \
    "$2" | tr -d '\r'
}

# The download path of a seeded file, its name percent-encoded.
content_path() {
  echo "/api/roms/$1/content/$(jq -rn --arg n "$2" '$n | @uri')"
}

# Plain HTTP and TLS reach the same RomM.
[[ "$(curl -sS -o "$WORK/body" -w '%{http_code}' "http://$PROXY:8080/api/heartbeat")" == "$HTTP_OK" ]] ||
  fail "plain HTTP heartbeat"
ok "plain HTTP heartbeat answers"

[[ "$(tls_status /api/heartbeat)" == "$HTTP_OK" ]] || fail "HTTPS heartbeat with the test CA"
[[ "$(heartbeat_version)" == "$EXPECTED_ROMM_VERSION" ]] ||
  fail "RomM is $(heartbeat_version), compose.yaml pins $EXPECTED_ROMM_VERSION"
cp "$WORK/body" "$WORK/heartbeat"
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

# AES-only site: TLS 1.2 with AES-128-GCM and nothing else, so the transport's fallback from
# ChaCha20 is tested against a server that really lacks it.
aes_only() {
  curl -sS -o /dev/null -w '%{http_code}' --cacert "$CERTS/ca.crt" "$@" \
    "https://$PROXY:8445/api/heartbeat" 2>/dev/null || true
}
[[ "$(aes_only --tlsv1.2 --tls-max 1.2 --ciphers ECDHE-ECDSA-AES128-GCM-SHA256)" == "$HTTP_OK" ]] ||
  fail "the AES-only site refused AES-128-GCM over TLS 1.2"
ok "AES-only site accepts AES-128-GCM over TLS 1.2"
[[ "$(aes_only --tlsv1.3)" != "$HTTP_OK" ]] || fail "the AES-only site accepted TLS 1.3"
ok "AES-only site refuses TLS 1.3"
[[ "$(aes_only --tls-max 1.2 --ciphers ECDHE-ECDSA-CHACHA20-POLY1305)" != "$HTTP_OK" ]] ||
  fail "the AES-only site accepted ChaCha20-Poly1305"
ok "AES-only site refuses ChaCha20-Poly1305"

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
etag="$(header etag "$WORK/headers")"
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

# Download conversion (RomM 5.4, rom-converto): a file is served as stored when its format is
# listed, converted into the first listed format RomM can make otherwise; 202 while it converts, 406
# when no listed format can be served.
jq -e '.CONVERTO.ENABLED == true' "$WORK/heartbeat" >/dev/null ||
  fail "the heartbeat does not report download conversion (CONVERTO.ENABLED)"
ok "the heartbeat reports download conversion"

[[ "$(tls_status "$content?format=iso,cso" "${auth[@]}")" == "$HTTP_OK" ]] ||
  fail "downloading the payload with ?format=iso,cso"
[[ "$(sha1sum <"$WORK/body" | cut -d' ' -f1)" == "$SHA1" ]] ||
  fail "?format=iso,cso did not serve the stored ISO as it is"
ok "?format= listing the stored format serves the stored file as it is"

for role in fast slow; do
  disc_id="$(jq -r ".convert.$role.rom_id" "$SEED")"
  [[ "$(tls_status "/api/roms/$disc_id" "${auth[@]}")" == "$HTTP_OK" ]] ||
    fail "reading convert disc ROM $disc_id"
  jq -e '.download_formats | index("cso") and index("zso") and index("iso")' "$WORK/body" >/dev/null ||
    fail "ROM $disc_id is offered as $(jq -c .download_formats "$WORK/body"), not cso, zso and iso"
done
ok "both convert discs are offered as cso, zso and iso (download_formats)"

disc_id="$(jq -r .convert.fast.rom_id "$SEED")"
disc="$(content_path "$disc_id" "$(jq -r .convert.fast.file_name "$SEED")")"
iso_size="$(jq -r .convert.fast.iso.size "$SEED")"
[[ "$(tls_status "$disc?format=chd" "${auth[@]}")" == "$HTTP_OK" ]] || fail "?format=chd on a CHD"
[[ "$(sha1sum <"$WORK/body" | cut -d' ' -f1)" == "$(jq -r .convert.fast.sha1 "$SEED")" ]] ||
  fail "?format=chd did not serve the stored CHD"
ok "?format=chd serves the stored CHD"

[[ "$(tls_status "$disc?format=rvz" "${auth[@]}")" == "$HTTP_NOT_ACCEPTABLE" ]] ||
  fail "?format=rvz (no conversion makes one from a PSP CHD) must be $HTTP_NOT_ACCEPTABLE"
ok "?format= with no format RomM can make is refused ($HTTP_NOT_ACCEPTABLE)"

# HEAD reports what a GET would serve but never starts a conversion.
[[ "$(tls_status "$disc?format=cso" "${auth[@]}" --head -D "$WORK/headers")" == "$HTTP_ACCEPTED" ]] ||
  fail "HEAD ?format=cso before any conversion must be $HTTP_ACCEPTED"
[[ "$(header retry-after "$WORK/headers")" == "$RETRY_AFTER_SECONDS" ]] ||
  fail "HEAD $HTTP_ACCEPTED came with Retry-After '$(header retry-after "$WORK/headers")'"
ok "HEAD ?format=cso before converting: $HTTP_ACCEPTED, Retry-After $RETRY_AFTER_SECONDS"

# The fast disc converts within RomM's 15 s hold, so the first GET brings the CSO.
[[ "$(tls_status "$disc?format=cso" "${auth[@]}" -D "$WORK/headers")" == "$HTTP_OK" ]] ||
  fail "GET ?format=cso on the fast disc"
[[ "$(head -c 4 "$WORK/body")" == "$CSO_MAGIC" ]] || fail "the converted file is not a CSO"
cso_iso_size="$(od -An -t u8 -j "$CSO_SIZE_AT" -N 8 "$WORK/body" | tr -d ' ')"
[[ "$cso_iso_size" == "$iso_size" ]] ||
  fail "the CSO holds $cso_iso_size bytes of disc, the seeded ISO has $iso_size"
header content-disposition "$WORK/headers" | grep -q '\.cso' ||
  fail "the converted download is not named .cso: $(header content-disposition "$WORK/headers")"
cso_etag="$(header etag "$WORK/headers")"
[[ -n "$cso_etag" ]] || fail "the converted download has no ETag"
[[ "$(header content-length "$WORK/headers")" == "$(wc -c <"$WORK/body" | tr -d ' ')" ]] ||
  fail "the converted download's Content-Length is not its size"
cp "$WORK/body" "$WORK/cso"
ok "GET ?format=cso converts the CHD: a CSO of the seeded ISO ($iso_size bytes), $(wc -c <"$WORK/cso" | tr -d ' ') bytes, named .cso, ETag $cso_etag"

# Resuming a converted download. RomM refreshes a served copy's modification time on every GET (its
# cache cleanup measures demand by it), which nginx's ETag is made of: once a second has passed,
# If-Range may no longer match and the whole file comes back. Either answer is safe; this records
# which one this RomM gives.
sleep "$ETAG_CLOCK_SECONDS"
status="$(tls_status "$disc?format=cso" "${auth[@]}" "${range[@]}" -H "If-Range: $cso_etag" -D "$WORK/headers")"
case "$status" in
"$HTTP_PARTIAL")
  cmp -s "$WORK/body" <(tail -c +"$((RANGE_START + 1))" "$WORK/cso") ||
    fail "the resumed converted bytes do not match"
  ok "a converted download resumes with If-Range ($HTTP_PARTIAL)"
  ;;
"$HTTP_OK")
  cmp -s "$WORK/body" "$WORK/cso" || fail "the restarted converted download is not the same CSO"
  ok "a converted download restarts on resume ($HTTP_OK): ETag $cso_etag became $(header etag "$WORK/headers")"
  ;;
*) fail "resuming the converted download returned HTTP $status" ;;
esac

# The slow disc's conversion outlasts the 15 s hold: 202 with Retry-After, then the CSO once ready.
slow="$(content_path "$(jq -r .convert.slow.rom_id "$SEED")" "$(jq -r .convert.slow.file_name "$SEED")")"
[[ "$(tls_status "$slow?format=cso" "${auth[@]}" -D "$WORK/headers")" == "$HTTP_ACCEPTED" ]] ||
  fail "GET ?format=cso on the slow disc must be $HTTP_ACCEPTED while it converts"
[[ "$(header retry-after "$WORK/headers")" == "$RETRY_AFTER_SECONDS" && ! -s "$WORK/body" ]] ||
  fail "the $HTTP_ACCEPTED came with Retry-After '$(header retry-after "$WORK/headers")' and $(wc -c <"$WORK/body") body bytes"
ok "GET ?format=cso while converting: $HTTP_ACCEPTED, Retry-After $RETRY_AFTER_SECONDS, no body"
for ((poll = 1; ; poll++)); do
  status="$(tls_status "$slow?format=cso" "${auth[@]}")"
  [[ "$status" == "$HTTP_ACCEPTED" && "$poll" -lt "$CONVERT_POLLS_MAX" ]] || break
  sleep "$CONVERT_POLL_SECONDS"
done
[[ "$status" == "$HTTP_OK" && "$(head -c 4 "$WORK/body")" == "$CSO_MAGIC" ]] ||
  fail "the slow disc's conversion ended with HTTP $status after $poll polls"
ok "the conversion went on after the $HTTP_ACCEPTED: the CSO came after $poll more request(s)"

echo "SKIFF INTEGRATION SERVER OK"
