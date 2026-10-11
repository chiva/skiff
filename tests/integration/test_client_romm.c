/*
 * The RomM client (skiff/romm.h) against the integration RomM behind Caddy, over the curl transport
 * and the TLS stack the PSP links: the version check, the PSP platform, every ROM page by page, the
 * seeded favourite with its file, the seeded collections (listed only with collections.read) and
 * their games, a ROM's files, and a download URL built from a file name full of reserved characters
 * that brings back exactly the bytes RomM recorded the CRC-32 of, and the seeded cover, fetched
 * without the token from the URL its ROM's details give.
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

enum {
    PATH_MAX_LENGTH = 256,
    HEX_BASE = 16,
    DECIMAL_BASE = 10,
    SEEDED_ROMS = 2,
    /* The payload (tests/integration/seed.py). */
    SEEDED_FAVOURITES = 1,
    HTTP_OK = 200,
    HTTP_NOT_FOUND = 404,
    /* The first bytes of a cover, enough to tell its format. */
    MAGIC_BYTES = 8,
};

/* The seed uploads a PNG cover (tests/integration/seed.py). */
#define MISSING_COVER SKIFF_ROMM_COVER_PREFIX "roms/0/0/cover/small.png?ts=x"
static const unsigned char PNG_MAGIC[] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};

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
    unsigned char magic[MAGIC_BYTES];
} received;

static const char *token;
/* A token that may list collections, and the seeded collections (tests/integration/seed.py). */
static const char *collections_token;
static uint64_t shelf_id;
static uint64_t shelf_roms;
static uint64_t empty_shelf_id;
static uint64_t smart_shelf_id;
static uint64_t viewer_shelf_id;
static uint64_t viewer_private_id;
static skiff_romm_collection_list collections;
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

static uint64_t env_number(const char *name) {
    return strtoull(required_env(name), NULL, DECIMAL_BASE);
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
    const skiff_romm_list_query all = {.platform_id = platform.id, .filter = SKIFF_ROMM_LIST_ALL};
    skiff_romm_rom_page page;
    int seen_payload = 0;
    int seen_extra = 0;
    for (uint64_t offset = 0; offset <= SEEDED_ROMS; offset++) {
        TEST_ASSERT_EQUAL_INT(SKIFF_OK, skiff_romm_list_roms(&client, &all, offset, 1, &page));
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
    TEST_ASSERT_EQUAL_INT(SKIFF_OK,
                          skiff_romm_list_roms(&client, &all, 0, SKIFF_ROMM_PAGE_SIZE, &page));
    TEST_ASSERT_EQUAL_size_t(SEEDED_ROMS, page.count);
    TEST_ASSERT_EQUAL_UINT64(extra.rom_id, page.items[0].id);
    TEST_ASSERT_EQUAL_UINT64(payload.rom_id, page.items[1].id);
}

static void test_the_favourites_hold_the_seeded_favourite_with_its_file(void) {
    skiff_romm_platform platform;
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, skiff_romm_find_platform(&client, PLATFORM_SLUG, &platform));
    const skiff_romm_list_query favourites = {
        .platform_id = platform.id, .filter = SKIFF_ROMM_LIST_FAVOURITES, .with_files = 1};
    skiff_romm_rom_page page;
    TEST_ASSERT_EQUAL_INT(
        SKIFF_OK, skiff_romm_list_roms(&client, &favourites, 0, SKIFF_ROMM_PAGE_SIZE, &page));
    TEST_PRINTF("favourites: %zu of %llu%s%s", page.count, (unsigned long long)page.total,
                page.count > 0 ? ", first file " : "",
                page.count > 0 ? page.items[0].file.file_name : "");
    TEST_ASSERT_EQUAL_UINT64(SEEDED_FAVOURITES, page.total);
    TEST_ASSERT_EQUAL_size_t(SEEDED_FAVOURITES, page.count);
    TEST_ASSERT_EQUAL_UINT64(payload.rom_id, page.items[0].id);
    TEST_ASSERT_TRUE(page.items[0].has_file);
    TEST_ASSERT_EQUAL_STRING(payload.file_name, page.items[0].file.file_name);
    TEST_ASSERT_EQUAL_UINT64(payload.size, page.items[0].file.size);
    TEST_ASSERT_TRUE(page.items[0].file.has_crc32);
    TEST_ASSERT_EQUAL_HEX32(payload.crc32, page.items[0].file.crc32);
}

