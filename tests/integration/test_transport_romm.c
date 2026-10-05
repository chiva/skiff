/*
 * The host build's curl transport against the integration RomM behind Caddy (tests/integration/),
 * over the TLS stack the PSP links. It checks what the hermetic unit tests cannot: certificate
 * trust, the PSP-clock rule (108), client certificates, the cipher suite negotiated and the
 * fallback to AES-GCM, keep-alive and resuming a real download.
 *
 * Run by `scripts/dev.sh romm-test` (tests/integration/transport-test.sh) inside the compose
 * network, with the server's details in SKIFF_IT_* environment variables. Not a ctest test: it
 * needs the server.
 */
#include <mbedtls/platform_time.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "skiff/curl_transport.h"

#include "unity.h"

enum { URL_MAX = 512, PATH_MAX_LENGTH = 256, RESUME_OFFSET = 1000 };

#define TLS_SITE "https://proxy:8443"
#define MTLS_SITE "https://proxy:8444"
#define PLAIN_SITE "http://proxy:8080"
/* TLS 1.2 with AES-128-GCM only (tests/integration/Caddyfile). */
#define AES_ONLY_SITE "https://proxy:8445"
#define TLS13 "TLSv1.3"
#define TLS12 "TLSv1.2"
#define TLS13_CHACHA20 "TLS1-3-CHACHA20-POLY1305-SHA256"
#define AES_128_GCM "AES-128-GCM"
#define HEARTBEAT "/api/heartbeat"
#define CLOCK_RESET_TO_2000 ((mbedtls_time_t)946684800)
#define CLOCK_IN_2100 ((mbedtls_time_t)4102444800LL)
#define TEST_CONNECT_TIMEOUT_S 5L
#define TEST_STALL_TIMEOUT_S 10L

typedef struct server_details {
    const char *certs;
    const char *token;
    const char *rom_id;
    const char *file_name; /* URL-encoded */
    size_t size;
} server_details;

typedef struct body_buffer {
    unsigned char *bytes;
    size_t capacity;
    size_t size;
} body_buffer;

static server_details server;
static char ca_file[PATH_MAX_LENGTH];
static char authorization[PATH_MAX_LENGTH];
static skiff_transport *transport;
static skiff_http_response response;
static body_buffer body;
static body_buffer full_file;

static skiff_err collect(void *ctx, const unsigned char *data, size_t size) {
    body_buffer *target = ctx;
    if (size > target->capacity - target->size) {
        return SKIFF_ERR_BUFFER_TOO_SMALL;
    }
    memcpy(target->bytes + target->size, data, size);
    target->size += size;
    return SKIFF_OK;
}

static const char *required_env(const char *name) {
    const char *value = getenv(name);
    if (value == NULL || value[0] == '\0') {
        printf("missing environment variable %s (run through scripts/dev.sh romm-test)\n", name);
        exit(EXIT_FAILURE);
    }
    return value;
}

static void cert_path(char *out, size_t out_size, const char *file) {
    snprintf(out, out_size, "%s/%s", server.certs, file);
}

void setUp(void) {
    transport = NULL;
    body.size = 0;
}

void tearDown(void) {
    skiff_transport_destroy(transport);
    mbedtls_platform_set_time(time);
}

/* A fresh transport per check, so no kept connection or TLS session skips verification. */
static void connect_with(const char *client_type) {
    char cert[PATH_MAX_LENGTH];
    char key[PATH_MAX_LENGTH];
    static const skiff_http_header TEST_HEADERS[] = {{"X-Skiff-Test", "1"}};
    const skiff_http_header token_header[] = {{"Authorization", authorization}};
    skiff_curl_config config = {.ca_file = ca_file,
                                .default_headers = TEST_HEADERS,
                                .default_header_count = 1,
                                .connect_timeout_s = TEST_CONNECT_TIMEOUT_S,
                                .stall_timeout_s = TEST_STALL_TIMEOUT_S};
    if (client_type != NULL && strcmp(client_type, "token") == 0) {
        config.default_headers = token_header;
    } else if (client_type != NULL) {
        char name[PATH_MAX_LENGTH];
        snprintf(name, sizeof name, "client-%s.crt", client_type);
        cert_path(cert, sizeof cert, name);
        snprintf(name, sizeof name, "client-%s.key", client_type);
        cert_path(key, sizeof key, name);
        config.client_cert = cert;
        config.client_key = key;
    }
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, skiff_curl_transport_create(&config, &transport));
}

static skiff_err get_with(const char *url, int has_range, const char *if_range) {
    const skiff_http_request request = {.url = url,
                                        .has_range = has_range,
                                        .range_start = has_range ? RESUME_OFFSET : 0,
                                        .if_range = if_range,
                                        .on_body = collect,
                                        .body_ctx = &body};
    body.size = 0;
    const skiff_err err = skiff_transport_perform(transport, &request, &response);
    TEST_PRINTF(
        "GET %s -> %s (%d), HTTP %ld, %llu body bytes, %ld new connection(s), TLS '%s' '%s'", url,
        skiff_err_name(err), (int)err, response.status, (unsigned long long)response.body_bytes,
        response.new_connections, response.tls_version, response.tls_cipher);
    return err;
}

