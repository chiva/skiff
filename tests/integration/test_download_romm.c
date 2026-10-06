/*
 * Resumable downloads (skiff/download.h) against the integration RomM behind Caddy, over the curl
 * transport and the TLS stack the PSP links, into a temporary directory: a download stopped part
 * way continues on a new connection with 206 from the saved offset, a .resume file whose ETag the
 * server no longer has starts over with 200, and either way the file matches the CRC-32 RomM
 * recorded for it.
 *
 * Run by `scripts/dev.sh romm-test` (tests/integration/transport-test.sh), with the server's
 * details in SKIFF_IT_* environment variables. Not a ctest test: it needs the server.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "skiff/curl_transport.h"
#include "skiff/download.h"

#include "storage_posix.h"
#include "temp_dir.h"
#include "unity.h"

enum { URL_MAX = 512, PATH_MAX_LENGTH = 256, HEX_BASE = 16, DECIMAL_BASE = 10 };

#define TLS_SITE "https://proxy:8443"
#define TARGET_NAME "Skiff Test Payload.iso"
#define STALE_ETAG "\"skiff-stale-etag\""
#define TEST_CONNECT_TIMEOUT_S 5L
#define TEST_STALL_TIMEOUT_S 10L
/* The stop hook ends the first attempt once this share of the file has arrived. */
#define STOP_NUMERATOR 2
#define STOP_DENOMINATOR 5

typedef struct server_details {
    const char *certs;
    const char *token;
    const char *rom_id;
    const char *file_name; /* URL-encoded */
    uint64_t size;
    uint32_t crc32;
} server_details;

/* Stops the transfer once `stop_at` bytes are in; counts how often it was asked. */
typedef struct stop_point {
    uint64_t done;
    uint64_t stop_at;
    int polls;
} stop_point;

static server_details server;
static char ca_file[PATH_MAX_LENGTH];
static char authorization[PATH_MAX_LENGTH];
static char url[URL_MAX];
static char dir[TEMP_DIR_PATH_MAX];
static char target[TEMP_DIR_PATH_MAX];
static char state_path[TEMP_DIR_PATH_MAX + sizeof SKIFF_DOWNLOAD_STATE_SUFFIX];
static skiff_transport *transport;
static skiff_storage *storage;
static skiff_http_header headers[1];
static skiff_download_spec spec;
static skiff_download_result result;
static stop_point stop;

static const char *required_env(const char *name) {
    const char *value = getenv(name);
    if (value == NULL || value[0] == '\0') {
        printf("missing environment variable %s (run through scripts/dev.sh romm-test)\n", name);
        exit(EXIT_FAILURE);
    }
    return value;
}

static void on_progress(void *ctx, uint64_t done, uint64_t total) {
    (void)total;
    ((stop_point *)ctx)->done = done;
}

static skiff_err stop_when_due(void *ctx) {
    stop_point *point = ctx;
    point->polls++;
    return point->stop_at > 0 && point->done >= point->stop_at ? SKIFF_ERR_NET_CONNECTION_LOST
                                                               : SKIFF_OK;
}

/* A new transport, so every attempt is a new connection and TLS handshake, as after a suspend. */
static void new_connection(void) {
    skiff_transport_destroy(transport);
    transport = NULL;
    const skiff_curl_config config = {.ca_file = ca_file,
                                      .connect_timeout_s = TEST_CONNECT_TIMEOUT_S,
                                      .stall_timeout_s = TEST_STALL_TIMEOUT_S};
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, skiff_curl_transport_create(&config, &transport));
}

void setUp(void) {
    TEST_ASSERT_EQUAL_INT(0, temp_dir_create(dir, sizeof dir));
    TEST_ASSERT_TRUE(temp_dir_path(dir, TARGET_NAME, target, sizeof target));
    snprintf(state_path, sizeof state_path, "%s" SKIFF_DOWNLOAD_STATE_SUFFIX, target);
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, skiff_posix_storage_create(&storage));
    memset(&stop, 0, sizeof stop);
    headers[0].name = "Authorization";
    headers[0].value = authorization;
    memset(&spec, 0, sizeof spec);
    spec.url = url;
    spec.headers = headers;
    spec.header_count = 1;
    spec.target_path = target;
    spec.expected_size = server.size;
    spec.has_expected_crc32 = 1;
    spec.expected_crc32 = server.crc32;
    spec.should_stop = stop_when_due;
    spec.stop_ctx = &stop;
    spec.on_progress = on_progress;
    spec.progress_ctx = &stop;
    new_connection();
}

void tearDown(void) {
    skiff_transport_destroy(transport);
    transport = NULL;
    skiff_storage_destroy(storage);
    temp_dir_remove(dir);
}

static skiff_err attempt(void) {
    const skiff_err err = skiff_download_attempt(transport, storage, &spec, &result);
    TEST_PRINTF("attempt -> %s: HTTP %ld, resumed from %llu, received %llu, restarted %d, "
                "complete %d, %d stop polls, TLS '%s' '%s'",
                skiff_err_name(err), result.response.status,
                (unsigned long long)result.resumed_from, (unsigned long long)result.bytes_received,
                result.restarted, result.complete, stop.polls, result.response.tls_version,
                result.response.tls_cipher);
    return err;
}

