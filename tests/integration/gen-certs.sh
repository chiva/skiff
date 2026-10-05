#!/usr/bin/env bash
# Test PKI for the integration server: a test CA, a server certificate for the proxy, and client
# certificates for mTLS (ECDSA P-256 and RSA-2048, the two types the PSP measurements compare), plus
# one from an unrelated CA that the proxy must reject. Runs in the skiff-host image (OpenSSL 3).
# Existing files are kept while valid, so copies on a PSP's Memory Stick keep working; the server
# certificate is reissued when its addresses change (another LAN IP).
# Usage: tests/integration/gen-certs.sh <out-dir> [<lan-ip>]
# Keys are test material: they live under build/ (git-ignored) and are never committed.
set -euo pipefail

readonly OUT="${1:?usage: gen-certs.sh <out-dir> [<lan-ip>]}"
readonly LAN_IP="${2:-}"
readonly CA_DAYS=365
readonly LEAF_DAYS=90
# Reissue anything that expires within a day, so a run never starts with a dying certificate.
readonly RENEW_SECONDS=86400
readonly CA_SUBJECT="/CN=Skiff Test CA"
readonly WRONG_CA_SUBJECT="/CN=Skiff Untrusted CA"

# The proxy is reached as localhost from the host, as `proxy` from containers on the compose
# network, and by IP from a PSP on the LAN.
SERVER_SAN="DNS:localhost,DNS:proxy,IP:127.0.0.1"
if [[ -n "$LAN_IP" ]]; then
  SERVER_SAN+=",IP:$LAN_IP"
fi
readonly SERVER_SAN

mkdir -p "$OUT"
cd "$OUT"
umask 077

valid() {
  [[ -f "$1" ]] && openssl x509 -in "$1" -noout -checkend "$RENEW_SECONDS" >/dev/null
}

new_key() {
  local key="$1" type="$2"
  case "$type" in
  ecdsa) openssl genpkey -quiet -algorithm EC -pkeyopt ec_paramgen_curve:P-256 -out "$key" ;;
  rsa) openssl genpkey -quiet -algorithm RSA -pkeyopt rsa_keygen_bits:2048 -out "$key" ;;
  esac
}

new_ca() {
  local name="$1" subject="$2"
  new_key "$name.key" ecdsa
  openssl req -x509 -new -key "$name.key" -out "$name.crt" -days "$CA_DAYS" -subj "$subject" \
    -addext "basicConstraints=critical,CA:TRUE" -addext "keyUsage=critical,keyCertSign,cRLSign"
}

# new_leaf <name> <key-type> <subject> <extensions> <ca-name>
new_leaf() {
  local name="$1" type="$2" subject="$3" extensions="$4" ca="$5"
  new_key "$name.key" "$type"
  openssl req -new -key "$name.key" -subj "$subject" -out "$name.csr"
  # OpenSSL 3.0 reports a successful signature on stderr; show its output only when it fails.
  local output
  if ! output="$(openssl x509 -req -in "$name.csr" -CA "$ca.crt" -CAkey "$ca.key" -CAcreateserial \
    -days "$LEAF_DAYS" -out "$name.crt" -extfile <(printf '%s\n' "$extensions") 2>&1)"; then
    echo "$output" >&2
    return 1
  fi
  rm -f "$name.csr"
}

client_extensions() {
  printf '%s\n' "basicConstraints=critical,CA:FALSE" "keyUsage=critical,digitalSignature" \
    "extendedKeyUsage=clientAuth"
}

if ! valid ca.crt; then
  # Everything else chains to the CA, so a new CA reissues all of it.
  rm -f ./*.crt ./*.key ./*.srl server.san
  new_ca ca "$CA_SUBJECT"
  echo "gen-certs: new test CA (copy ca.crt to the PSP again)" >&2
fi

if ! valid server.crt || [[ "$(cat server.san 2>/dev/null)" != "$SERVER_SAN" ]]; then
  new_leaf server ecdsa "/CN=skiff-test-server" "$(printf '%s\n' \
    "basicConstraints=critical,CA:FALSE" "keyUsage=critical,digitalSignature" \
    "extendedKeyUsage=serverAuth" "subjectAltName=$SERVER_SAN")" ca
  printf '%s' "$SERVER_SAN" >server.san
  echo "gen-certs: server certificate for $SERVER_SAN" >&2
fi

for type in ecdsa rsa; do
  if ! valid "client-$type.crt"; then
    new_leaf "client-$type" "$type" "/CN=skiff-test-client-$type" "$(client_extensions)" ca
    echo "gen-certs: client certificate client-$type" >&2
  fi
done

if ! valid wrong-ca.crt || ! valid client-wrong-ca.crt; then
  new_ca wrong-ca "$WRONG_CA_SUBJECT"
  new_leaf client-wrong-ca ecdsa "/CN=skiff-test-client-wrong-ca" "$(client_extensions)" wrong-ca
  echo "gen-certs: client certificate from an untrusted CA" >&2
fi

# Certificates are public; keys stay readable by their owner only.
chmod 644 ./*.crt
