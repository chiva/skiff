/*
 * The RomM client (skiff/romm.h) against the integration RomM behind Caddy, over the curl transport
 * and the TLS stack the PSP links: the version check, the PSP platform, every ROM page by page, a
 * ROM's files, and a download URL built from a file name full of reserved characters that brings
 * back exactly the bytes RomM recorded the CRC-32 of.
 *
 * Run by `scripts/dev.sh romm-test` (tests/integration/transport-test.sh), with the server's
 * details in SKIFF_IT_* environment variables. Not a ctest test: it needs the server.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>

#include "skiff/curl_transport.h"
#include "skiff/romm.h"

#include "unity.h"

enum { PATH_MAX_LENGTH = 256, HEX_BASE = 16, DECIMAL_BASE = 10, SEEDED_ROMS = 2 };

#define TLS_SITE "https://proxy:8443"
#define PLATFORM_SLUG "psp"
#define TEST_CONNECT_TIMEOUT_S 5L
#define TEST_STALL_TIMEOUT_S 10L

typedef struct seeded_file {
    uint64_t rom_id;
    const char *file_name; /* as stored, not encoded */
    uint64_t size;
    uint32_t crc32;
} seeded_file;

/* What a download received. */
typedef struct received {
    uint64_t bytes;
    uint32_t crc32;
} received;

static const char *token;
static seeded_file payload;
static seeded_file extra;
static char ca_file[PATH_MAX_LENGTH];
static skiff_transport *transport;
static skiff_romm_client client;

static const char *required_env(const char *name) {
    const char *value = getenv(name);
    if (value == NULL || value[0] == '\0') {
        printf("missing environment variable %s (run through scripts/dev.sh romm-test)\n", name);
        exit(EXIT_FAILURE);
    }
    return value;
}

static seeded_file seeded(const char *prefix) {
    char name[PATH_MAX_LENGTH];
    seeded_file file;
    snprintf(name, sizeof name, "SKIFF_IT_%sROM_ID", prefix);
    file.rom_id = strtoull(required_env(name), NULL, DECIMAL_BASE);
    snprintf(name, sizeof name, "SKIFF_IT_%sFILE_NAME_RAW", prefix);
    file.file_name = required_env(name);
    snprintf(name, sizeof name, "SKIFF_IT_%sSIZE", prefix);
    file.size = strtoull(required_env(name), NULL, DECIMAL_BASE);
    snprintf(name, sizeof name, "SKIFF_IT_%sCRC32", prefix);
    file.crc32 = (uint32_t)strtoul(required_env(name), NULL, HEX_BASE);
    return file;
}

void setUp(void) {
    const skiff_curl_config config = {.ca_file = ca_file,
                                      .connect_timeout_s = TEST_CONNECT_TIMEOUT_S,
                                      .stall_timeout_s = TEST_STALL_TIMEOUT_S};
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, skiff_curl_transport_create(&config, &transport));
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, skiff_romm_client_init(&client, transport, TLS_SITE, token));
}

void tearDown(void) {
    skiff_romm_client_clear(&client);
    skiff_transport_destroy(transport);
    transport = NULL;
}

static void test_the_server_version_is_supported(void) {
    skiff_romm_server server;
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, skiff_romm_heartbeat(&client, &server));
    TEST_PRINTF("RomM %s: known %d, newer than tested %d", server.version, server.version_known,
                server.newer_than_tested);
    TEST_ASSERT_TRUE(server.version_known);
    TEST_ASSERT_FALSE(server.newer_than_tested);
}

