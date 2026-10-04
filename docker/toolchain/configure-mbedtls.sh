#!/bin/sh
# Applies Skiff's build profile to an unpacked Mbed TLS 4.x source tree by editing its default config
# headers in place. Editing the headers (rather than passing MBEDTLS_USER_CONFIG_FILE) means the
# installed headers carry the profile, so libcurl, Skiff and mbedtls itself are always compiled
# against the same struct layouts.
#
# Uses only sed and grep from the pinned base image instead of Mbed TLS's scripts/config.py, so the
# image build installs no unpinned tools. Every edit must change exactly one line, so an option that
# an mbedtls update renamed or removed stops the build instead of being skipped silently.
# Usage: configure-mbedtls.sh <mbedtls-source-dir>
set -eu

cd "${1:?usage: configure-mbedtls.sh <mbedtls-source-dir>}"
readonly TLS_CONFIG=include/mbedtls/mbedtls_config.h
readonly CRYPTO_CONFIG=tf-psa-crypto/include/psa/crypto_config.h

count_lines() { cat "$TLS_CONFIG" "$CRYPTO_CONFIG" | grep -cE "$1" || true; }

# Fails unless <name> appears exactly once across the config headers, in the given state (on/off).
require_state() {
  name="$1"
  state="$2"
  enabled="$(count_lines "^#define ${name}([[:space:]]|\$)")"
  disabled="$(count_lines "^//[[:space:]]*#define ${name}([[:space:]]|\$)")"
  case "$state" in
  on) expected_enabled=1 expected_disabled=0 ;;
  off) expected_enabled=0 expected_disabled=1 ;;
  esac
  if [ "$enabled" -ne "$expected_enabled" ] || [ "$disabled" -ne "$expected_disabled" ]; then
    echo "error: $name must be present once and $state" \
      "(found $enabled enabled and $disabled disabled lines)" >&2
    exit 1
  fi
}

# Rewrites the one line "<from><name>" in whichever config header holds it to "<to><name>". Writes
# through a temporary file rather than `sed -i`, whose syntax differs between GNU, BusyBox and BSD
# sed (BSD would take -E as a backup suffix and silently match nothing).
edit_option() {
  name="$1"
  from="$2"
  to="$3"
  pattern="^${from}${name}([[:space:]]|\$)"
  matches="$(count_lines "$pattern")"
  if [ "$matches" -ne 1 ]; then
    echo "error: expected exactly one '${from}${name}' line in the config headers, found $matches" >&2
    exit 1
  fi
  for header in "$TLS_CONFIG" "$CRYPTO_CONFIG"; do
    sed -E "s@${pattern}@${to}${name}\\1@" "$header" >"$header.skiff-tmp"
    mv "$header.skiff-tmp" "$header"
  done
}
# Each edit is verified afterwards, so a sed that matched nothing can never pass silently.
turn_on() {
  edit_option "$1" '//#define ' '#define '
  require_state "$1" on
}
turn_off() {
  edit_option "$1" '#define ' '//#define '
  require_state "$1" off
}

# Entropy (release blocker, see docs/development/architecture.md "Randomness for TLS"). mbedtls's
# built-in sources only support Unix and Windows. Skiff supplies mbedtls_platform_get_entropy(); any
# EBOOT that links mbedtls without it fails to link.
turn_off MBEDTLS_PSA_BUILTIN_GET_ENTROPY
turn_on MBEDTLS_PSA_DRIVER_GET_ENTROPY

# mbedtls only knows how to read a millisecond clock on Unix and Windows. Skiff supplies
# mbedtls_ms_time() (src/platform/psp/mbedtls_time.c); TLS 1.3 uses it to age session tickets.
turn_on MBEDTLS_PLATFORM_MS_TIME_ALT

# Persistent PSA keys: Skiff keeps none, and the file backend would write into the working directory.
turn_off MBEDTLS_PSA_CRYPTO_STORAGE_C
turn_off MBEDTLS_PSA_ITS_FILE_C

# Not portable to the PSP (they refuse non-Unix targets) and unused: curl brings its own sockets.
turn_off MBEDTLS_NET_C
turn_off MBEDTLS_TIMING_C

# Skiff is a TLS client only: no server side, no DTLS.
turn_off MBEDTLS_SSL_SRV_C
turn_off MBEDTLS_SSL_CACHE_C
turn_off MBEDTLS_SSL_COOKIE_C
turn_off MBEDTLS_SSL_TICKET_C
turn_off MBEDTLS_SSL_PROTO_DTLS
turn_off MBEDTLS_SSL_DTLS_ANTI_REPLAY
turn_off MBEDTLS_SSL_DTLS_CLIENT_PORT_REUSE
turn_off MBEDTLS_SSL_DTLS_CONNECTION_ID
turn_off MBEDTLS_SSL_DTLS_HELLO_VERIFY

# Renegotiation is a historical attack surface and RomM never asks for it.
turn_off MBEDTLS_SSL_RENEGOTIATION

# No certificates or CSRs are created on the device; client certificates are made on a computer.
turn_off MBEDTLS_X509_CRT_WRITE_C
turn_off MBEDTLS_X509_CSR_WRITE_C
turn_off MBEDTLS_X509_CSR_PARSE_C
turn_off MBEDTLS_X509_CREATE_C

# Code size: the self-tests and debug strings cost RAM on a 32 MB machine.
turn_off MBEDTLS_SELF_TEST
turn_off MBEDTLS_DEBUG_C

# Settings the security of Skiff depends on but this script does not edit: fail if an mbedtls update
# changes their upstream defaults, and also if it renames or removes them, so a missing option can
# never pass as "disabled".
require_state MBEDTLS_PSA_CRYPTO_EXTERNAL_RNG off
# mbedtls's own seed-file source and its "no true entropy source" mode would also let TLS run on
# something other than Skiff's hook (Skiff's seed file lives inside its pool instead).
require_state MBEDTLS_ENTROPY_NV_SEED off
require_state MBEDTLS_ENTROPY_NO_SOURCES_OK off
require_state MBEDTLS_SSL_PROTO_TLS1_3 on
require_state MBEDTLS_SSL_PROTO_TLS1_2 on
require_state MBEDTLS_HAVE_TIME_DATE on
echo "mbedtls: Skiff profile applied"
