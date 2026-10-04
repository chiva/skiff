#!/bin/sh
# Applies Skiff's build profile to an unpacked Mbed TLS 4.x source tree, editing its default config
# headers in place with Mbed TLS's own scripts/config.py. Editing the headers (rather than passing
# MBEDTLS_USER_CONFIG_FILE) means the installed headers carry the profile, so libcurl, Skiff and
# mbedtls itself are always compiled against the same struct layouts.
# Usage: configure-mbedtls.sh <mbedtls-source-dir>
set -eu

cd "${1:?usage: configure-mbedtls.sh <mbedtls-source-dir>}"
config() { python3 scripts/config.py "$@"; }

# Entropy (release blocker, see docs/development/architecture.md "Randomness for TLS"). mbedtls's
# built-in sources only support Unix and Windows. Skiff supplies mbedtls_platform_get_entropy(); any
# EBOOT that links mbedtls without it fails to link.
config unset MBEDTLS_PSA_BUILTIN_GET_ENTROPY
config set MBEDTLS_PSA_DRIVER_GET_ENTROPY

# mbedtls only knows how to read a millisecond clock on Unix and Windows. Skiff supplies
# mbedtls_ms_time() (src/platform/psp/mbedtls_time.c); TLS 1.3 uses it to age session tickets.
config set MBEDTLS_PLATFORM_MS_TIME_ALT

# Persistent PSA keys: Skiff keeps none, and the file backend would write into the working directory.
config unset MBEDTLS_PSA_CRYPTO_STORAGE_C
config unset MBEDTLS_PSA_ITS_FILE_C

# Not portable to the PSP (they refuse non-Unix targets) and unused: curl brings its own sockets.
config unset MBEDTLS_NET_C
config unset MBEDTLS_TIMING_C

# Skiff is a TLS client only: no server side, no DTLS.
config unset MBEDTLS_SSL_SRV_C
config unset MBEDTLS_SSL_CACHE_C
config unset MBEDTLS_SSL_COOKIE_C
config unset MBEDTLS_SSL_TICKET_C
config unset MBEDTLS_SSL_PROTO_DTLS
config unset MBEDTLS_SSL_DTLS_ANTI_REPLAY
config unset MBEDTLS_SSL_DTLS_CLIENT_PORT_REUSE
config unset MBEDTLS_SSL_DTLS_CONNECTION_ID
config unset MBEDTLS_SSL_DTLS_HELLO_VERIFY

# Renegotiation is a historical attack surface and RomM never asks for it.
config unset MBEDTLS_SSL_RENEGOTIATION

# No certificates or CSRs are created on the device; client certificates are made on a computer.
config unset MBEDTLS_X509_CRT_WRITE_C
config unset MBEDTLS_X509_CSR_WRITE_C
config unset MBEDTLS_X509_CSR_PARSE_C
config unset MBEDTLS_X509_CREATE_C

# Code size: the self-tests and debug strings cost RAM on a 32 MB machine.
config unset MBEDTLS_SELF_TEST
config unset MBEDTLS_DEBUG_C

# Fail the image build if an mbedtls update renamed or reshaped any setting the security of Skiff
# depends on, instead of silently falling back to the upstream default.
require_set() {
  config get "$1" >/dev/null || {
    echo "error: $1 must be enabled" >&2
    exit 1
  }
}
require_unset() {
  if config get "$1" >/dev/null; then
    echo "error: $1 must be disabled" >&2
    exit 1
  fi
}
require_set MBEDTLS_PSA_DRIVER_GET_ENTROPY
require_unset MBEDTLS_PSA_BUILTIN_GET_ENTROPY
require_set MBEDTLS_PLATFORM_MS_TIME_ALT
require_unset MBEDTLS_PSA_CRYPTO_EXTERNAL_RNG
require_set MBEDTLS_SSL_PROTO_TLS1_3
require_set MBEDTLS_SSL_PROTO_TLS1_2
require_set MBEDTLS_HAVE_TIME_DATE
echo "mbedtls: Skiff profile applied"