/* The listed collection with id and kind, or NULL. */
static const skiff_romm_collection *listed(uint64_t id, int smart) {
    for (size_t i = 0; i < collections.count; i++) {
        if (collections.items[i].id == id && collections.items[i].smart == smart) {
            return &collections.items[i];
        }
    }
    return NULL;
}

static void test_a_token_with_collections_read_lists_every_visible_collection(void) {
    skiff_romm_client reader;
    TEST_ASSERT_EQUAL_INT(SKIFF_OK,
                          skiff_romm_client_init(&reader, transport, TLS_SITE, collections_token));
    const skiff_err err = skiff_romm_list_collections(&reader, &collections);
    skiff_romm_client_clear(&reader);
    TEST_PRINTF("collections: %s, %zu", skiff_err_name(err), collections.count);
    for (size_t i = 0; i < collections.count; i++) {
        TEST_PRINTF("  %llu%s '%s': %llu ROMs", (unsigned long long)collections.items[i].id,
                    collections.items[i].smart ? " (smart)" : "", collections.items[i].name,
                    (unsigned long long)collections.items[i].rom_count);
    }
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, err);
    TEST_PRINTF("the admin's shelf, empty shelf and smart shelf, and the viewer's public shelf");
    TEST_ASSERT_EQUAL_size_t(4, collections.count);
    TEST_ASSERT_NOT_NULL(listed(shelf_id, 0));
    TEST_ASSERT_EQUAL_UINT64(shelf_roms, listed(shelf_id, 0)->rom_count);
    TEST_ASSERT_NOT_NULL(listed(empty_shelf_id, 0));
    TEST_ASSERT_EQUAL_UINT64(0, listed(empty_shelf_id, 0)->rom_count);
    TEST_ASSERT_NOT_NULL(listed(smart_shelf_id, 1));
    TEST_ASSERT_EQUAL_UINT64(1, listed(smart_shelf_id, 1)->rom_count);
    TEST_ASSERT_NOT_NULL(listed(viewer_shelf_id, 0));
    TEST_PRINTF("not the viewer's private one, nor the admin's favourites");
    TEST_ASSERT_NULL(listed(viewer_private_id, 0));
    TEST_ASSERT_EQUAL_STRING("Skiff Shelf", listed(shelf_id, 0)->name);
}

static void test_a_token_without_collections_read_is_refused_the_collections(void) {
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_ROMM_FORBIDDEN,
                          skiff_romm_list_collections(&client, &collections));
    TEST_ASSERT_EQUAL_size_t(0, collections.count);
}

/* The total of a collection's PSP games, as the first page of 25 gives it. */
static uint64_t collection_total(uint64_t platform_id, skiff_romm_list_filter filter,
                                 uint64_t collection_id) {
    const skiff_romm_list_query query = {.platform_id = platform_id,
                                         .filter = filter,
                                         .collection_id = collection_id,
                                         .with_files = 1};
    static skiff_romm_rom_page page;
    TEST_ASSERT_EQUAL_INT(SKIFF_OK,
                          skiff_romm_list_roms(&client, &query, 0, SKIFF_ROMM_PAGE_SIZE, &page));
    TEST_PRINTF("%s %llu: %zu of %llu%s%s",
                filter == SKIFF_ROMM_LIST_SMART_COLLECTION ? "smart collection" : "collection",
                (unsigned long long)collection_id, page.count, (unsigned long long)page.total,
                page.count > 0 ? ", first " : "", page.count > 0 ? page.items[0].fs_name : "");
    for (size_t i = 0; i < page.count; i++) {
        TEST_ASSERT_TRUE(page.items[i].has_file);
    }
    return page.total;
}

static void test_a_collections_games_are_listed_with_roms_read_alone(void) {
    skiff_romm_platform platform;
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, skiff_romm_find_platform(&client, PLATFORM_SLUG, &platform));
    TEST_ASSERT_EQUAL_UINT64(shelf_roms,
                             collection_total(platform.id, SKIFF_ROMM_LIST_COLLECTION, shelf_id));
    TEST_ASSERT_EQUAL_UINT64(
        0, collection_total(platform.id, SKIFF_ROMM_LIST_COLLECTION, empty_shelf_id));
    TEST_ASSERT_EQUAL_UINT64(
        1, collection_total(platform.id, SKIFF_ROMM_LIST_SMART_COLLECTION, smart_shelf_id));
    TEST_ASSERT_EQUAL_UINT64(
        1, collection_total(platform.id, SKIFF_ROMM_LIST_COLLECTION, viewer_shelf_id));
    TEST_PRINTF("another user's private collection lists nothing (RomM 5.4 checks the owner)");
    TEST_ASSERT_EQUAL_UINT64(
        0, collection_total(platform.id, SKIFF_ROMM_LIST_COLLECTION, viewer_private_id));
}

