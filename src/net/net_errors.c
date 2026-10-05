#include <curl/curl.h>

#include "skiff/curl_transport.h"

static skiff_err untrusted_or_clock(const skiff_net_failure *failure) {
    return failure->clock_now < SKIFF_TLS_CLOCK_FLOOR ? SKIFF_ERR_NET_TLS_CLOCK
                                                      : SKIFF_ERR_NET_TLS_UNTRUSTED;
}

/*
 * The connection broke. Once a response has started, without TLS, or on a connection kept from an
 * earlier request (the server closed it while idle), the connection was lost. Over a new TLS
 * connection before any response:
 *   - the handshake had completed: the server closed right after it, which is how a TLS 1.3
 *     server refuses a missing or unaccepted client certificate (it checks the certificate after
 *     the client considers the handshake done);
 *   - it had not: the handshake failed, which with a client certificate configured most likely
 *     means the server rejected that certificate.
 */
static skiff_err broken_connection(const skiff_net_failure *failure) {
    if (failure->got_response || !failure->uses_tls || failure->reused_connection) {
        return SKIFF_ERR_NET_CONNECTION_LOST;
    }
    if (failure->tls_established || failure->client_cert_configured) {
        return SKIFF_ERR_NET_TLS_CLIENT_CERT;
    }
    return SKIFF_ERR_NET_TLS_HANDSHAKE;
}

skiff_err skiff_net_error_from_curl(const skiff_net_failure *failure) {
    if (failure == NULL) {
        return SKIFF_ERR_INVALID_ARG;
    }
    switch ((CURLcode)failure->curl_code) {
    case CURLE_OK:
        return SKIFF_OK;
    case CURLE_WRITE_ERROR:
    case CURLE_ABORTED_BY_CALLBACK:
        return failure->body_error != SKIFF_OK ? failure->body_error : SKIFF_ERR_INVALID_ARG;
    case CURLE_COULDNT_RESOLVE_HOST:
    case CURLE_COULDNT_RESOLVE_PROXY:
        return SKIFF_ERR_NET_DNS;
    case CURLE_COULDNT_CONNECT:
        return SKIFF_ERR_NET_CONNECT;
    case CURLE_OPERATION_TIMEDOUT:
        return SKIFF_ERR_NET_TIMEOUT;
    case CURLE_PEER_FAILED_VERIFICATION:
        return untrusted_or_clock(failure);
    case CURLE_SSL_CERTPROBLEM:
        return SKIFF_ERR_NET_TLS_CLIENT_CERT;
    case CURLE_SSL_CONNECT_ERROR:
    case CURLE_RECV_ERROR:
    case CURLE_SEND_ERROR:
    case CURLE_GOT_NOTHING:
    case CURLE_PARTIAL_FILE:
        return broken_connection(failure);
    case CURLE_OUT_OF_MEMORY:
        return SKIFF_ERR_NO_MEMORY;
    case CURLE_URL_MALFORMAT:
    case CURLE_UNSUPPORTED_PROTOCOL:
    case CURLE_SSL_CACERT_BADFILE:
        return SKIFF_ERR_CONFIG_INVALID_VALUE;
    default:
        return SKIFF_ERR_NET_CONNECT;
    }
}
