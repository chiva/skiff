#ifndef SKIFF_CURL_TRANSPORT_H
#define SKIFF_CURL_TRANSPORT_H

/*
 * The real transport: libcurl over Mbed TLS (docs/development/tls.md), on the PSP and in host tests
 * alike. One curl handle per transport, reused for every request, keeps the connection alive.
 *
 * A whole transfer has no limit unless total_timeout_s sets one, because a download can take an
 * hour. Instead a connection that delivers nothing for stall_timeout_s seconds counts as a timeout.
 *
 * curl is built without a resolver thread (the PSP's resolver is synchronous) and runs without
 * signals, so it cannot time out a name lookup: a PSP whose DNS server does not answer would wait
 * for minutes. A transport given a resolve hook looks names up through it instead, before the
 * request, and hands curl the address.
 */

#include <stddef.h>
#include <stdint.h>

#include "skiff/error.h"
#include "skiff/transport.h"

#define SKIFF_CURL_CONNECT_TIMEOUT_S 10L
#define SKIFF_CURL_STALL_TIMEOUT_S 30L
/* Room for a dotted IPv4 address and its terminator. */
#define SKIFF_CURL_ADDRESS_MAX 16
/* What a host name may be, as curl hands it out of a URL. */
#define SKIFF_CURL_HOST_MAX 256

/*
 * Looks host up and writes its IPv4 address ("192.168.1.20") into address. Returns SKIFF_OK, or
 * SKIFF_ERR_NET_DNS when the name does not resolve in the time the hook allows; any other error is
 * passed on as the request's. A host that already is an address is written back as it is.
 */
typedef skiff_err (*skiff_curl_resolve_fn)(void *ctx, const char *host, char *address,
                                           size_t address_size);

/* 2026-10-01 00:00:00 UTC. A clock earlier than this cannot be right (it predates this code), so a
 * certificate that fails verification then is reported as SKIFF_ERR_NET_TLS_CLOCK: the PSP's date
 * is wrong, typically reset by a flat battery, and must be fixed before anything else about the
 * certificate can be judged. A clock in the future gets no such reading. */
#define SKIFF_TLS_CLOCK_FLOOR 1790812800LL

typedef struct skiff_curl_config {
    /* PEM files; NULL for none. Without a CA file no server certificate is trusted. client_key goes
     * with client_cert. */
    const char *ca_file;
    const char *client_cert;
    const char *client_key;
    /* Sent on every request, before the request's own headers: custom headers from config.ini, such
     * as a proxy's access token. Copied by skiff_curl_transport_create(). */
    const skiff_http_header *default_headers;
    size_t default_header_count;
    /* 0 selects SKIFF_CURL_CONNECT_TIMEOUT_S and SKIFF_CURL_STALL_TIMEOUT_S. */
    long connect_timeout_s;
    long stall_timeout_s;
    /* The most a whole request may take, in seconds, or 0 for no limit (downloads). */
    long total_timeout_s;
    /* NULL lets curl resolve names itself (host builds, whose resolver times out). The address is
     * kept for the next requests to the same host and port until one fails on the network. */
    skiff_curl_resolve_fn resolve;
    void *resolve_ctx;
} skiff_curl_config;

/*
 * Initialises libcurl, and with it TLS, once per program. Returns SKIFF_ERR_NET_ENTROPY when it
 * fails, which in practice means TLS could not be seeded; on the PSP, skiff_psp_entropy_status()
 * tells whether ARK is missing. Pair with skiff_net_global_cleanup().
 */
skiff_err skiff_net_global_init(void);
void skiff_net_global_cleanup(void);

/*
 * Creates a transport (free it with skiff_transport_destroy()). Returns SKIFF_ERR_INVALID_ARG for a
 * NULL out, a client_key without client_cert, invalid default headers (skiff_http_headers_valid())
 * or a negative timeout, SKIFF_ERR_NO_MEMORY when allocation fails; *out is NULL on error.
 */
skiff_err skiff_curl_transport_create(const skiff_curl_config *config, skiff_transport **out);

/* What the curl transport knows when a request fails, for skiff_net_error_from_curl(). */
typedef struct skiff_net_failure {
    int curl_code;
    /* The clock certificates were checked against, in seconds since 1970 (mbedtls_time()). curl's
     * Mbed TLS backend does not report the verify flags (CURLINFO_SSL_VERIFYRESULT stays 0), so the
     * clock alone decides between SKIFF_ERR_NET_TLS_CLOCK and SKIFF_ERR_NET_TLS_UNTRUSTED. */
    int64_t clock_now;
    int uses_tls;
    /* This request completed a TLS handshake on a new connection. */
    int tls_established;
    /* This request went over a connection kept from an earlier one. */
    int reused_connection;
    int got_response;
    int client_cert_configured;
    /* What the body callback or the stop hook returned when it stopped the transfer. */
    skiff_err callback_error;
} skiff_net_failure;

/* Maps a failed request to a Skiff error; see docs/development/architecture.md ("Transport"). */
skiff_err skiff_net_error_from_curl(const skiff_net_failure *failure);

#endif