static skiff_err count_body(void *ctx, const unsigned char *data, size_t size) {
    received *got = ctx;
    for (size_t i = 0; i < size && got->bytes + i < MAGIC_BYTES; i++) {
        got->magic[got->bytes + i] = data[i];
    }
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
    TEST_ASSERT_EQUAL_INT(SKIFF_ROMM_NAME_OK, rom.files[0].name_status);

    char url[SKIFF_ROMM_CONTENT_URL_MAX];
    TEST_ASSERT_EQUAL_INT(
        SKIFF_OK, skiff_romm_content_url(&client, rom.summary.id, &rom.files[0], url, sizeof url));
    skiff_http_header authorization;
    TEST_ASSERT_EQUAL_size_t(1, skiff_romm_auth_header(&client, &authorization));
    received got = {0, 0, {0}};
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

/* GETs url without any header, the way the app fetches a cover. */
static long get_without_token(const char *url, received *got) {
    memset(got, 0, sizeof *got);
    skiff_http_request request;
    memset(&request, 0, sizeof request);
    request.url = url;
    request.on_body = count_body;
    request.body_ctx = got;
    skiff_http_response response;
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, skiff_transport_perform(transport, &request, &response));
    TEST_PRINTF("GET %s -> HTTP %ld, %llu bytes", url, response.status,
                (unsigned long long)got->bytes);
    return response.status;
}

static void test_a_cover_comes_without_the_token_from_its_rom_details(void) {
    skiff_romm_rom rom;
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, skiff_romm_get_rom(&client, payload.rom_id, &rom));
    TEST_PRINTF("ROM %llu cover path '%s'", (unsigned long long)payload.rom_id, rom.cover_path);
    TEST_ASSERT_NOT_EQUAL(0, rom.cover_path[0]);
    char url[SKIFF_ROMM_COVER_URL_MAX];
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, skiff_romm_cover_url(&client, rom.cover_path, url, sizeof url));
    received got;
    TEST_ASSERT_EQUAL_INT(HTTP_OK, get_without_token(url, &got));
    TEST_ASSERT_GREATER_THAN_UINT64(sizeof PNG_MAGIC, got.bytes);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(PNG_MAGIC, got.magic, sizeof PNG_MAGIC);
    TEST_PRINTF("the extra ROM has none");
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, skiff_romm_get_rom(&client, extra.rom_id, &rom));
    TEST_ASSERT_EQUAL_STRING("", rom.cover_path);
    TEST_PRINTF("a cover RomM does not have is a 404");
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, skiff_romm_cover_url(&client, MISSING_COVER, url, sizeof url));
    TEST_ASSERT_EQUAL_INT(HTTP_NOT_FOUND, get_without_token(url, &got));
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
    collections_token = required_env("SKIFF_IT_COLLECTIONS_TOKEN");
    shelf_id = env_number("SKIFF_IT_SHELF_ID");
    shelf_roms = env_number("SKIFF_IT_SHELF_ROMS");
    empty_shelf_id = env_number("SKIFF_IT_EMPTY_SHELF_ID");
    smart_shelf_id = env_number("SKIFF_IT_SMART_SHELF_ID");
    viewer_shelf_id = env_number("SKIFF_IT_VIEWER_SHELF_ID");
    viewer_private_id = env_number("SKIFF_IT_VIEWER_PRIVATE_ID");
    payload = seeded("");
    extra = seeded("EXTRA_");
    if (skiff_net_global_init() != SKIFF_OK) {
        printf("setup failed: TLS could not be seeded\n");
        return EXIT_FAILURE;
    }

    UNITY_BEGIN();
    RUN_TEST(test_the_server_version_is_supported);
    RUN_TEST(test_every_rom_is_listed_page_by_page);
    RUN_TEST(test_the_favourites_hold_the_seeded_favourite_with_its_file);
    RUN_TEST(test_a_token_with_collections_read_lists_every_visible_collection);
    RUN_TEST(test_a_token_without_collections_read_is_refused_the_collections);
    RUN_TEST(test_a_collections_games_are_listed_with_roms_read_alone);
    RUN_TEST(test_a_reserved_file_name_downloads_through_its_url);
    RUN_TEST(test_a_cover_comes_without_the_token_from_its_rom_details);
    RUN_TEST(test_a_wrong_token_is_refused_by_romm);
    const int failures = UNITY_END();

    skiff_net_global_cleanup();
    return failures;
}
