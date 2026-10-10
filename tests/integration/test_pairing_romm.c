/*
 * Pairing (skiff/romm_pairing.h) against the integration RomM behind Caddy, over the curl transport
 * and the TLS stack the PSP links: Skiff starts a pairing, polls while the player has not approved
 * yet, the admin approves it through RomM's own endpoint (what the web UI does), and the token
 * Skiff receives browses the PSP platform and the player's favourites and downloads a ROM, with the
 * scopes Skiff asked for, and is refused RomM's collections, which need a scope Skiff does not ask
 * for.
 * Then a refused pairing (207) and a code that was already used (208). A pairing that runs out of
 * time is not run: RomM gives every pairing 10 minutes; the unit tests replay that answer.
 *
 * Run by `scripts/dev.sh romm-test` (tests/integration/transport-test.sh), with the server's
 * details in SKIFF_IT_* environment variables. Not a ctest test: it needs the server.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "skiff/curl_transport.h"
#include "skiff/romm.h"
#include "skiff/romm_pairing.h"

#include "unity.h"

enum { PATH_MAX_LENGTH = 256, BODY_MAX = 512, DECIMAL_BASE = 10, HTTP_FORBIDDEN = 403 };

#define TLS_SITE "https://proxy:8443"
#define PLATFORM_SLUG "psp"
#define TEST_CONNECT_TIMEOUT_S 5L
#define TEST_STALL_TIMEOUT_S 10L
/* Fixed, so every run pairs the same device: RomM keeps one record per identifier. */
#define TEST_IDENTIFIER "000000000000000000000000005c1ff0"
/* What Skiff asks for (src/romm/pairing.c); the admin grants all of it. */
#define APPROVED_SCOPES "[\"platforms.read\",\"roms.read\"]"
/* More polls than a pending pairing needs, so a stuck flow fails instead of hanging. */
#define POLLS_MAX 6

static char ca_file[PATH_MAX_LENGTH];
static const char *admin_authorization;
static uint64_t payload_rom_id;
static const char *payload_file_name; /* percent-encoded */
static uint64_t payload_size;
static skiff_transport *transport;
static skiff_romm_client client;
static skiff_romm_pairing pairing;
static skiff_romm_pairing_result result;

static const char *required_env(const char *name) {
    const char *value = getenv(name);
    if (value == NULL || value[0] == '\0') {
        printf("missing environment variable %s (run through scripts/dev.sh romm-test)\n", name);
        exit(EXIT_FAILURE);
    }
    return value;
}

void setUp(void) {
    const skiff_curl_config config = {.ca_file = ca_file,
                                      .connect_timeout_s = TEST_CONNECT_TIMEOUT_S,
                                      .stall_timeout_s = TEST_STALL_TIMEOUT_S};
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, skiff_curl_transport_create(&config, &transport));
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, skiff_romm_client_init(&client, transport, TLS_SITE, NULL));
    memset(&pairing, 0, sizeof pairing);
    memset(&result, 0, sizeof result);
}

void tearDown(void) {
    skiff_romm_pairing_clear(&pairing, &result);
    skiff_romm_client_clear(&client);
    skiff_transport_destroy(transport);
    transport = NULL;
}

static void sleep_seconds(uint32_t seconds) {
    const struct timespec wait = {.tv_sec = (time_t)seconds, .tv_nsec = 0};
    nanosleep(&wait, NULL);
}

/* What the web UI sends when the admin approves or refuses user_code. */
static long admin_post(const char *path, const char *json) {
    char url[PATH_MAX_LENGTH];
    snprintf(url, sizeof url, TLS_SITE "%s", path);
    const skiff_http_header authorization = {"Authorization", admin_authorization};
    skiff_http_request request;
    memset(&request, 0, sizeof request);
    request.method = SKIFF_HTTP_POST;
    request.url = url;
    request.headers = &authorization;
    request.header_count = 1;
    request.body = json;
    request.body_size = strlen(json);
    request.content_type = "application/json";
    skiff_http_response response;
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, skiff_transport_perform(transport, &request, &response));
    TEST_PRINTF("admin POST %s -> HTTP %ld", path, response.status);
    return response.status;
}

static void start(void) {
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, skiff_romm_pairing_start(&client, TEST_IDENTIFIER, &pairing));
    TEST_PRINTF("pairing: enter %s at %s (every %u s, for %u s)", pairing.user_code,
                pairing.verification_url, (unsigned)pairing.interval_s,
                (unsigned)pairing.expires_in_s);
}

/* Polls as the app does: wait the interval, ask, until the answer is not pending. */
static skiff_err poll_until_answered(void) {
    for (int i = 0; i < POLLS_MAX; i++) {
        sleep_seconds(pairing.interval_s);
        const skiff_err err = skiff_romm_pairing_poll(&client, &pairing, &result);
        TEST_PRINTF("poll %d -> %s, state %d", i + 1, skiff_err_name(err), (int)result.state);
        if (err != SKIFF_OK || result.state == SKIFF_ROMM_PAIRING_APPROVED) {
            return err;
        }
    }
    TEST_FAIL_MESSAGE("the pairing was never answered");
    return SKIFF_ERR_ROMM_BAD_RESPONSE;
}

static skiff_err count_body(void *ctx, const unsigned char *data, size_t size) {
    (void)data;
    *(uint64_t *)ctx += size;
    return SKIFF_OK;
}

