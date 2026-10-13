/*
 * The RomM client (skiff/romm.h) against the integration RomM behind Caddy, over the curl transport
 * and the TLS stack the PSP links: the version check, the PSP platform, every ROM page by page, the
 * seeded favourite with its file, the seeded collections (listed only with collections.read) and
 * their games, a ROM's files, and a download URL built from a file name full of reserved characters
 * that brings back exactly the bytes RomM recorded the CRC-32 of, the seeded cover, fetched
 * without the token from the URL its ROM's details give, and the seeded description and facts.
 *
 * Then games sent from RomM's web UI (skiff/romm_installs.h): a PSP paired with every scope Skiff
 * asks for registers remote_install, the admin sends it the payload and the seed's game of another
 * platform as the web UI does, and the PSP claims both (oldest first), finds the second is not on
 * the PSP platform and reports it failed with its Spanish reason, reports the first done (a second
 * report is SKIFF_ERR_ROMM_NOT_FOUND), finds nothing left to claim, and turns remote_install off,
 * after which RomM refuses to send it anything.
 *
 * Run by `scripts/dev.sh romm-test` (tests/integration/transport-test.sh), with the server's
 * details in SKIFF_IT_* environment variables. Not a ctest test: it needs the server.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <zlib.h>

#include "skiff/curl_transport.h"
#include "skiff/i18n.h"
#include "skiff/romm.h"
#include "skiff/romm_installs.h"
#include "skiff/romm_pairing.h"

#include "unity.h"

enum {
    PATH_MAX_LENGTH = 256,
    HEX_BASE = 16,
    DECIMAL_BASE = 10,
    /* The payload, the extra file, the save disc and the two discs RomM converts
     * (tests/integration/seed.py). */
    SEEDED_ROMS = 5,
    /* The payload (tests/integration/seed.py). */
    SEEDED_FAVOURITES = 1,
    HTTP_OK = 200,
    HTTP_CREATED = 201,
    HTTP_BAD_REQUEST = 400,
    HTTP_NOT_FOUND = 404,
    BODY_MAX = 512,
    POLLS_MAX = 6,
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
/* Fixed, so every run pairs the same device: RomM keeps one record per identifier. */
#define SENDS_IDENTIFIER "0000000000000000000000005e7d000d"
#define ALL_SCOPES                                                                                 \
    "[\"platforms.read\",\"roms.read\",\"assets.read\",\"assets.write\",\"devices.read\","         \
    "\"devices.write\",\"collections.read\"]"

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
/* The synthetic PSP disc RomM reads a save ID from. */
static uint64_t save_disc_rom_id;
static const char *save_disc_target;
/* The seed's game of another platform, which RomM lets a player send to the PSP. */
static uint64_t foreign_rom_id;
/* The synthetic PSP discs stored as CHD, which RomM converts on download, and the save IDs RomM
 * reads from them as from the save disc. */
static uint64_t convert_rom_id;
static uint64_t convert_slow_rom_id;
static const char *convert_save_target;
static const char *convert_slow_save_target;
static const char *admin_authorization;
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
    TEST_PRINTF("a full page holds all five, in name order");
    TEST_ASSERT_EQUAL_INT(SKIFF_OK,
                          skiff_romm_list_roms(&client, &all, 0, SKIFF_ROMM_PAGE_SIZE, &page));
    TEST_ASSERT_EQUAL_size_t(SEEDED_ROMS, page.count);
    TEST_ASSERT_EQUAL_UINT64(convert_rom_id, page.items[0].id);
    TEST_ASSERT_EQUAL_UINT64(convert_slow_rom_id, page.items[1].id);
    TEST_ASSERT_EQUAL_UINT64(extra.rom_id, page.items[2].id);
    TEST_ASSERT_EQUAL_UINT64(save_disc_rom_id, page.items[3].id);
    TEST_ASSERT_EQUAL_UINT64(payload.rom_id, page.items[4].id);
}

