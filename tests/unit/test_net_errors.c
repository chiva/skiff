/*
 * The table behind every network error a player sees: one row per rule in
 * skiff_net_error_from_curl() (docs/development/architecture.md, "Transport").
 * tests/integration/test_transport_romm.c checks the TLS rows against a real proxy, which is where
 * the client-certificate rows come from.
 */
#include <curl/curl.h>

#include "skiff/curl_transport.h"

#include "unity.h"

/* A PSP whose clock reset to 2000, and a correct clock four days after the floor. */
#define CLOCK_RESET_TO_2000 946684800LL
#define CLOCK_CORRECT (SKIFF_TLS_CLOCK_FLOOR + 3600LL * 24 * 4)

/* Readable names for the row flags. */
enum {
    PLAIN = 0,
    TLS = 1,
    HANDSHAKE_PENDING = 0,
    HANDSHAKE_DONE = 1,
    NO_RESPONSE = 0,
    RESPONSE = 1,
    NO_CERT = 0,
    CERT = 1,
    NEW_CONNECTION = 0,
    KEPT_CONNECTION = 1
};

typedef struct mapping_row {
    const char *label;
    skiff_net_failure failure;
    skiff_err expected;
} mapping_row;

#define FAILURE_ON(code, clock, tls, handshake, connection, response, cert, body)                  \
    {(int)(code), (clock), (tls), (handshake), (connection), (response), (cert), (body)}
#define FAILURE(code, clock, tls, handshake, response, cert, body)                                 \
    FAILURE_ON(code, clock, tls, handshake, NEW_CONNECTION, response, cert, body)
/* A failure where only the curl code matters. */
#define CODE_ONLY(code)                                                                            \
    FAILURE(code, CLOCK_CORRECT, TLS, HANDSHAKE_PENDING, NO_RESPONSE, NO_CERT, SKIFF_OK)