static void test_every_rom_is_listed_page_by_page(void) {
    skiff_romm_platform platform;
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, skiff_romm_find_platform(&client, PLATFORM_SLUG, &platform));
    TEST_ASSERT_EQUAL_UINT64(SEEDED_ROMS, platform.rom_count);
    skiff_romm_rom_page page;
    int seen_payload = 0;
    int seen_extra = 0;
    for (uint64_t offset = 0; offset <= SEEDED_ROMS; offset++) {
        TEST_ASSERT_EQUAL_INT(SKIFF_OK,
                              skiff_romm_list_roms(&client, platform.id, offset, 1, &page));
        TEST_PRINTF("offset %llu: %zu item(s) of %llu%s%s", (unsigned long long)offset, page.count,
                    (unsigned long long)page.total, page.count > 0 ? ", " : "",
                    page.count > 0 ? page.items[0].fs_name : "");
        TEST_ASSERT_EQUAL_UINT64(SEEDED_ROMS, page.total);
        TEST_ASSERT_EQUAL_size_t(offset < SEEDED_ROMS ? 1 : 0, page.count);
        for (size_t i = 0; i < page.count; i++) {
            seen_payload += page.items[i].id == payload.rom_id;
            seen_extra += page.items[i].id == extra.rom_id;
        }
    }
    TEST_ASSERT_EQUAL_INT(1, seen_payload);
    TEST_ASSERT_EQUAL_INT(1, seen_extra);
    TEST_PRINTF("a full page holds both, in name order");
    TEST_ASSERT_EQUAL_INT(
        SKIFF_OK, skiff_romm_list_roms(&client, platform.id, 0, SKIFF_ROMM_PAGE_SIZE, &page));
    TEST_ASSERT_EQUAL_size_t(SEEDED_ROMS, page.count);
    TEST_ASSERT_EQUAL_UINT64(extra.rom_id, page.items[0].id);
    TEST_ASSERT_EQUAL_UINT64(payload.rom_id, page.items[1].id);
}

static skiff_err count_body(void *ctx, const unsigned char *data, size_t size) {
    received *got = ctx;
    got->crc32 = (uint32_t)crc32(got->crc32, data, (uInt)size);
    got->bytes += size;
    return SKIFF_OK;
}

static void test_a_reserved_file_name_downloads_through_its_url(void) {
    skiff_romm_rom rom;
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, skiff_romm_get_rom(&client, extra.rom_id, &rom));
    TEST_ASSERT_EQUAL_size_t(1, rom.file_count);
    TEST_ASSERT_EQUAL_size_t(1, rom.stored_count);
    TEST_ASSERT_EQUAL_STRING(extra.file_name, rom.files[0].file_name);
    TEST_ASSERT_EQUAL_UINT64(extra.size, rom.files[0].size);
    TEST_ASSERT_TRUE(rom.files[0].has_crc32);
    TEST_ASSERT_EQUAL_HEX32(extra.crc32, rom.files[0].crc32);

    char url[SKIFF_ROMM_CONTENT_URL_MAX];
    TEST_ASSERT_EQUAL_INT(
        SKIFF_OK,
        skiff_romm_content_url(&client, rom.summary.id, rom.files[0].file_name, url, sizeof url));
    skiff_http_header authorization;
    TEST_ASSERT_EQUAL_size_t(1, skiff_romm_auth_header(&client, &authorization));
    received got = {0, 0};
    skiff_http_request request;
    memset(&request, 0, sizeof request);
    request.url = url;
    request.headers = &authorization;
    request.header_count = 1;
    request.on_body = count_body;
    request.body_ctx = &got;
    skiff_http_response response;
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, skiff_transport_perform(transport, &request, &response));
    TEST_PRINTF("GET %s -> HTTP %ld, %llu bytes, CRC-32 %lx", url, response.status,
                (unsigned long long)got.bytes, (unsigned long)got.crc32);
    TEST_ASSERT_EQUAL_INT(200, response.status);
    TEST_ASSERT_EQUAL_UINT64(extra.size, got.bytes);
    TEST_ASSERT_EQUAL_HEX32(extra.crc32, got.crc32);
}

static void test_a_wrong_token_is_refused_by_romm(void) {
    skiff_romm_client wrong;
    TEST_ASSERT_EQUAL_INT(SKIFF_OK,
                          skiff_romm_client_init(&wrong, transport, TLS_SITE, "rmm_not_a_token"));
    skiff_romm_platform platform;
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_ROMM_UNAUTHORIZED,
                          skiff_romm_find_platform(&wrong, PLATFORM_SLUG, &platform));
    skiff_romm_client_clear(&wrong);
}

int main(void) {
    snprintf(ca_file, sizeof ca_file, "%s/ca.crt", required_env("SKIFF_IT_CERTS"));
    token = required_env("SKIFF_IT_TOKEN");
    payload = seeded("");
    extra = seeded("EXTRA_");
    if (skiff_net_global_init() != SKIFF_OK) {
        printf("setup failed: TLS could not be seeded\n");
        return EXIT_FAILURE;
    }

    UNITY_BEGIN();
    RUN_TEST(test_the_server_version_is_supported);
    RUN_TEST(test_every_rom_is_listed_page_by_page);
    RUN_TEST(test_a_reserved_file_name_downloads_through_its_url);
    RUN_TEST(test_a_wrong_token_is_refused_by_romm);
    const int failures = UNITY_END();

    skiff_net_global_cleanup();
    return failures;
}