/* The save ID the seed gave the disc id, or "" for a file that is not a disc. */
static const char *seeded_save_target(uint64_t id) {
    if (id == save_disc_rom_id) {
        return save_disc_target;
    }
    if (id == convert_rom_id) {
        return convert_save_target;
    }
    return id == convert_slow_rom_id ? convert_slow_save_target : "";
}

static void test_romm_reads_the_save_id_of_a_psp_disc(void) {
    skiff_romm_platform platform;
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, skiff_romm_find_platform(&client, PLATFORM_SLUG, &platform));
    const skiff_romm_list_query all = {.platform_id = platform.id, .filter = SKIFF_ROMM_LIST_ALL};
    skiff_romm_rom_page page;
    TEST_ASSERT_EQUAL_INT(SKIFF_OK,
                          skiff_romm_list_roms(&client, &all, 0, SKIFF_ROMM_PAGE_SIZE, &page));
    for (size_t i = 0; i < page.count; i++) {
        const skiff_romm_rom_summary *item = &page.items[i];
        TEST_PRINTF("ROM %llu %s: save_target \"%s\", layout %d", (unsigned long long)item->id,
                    item->fs_name, item->save_target, (int)item->save_layout);
        const char *expected = seeded_save_target(item->id);
        if (expected[0] != '\0') {
            TEST_PRINTF("  a disc, ISO or CHD: its save ID");
            TEST_ASSERT_EQUAL_STRING(expected, item->save_target);
            TEST_ASSERT_EQUAL_INT(SKIFF_ROMM_SAVE_LAYOUT_FOLDER_PREFIX, item->save_layout);
        } else {
            TEST_PRINTF("  not a disc RomM could read: no save ID");
            TEST_ASSERT_EQUAL_STRING("", item->save_target);
            TEST_ASSERT_EQUAL_INT(SKIFF_ROMM_SAVE_LAYOUT_NONE, item->save_layout);
        }
    }
    TEST_PRINTF("and in the disc's details");
    skiff_romm_rom rom;
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, skiff_romm_get_rom(&client, save_disc_rom_id, &rom));
    TEST_ASSERT_EQUAL_STRING(save_disc_target, rom.summary.save_target);
    TEST_ASSERT_EQUAL_INT(SKIFF_ROMM_SAVE_LAYOUT_FOLDER_PREFIX, rom.summary.save_layout);
    skiff_romm_server server;
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, skiff_romm_heartbeat(&client, &server));
    TEST_ASSERT_TRUE(skiff_romm_server_syncs_saves(&server));
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
    TEST_ASSERT_EQUAL_INT(SKIFF_OK,
                          skiff_romm_content_url(&client, rom.summary.id, &rom.files[0],
                                                 SKIFF_ROMM_FORMAT_STORED, url, sizeof url));
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

/* The payload's description and facts as Skiff keeps them: tests/integration/seed.py's
 * PAYLOAD_SUMMARY with its typographic punctuation made ASCII, and PAYLOAD_MANUAL_METADATA. */
#define SEEDED_DESCRIPTION                                                                         \
    "Skiff Test Payload is not a game: the integration seed makes up its bytes, its cover and "    \
    "this text, so Skiff's details screen has something to show.\n\nThis second paragraph is "     \
    "here to make the description scroll on a PSP - four or five lines beside the cover are not "  \
    "enough for it. Its \"quotes\", its dash and its ellipsis... come out as plain ASCII.\n\nA "   \
    "third paragraph closes it. Nothing here comes from a real game, a metadata provider or a "    \
    "player."
#define SEEDED_RELEASE_YEAR 2010