static const mapping_row ROWS[] = {
    {"success", CODE_ONLY(CURLE_OK), SKIFF_OK},
    {"unknown host", CODE_ONLY(CURLE_COULDNT_RESOLVE_HOST), SKIFF_ERR_NET_DNS},
    {"unknown proxy", CODE_ONLY(CURLE_COULDNT_RESOLVE_PROXY), SKIFF_ERR_NET_DNS},
    {"connection refused", CODE_ONLY(CURLE_COULDNT_CONNECT), SKIFF_ERR_NET_CONNECT},
    {"connect or stall timeout", CODE_ONLY(CURLE_OPERATION_TIMEDOUT), SKIFF_ERR_NET_TIMEOUT},
    {"server certificate not trusted, correct clock",
     FAILURE(CURLE_PEER_FAILED_VERIFICATION, CLOCK_CORRECT, TLS, HANDSHAKE_PENDING, NO_RESPONSE,
             NO_CERT, SKIFF_OK),
     SKIFF_ERR_NET_TLS_UNTRUSTED},
    {"server certificate rejected, clock reset to 2000",
     FAILURE(CURLE_PEER_FAILED_VERIFICATION, CLOCK_RESET_TO_2000, TLS, HANDSHAKE_PENDING,
             NO_RESPONSE, NO_CERT, SKIFF_OK),
     SKIFF_ERR_NET_TLS_CLOCK},
    {"server certificate rejected, clock one second before the floor",
     FAILURE(CURLE_PEER_FAILED_VERIFICATION, SKIFF_TLS_CLOCK_FLOOR - 1, TLS, HANDSHAKE_PENDING,
             NO_RESPONSE, NO_CERT, SKIFF_OK),
     SKIFF_ERR_NET_TLS_CLOCK},
    {"server certificate rejected, clock at the floor",
     FAILURE(CURLE_PEER_FAILED_VERIFICATION, SKIFF_TLS_CLOCK_FLOOR, TLS, HANDSHAKE_PENDING,
             NO_RESPONSE, NO_CERT, SKIFF_OK),
     SKIFF_ERR_NET_TLS_UNTRUSTED},
    {"handshake failed, no client certificate",
     FAILURE(CURLE_SSL_CONNECT_ERROR, CLOCK_CORRECT, TLS, HANDSHAKE_PENDING, NO_RESPONSE, NO_CERT,
             SKIFF_OK),
     SKIFF_ERR_NET_TLS_HANDSHAKE},
    {"handshake failed with a client certificate (untrusted CA, seen on Caddy)",
     FAILURE(CURLE_SSL_CONNECT_ERROR, CLOCK_CORRECT, TLS, HANDSHAKE_PENDING, NO_RESPONSE, CERT,
             SKIFF_OK),
     SKIFF_ERR_NET_TLS_CLIENT_CERT},
    {"connection reset during the handshake, no client certificate",
     FAILURE(CURLE_RECV_ERROR, CLOCK_CORRECT, TLS, HANDSHAKE_PENDING, NO_RESPONSE, NO_CERT,
             SKIFF_OK),
     SKIFF_ERR_NET_TLS_HANDSHAKE},
    {"server closed right after the handshake, no client certificate (seen on Caddy)",
     FAILURE(CURLE_RECV_ERROR, CLOCK_CORRECT, TLS, HANDSHAKE_DONE, NO_RESPONSE, NO_CERT, SKIFF_OK),
     SKIFF_ERR_NET_TLS_CLIENT_CERT},
    {"server closed right after the handshake with a client certificate",
     FAILURE(CURLE_RECV_ERROR, CLOCK_CORRECT, TLS, HANDSHAKE_DONE, NO_RESPONSE, CERT, SKIFF_OK),
     SKIFF_ERR_NET_TLS_CLIENT_CERT},
    {"kept TLS connection closed by the server before the response, client certificate in use",
     FAILURE_ON(CURLE_GOT_NOTHING, CLOCK_CORRECT, TLS, HANDSHAKE_PENDING, KEPT_CONNECTION,
                NO_RESPONSE, CERT, SKIFF_OK),
     SKIFF_ERR_NET_CONNECTION_LOST},
    {"kept TLS connection reset before the response",
     FAILURE_ON(CURLE_RECV_ERROR, CLOCK_CORRECT, TLS, HANDSHAKE_PENDING, KEPT_CONNECTION,
                NO_RESPONSE, NO_CERT, SKIFF_OK),
     SKIFF_ERR_NET_CONNECTION_LOST},
    {"client certificate or key unreadable", CODE_ONLY(CURLE_SSL_CERTPROBLEM),
     SKIFF_ERR_NET_TLS_CLIENT_CERT},
    {"cut off mid-body over TLS",
     FAILURE(CURLE_PARTIAL_FILE, CLOCK_CORRECT, TLS, HANDSHAKE_DONE, RESPONSE, CERT, SKIFF_OK),
     SKIFF_ERR_NET_CONNECTION_LOST},
    {"receive error mid-body over TLS",
     FAILURE(CURLE_RECV_ERROR, CLOCK_CORRECT, TLS, HANDSHAKE_DONE, RESPONSE, NO_CERT, SKIFF_OK),
     SKIFF_ERR_NET_CONNECTION_LOST},
    {"send error, plain HTTP",
     FAILURE(CURLE_SEND_ERROR, CLOCK_CORRECT, PLAIN, HANDSHAKE_PENDING, NO_RESPONSE, NO_CERT,
             SKIFF_OK),
     SKIFF_ERR_NET_CONNECTION_LOST},
    {"empty reply, plain HTTP",
     FAILURE(CURLE_GOT_NOTHING, CLOCK_CORRECT, PLAIN, HANDSHAKE_PENDING, NO_RESPONSE, NO_CERT,
             SKIFF_OK),
     SKIFF_ERR_NET_CONNECTION_LOST},
    {"body callback stopped (Memory Stick full)",
     FAILURE(CURLE_WRITE_ERROR, CLOCK_CORRECT, TLS, HANDSHAKE_DONE, RESPONSE, NO_CERT,
             SKIFF_ERR_STORAGE_NO_SPACE),
     SKIFF_ERR_STORAGE_NO_SPACE},
    {"aborted by callback",
     FAILURE(CURLE_ABORTED_BY_CALLBACK, CLOCK_CORRECT, TLS, HANDSHAKE_DONE, RESPONSE, NO_CERT,
             SKIFF_ERR_STORAGE_IO),
     SKIFF_ERR_STORAGE_IO},
    {"write error without a recorded reason", CODE_ONLY(CURLE_WRITE_ERROR), SKIFF_ERR_INVALID_ARG},
    {"out of memory", CODE_ONLY(CURLE_OUT_OF_MEMORY), SKIFF_ERR_NO_MEMORY},
    {"malformed server address", CODE_ONLY(CURLE_URL_MALFORMAT), SKIFF_ERR_CONFIG_INVALID_VALUE},
    {"unsupported scheme", CODE_ONLY(CURLE_UNSUPPORTED_PROTOCOL), SKIFF_ERR_CONFIG_INVALID_VALUE},
    {"CA file unreadable", CODE_ONLY(CURLE_SSL_CACERT_BADFILE), SKIFF_ERR_CONFIG_INVALID_VALUE},
    {"anything else", CODE_ONLY(CURLE_TOO_MANY_REDIRECTS), SKIFF_ERR_NET_CONNECT},
};

void setUp(void) {}

void tearDown(void) {}

static void test_every_row_maps_as_documented(void) {
    for (size_t i = 0; i < sizeof ROWS / sizeof ROWS[0]; i++) {
        const skiff_err actual = skiff_net_error_from_curl(&ROWS[i].failure);
        TEST_PRINTF("%s: curl %d -> %s", ROWS[i].label, ROWS[i].failure.curl_code,
                    skiff_err_name(actual));
        TEST_ASSERT_EQUAL_STRING_MESSAGE(skiff_err_name(ROWS[i].expected), skiff_err_name(actual),
                                         ROWS[i].label);
    }
}

static void test_null_failure(void) {
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_INVALID_ARG, skiff_net_error_from_curl(NULL));
}

static void test_clock_floor_is_2026_10_01(void) {
    /* 56 years of 365 days, 14 leap days (1972-2024), then January to September 2026. */
    const long long days = 56LL * 365 + 14 + 273;
    TEST_ASSERT_EQUAL_INT64(days * 86400, SKIFF_TLS_CLOCK_FLOOR);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_every_row_maps_as_documented);
    RUN_TEST(test_null_failure);
    RUN_TEST(test_clock_floor_is_2026_10_01);
    return UNITY_END();
}