static skiff_err get(const char *url) { return get_with(url, 0, NULL); }

static mbedtls_time_t clock_reset_to_2000(mbedtls_time_t *out) {
    if (out != NULL) {
        *out = CLOCK_RESET_TO_2000;
    }
    return CLOCK_RESET_TO_2000;
}

static mbedtls_time_t clock_in_2100(mbedtls_time_t *out) {
    if (out != NULL) {
        *out = CLOCK_IN_2100;
    }
    return CLOCK_IN_2100;
}

static void content_url(char *out, size_t out_size) {
    snprintf(out, out_size, TLS_SITE "/api/roms/%s/content/%s", server.rom_id, server.file_name);
}

static void test_https_heartbeat_through_the_test_ca(void) {
    connect_with(NULL);
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, get(TLS_SITE HEARTBEAT));
    TEST_ASSERT_EQUAL_INT64(200, response.status);
    TEST_ASSERT_NOT_NULL(memchr(body.bytes, '{', body.size));
}

/*
 * What the transport offers, seen from a server: Go's TLS stack (Caddy) on a machine with AES
 * hardware picks AES-GCM unless the client lists ChaCha20 first, which it takes as a client without
 * AES hardware. So ChaCha20 here means the transport's ClientHello put it first, as a PSP needs
 * (tests/unit/test_host_tls.c has the speeds).
 */
static void test_chacha20_is_negotiated_over_tls_1_3(void) {
    connect_with(NULL);
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, get(TLS_SITE HEARTBEAT));
    TEST_ASSERT_EQUAL_STRING(TLS13, response.tls_version);
    TEST_ASSERT_EQUAL_STRING(TLS13_CHACHA20, response.tls_cipher);
}

static void test_server_untrusted_without_the_ca(void) {
    const skiff_curl_config config = {.connect_timeout_s = TEST_CONNECT_TIMEOUT_S};
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, skiff_curl_transport_create(&config, &transport));
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_NET_TLS_UNTRUSTED, get(TLS_SITE HEARTBEAT));
}

static void test_psp_clock_reset_to_2000_is_reported_as_the_clock(void) {
    mbedtls_platform_set_time(clock_reset_to_2000);
    connect_with(NULL);
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_NET_TLS_CLOCK, get(TLS_SITE HEARTBEAT));
}

static void test_clock_in_the_future_is_reported_as_untrusted(void) {
    mbedtls_platform_set_time(clock_in_2100);
    connect_with(NULL);
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_NET_TLS_UNTRUSTED, get(TLS_SITE HEARTBEAT));
}

static void test_client_certificates(void) {
    static const char *const ACCEPTED[] = {"ecdsa", "rsa"};
    for (size_t i = 0; i < sizeof ACCEPTED / sizeof ACCEPTED[0]; i++) {
        TEST_PRINTF("client certificate: %s", ACCEPTED[i]);
        connect_with(ACCEPTED[i]);
        TEST_ASSERT_EQUAL_INT(SKIFF_OK, get(MTLS_SITE HEARTBEAT));
        TEST_ASSERT_EQUAL_INT64(200, response.status);
        skiff_transport_destroy(transport);
        transport = NULL;
    }
}

static void test_mtls_site_without_a_client_certificate(void) {
    connect_with(NULL);
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_NET_TLS_CLIENT_CERT, get(MTLS_SITE HEARTBEAT));
}

static void test_client_certificate_from_an_untrusted_ca(void) {
    connect_with("wrong-ca");
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_NET_TLS_CLIENT_CERT, get(MTLS_SITE HEARTBEAT));
}

/* Skiff offers ChaCha20-Poly1305 first (tests/unit/test_host_tls.c); a server without it must
 * still be reachable through AES-GCM. */
static void test_server_without_chacha20_is_reached_over_aes_gcm(void) {
    connect_with(NULL);
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, get(AES_ONLY_SITE HEARTBEAT));
    TEST_ASSERT_EQUAL_INT64(200, response.status);
    TEST_ASSERT_EQUAL_STRING(TLS12, response.tls_version);
    TEST_ASSERT_NOT_NULL(strstr(response.tls_cipher, AES_128_GCM));
}

static void test_plain_http(void) {
    connect_with(NULL);
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, get(PLAIN_SITE HEARTBEAT));
    TEST_ASSERT_EQUAL_INT64(200, response.status);
}

static void test_unknown_host(void) {
    connect_with(NULL);
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_NET_DNS, get("https://skiff-nonexistent.invalid" HEARTBEAT));
}

static void test_closed_port(void) {
    connect_with(NULL);
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_NET_CONNECT, get("http://proxy:9" HEARTBEAT));
}