static void test_a_roms_details_carry_its_description_and_facts(void) {
    skiff_romm_rom rom;
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, skiff_romm_get_rom(&client, payload.rom_id, &rom));
    const skiff_romm_rom_metadata *m = &rom.metadata;
    TEST_PRINTF("ROM %llu: year %d, %zu genre(s), '%s' / '%s', players '%s', rating %u",
                (unsigned long long)payload.rom_id, m->release_year, m->genre_count, m->developer,
                m->publisher, m->players, m->rating);
    TEST_PRINTF("description: '%s'", m->description);
    TEST_ASSERT_EQUAL_STRING(SEEDED_DESCRIPTION, m->description);
    TEST_ASSERT_EQUAL_INT(SEEDED_RELEASE_YEAR, m->release_year);
    TEST_ASSERT_EQUAL_size_t(2, m->genre_count);
    TEST_ASSERT_EQUAL_STRING("Puzzle", m->genres[0]);
    TEST_ASSERT_EQUAL_STRING("Simulator", m->genres[1]);
    TEST_ASSERT_EQUAL_STRING("Skiff Test Studio", m->developer);
    TEST_ASSERT_EQUAL_STRING("Skiff Test Publishing", m->publisher);
    TEST_PRINTF("manual metadata sets no player count (RomM's \"1\") and no rating");
    TEST_ASSERT_EQUAL_STRING("", m->players);
    TEST_ASSERT_EQUAL_UINT(0, m->rating);
    TEST_ASSERT_EQUAL_size_t(0, m->region_count);
    TEST_PRINTF("the extra ROM has none of it");
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, skiff_romm_get_rom(&client, extra.rom_id, &rom));
    TEST_ASSERT_EQUAL_STRING("", m->description);
    TEST_ASSERT_EQUAL_INT(0, m->release_year);
    TEST_ASSERT_EQUAL_size_t(0, m->genre_count);
}

/* Sends a request as the admin, as RomM's web UI does; returns the HTTP status. */
static long admin_request(skiff_http_method method, const char *path, const char *body) {
    char url[PATH_MAX_LENGTH];
    snprintf(url, sizeof url, TLS_SITE "%s", path);
    const skiff_http_header authorization = {"Authorization", admin_authorization};
    skiff_http_request request;
    memset(&request, 0, sizeof request);
    request.method = method;
    request.url = url;
    request.headers = &authorization;
    request.header_count = 1;
    if (body != NULL) {
        request.body = body;
        request.body_size = strlen(body);
        request.content_type = "application/json";
    }
    skiff_http_response response;
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, skiff_transport_perform(transport, &request, &response));
    TEST_PRINTF("admin %s -> HTTP %ld", path, response.status);
    return response.status;
}

/* Pairs a PSP as the app does, the admin approving every scope; its token goes to *out. */
static void pair_psp(char *device_id, size_t device_id_size, skiff_romm_client *out) {
    skiff_romm_client open_client;
    skiff_romm_pairing pairing;
    skiff_romm_pairing_result result;
    char body[BODY_MAX];
    TEST_ASSERT_EQUAL_INT(SKIFF_OK,
                          skiff_romm_client_init(&open_client, transport, TLS_SITE, NULL));
    TEST_ASSERT_EQUAL_INT(SKIFF_OK,
                          skiff_romm_pairing_start(&open_client, SENDS_IDENTIFIER, &pairing));
    snprintf(body, sizeof body, "{\"user_code\":\"%s\",\"approved_scopes\":%s}", pairing.user_code,
             ALL_SCOPES);
    TEST_ASSERT_EQUAL_INT64(HTTP_OK,
                            admin_request(SKIFF_HTTP_POST, "/api/auth/device/approve", body));
    for (int i = 0; i < POLLS_MAX; i++) {
        const struct timespec wait = {.tv_sec = (time_t)pairing.interval_s, .tv_nsec = 0};
        nanosleep(&wait, NULL);
        TEST_ASSERT_EQUAL_INT(SKIFF_OK, skiff_romm_pairing_poll(&open_client, &pairing, &result));
        if (result.state == SKIFF_ROMM_PAIRING_APPROVED) {
            break;
        }
    }
    TEST_ASSERT_EQUAL_INT(SKIFF_ROMM_PAIRING_APPROVED, result.state);
    TEST_PRINTF("paired as device %s with %s", result.device_id, result.scopes);
    snprintf(device_id, device_id_size, "%s", result.device_id);
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, skiff_romm_client_init(out, transport, TLS_SITE, result.token));
    skiff_romm_pairing_clear(&pairing, &result);
}