/* Starts a download and stops it part way; returns the offset it saved. */
static uint64_t download_part_way(void) {
    stop.stop_at = server.size * STOP_NUMERATOR / STOP_DENOMINATOR;
    TEST_ASSERT_EQUAL_INT_MESSAGE(SKIFF_ERR_NET_CONNECTION_LOST, attempt(),
                                  "the stop hook's error comes back unchanged");
    TEST_ASSERT_FALSE(result.complete);
    TEST_ASSERT_TRUE_MESSAGE(result.bytes_received >= stop.stop_at, "stopped after the point");
    TEST_ASSERT_TRUE_MESSAGE(result.bytes_received < server.size, "stopped before the end");
    stop.stop_at = 0;
    return result.bytes_received;
}

static void assert_complete(void) {
    TEST_ASSERT_TRUE(result.complete);
    uint64_t size = 0;
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, skiff_storage_size(storage, target, &size));
    TEST_ASSERT_EQUAL_UINT64(server.size, size);
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_STORAGE_NOT_FOUND,
                          skiff_storage_size(storage, state_path, &size));
}

static void test_stopped_download_resumes_on_a_new_connection(void) {
    const uint64_t saved = download_part_way();
    new_connection();
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, attempt());
    TEST_ASSERT_EQUAL_INT64(206, result.response.status);
    TEST_ASSERT_EQUAL_UINT64(saved, result.resumed_from);
    TEST_ASSERT_EQUAL_UINT64(server.size - saved, result.bytes_received);
    TEST_ASSERT_FALSE(result.restarted);
    assert_complete();
}

static void test_progress_for_another_version_of_the_file_starts_over(void) {
    download_part_way();
    /* Rewrite the saved ETag, as if the file had been replaced on the server since. */
    char text[SKIFF_DOWNLOAD_STATE_MAX + 1];
    size_t length = 0;
    skiff_file *file = NULL;
    TEST_ASSERT_EQUAL_INT(SKIFF_OK,
                          skiff_storage_open(storage, state_path, SKIFF_FILE_READ, 0, &file));
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, skiff_file_read(file, text, sizeof text, &length));
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, skiff_file_close(file));
    skiff_download_state state;
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, skiff_download_state_parse(text, length, &state));
    TEST_PRINTF("saved ETag %s at %llu, replaced by " STALE_ETAG, state.etag,
                (unsigned long long)state.offset);
    snprintf(state.etag, sizeof state.etag, "%s", STALE_ETAG);
    TEST_ASSERT_EQUAL_INT(SKIFF_OK,
                          skiff_download_state_format(&state, text, sizeof text, &length));
    TEST_ASSERT_EQUAL_INT(SKIFF_OK,
                          skiff_storage_open(storage, state_path, SKIFF_FILE_REPLACE, 0, &file));
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, skiff_file_write(file, text, length));
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, skiff_file_close(file));

    new_connection();
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, attempt());
    TEST_ASSERT_EQUAL_INT64(200, result.response.status);
    TEST_ASSERT_TRUE(result.restarted);
    TEST_ASSERT_EQUAL_UINT64(server.size, result.bytes_received);
    assert_complete();
}

static void test_a_wrong_checksum_leaves_nothing_behind(void) {
    spec.expected_crc32 = server.crc32 ^ 1U;
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_ROMM_CHECKSUM, attempt());
    uint64_t size = 0;
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_STORAGE_NOT_FOUND, skiff_storage_size(storage, target, &size));
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_STORAGE_NOT_FOUND,
                          skiff_storage_size(storage, state_path, &size));
}

int main(void) {
    server.certs = required_env("SKIFF_IT_CERTS");
    server.token = required_env("SKIFF_IT_TOKEN");
    server.rom_id = required_env("SKIFF_IT_ROM_ID");
    server.file_name = required_env("SKIFF_IT_FILE_NAME");
    server.size = strtoull(required_env("SKIFF_IT_SIZE"), NULL, DECIMAL_BASE);
    server.crc32 = (uint32_t)strtoul(required_env("SKIFF_IT_CRC32"), NULL, HEX_BASE);
    snprintf(ca_file, sizeof ca_file, "%s/ca.crt", server.certs);
    snprintf(authorization, sizeof authorization, "Bearer %s", server.token);
    snprintf(url, sizeof url, TLS_SITE "/api/roms/%s/content/%s", server.rom_id, server.file_name);
    if (skiff_net_global_init() != SKIFF_OK) {
        printf("setup failed: TLS could not be seeded\n");
        return EXIT_FAILURE;
    }

    UNITY_BEGIN();
    RUN_TEST(test_stopped_download_resumes_on_a_new_connection);
    RUN_TEST(test_progress_for_another_version_of_the_file_starts_over);
    RUN_TEST(test_a_wrong_checksum_leaves_nothing_behind);
    const int failures = UNITY_END();

    skiff_net_global_cleanup();
    return failures;
}