/* The status of a GET with client's token. */
static long paired_get_status(const skiff_romm_client *paired, const char *path) {
    char url[PATH_MAX_LENGTH];
    snprintf(url, sizeof url, TLS_SITE "%s", path);
    skiff_http_header authorization;
    TEST_ASSERT_EQUAL_size_t(1, skiff_romm_auth_header(paired, &authorization));
    skiff_http_request request;
    memset(&request, 0, sizeof request);
    request.url = url;
    request.headers = &authorization;
    request.header_count = 1;
    skiff_http_response response;
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, skiff_transport_perform(transport, &request, &response));
    TEST_PRINTF("GET %s with the paired token -> HTTP %ld", path, response.status);
    return response.status;
}

static void test_an_approved_pairing_gives_a_token_that_browses_and_downloads(void) {
    start();
    TEST_PRINTF("before the admin approves, RomM says pending (or slow_down if polled too soon)");
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, skiff_romm_pairing_poll(&client, &pairing, &result));
    TEST_ASSERT_NOT_EQUAL_INT(SKIFF_ROMM_PAIRING_APPROVED, result.state);

    char approve[BODY_MAX];
    snprintf(approve, sizeof approve,
             "{\"user_code\":\"%s\",\"approved_scopes\":" APPROVED_SCOPES "}", pairing.user_code);
    TEST_ASSERT_EQUAL_INT64(200, admin_post("/api/auth/device/approve", approve));
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, poll_until_answered());
    TEST_ASSERT_EQUAL_INT(SKIFF_ROMM_PAIRING_APPROVED, result.state);
    TEST_ASSERT_GREATER_THAN_size_t(0, strlen(result.token));
    TEST_PRINTF("RomM's device id for this PSP: %s", result.device_id);
    TEST_ASSERT_GREATER_THAN_size_t(0, strlen(result.device_id));

    TEST_PRINTF("the code is spent: asking again is an expired pairing");
    skiff_romm_pairing_result again;
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_ROMM_PAIRING_EXPIRED,
                          skiff_romm_pairing_poll(&client, &pairing, &again));

    skiff_romm_client paired;
    TEST_ASSERT_EQUAL_INT(SKIFF_OK,
                          skiff_romm_client_init(&paired, transport, TLS_SITE, result.token));
    skiff_romm_platform platform;
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, skiff_romm_find_platform(&paired, PLATFORM_SLUG, &platform));
    const skiff_romm_list_query all = {.platform_id = platform.id, .filter = SKIFF_ROMM_LIST_ALL};
    skiff_romm_rom_page page;
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, skiff_romm_list_roms(&paired, &all, 0, 1, &page));
    TEST_ASSERT_EQUAL_size_t(1, page.count);

    TEST_PRINTF("the player's favourites need no scope beyond roms.read");
    const skiff_romm_list_query favourites = {
        .platform_id = platform.id, .filter = SKIFF_ROMM_LIST_FAVOURITES, .with_files = 1};
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, skiff_romm_list_roms(&paired, &favourites, 0, 1, &page));
    TEST_PRINTF("favourites with the paired token: %zu of %llu", page.count,
                (unsigned long long)page.total);
    TEST_ASSERT_EQUAL_size_t(1, page.count);
    TEST_ASSERT_EQUAL_UINT64(payload_rom_id, page.items[0].id);
    TEST_ASSERT_TRUE(page.items[0].has_file);
    TEST_PRINTF("RomM's collections need collections.read, which Skiff does not ask for");
    TEST_ASSERT_EQUAL_INT64(HTTP_FORBIDDEN, paired_get_status(&paired, "/api/collections"));

    char url[PATH_MAX_LENGTH];
    snprintf(url, sizeof url, TLS_SITE "/api/roms/%llu/content/%s",
             (unsigned long long)payload_rom_id, payload_file_name);
    skiff_http_header authorization;
    TEST_ASSERT_EQUAL_size_t(1, skiff_romm_auth_header(&paired, &authorization));
    uint64_t received = 0;
    skiff_http_request request;
    memset(&request, 0, sizeof request);
    request.url = url;
    request.headers = &authorization;
    request.header_count = 1;
    request.on_body = count_body;
    request.body_ctx = &received;
    skiff_http_response response;
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, skiff_transport_perform(transport, &request, &response));
    TEST_PRINTF("download with the paired token -> HTTP %ld, %llu bytes", response.status,
                (unsigned long long)received);
    TEST_ASSERT_EQUAL_INT64(200, response.status);
    TEST_ASSERT_EQUAL_UINT64(payload_size, received);
    skiff_romm_client_clear(&paired);
}

static void test_a_refused_pairing_ends_with_207(void) {
    start();
    char deny[BODY_MAX];
    snprintf(deny, sizeof deny, "{\"user_code\":\"%s\"}", pairing.user_code);
    TEST_ASSERT_EQUAL_INT64(200, admin_post("/api/auth/device/deny", deny));
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_ROMM_PAIRING_DENIED, poll_until_answered());
    TEST_ASSERT_EQUAL_STRING("", result.token);
}

int main(void) {
    snprintf(ca_file, sizeof ca_file, "%s/ca.crt", required_env("SKIFF_IT_CERTS"));
    admin_authorization = required_env("SKIFF_IT_ADMIN_AUTHORIZATION");
    payload_rom_id = strtoull(required_env("SKIFF_IT_ROM_ID"), NULL, DECIMAL_BASE);
    payload_file_name = required_env("SKIFF_IT_FILE_NAME");
    payload_size = strtoull(required_env("SKIFF_IT_SIZE"), NULL, DECIMAL_BASE);
    if (skiff_net_global_init() != SKIFF_OK) {
        printf("setup failed: TLS could not be seeded\n");
        return EXIT_FAILURE;
    }

    UNITY_BEGIN();
    RUN_TEST(test_an_approved_pairing_gives_a_token_that_browses_and_downloads);
    RUN_TEST(test_a_refused_pairing_ends_with_207);
    const int failures = UNITY_END();
    skiff_net_global_cleanup();
    return failures;
}