/* What RomM's web UI sends when the player ticks the PSP in Install on device. */
static long admin_send_game(const char *device_id, uint64_t rom_id) {
    char path[PATH_MAX_LENGTH];
    char body[BODY_MAX];
    snprintf(path, sizeof path, "/api/devices/%s/installs", device_id);
    snprintf(body, sizeof body, "{\"rom_id\":%llu}", (unsigned long long)rom_id);
    return admin_request(SKIFF_HTTP_POST, path, body);
}

static skiff_romm_install_claim claim;

static void claim_sends(skiff_romm_client *psp, const char *device_id) {
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, skiff_romm_installs_claim(psp, device_id, &claim));
    TEST_PRINTF("claim: %zu of %zu kept, %zu dropped", claim.count, claim.total, claim.dropped);
    for (size_t i = 0; i < claim.count; i++) {
        TEST_PRINTF("  %s: ROM %llu, %zu file(s)", claim.requests[i].id,
                    (unsigned long long)claim.requests[i].rom_id, claim.requests[i].file_count);
    }
}

static void test_a_psp_receives_the_games_the_web_ui_sends(void) {
    skiff_romm_server server;
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, skiff_romm_heartbeat(&client, &server));
    TEST_ASSERT_TRUE_MESSAGE(server.installs_enabled, "RomM 5.4 takes sends by default");
    char device_id[SKIFF_CONFIG_DEVICE_ID_MAX];
    skiff_romm_client psp;
    pair_psp(device_id, sizeof device_id, &psp);
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, skiff_romm_installs_register(&psp, device_id, 1));
    TEST_PRINTF("an earlier run that stopped half way may have left sends: end them first");
    claim_sends(&psp, device_id);
    for (size_t i = 0; i < claim.count; i++) {
        TEST_ASSERT_EQUAL_INT(SKIFF_OK,
                              skiff_romm_installs_report(&psp, device_id, claim.requests[i].id,
                                                         SKIFF_ROMM_INSTALL_FAILED, NULL));
    }

    TEST_ASSERT_EQUAL_INT64(HTTP_CREATED, admin_send_game(device_id, payload.rom_id));
    TEST_ASSERT_EQUAL_INT64(HTTP_CREATED, admin_send_game(device_id, foreign_rom_id));
    claim_sends(&psp, device_id);
    TEST_ASSERT_EQUAL_size_t(2, claim.total);
    TEST_ASSERT_EQUAL_size_t(2, claim.count);
    TEST_ASSERT_EQUAL_size_t(0, claim.dropped);
    TEST_ASSERT_EQUAL_UINT64_MESSAGE(payload.rom_id, claim.requests[0].rom_id, "oldest first");
    TEST_ASSERT_EQUAL_UINT64(foreign_rom_id, claim.requests[1].rom_id);
    TEST_ASSERT_EQUAL_size_t(1, claim.requests[0].file_count);
    const skiff_romm_install_claim first = claim;

    TEST_PRINTF("RomM checks no platform: Skiff finds the second is not a PSP game");
    skiff_romm_platform platform;
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, skiff_romm_find_platform(&psp, PLATFORM_SLUG, &platform));
    skiff_romm_rom rom;
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, skiff_romm_get_rom(&psp, foreign_rom_id, &rom));
    TEST_PRINTF("ROM %llu '%s' on platform %llu, the PSP's is %llu",
                (unsigned long long)foreign_rom_id, rom.summary.fs_name,
                (unsigned long long)rom.summary.platform_id, (unsigned long long)platform.id);
    TEST_ASSERT_NOT_EQUAL_UINT64(platform.id, rom.summary.platform_id);
    char reason[SKIFF_ROMM_INSTALL_REASON_MAX_BYTES + 1];
    TEST_ASSERT_EQUAL_INT(SKIFF_OK,
                          skiff_error_line(SKIFF_LANGUAGE_SPANISH, SKIFF_ERR_ROMM_SEND_NOT_PSP,
                                           reason, sizeof reason));
    TEST_ASSERT_EQUAL_INT(SKIFF_OK,
                          skiff_romm_installs_report(&psp, device_id, first.requests[1].id,
                                                     SKIFF_ROMM_INSTALL_FAILED, reason));
    TEST_ASSERT_EQUAL_INT(SKIFF_OK,
                          skiff_romm_installs_report(&psp, device_id, first.requests[0].id,
                                                     SKIFF_ROMM_INSTALL_DONE, NULL));
    TEST_PRINTF("a report is once only");
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_ROMM_NOT_FOUND,
                          skiff_romm_installs_report(&psp, device_id, first.requests[0].id,
                                                     SKIFF_ROMM_INSTALL_DONE, NULL));
    claim_sends(&psp, device_id);
    TEST_ASSERT_EQUAL_size_t(0, claim.total);

    TEST_PRINTF("turned off, the PSP can no longer be sent games");
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, skiff_romm_installs_register(&psp, device_id, 0));
    TEST_ASSERT_EQUAL_INT64(HTTP_BAD_REQUEST, admin_send_game(device_id, payload.rom_id));
    TEST_PRINTF("a token not bound to the PSP (the seed's API token) is refused the claim");
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_ROMM_SENDS_FORBIDDEN,
                          skiff_romm_installs_claim(&client, device_id, &claim));
    skiff_romm_client_clear(&psp);
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
    save_disc_rom_id = env_number("SKIFF_IT_SAVE_DISC_ROM_ID");
    save_disc_target = required_env("SKIFF_IT_SAVE_DISC_TARGET");
    foreign_rom_id = env_number("SKIFF_IT_FOREIGN_ROM_ID");
    convert_rom_id = env_number("SKIFF_IT_CONVERT_ROM_ID");
    convert_slow_rom_id = env_number("SKIFF_IT_CONVERT_SLOW_ROM_ID");
    convert_save_target = required_env("SKIFF_IT_CONVERT_SAVE_TARGET");
    convert_slow_save_target = required_env("SKIFF_IT_CONVERT_SLOW_SAVE_TARGET");
    admin_authorization = required_env("SKIFF_IT_ADMIN_AUTHORIZATION");
    if (skiff_net_global_init() != SKIFF_OK) {
        printf("setup failed: TLS could not be seeded\n");
        return EXIT_FAILURE;
    }

    UNITY_BEGIN();
    RUN_TEST(test_the_server_version_is_supported);
    RUN_TEST(test_every_rom_is_listed_page_by_page);
    RUN_TEST(test_romm_reads_the_save_id_of_a_psp_disc);
    RUN_TEST(test_the_favourites_hold_the_seeded_favourite_with_its_file);
    RUN_TEST(test_a_token_with_collections_read_lists_every_visible_collection);
    RUN_TEST(test_a_token_without_collections_read_is_refused_the_collections);
    RUN_TEST(test_a_collections_games_are_listed_with_roms_read_alone);
    RUN_TEST(test_a_reserved_file_name_downloads_through_its_url);
    RUN_TEST(test_a_cover_comes_without_the_token_from_its_rom_details);
    RUN_TEST(test_a_roms_details_carry_its_description_and_facts);
    RUN_TEST(test_a_wrong_token_is_refused_by_romm);
    RUN_TEST(test_a_psp_receives_the_games_the_web_ui_sends);
    const int failures = UNITY_END();

    skiff_net_global_cleanup();
    return failures;
}