static void test_keep_alive_pays_the_handshake_once(void) {
    connect_with(NULL);
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, get(TLS_SITE HEARTBEAT));
    TEST_ASSERT_EQUAL_INT64(1, response.new_connections);
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, get(TLS_SITE HEARTBEAT));
    TEST_ASSERT_EQUAL_INT64(0, response.new_connections);
    TEST_ASSERT_EQUAL_STRING_MESSAGE(TLS13_CHACHA20, response.tls_cipher,
                                     "a kept connection still reports its cipher suite");
}

static void test_token_is_required(void) {
    char url[URL_MAX];
    snprintf(url, sizeof url, TLS_SITE "/api/roms/%s", server.rom_id);
    connect_with(NULL);
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, get(url));
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_ROMM_UNAUTHORIZED, skiff_http_status_error(response.status));
}

static void download_whole_file(char *url, size_t url_size) {
    content_url(url, url_size);
    connect_with("token");
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, get(url));
    TEST_ASSERT_EQUAL_INT64(200, response.status);
    TEST_ASSERT_EQUAL_UINT64(server.size, response.body_bytes);
    TEST_ASSERT_NOT_EQUAL_MESSAGE(0, strlen(response.etag), "resuming needs the ETag");
    memcpy(full_file.bytes, body.bytes, body.size);
    full_file.size = body.size;
}

static void test_download_and_resume_with_the_current_etag(void) {
    char url[URL_MAX];
    char etag[SKIFF_HTTP_ETAG_MAX];
    download_whole_file(url, sizeof url);
    snprintf(etag, sizeof etag, "%s", response.etag);
    TEST_PRINTF("ETag %s", etag);
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, get_with(url, 1, etag));
    TEST_ASSERT_EQUAL_INT64(206, response.status);
    TEST_ASSERT_TRUE(response.has_content_range);
    TEST_ASSERT_EQUAL_UINT64(RESUME_OFFSET, response.range_start);
    TEST_ASSERT_EQUAL_UINT64(server.size, response.range_total);
    TEST_ASSERT_EQUAL_size_t(server.size - RESUME_OFFSET, body.size);
    TEST_ASSERT_EQUAL_MEMORY(full_file.bytes + RESUME_OFFSET, body.bytes, body.size);
}

static void test_resume_with_a_stale_etag_restarts(void) {
    char url[URL_MAX];
    download_whole_file(url, sizeof url);
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, get_with(url, 1, "\"stale\""));
    TEST_ASSERT_EQUAL_INT64(200, response.status);
    TEST_ASSERT_FALSE(response.has_content_range);
    TEST_ASSERT_EQUAL_size_t(server.size, body.size);
    TEST_ASSERT_EQUAL_MEMORY(full_file.bytes, body.bytes, body.size);
}

int main(void) {
    server.certs = required_env("SKIFF_IT_CERTS");
    server.token = required_env("SKIFF_IT_TOKEN");
    server.rom_id = required_env("SKIFF_IT_ROM_ID");
    server.file_name = required_env("SKIFF_IT_FILE_NAME");
    server.size = (size_t)strtoull(required_env("SKIFF_IT_SIZE"), NULL, 10);
    cert_path(ca_file, sizeof ca_file, "ca.crt");
    snprintf(authorization, sizeof authorization, "Bearer %s", server.token);
    body.capacity = server.size;
    full_file.capacity = server.size;
    body.bytes = malloc(server.size);
    full_file.bytes = malloc(server.size);
    if (body.bytes == NULL || full_file.bytes == NULL || skiff_net_global_init() != SKIFF_OK) {
        printf("setup failed: no memory for %zu bytes, or TLS could not be seeded\n", server.size);
        return EXIT_FAILURE;
    }

    UNITY_BEGIN();
    RUN_TEST(test_https_heartbeat_through_the_test_ca);
    RUN_TEST(test_chacha20_is_negotiated_over_tls_1_3);
    RUN_TEST(test_server_untrusted_without_the_ca);
    RUN_TEST(test_psp_clock_reset_to_2000_is_reported_as_the_clock);
    RUN_TEST(test_clock_in_the_future_is_reported_as_untrusted);
    RUN_TEST(test_client_certificates);
    RUN_TEST(test_mtls_site_without_a_client_certificate);
    RUN_TEST(test_client_certificate_from_an_untrusted_ca);
    RUN_TEST(test_server_without_chacha20_is_reached_over_aes_gcm);
    RUN_TEST(test_plain_http);
    RUN_TEST(test_unknown_host);
    RUN_TEST(test_closed_port);
    RUN_TEST(test_keep_alive_pays_the_handshake_once);
    RUN_TEST(test_token_is_required);
    RUN_TEST(test_download_and_resume_with_the_current_etag);
    RUN_TEST(test_resume_with_a_stale_etag_restarts);
    const int failures = UNITY_END();

    skiff_net_global_cleanup();
    free(body.bytes);
    free(full_file.bytes);
    return failures;
}
