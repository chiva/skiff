/*
 * The RomM client (skiff/romm.h) against the fake transport replaying responses recorded from RomM
 * 5.3.1 (tests/fixtures/romm/): the version policy, the PSP platform, two pages of one ROM and the
 * empty page after them, a ROM's files and cover, download URLs that survive any file name, and
 * cover URLs that only ever lead to RomM's own pictures. Every way a
 * response can be wrong (a proxy's login page, a cut or oversized body, a field that does not fit)
 * is SKIFF_ERR_ROMM_BAD_RESPONSE, never a half-filled result.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "skiff/romm.h"

#include "fake_transport.h"
#include "unity.h"

#define BASE_URL "https://romm.test"
#define TOKEN "rmm_0123456789abcdef"
#define PSP_PLATFORM_ID 1
#define PAYLOAD_ROM_ID 1
#define EXTRA_ROM_ID 2
#define PAYLOAD_NAME "Skiff Test Payload.iso"
#define EXTRA_NAME "Skiff Extra #2 (Caf\xC3\xA9 & Co+).iso"
#define PAYLOAD_CRC32 0xce22e2c3U
#define EXTRA_CRC32 0x18119276U
/* What skiff_romm_list_roms() sends after platform_ids, limit and offset; the recorder asks the
 * same (tests/integration/record-fixtures.sh), so a change on either side fails here. */
#define LIST_QUERY                                                                                 \
    "&order_by=name&order_dir=asc&with_char_index=false&with_filter_values=false"                  \
    "&with_rom_id_index=false"
#define PAGE_PATH(offset) "/api/roms?platform_ids=1&limit=1&offset=" #offset LIST_QUERY
/* What it adds for the player's favourites with each ROM's files (the recorder asks the same). */
#define FAVOURITES_PAGE_PATH(offset) PAGE_PATH(offset) "&favorite=true&with_files=true"
#define JSON_OK "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n\r\n"
#define RAW_MAX 4096

static const skiff_romm_list_query ALL_ROMS = {.platform_id = PSP_PLATFORM_ID,
                                               .filter = SKIFF_ROMM_LIST_ALL};
static const skiff_romm_list_query FAVOURITES_WITH_FILES = {
    .platform_id = PSP_PLATFORM_ID, .filter = SKIFF_ROMM_LIST_FAVOURITES, .with_files = 1};

static fake_transport fake;
static skiff_romm_client client;

void setUp(void) {
    fake_transport_init(&fake);
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, skiff_romm_client_init(&client, &fake.base, BASE_URL, TOKEN));
}

void tearDown(void) { skiff_transport_destroy(&fake.base); }

static void serve_fixture(const char *path, const char *fixture) {
    TEST_ASSERT_NOT_NULL(fake_transport_add_fixture(&fake, path, fixture));
}

static fake_route *serve_raw(const char *path, const char *raw) {
    fake_route *route = fake_transport_add_raw(&fake, path, raw, strlen(raw));
    TEST_ASSERT_NOT_NULL(route);
    return route;
}

/* A 200 JSON response whose body is a ROM page holding item. */
static void serve_page_item(const char *item) {
    char raw[RAW_MAX];
    snprintf(raw, sizeof raw, JSON_OK "{\"items\":[%s],\"total\":1,\"limit\":1,\"offset\":0}",
             item);
    serve_raw(PAGE_PATH(0), raw);
}

static skiff_err list_first_page(skiff_romm_rom_page *page) {
    const skiff_err err = skiff_romm_list_roms(&client, &ALL_ROMS, 0, 1, page);
    TEST_PRINTF("list -> %s, %zu item(s) of %llu", skiff_err_name(err), page->count,
                (unsigned long long)page->total);
    return err;
}

/* ---- The version policy ---- */

typedef struct version_case {
    const char *version;
    skiff_err expected;
    int known;
    int newer;
} version_case;

static void test_the_version_policy(void) {
    static const version_case CASES[] = {
        {"5.3.1", SKIFF_OK, 1, 0},
        {"5.3.0", SKIFF_OK, 1, 0},
        {"5.3", SKIFF_OK, 1, 0},
        {"5.3.2-beta.1", SKIFF_OK, 1, 0},
        {"5.4.0", SKIFF_OK, 1, 1},
        {"5.10.0", SKIFF_OK, 1, 1},
        {"6.0.0", SKIFF_OK, 1, 1},
        {"5.2.9", SKIFF_ERR_ROMM_UNSUPPORTED_VERSION, 1, 0},
        {"4.8.1", SKIFF_ERR_ROMM_UNSUPPORTED_VERSION, 1, 0},
        {"development", SKIFF_OK, 0, 1},
        {"5", SKIFF_OK, 0, 1},
        {"99999999999.0", SKIFF_OK, 0, 1},
        {"", SKIFF_ERR_ROMM_BAD_RESPONSE, 0, 0},
        {"5.3\x1b[2J", SKIFF_ERR_ROMM_BAD_RESPONSE, 0, 0},
        {"5.3.1\x7f", SKIFF_ERR_ROMM_BAD_RESPONSE, 0, 0},
        {"5.3.1-a-version-string-longer-than-the-field", SKIFF_ERR_ROMM_BAD_RESPONSE, 0, 0},
    };
    for (size_t i = 0; i < sizeof CASES / sizeof CASES[0]; i++) {
        skiff_romm_server server;
        const skiff_err err = skiff_romm_check_version(CASES[i].version, &server);
        TEST_PRINTF("'%s' -> %s, known %d (%d.%d.%d), newer than tested %d", CASES[i].version,
                    skiff_err_name(err), server.version_known, server.major, server.minor,
                    server.patch, server.newer_than_tested);
        TEST_ASSERT_EQUAL_STRING(skiff_err_name(CASES[i].expected), skiff_err_name(err));
        TEST_ASSERT_EQUAL_INT(CASES[i].known, server.version_known);
        TEST_ASSERT_EQUAL_INT(CASES[i].newer, server.newer_than_tested);
    }
    skiff_romm_server server;
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_ROMM_BAD_RESPONSE, skiff_romm_check_version(NULL, &server));
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_INVALID_ARG, skiff_romm_check_version("5.3.1", NULL));
}

static void test_heartbeat_reads_the_recorded_version_without_the_token(void) {
    serve_fixture("/api/heartbeat", "romm/heartbeat.http");
    skiff_romm_server server;
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, skiff_romm_heartbeat(&client, &server));
    TEST_ASSERT_EQUAL_STRING("5.3.1", server.version);
    TEST_ASSERT_EQUAL_INT(0, server.newer_than_tested);
    TEST_PRINTF("request headers: '%s'", fake.log[0].headers);
    TEST_ASSERT_NULL(strstr(fake.log[0].headers, TOKEN));
}

static void test_an_old_server_is_refused_and_still_named(void) {
    serve_raw("/api/heartbeat", JSON_OK "{\"SYSTEM\":{\"VERSION\":\"5.0.2\"}}");
    skiff_romm_server server;
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_ROMM_UNSUPPORTED_VERSION,
                          skiff_romm_heartbeat(&client, &server));
    TEST_ASSERT_EQUAL_STRING("5.0.2", server.version);
}

static void test_a_heartbeat_with_an_escaped_control_is_a_bad_response(void) {
    serve_raw("/api/heartbeat", JSON_OK "{\"SYSTEM\":{\"VERSION\":\"5.3\\u001b[2J\"}}");
    skiff_romm_server server;
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_ROMM_BAD_RESPONSE, skiff_romm_heartbeat(&client, &server));
    TEST_ASSERT_EQUAL_STRING("", server.version);
}

static void test_a_heartbeat_without_a_version_is_a_bad_response(void) {
    serve_raw("/api/heartbeat", JSON_OK "{\"SYSTEM\":{\"VERSION\":5.3}}");
    skiff_romm_server server;
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_ROMM_BAD_RESPONSE, skiff_romm_heartbeat(&client, &server));
    TEST_ASSERT_EQUAL_STRING("", server.version);
}

/* ---- Platforms ---- */

static void test_the_psp_platform_is_found_with_the_token(void) {
    serve_fixture("/api/platforms", "romm/platforms.http");
    skiff_romm_platform platform;
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, skiff_romm_find_platform(&client, "psp", &platform));
    TEST_PRINTF("platform %llu '%s' with %llu ROMs", (unsigned long long)platform.id, platform.name,
                (unsigned long long)platform.rom_count);
    TEST_ASSERT_EQUAL_UINT64(PSP_PLATFORM_ID, platform.id);
    TEST_ASSERT_EQUAL_STRING("psp", platform.slug);
    TEST_ASSERT_EQUAL_STRING("PlayStation Portable", platform.name);
    TEST_ASSERT_EQUAL_UINT64(2, platform.rom_count);
    TEST_ASSERT_NOT_NULL(strstr(fake.log[0].headers, "Authorization: Bearer " TOKEN "\n"));
}

static void test_a_missing_platform_is_not_found(void) {
    serve_fixture("/api/platforms", "romm/platforms.http");
    skiff_romm_platform platform;
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_ROMM_NOT_FOUND,
                          skiff_romm_find_platform(&client, "ps2", &platform));
    TEST_ASSERT_EQUAL_UINT64(0, platform.id);
}

static void test_a_rejected_token_is_a_romm_refusal(void) {
    serve_fixture("/api/platforms", "romm/roms-unauthorized.http");
    skiff_romm_platform platform;
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_ROMM_UNAUTHORIZED,
                          skiff_romm_find_platform(&client, "psp", &platform));
}

static void test_a_platform_list_that_is_not_an_array_is_a_bad_response(void) {
    serve_raw("/api/platforms", JSON_OK "{\"detail\":\"oops\"}");
    skiff_romm_platform platform;
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_ROMM_BAD_RESPONSE,
                          skiff_romm_find_platform(&client, "psp", &platform));
}

static void test_a_psp_platform_with_a_bad_field_is_a_bad_response(void) {
    serve_raw("/api/platforms", JSON_OK "[{\"slug\":\"psp\",\"id\":-1,\"rom_count\":1}]");
    skiff_romm_platform platform;
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_ROMM_BAD_RESPONSE,
                          skiff_romm_find_platform(&client, "psp", &platform));
    TEST_ASSERT_EQUAL_STRING("", platform.slug);
}

/* ---- ROM pages ---- */

static void test_two_pages_of_one_rom_then_an_empty_page(void) {
    serve_fixture(PAGE_PATH(0), "romm/roms-page-0.http");
    serve_fixture(PAGE_PATH(1), "romm/roms-page-1.http");
    serve_fixture(PAGE_PATH(2), "romm/roms-page-2.http");
    skiff_romm_rom_page page;

    TEST_ASSERT_EQUAL_INT(SKIFF_OK, skiff_romm_list_roms(&client, &ALL_ROMS, 0, 1, &page));
    TEST_PRINTF("page 0: '%s' (%s), %llu bytes, CRC-32 %lx", page.items[0].name,
                page.items[0].fs_name, (unsigned long long)page.items[0].size,
                (unsigned long)page.items[0].crc32);
    TEST_ASSERT_EQUAL_UINT64(2, page.total);
    TEST_ASSERT_EQUAL_size_t(1, page.count);
    TEST_ASSERT_EQUAL_UINT64(EXTRA_ROM_ID, page.items[0].id);
    TEST_ASSERT_EQUAL_STRING(EXTRA_NAME, page.items[0].fs_name);
    TEST_ASSERT_EQUAL_STRING("Skiff Extra #2", page.items[0].name);
    TEST_ASSERT_TRUE(page.items[0].has_crc32);
    TEST_ASSERT_EQUAL_HEX32(EXTRA_CRC32, page.items[0].crc32);
    TEST_ASSERT_FALSE(page.items[0].multiple_files);

    TEST_ASSERT_EQUAL_INT(SKIFF_OK, skiff_romm_list_roms(&client, &ALL_ROMS, 1, 1, &page));
    TEST_ASSERT_EQUAL_UINT64(1, page.offset);
    TEST_ASSERT_EQUAL_STRING(PAYLOAD_NAME, page.items[0].fs_name);
    TEST_ASSERT_EQUAL_UINT64(4096, page.items[0].size);
    TEST_ASSERT_EQUAL_HEX32(PAYLOAD_CRC32, page.items[0].crc32);

    TEST_ASSERT_EQUAL_INT(SKIFF_OK, skiff_romm_list_roms(&client, &ALL_ROMS, 2, 1, &page));
    TEST_ASSERT_EQUAL_size_t(0, page.count);
    TEST_ASSERT_EQUAL_UINT64(2, page.total);
}

static void test_another_page_than_the_one_asked_for_is_refused(void) {
    serve_fixture(PAGE_PATH(0), "romm/roms-page-1.http");
    skiff_romm_rom_page page;
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_ROMM_BAD_RESPONSE, list_first_page(&page));
    TEST_ASSERT_EQUAL_size_t(0, page.count);
}

static void test_a_page_longer_than_asked_is_refused(void) {
    char raw[RAW_MAX];
    snprintf(raw, sizeof raw, JSON_OK "{\"items\":[%s,%s],\"total\":2,\"limit\":1,\"offset\":0}",
             "{\"id\":1,\"platform_id\":1,\"fs_name\":\"a.iso\",\"fs_size_bytes\":1}",
             "{\"id\":2,\"platform_id\":1,\"fs_name\":\"b.iso\",\"fs_size_bytes\":1}");
    serve_raw(PAGE_PATH(0), raw);
    skiff_romm_rom_page page;
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_ROMM_BAD_RESPONSE, list_first_page(&page));
}

static void test_page_limits_are_checked(void) {
    skiff_romm_rom_page page;
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_INVALID_ARG,
                          skiff_romm_list_roms(&client, &ALL_ROMS, 0, 0, &page));
    TEST_ASSERT_EQUAL_INT(
        SKIFF_ERR_INVALID_ARG,
        skiff_romm_list_roms(&client, &ALL_ROMS, 0, SKIFF_ROMM_PAGE_SIZE + 1, &page));
    TEST_ASSERT_EQUAL_size_t(0, fake.request_count);
}

static void test_favourites_list_with_their_files_then_end(void) {
    serve_fixture(FAVOURITES_PAGE_PATH(0), "romm/roms-favorites-page-0.http");
    serve_fixture(FAVOURITES_PAGE_PATH(1), "romm/roms-favorites-page-1.http");
    skiff_romm_rom_page page;

    TEST_ASSERT_EQUAL_INT(SKIFF_OK,
                          skiff_romm_list_roms(&client, &FAVOURITES_WITH_FILES, 0, 1, &page));
    TEST_PRINTF("favourites page 0: %zu of %llu, '%s', file '%s' %llu bytes CRC-32 %lx", page.count,
                (unsigned long long)page.total, page.items[0].fs_name, page.items[0].file.file_name,
                (unsigned long long)page.items[0].file.size,
                (unsigned long)page.items[0].file.crc32);
    TEST_ASSERT_EQUAL_UINT64(1, page.total);
    TEST_ASSERT_EQUAL_size_t(1, page.count);
    TEST_ASSERT_EQUAL_UINT64(PAYLOAD_ROM_ID, page.items[0].id);
    TEST_ASSERT_TRUE(page.items[0].has_file);
    TEST_ASSERT_EQUAL_STRING(PAYLOAD_NAME, page.items[0].file.file_name);
    TEST_ASSERT_EQUAL_UINT64(4096, page.items[0].file.size);
    TEST_ASSERT_TRUE(page.items[0].file.has_crc32);
    TEST_ASSERT_EQUAL_HEX32(PAYLOAD_CRC32, page.items[0].file.crc32);
    TEST_ASSERT_EQUAL_INT(SKIFF_ROMM_NAME_OK, page.items[0].file.name_status);

    TEST_ASSERT_EQUAL_INT(SKIFF_OK,
                          skiff_romm_list_roms(&client, &FAVOURITES_WITH_FILES, 1, 1, &page));
    TEST_ASSERT_EQUAL_size_t(0, page.count);
    TEST_ASSERT_EQUAL_UINT64(1, page.total);
}

static void test_a_list_asks_only_for_what_its_query_wants(void) {
    const skiff_romm_list_query favourites = {.platform_id = PSP_PLATFORM_ID,
                                              .filter = SKIFF_ROMM_LIST_FAVOURITES};
    const skiff_romm_list_query all_with_files = {
        .platform_id = PSP_PLATFORM_ID, .filter = SKIFF_ROMM_LIST_ALL, .with_files = 1};
    const char *empty = JSON_OK "{\"items\":[],\"total\":0,\"limit\":1,\"offset\":0}";
    serve_raw(PAGE_PATH(0) "&favorite=true", empty);
    serve_raw(PAGE_PATH(0) "&with_files=true", empty);
    skiff_romm_rom_page page;
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, skiff_romm_list_roms(&client, &favourites, 0, 1, &page));
    TEST_PRINTF("a player without favourites gets an empty list: %llu",
                (unsigned long long)page.total);
    TEST_ASSERT_EQUAL_UINT64(0, page.total);
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, skiff_romm_list_roms(&client, &all_with_files, 0, 1, &page));
    TEST_ASSERT_EQUAL_size_t(2, fake.request_count);
    TEST_ASSERT_EQUAL_STRING(PAGE_PATH(0) "&favorite=true", fake.log[0].url + strlen(BASE_URL));
    TEST_ASSERT_EQUAL_STRING(PAGE_PATH(0) "&with_files=true", fake.log[1].url + strlen(BASE_URL));
}

static void test_a_favourite_of_another_platform_is_refused(void) {
    serve_raw(FAVOURITES_PAGE_PATH(0),
              JSON_OK "{\"items\":[{\"id\":9,\"platform_id\":2,\"fs_name\":\"a.iso\","
                      "\"fs_size_bytes\":1,\"files\":[]}],\"total\":1,\"limit\":1,\"offset\":0}");
    skiff_romm_rom_page page;
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_ROMM_BAD_RESPONSE,
                          skiff_romm_list_roms(&client, &FAVOURITES_WITH_FILES, 0, 1, &page));
    TEST_ASSERT_EQUAL_size_t(0, page.count);
}

typedef struct listed_file_case {
    const char *what;
    const char *item;
    int has_file;
    skiff_romm_name_status file_status;
} listed_file_case;

static void test_a_listed_file_is_kept_only_when_it_is_the_roms_only_one(void) {
#define ITEM_HEAD "{\"id\":5,\"platform_id\":1,\"fs_name\":\"Game.iso\",\"fs_size_bytes\":4,"
#define ITEM_BAD_NAME                                                                              \
    "{\"id\":5,\"platform_id\":1,\"fs_name\":\"G\\u0007.iso\",\"fs_size_bytes\":4,"
#define GOOD_FILE "{\"rom_id\":5,\"file_name\":\"Game.iso\",\"file_size_bytes\":4}"
    static const listed_file_case CASES[] = {
        {"no files listed", ITEM_HEAD "\"files\":[]}", 0, SKIFF_ROMM_NAME_OK},
        {"no files field", ITEM_HEAD "\"crc_hash\":null}", 0, SKIFF_ROMM_NAME_OK},
        {"one file", ITEM_HEAD "\"files\":[" GOOD_FILE "]}", 1, SKIFF_ROMM_NAME_OK},
        {"two files", ITEM_HEAD "\"files\":[" GOOD_FILE "," GOOD_FILE "]}", 0, SKIFF_ROMM_NAME_OK},
        {"a file of another ROM",
         ITEM_HEAD "\"files\":[{\"rom_id\":6,\"file_name\":\"Game.iso\",\"file_size_bytes\":4}]}",
         0, SKIFF_ROMM_NAME_OK},
        {"a file without a size",
         ITEM_HEAD "\"files\":[{\"rom_id\":5,\"file_name\":\"Game.iso\"}]}", 0, SKIFF_ROMM_NAME_OK},
        {"files that are not an array", ITEM_HEAD "\"files\":{}}", 0, SKIFF_ROMM_NAME_OK},
        {"a file name with a control character",
         ITEM_HEAD
         "\"files\":[{\"rom_id\":5,\"file_name\":\"G\\u0007.iso\",\"file_size_bytes\":4}]}",
         1, SKIFF_ROMM_NAME_CONTROL_CHAR},
        {"a clean file of a ROM with an unusable name", ITEM_BAD_NAME "\"files\":[" GOOD_FILE "]}",
         1, SKIFF_ROMM_NAME_CONTROL_CHAR},
    };
#undef ITEM_HEAD
#undef ITEM_BAD_NAME
#undef GOOD_FILE
    for (size_t i = 0; i < sizeof CASES / sizeof CASES[0]; i++) {
        char json[RAW_MAX];
        snprintf(json, sizeof json, "{\"items\":[%s],\"total\":1,\"offset\":0}", CASES[i].item);
        skiff_romm_rom_page page;
        const skiff_err err = skiff_romm_parse_rom_page(json, strlen(json), &page);
        TEST_PRINTF("%s -> %s, has_file %d, file status %d", CASES[i].what, skiff_err_name(err),
                    page.items[0].has_file, (int)page.items[0].file.name_status);
        TEST_ASSERT_EQUAL_INT_MESSAGE(SKIFF_OK, err, CASES[i].what);
        TEST_ASSERT_EQUAL_size_t_MESSAGE(1, page.count, CASES[i].what);
        TEST_ASSERT_EQUAL_INT_MESSAGE(CASES[i].has_file, page.items[0].has_file, CASES[i].what);
        TEST_ASSERT_EQUAL_INT_MESSAGE(CASES[i].file_status, page.items[0].file.name_status,
                                      CASES[i].what);
        if (!CASES[i].has_file) {
            TEST_ASSERT_EQUAL_STRING_MESSAGE("", page.items[0].file.file_name, CASES[i].what);
        }
    }
}

typedef struct item_case {
    const char *why;
    const char *item;
    skiff_err expected;
} item_case;

static void test_every_field_is_checked_before_a_rom_is_shown(void) {
    const item_case cases[] = {
        {"minimal", "{\"id\":7,\"platform_id\":1,\"fs_name\":\"a.iso\",\"fs_size_bytes\":1}",
         SKIFF_OK},
        {"name and crc null",
         "{\"id\":7,\"platform_id\":1,\"name\":null,\"fs_name\":\"a.iso\",\"fs_size_bytes\":1,"
         "\"crc_hash\":null}",
         SKIFF_OK},
        {"crc empty",
         "{\"id\":7,\"platform_id\":1,\"fs_name\":\"a.iso\",\"fs_size_bytes\":1,\"crc_hash\":\"\"}",
         SKIFF_OK},
        {"crc without leading zeros",
         "{\"id\":7,\"platform_id\":1,\"fs_name\":\"a.iso\",\"fs_size_bytes\":1,\"crc_hash\":"
         "\"abc\"}",
         SKIFF_OK},
        {"4 GiB - 1 bytes",
         "{\"id\":7,\"platform_id\":1,\"fs_name\":\"a.iso\",\"fs_size_bytes\":4294967295}",
         SKIFF_OK},
        {"no id", "{\"fs_name\":\"a.iso\",\"fs_size_bytes\":1}", SKIFF_ERR_ROMM_BAD_RESPONSE},
        {"id as text", "{\"id\":\"7\",\"fs_name\":\"a.iso\",\"fs_size_bytes\":1}",
         SKIFF_ERR_ROMM_BAD_RESPONSE},
        {"no fs_name", "{\"id\":7,\"platform_id\":1,\"fs_size_bytes\":1}",
         SKIFF_ERR_ROMM_BAD_RESPONSE},
        {"empty fs_name", "{\"id\":7,\"platform_id\":1,\"fs_name\":\"\",\"fs_size_bytes\":1}",
         SKIFF_ERR_ROMM_BAD_RESPONSE},
        {"negative size", "{\"id\":7,\"platform_id\":1,\"fs_name\":\"a.iso\",\"fs_size_bytes\":-1}",
         SKIFF_ERR_ROMM_BAD_RESPONSE},
        {"fractional size",
         "{\"id\":7,\"platform_id\":1,\"fs_name\":\"a.iso\",\"fs_size_bytes\":1.5}",
         SKIFF_ERR_ROMM_BAD_RESPONSE},
        {"size past 2^53",
         "{\"id\":7,\"platform_id\":1,\"fs_name\":\"a.iso\",\"fs_size_bytes\":1e300}",
         SKIFF_ERR_ROMM_BAD_RESPONSE},
        {"id 2^53 + 1, which a double reads as 2^53",
         "{\"id\":9007199254740993,\"platform_id\":1,\"fs_name\":\"a.iso\",\"fs_size_bytes\":1}",
         SKIFF_ERR_ROMM_BAD_RESPONSE},
        {"fs_name with an escaped NUL",
         "{\"id\":7,\"platform_id\":1,\"fs_name\":\"a.iso\\u0000.txt\",\"fs_size_bytes\":1}",
         SKIFF_ERR_ROMM_BAD_RESPONSE},
        {"an escaped backslash before u0000 is just text",
         "{\"id\":7,\"platform_id\":1,\"fs_name\":\"a\\\\u0000.iso\",\"fs_size_bytes\":1}",
         SKIFF_OK},
        {"crc not hex",
         "{\"id\":7,\"platform_id\":1,\"fs_name\":\"a.iso\",\"fs_size_bytes\":1,\"crc_hash\":"
         "\"xyz\"}",
         SKIFF_ERR_ROMM_BAD_RESPONSE},
        {"crc of nine digits",
         "{\"id\":7,\"platform_id\":1,\"fs_name\":\"a.iso\",\"fs_size_bytes\":1,\"crc_hash\":"
         "\"123456789\"}",
         SKIFF_ERR_ROMM_BAD_RESPONSE},
    };
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        tearDown();
        setUp();
        serve_page_item(cases[i].item);
        skiff_romm_rom_page page;
        const skiff_err err = list_first_page(&page);
        TEST_PRINTF("%s -> %s", cases[i].why, skiff_err_name(err));
        TEST_ASSERT_EQUAL_STRING_MESSAGE(skiff_err_name(cases[i].expected), skiff_err_name(err),
                                         cases[i].why);
        if (err == SKIFF_OK) {
            TEST_ASSERT_EQUAL_UINT64(7, page.items[0].id);
        } else {
            TEST_ASSERT_EQUAL_size_t(0, page.count);
            TEST_ASSERT_EQUAL_UINT64(0, page.items[0].id);
        }
    }
}

/* ---- Names that cannot be used as they are ---- */

#define ROM_ITEM(id, fs_name)                                                                      \
    "{\"id\":" #id ",\"platform_id\":1,\"name\":\"Game " #id "\",\"fs_name\":\"" fs_name "\","     \
    "\"fs_size_bytes\":" #id "}"

static void assert_usable(const skiff_romm_rom_summary *rom, uint64_t id, const char *fs_name) {
    TEST_ASSERT_EQUAL_UINT64(id, rom->id);
    TEST_ASSERT_EQUAL_STRING(fs_name, rom->fs_name);
    TEST_ASSERT_EQUAL_INT(SKIFF_ROMM_NAME_OK, rom->name_status);
}

static void test_a_rom_with_a_control_character_is_listed_but_not_downloadable(void) {
    static const char page_json[] = "{\"items\":[" ROM_ITEM(1, "a.iso") "," ROM_ITEM(
        2, "b\\u001b[2Jc\\td.iso") "," ROM_ITEM(3, "e.iso") "],\"total\":3,\"offset\":0}";
    skiff_romm_rom_page page;
    TEST_ASSERT_EQUAL_INT(SKIFF_OK,
                          skiff_romm_parse_rom_page(page_json, sizeof page_json - 1, &page));
    TEST_PRINTF("middle ROM shows as '%s', status %d", page.items[1].fs_name,
                page.items[1].name_status);
    TEST_ASSERT_EQUAL_size_t(3, page.count);
    assert_usable(&page.items[0], 1, "a.iso");
    TEST_ASSERT_EQUAL_UINT64(2, page.items[1].id);
    TEST_ASSERT_EQUAL_STRING("b?[2Jc?d.iso", page.items[1].fs_name);
    TEST_ASSERT_EQUAL_STRING("Game 2", page.items[1].name);
    TEST_ASSERT_EQUAL_UINT64(2, page.items[1].size);
    TEST_ASSERT_EQUAL_INT(SKIFF_ROMM_NAME_CONTROL_CHAR, page.items[1].name_status);
    assert_usable(&page.items[2], 3, "e.iso");
}

static void test_a_title_with_a_control_character_marks_its_rom(void) {
    static const char page_json[] =
        "{\"items\":[{\"id\":4,\"platform_id\":1,\"name\":\"Bad\\u0007Title\",\"fs_name\":"
        "\"ok.iso\",\"fs_size_bytes\":1}],\"total\":1,\"offset\":0}";
    skiff_romm_rom_page page;
    TEST_ASSERT_EQUAL_INT(SKIFF_OK,
                          skiff_romm_parse_rom_page(page_json, sizeof page_json - 1, &page));
    TEST_ASSERT_EQUAL_STRING("Bad?Title", page.items[0].name);
    TEST_ASSERT_EQUAL_STRING("ok.iso", page.items[0].fs_name);
    TEST_ASSERT_EQUAL_INT(SKIFF_ROMM_NAME_CONTROL_CHAR, page.items[0].name_status);
}

static void test_a_file_with_a_control_character_is_listed_but_not_downloadable(void) {
    static const char rom_json[] =
        "{\"id\":3,\"platform_id\":1,\"fs_name\":\"Folder\",\"fs_size_bytes\":3,\"files\":["
        "{\"rom_id\":3,\"file_name\":\"one.bin\",\"file_size_bytes\":1},"
        "{\"rom_id\":3,\"file_name\":\"tw\\u000ao.bin\",\"file_size_bytes\":1},"
        "{\"rom_id\":3,\"file_name\":\"three.bin\",\"file_size_bytes\":1}]}";
    skiff_romm_rom rom;
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, skiff_romm_parse_rom(rom_json, sizeof rom_json - 1, &rom));
    TEST_ASSERT_EQUAL_size_t(3, rom.stored_count);
    TEST_ASSERT_EQUAL_STRING("one.bin", rom.files[0].file_name);
    TEST_ASSERT_EQUAL_INT(SKIFF_ROMM_NAME_OK, rom.files[0].name_status);
    TEST_ASSERT_EQUAL_STRING("tw?o.bin", rom.files[1].file_name);
    TEST_ASSERT_EQUAL_INT(SKIFF_ROMM_NAME_CONTROL_CHAR, rom.files[1].name_status);
    TEST_ASSERT_EQUAL_STRING("three.bin", rom.files[2].file_name);
    TEST_ASSERT_EQUAL_INT(SKIFF_ROMM_NAME_OK, rom.files[2].name_status);
    TEST_ASSERT_EQUAL_INT(SKIFF_ROMM_NAME_OK, rom.summary.name_status);
    char url[SKIFF_ROMM_CONTENT_URL_MAX];
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_INVALID_ARG,
                          skiff_romm_content_url(&client, 3, &rom.files[1], url, sizeof url));
    TEST_ASSERT_EQUAL_INT(SKIFF_OK,
                          skiff_romm_content_url(&client, 3, &rom.files[2], url, sizeof url));
}

/* A ROM page of one item whose fs_name is `name`. */
static void parse_with_fs_name(const char *name, skiff_romm_rom_page *page) {
    static char json[RAW_MAX];
    snprintf(json, sizeof json,
             "{\"items\":[{\"id\":5,\"platform_id\":1,\"fs_name\":\"%s\",\"fs_size_bytes\":1}],"
             "\"total\":1,\"offset\":0}",
             name);
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, skiff_romm_parse_rom_page(json, strlen(json), page));
}

static void test_a_clean_file_of_a_rom_with_an_unusable_name_gets_no_url(void) {
    static const char rom_json[] =
        "{\"id\":3,\"platform_id\":1,\"fs_name\":\"Fold\\u001ber\",\"fs_size_bytes\":1,"
        "\"files\":[{\"rom_id\":3,\"file_name\":\"clean.iso\",\"file_size_bytes\":1}]}";
    skiff_romm_rom rom;
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, skiff_romm_parse_rom(rom_json, sizeof rom_json - 1, &rom));
    TEST_ASSERT_EQUAL_INT(SKIFF_ROMM_NAME_CONTROL_CHAR, rom.summary.name_status);
    TEST_ASSERT_EQUAL_STRING("clean.iso", rom.files[0].file_name);
    TEST_ASSERT_EQUAL_INT(SKIFF_ROMM_NAME_CONTROL_CHAR, rom.files[0].name_status);
    char url[SKIFF_ROMM_CONTENT_URL_MAX];
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_INVALID_ARG,
                          skiff_romm_content_url(&client, 3, &rom.files[0], url, sizeof url));
}

static void test_c1_control_characters_count_as_one_control_each(void) {
    static const char page_json[] =
        "{\"items\":[{\"id\":5,\"platform_id\":1,\"name\":\"x\\u0085y\",\"fs_name\":"
        "\"a\\u009b2J\xC2\x85"
        "b.iso\",\"fs_size_bytes\":1}],\"total\":1,\"offset\":0}";
    skiff_romm_rom_page page;
    TEST_ASSERT_EQUAL_INT(SKIFF_OK,
                          skiff_romm_parse_rom_page(page_json, sizeof page_json - 1, &page));
    TEST_PRINTF("escaped and raw C1 controls show as '%s' / '%s'", page.items[0].name,
                page.items[0].fs_name);
    TEST_ASSERT_EQUAL_STRING("x?y", page.items[0].name);
    TEST_ASSERT_EQUAL_STRING("a?2J?b.iso", page.items[0].fs_name);
    TEST_ASSERT_EQUAL_INT(SKIFF_ROMM_NAME_CONTROL_CHAR, page.items[0].name_status);

    static const char rom_json[] =
        "{\"id\":3,\"platform_id\":1,\"fs_name\":\"Folder\",\"fs_size_bytes\":1,\"files\":["
        "{\"rom_id\":3,\"file_name\":\"f\\u009b.iso\",\"file_size_bytes\":1}]}";
    skiff_romm_rom rom;
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, skiff_romm_parse_rom(rom_json, sizeof rom_json - 1, &rom));
    TEST_ASSERT_EQUAL_STRING("f?.iso", rom.files[0].file_name);
    TEST_ASSERT_EQUAL_INT(SKIFF_ROMM_NAME_CONTROL_CHAR, rom.files[0].name_status);
    char url[SKIFF_ROMM_CONTENT_URL_MAX];
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_INVALID_ARG,
                          skiff_romm_content_url(&client, 3, &rom.files[0], url, sizeof url));

    TEST_PRINTF("U+00A0 and U+00E9 share the C2/C3 lead bytes but are text");
    static const char text_json[] =
        "{\"items\":[{\"id\":5,\"platform_id\":1,\"fs_name\":\"a\\u00a0b\\u00e9.iso\","
        "\"fs_size_bytes\":1}],\"total\":1,\"offset\":0}";
    TEST_ASSERT_EQUAL_INT(SKIFF_OK,
                          skiff_romm_parse_rom_page(text_json, sizeof text_json - 1, &page));
    TEST_ASSERT_EQUAL_STRING("a\xC2\xA0"
                             "b\xC3\xA9.iso",
                             page.items[0].fs_name);
    TEST_ASSERT_EQUAL_INT(SKIFF_ROMM_NAME_OK, page.items[0].name_status);

    TEST_PRINTF("strict fields refuse a C1 control");
    skiff_romm_server server;
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_ROMM_BAD_RESPONSE,
                          skiff_romm_check_version("5.3\xC2\x9b", &server));
}

static void test_a_raw_delete_byte_in_a_name_is_still_refused(void) {
    static const char page_json[] = "{\"items\":[{\"id\":5,\"platform_id\":1,\"fs_name\":"
                                    "\"a\x7f.iso\",\"fs_size_bytes\":1}],\"total\":1,\"offset\":0}";
    skiff_romm_rom_page page;
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_ROMM_BAD_RESPONSE,
                          skiff_romm_parse_rom_page(page_json, sizeof page_json - 1, &page));
}

static void test_an_overlong_name_is_cut_at_a_character_with_a_marker(void) {
    enum { FIELD = SKIFF_ROMM_FILE_NAME_MAX };
    char name[FIELD + 8];
    skiff_romm_rom_page page;

    TEST_PRINTF("a name that just fits is kept whole");
    memset(name, 'a', FIELD - 1);
    name[FIELD - 1] = '\0';
    parse_with_fs_name(name, &page);
    TEST_ASSERT_EQUAL_STRING(name, page.items[0].fs_name);
    TEST_ASSERT_EQUAL_INT(SKIFF_ROMM_NAME_OK, page.items[0].name_status);

    TEST_PRINTF("one byte more is cut, with the marker in its last byte");
    memset(name, 'a', FIELD);
    name[FIELD] = '\0';
    parse_with_fs_name(name, &page);
    TEST_ASSERT_EQUAL_INT(SKIFF_ROMM_NAME_TOO_LONG, page.items[0].name_status);
    TEST_ASSERT_EQUAL_size_t(FIELD - 1, strlen(page.items[0].fs_name));
    TEST_ASSERT_EQUAL_STRING(SKIFF_ROMM_NAME_CUT_MARKER, page.items[0].fs_name + FIELD - 2);

    TEST_PRINTF("a two-byte 'e acute' across the cut is dropped whole, never split");
    memset(name, 'a', FIELD - 3);
    memcpy(name + FIELD - 3, "\xC3\xA9xyz", 6);
    parse_with_fs_name(name, &page);
    TEST_ASSERT_EQUAL_INT(SKIFF_ROMM_NAME_TOO_LONG, page.items[0].name_status);
    TEST_ASSERT_EQUAL_size_t(FIELD - 2, strlen(page.items[0].fs_name));
    TEST_ASSERT_EQUAL_CHAR('a', page.items[0].fs_name[FIELD - 4]);
    TEST_ASSERT_EQUAL_STRING(SKIFF_ROMM_NAME_CUT_MARKER, page.items[0].fs_name + FIELD - 3);

    TEST_PRINTF("too long and a control character: the control character is reported");
    memset(name, 'a', FIELD + 2);
    memcpy(name, "\\u0001", 6);
    name[FIELD + 2] = '\0';
    parse_with_fs_name(name, &page);
    TEST_ASSERT_EQUAL_INT(SKIFF_ROMM_NAME_CONTROL_CHAR, page.items[0].name_status);
    TEST_ASSERT_EQUAL_CHAR(SKIFF_ROMM_NAME_REPLACEMENT, page.items[0].fs_name[0]);
}

static void test_an_overlong_file_name_is_listed_but_not_downloadable(void) {
    char json[RAW_MAX];
    char name[SKIFF_ROMM_FILE_NAME_MAX + 1];
    memset(name, 'f', sizeof name - 1);
    name[sizeof name - 1] = '\0';
    snprintf(json, sizeof json,
             "{\"id\":3,\"platform_id\":1,\"fs_name\":\"Folder\",\"fs_size_bytes\":1,\"files\":["
             "{\"rom_id\":3,\"file_name\":\"%s\",\"file_size_bytes\":1}]}",
             name);
    skiff_romm_rom rom;
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, skiff_romm_parse_rom(json, strlen(json), &rom));
    TEST_ASSERT_EQUAL_INT(SKIFF_ROMM_NAME_TOO_LONG, rom.files[0].name_status);
    char url[SKIFF_ROMM_CONTENT_URL_MAX];
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_INVALID_ARG,
                          skiff_romm_content_url(&client, 3, &rom.files[0], url, sizeof url));
}

static void test_structural_problems_still_refuse_the_page(void) {
    static const char *const BROKEN[] = {
        "{\"items\":[" ROM_ITEM(1, "a.iso") ",{\"id\":2,\"platform_id\":1,\"fs_name\":7,"
                                            "\"fs_size_bytes\":1}],\"total\":2,\"offset\":0}",
        "{\"items\":[" ROM_ITEM(1, "a.iso") ",{\"id\":2,\"platform_id\":1,\"fs_name\":\"\","
                                            "\"fs_size_bytes\":1}],\"total\":2,\"offset\":0}",
        "{\"items\":[" ROM_ITEM(1,
                                "a.iso") ",{\"id\":2,\"platform_id\":1,\"fs_name\":\"b\\u0000"
                                         "c.iso\",\"fs_size_bytes\":1}],\"total\":2,\"offset\":0}",
        "{\"items\":[" ROM_ITEM(1, "a.iso") ",{\"platform_id\":1,\"fs_name\":\"b.iso\","
                                            "\"fs_size_bytes\":1}],\"total\":2,\"offset\":0}",
    };
    for (size_t i = 0; i < sizeof BROKEN / sizeof BROKEN[0]; i++) {
        skiff_romm_rom_page page;
        TEST_ASSERT_EQUAL_INT(SKIFF_ERR_ROMM_BAD_RESPONSE,
                              skiff_romm_parse_rom_page(BROKEN[i], strlen(BROKEN[i]), &page));
        TEST_ASSERT_EQUAL_size_t(0, page.count);
    }
}

static void test_a_rom_of_another_platform_is_refused(void) {
    serve_page_item("{\"id\":7,\"platform_id\":2,\"fs_name\":\"a.iso\",\"fs_size_bytes\":1}");
    skiff_romm_rom_page page;
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_ROMM_BAD_RESPONSE, list_first_page(&page));
    TEST_ASSERT_EQUAL_size_t(0, page.count);
}

static void test_a_page_beyond_its_total_is_refused(void) {
    serve_raw(PAGE_PATH(0), JSON_OK "{\"items\":[{\"id\":7,\"platform_id\":1,\"fs_name\":"
                                    "\"a.iso\",\"fs_size_bytes\":1}],\"total\":0,\"offset\":0}");
    skiff_romm_rom_page page;
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_ROMM_BAD_RESPONSE, list_first_page(&page));
    static const char past_the_end[] = "{\"items\":[],\"total\":1,\"offset\":2}";
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_ROMM_BAD_RESPONSE,
                          skiff_romm_parse_rom_page(past_the_end, sizeof past_the_end - 1, &page));
}

static void test_a_file_of_another_rom_past_the_kept_ones_is_refused_too(void) {
    char json[RAW_MAX] = "{\"id\":3,\"platform_id\":1,\"fs_name\":\"Folder\",\"fs_size_bytes\":20,"
                         "\"files\":[";
    for (size_t i = 0; i <= SKIFF_ROMM_FILES_MAX; i++) {
        char file[96];
        /* The file after the last one kept belongs to ROM 4. */
        snprintf(file, sizeof file,
                 "%s{\"rom_id\":%d,\"file_name\":\"part%zu.bin\",\"file_size_bytes\":1}",
                 i == 0 ? "" : ",", i == SKIFF_ROMM_FILES_MAX ? 4 : 3, i);
        const size_t used = strlen(json);
        TEST_ASSERT_LESS_THAN_size_t(sizeof json - 4, used + strlen(file));
        memcpy(json + used, file, strlen(file) + 1);
    }
    memcpy(json + strlen(json), "]}", 3);
    skiff_romm_rom rom;
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_ROMM_BAD_RESPONSE,
                          skiff_romm_parse_rom(json, strlen(json), &rom));
    TEST_ASSERT_EQUAL_size_t(0, rom.stored_count);
}

static void test_control_characters_in_a_string_are_refused(void) {
    static const char with_nul[] =
        "{\"items\":[{\"id\":7,\"platform_id\":1,\"fs_name\":\"a.iso\0x\","
        "\"fs_size_bytes\":1}],\"total\":1,\"offset\":0}";
    static const char with_newline[] =
        "{\"items\":[{\"id\":7,\"platform_id\":1,\"fs_name\":"
        "\"a.iso\nx\",\"fs_size_bytes\":1}],\"total\":1,\"offset\":0}";
    static const char nul_after[] = "{\"items\":[],\"total\":0,\"offset\":0}\0";
    skiff_romm_rom_page page;
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_ROMM_BAD_RESPONSE,
                          skiff_romm_parse_rom_page(with_nul, sizeof with_nul - 1, &page));
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_ROMM_BAD_RESPONSE,
                          skiff_romm_parse_rom_page(with_newline, sizeof with_newline - 1, &page));
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_ROMM_BAD_RESPONSE,
                          skiff_romm_parse_rom_page(nul_after, sizeof nul_after - 1, &page));
    TEST_PRINTF(
        "an escaped control character is not structure: the ROM is listed, not downloadable");
    static const char *const ESCAPED[] = {"\\u000a", "\\u000D", "\\u001b", "\\u001f",
                                          "\\u007f", "\\n",     "\\t"};
    for (size_t i = 0; i < sizeof ESCAPED / sizeof ESCAPED[0]; i++) {
        char item[RAW_MAX];
        snprintf(item, sizeof item,
                 "{\"items\":[{\"id\":7,\"platform_id\":1,\"fs_name\":\"a%s.iso\","
                 "\"fs_size_bytes\":1}],\"total\":1,\"offset\":0}",
                 ESCAPED[i]);
        TEST_ASSERT_EQUAL_INT(SKIFF_OK, skiff_romm_parse_rom_page(item, strlen(item), &page));
        TEST_PRINTF("fs_name with %s shows as '%s'", ESCAPED[i], page.items[0].fs_name);
        TEST_ASSERT_EQUAL_STRING("a?.iso", page.items[0].fs_name);
        TEST_ASSERT_EQUAL_INT(SKIFF_ROMM_NAME_CONTROL_CHAR, page.items[0].name_status);
    }
    TEST_PRINTF("an escaped non-ASCII character is text");
    static const char accented[] =
        "{\"items\":[{\"id\":7,\"platform_id\":1,\"fs_name\":"
        "\"caf\\u00e9.iso\",\"fs_size_bytes\":1}],\"total\":1,\"offset\":0}";
    TEST_ASSERT_EQUAL_INT(SKIFF_OK,
                          skiff_romm_parse_rom_page(accented, sizeof accented - 1, &page));
    TEST_ASSERT_EQUAL_STRING("caf\xC3\xA9.iso", page.items[0].fs_name);
    TEST_PRINTF("blanks between values are not inside a string");
    static const char spaced[] = "{\n\t\"items\": [],\r\n\"total\": 0, \"offset\": 0}";
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, skiff_romm_parse_rom_page(spaced, sizeof spaced - 1, &page));
}

static void test_a_file_of_another_rom_is_refused(void) {
    static const char json[] = "{\"id\":3,\"platform_id\":1,\"fs_name\":\"a.iso\","
                               "\"fs_size_bytes\":1,\"files\":[{\"rom_id\":4,\"file_name\":"
                               "\"a.iso\",\"file_size_bytes\":1}]}";
    skiff_romm_rom rom;
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_ROMM_BAD_RESPONSE,
                          skiff_romm_parse_rom(json, sizeof json - 1, &rom));
    TEST_ASSERT_EQUAL_size_t(0, rom.file_count);
}

static void test_the_largest_exact_id_is_accepted(void) {
    serve_page_item(
        "{\"id\":9007199254740991,\"platform_id\":1,\"fs_name\":\"a.iso\",\"fs_size_bytes\":1}");
    skiff_romm_rom_page page;
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, list_first_page(&page));
    TEST_ASSERT_EQUAL_UINT64(9007199254740991ULL, page.items[0].id);
}

static void test_valid_json_followed_by_junk_is_refused(void) {
    serve_raw(PAGE_PATH(0), JSON_OK "{\"items\":[],\"total\":0,\"offset\":0}<html>login</html>");
    skiff_romm_rom_page page;
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_ROMM_BAD_RESPONSE, list_first_page(&page));
    tearDown();
    setUp();
    serve_raw(PAGE_PATH(0), JSON_OK "{\"items\":[],\"total\":0,\"offset\":0}{\"items\":[]}");
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_ROMM_BAD_RESPONSE, list_first_page(&page));
    tearDown();
    setUp();
    serve_raw(PAGE_PATH(0), JSON_OK "{\"items\":[],\"total\":0,\"offset\":0}\r\n\t ");
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, list_first_page(&page));
}

/* A ROM page padded with an array of `values` zeros, which cJSON would turn into that many nodes.
 */
static char *padded_page(size_t values) {
    static const char head[] = "{\"items\":[],\"total\":0,\"offset\":0,\"pad\":[";
    const size_t length = sizeof head - 1 + 2 * values + 2;
    char *json = malloc(length + 1);
    TEST_ASSERT_NOT_NULL(json);
    memcpy(json, head, sizeof head - 1);
    size_t used = sizeof head - 1;
    for (size_t i = 0; i < values; i++) {
        json[used++] = '0';
        json[used++] = i + 1 < values ? ',' : ']';
    }
    json[used++] = '}';
    json[used] = '\0';
    return json;
}

static void test_a_body_of_too_many_tiny_values_is_refused_before_parsing(void) {
    skiff_romm_rom_page page;
    /* The page itself opens '{' and '[' and has 3 commas before "pad" opens its '['. */
    const size_t fixed_openings = 6;
    char *json = padded_page(SKIFF_ROMM_JSON_NODES_MAX - fixed_openings);
    TEST_PRINTF("%zu bytes holding %d values: just under the limit", strlen(json),
                SKIFF_ROMM_JSON_NODES_MAX - (int)fixed_openings);
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, skiff_romm_parse_rom_page(json, strlen(json), &page));
    free(json);
    json = padded_page(SKIFF_ROMM_JSON_NODES_MAX - fixed_openings + 1);
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_ROMM_BAD_RESPONSE,
                          skiff_romm_parse_rom_page(json, strlen(json), &page));
    free(json);
}

static void test_a_crc_without_leading_zeros_keeps_its_value(void) {
    serve_page_item(
        "{\"id\":7,\"platform_id\":1,\"fs_name\":\"a.iso\",\"fs_size_bytes\":4294967295,"
        "\"crc_hash\":\"abc\"}");
    skiff_romm_rom_page page;
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, list_first_page(&page));
    TEST_ASSERT_EQUAL_HEX32(0xabcU, page.items[0].crc32);
    TEST_ASSERT_EQUAL_UINT64(4294967295ULL, page.items[0].size);
}

/* ---- What is not RomM's JSON ---- */

static void test_a_proxy_login_page_is_a_bad_response(void) {
    serve_raw(PAGE_PATH(0), "HTTP/1.1 200 OK\r\nContent-Type: text/html\r\n\r\n"
                            "<!DOCTYPE html><html><body>Sign in to continue</body></html>");
    skiff_romm_rom_page page;
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_ROMM_BAD_RESPONSE, list_first_page(&page));
}

static void test_a_cut_response_is_a_bad_response(void) {
    serve_raw(PAGE_PATH(0),
              JSON_OK "{\"items\":[{\"id\":1,\"platform_id\":1,\"fs_name\":\"a.iso\",\"fs_si");
    skiff_romm_rom_page page;
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_ROMM_BAD_RESPONSE, list_first_page(&page));
}

static void test_an_empty_body_is_a_bad_response(void) {
    serve_raw(PAGE_PATH(0), JSON_OK);
    skiff_romm_rom_page page;
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_ROMM_BAD_RESPONSE, list_first_page(&page));
}

static void test_a_connection_lost_mid_body_keeps_its_error(void) {
    fake_route *route = fake_transport_add_fixture(&fake, PAGE_PATH(0), "romm/roms-page-0.http");
    TEST_ASSERT_NOT_NULL(route);
    route->fail_after_bytes = 100;
    route->fail_mid_body = SKIFF_ERR_NET_CONNECTION_LOST;
    skiff_romm_rom_page page;
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_NET_CONNECTION_LOST, list_first_page(&page));
}

static void test_a_server_error_is_a_romm_server_error(void) {
    serve_raw(PAGE_PATH(0), "HTTP/1.1 500 Internal Server Error\r\nContent-Type: text/plain\r\n"
                            "\r\nInternal Server Error");
    skiff_romm_rom_page page;
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_ROMM_SERVER, list_first_page(&page));
}

static void test_an_unreachable_server_keeps_the_network_error(void) {
    fake_route *route = serve_raw(PAGE_PATH(0), JSON_OK "{}");
    route->fail_before_response = SKIFF_ERR_NET_CONNECT;
    skiff_romm_rom_page page;
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_NET_CONNECT, list_first_page(&page));
}

/* A 200 response whose body is `bytes` long: valid JSON padded with blanks. */
static char *oversized_response(size_t bytes, int with_length) {
    char header[128];
    const int header_length =
        with_length ? snprintf(header, sizeof header,
                               "HTTP/1.1 200 OK\r\nContent-Length: %zu\r\n\r\n", bytes)
                    : snprintf(header, sizeof header, "HTTP/1.1 200 OK\r\n\r\n");
    char *raw = malloc((size_t)header_length + bytes + 1);
    TEST_ASSERT_NOT_NULL(raw);
    memcpy(raw, header, (size_t)header_length);
    memset(raw + header_length, ' ', bytes);
    static const char json[] = "{\"items\":[],\"total\":0,\"offset\":0}";
    memcpy(raw + header_length, json, sizeof json - 1);
    raw[(size_t)header_length + bytes] = '\0';
    return raw;
}

static void test_a_body_past_the_cap_is_refused_not_cut(void) {
    char *raw = oversized_response(SKIFF_ROMM_BODY_MAX + 1, 0);
    serve_raw(PAGE_PATH(0), raw);
    free(raw);
    skiff_romm_rom_page page;
    TEST_PRINTF("a body of %zu bytes without Content-Length", SKIFF_ROMM_BODY_MAX + 1);
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_ROMM_BAD_RESPONSE, list_first_page(&page));

    tearDown();
    setUp();
    raw = oversized_response(SKIFF_ROMM_BODY_MAX + 1, 1);
    serve_raw(PAGE_PATH(0), raw);
    free(raw);
    TEST_PRINTF("and one that announces its size is refused at the first chunk");
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_ROMM_BAD_RESPONSE, list_first_page(&page));

    tearDown();
    setUp();
    raw = oversized_response(SKIFF_ROMM_BODY_MAX, 0);
    serve_raw(PAGE_PATH(0), raw);
    free(raw);
    TEST_PRINTF("exactly the cap is read and parsed");
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, list_first_page(&page));
    TEST_ASSERT_EQUAL_size_t(0, page.count);
}

/* ---- One ROM ---- */

static void test_a_rom_comes_with_its_files(void) {
    serve_fixture("/api/roms/2", "romm/rom-extra.http");
    skiff_romm_rom rom;
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, skiff_romm_get_rom(&client, EXTRA_ROM_ID, &rom));
    TEST_PRINTF("ROM %llu: %zu file(s), first '%s' %llu bytes CRC-32 %lx",
                (unsigned long long)rom.summary.id, rom.file_count, rom.files[0].file_name,
                (unsigned long long)rom.files[0].size, (unsigned long)rom.files[0].crc32);
    TEST_ASSERT_EQUAL_size_t(1, rom.file_count);
    TEST_ASSERT_EQUAL_STRING(EXTRA_NAME, rom.files[0].file_name);
    TEST_ASSERT_EQUAL_UINT64(1536, rom.files[0].size);
    TEST_ASSERT_TRUE(rom.files[0].has_crc32);
    TEST_ASSERT_EQUAL_HEX32(EXTRA_CRC32, rom.files[0].crc32);
    TEST_PRINTF("its summary carries the only file too, as a list with files gives it");
    TEST_ASSERT_TRUE(rom.summary.has_file);
    TEST_ASSERT_EQUAL_STRING(EXTRA_NAME, rom.summary.file.file_name);
    TEST_ASSERT_EQUAL_UINT64(1536, rom.summary.file.size);
    TEST_ASSERT_EQUAL_STRING("/api/roms/2", fake.log[0].url + strlen(BASE_URL));
}

static void test_another_rom_under_the_id_is_refused(void) {
    serve_fixture("/api/roms/2", "romm/rom.http");
    skiff_romm_rom rom;
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_ROMM_BAD_RESPONSE,
                          skiff_romm_get_rom(&client, EXTRA_ROM_ID, &rom));
    TEST_ASSERT_EQUAL_size_t(0, rom.file_count);
}

static void test_a_rom_with_many_files_counts_them_all(void) {
    char json[RAW_MAX] = "{\"id\":3,\"platform_id\":1,\"fs_name\":\"Folder\",\"fs_size_bytes\":20,"
                         "\"has_multiple_files\":true,\"files\":[";
    const size_t files = SKIFF_ROMM_FILES_MAX + 4;
    for (size_t i = 0; i < files; i++) {
        char file[96];
        snprintf(file, sizeof file,
                 "%s{\"rom_id\":3,\"file_name\":\"part%zu.bin\",\"file_size_bytes\":1}",
                 i == 0 ? "" : ",", i);
        const size_t used = strlen(json);
        TEST_ASSERT_LESS_THAN_size_t(sizeof json - 4, used + strlen(file));
        memcpy(json + used, file, strlen(file) + 1);
    }
    memcpy(json + strlen(json), "]}", 3);
    skiff_romm_rom rom;
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, skiff_romm_parse_rom(json, strlen(json), &rom));
    TEST_ASSERT_EQUAL_size_t(files, rom.file_count);
    TEST_ASSERT_EQUAL_size_t(SKIFF_ROMM_FILES_MAX, rom.stored_count);
    TEST_ASSERT_TRUE(rom.summary.multiple_files);
    TEST_ASSERT_FALSE(rom.summary.has_file);
    TEST_ASSERT_EQUAL_STRING("part15.bin", rom.files[SKIFF_ROMM_FILES_MAX - 1].file_name);
    TEST_ASSERT_FALSE(rom.files[0].has_crc32);
}

#define ROM_HEAD "{\"id\":3,\"platform_id\":1,\"fs_name\":\"a.iso\",\"fs_size_bytes\":1"

static void test_a_rom_without_files_or_with_a_broken_file_is_refused(void) {
    static const char *const BROKEN[] = {
        ROM_HEAD "}",
        ROM_HEAD ",\"files\":[{\"rom_id\":3,\"file_name\":\"\",\"file_size_bytes\":1}]}",
        ROM_HEAD ",\"files\":[{\"rom_id\":3,\"file_name\":\"a\"}]}",
    };
    for (size_t i = 0; i < sizeof BROKEN / sizeof BROKEN[0]; i++) {
        skiff_romm_rom rom;
        TEST_ASSERT_EQUAL_INT(SKIFF_ERR_ROMM_BAD_RESPONSE,
                              skiff_romm_parse_rom(BROKEN[i], strlen(BROKEN[i]), &rom));
    }
    skiff_romm_rom_page page;
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_ROMM_BAD_RESPONSE, skiff_romm_parse_rom_page(NULL, 0, &page));
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_INVALID_ARG, skiff_romm_parse_rom_page("{}", 2, NULL));
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_INVALID_ARG, skiff_romm_parse_rom("{}", 2, NULL));
}

/* ---- Download URLs ---- */

typedef struct url_case {
    const char *file_name;
    const char *expected_tail;
} url_case;

/* A file whose name RomM serves it under, as skiff_romm_get_rom() fills it. */
static skiff_romm_file file_named(const char *name) {
    skiff_romm_file file;
    memset(&file, 0, sizeof file);
    snprintf(file.file_name, sizeof file.file_name, "%s", name);
    return file;
}

static void test_download_urls_encode_every_reserved_byte(void) {
    static const url_case CASES[] = {
        {PAYLOAD_NAME, "Skiff%20Test%20Payload.iso"},
        {EXTRA_NAME, "Skiff%20Extra%20%232%20%28Caf%C3%A9%20%26%20Co%2B%29.iso"},
        {"a/b?c=d", "a%2Fb%3Fc%3Dd"},
        {"..", ".."},
        {"AZaz09-._~", "AZaz09-._~"},
        {"100%", "100%25"},
    };
    for (size_t i = 0; i < sizeof CASES / sizeof CASES[0]; i++) {
        char url[SKIFF_ROMM_CONTENT_URL_MAX];
        char expected[SKIFF_ROMM_CONTENT_URL_MAX];
        snprintf(expected, sizeof expected, BASE_URL "/api/roms/7/content/%s",
                 CASES[i].expected_tail);
        const skiff_romm_file file = file_named(CASES[i].file_name);
        TEST_ASSERT_EQUAL_INT(SKIFF_OK, skiff_romm_content_url(&client, 7, &file, url, sizeof url));
        TEST_PRINTF("'%s' -> %s", CASES[i].file_name, url);
        TEST_ASSERT_EQUAL_STRING(expected, url);
    }
}

static void test_a_download_url_that_does_not_fit_is_refused(void) {
    const char *expected = BASE_URL "/api/roms/7/content/a%20b";
    const skiff_romm_file file = file_named("a b");
    char url[64];
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_BUFFER_TOO_SMALL,
                          skiff_romm_content_url(&client, 7, &file, url, strlen(expected)));
    TEST_ASSERT_EQUAL_STRING("", url);
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_BUFFER_TOO_SMALL,
                          skiff_romm_content_url(&client, 7, &file, url, 8));
    TEST_ASSERT_EQUAL_INT(SKIFF_OK,
                          skiff_romm_content_url(&client, 7, &file, url, strlen(expected) + 1));
    TEST_ASSERT_EQUAL_STRING(expected, url);
    const skiff_romm_file unnamed = file_named("");
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_INVALID_ARG,
                          skiff_romm_content_url(&client, 7, &unnamed, url, sizeof url));
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_INVALID_ARG,
                          skiff_romm_content_url(NULL, 7, &file, url, sizeof url));
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_INVALID_ARG,
                          skiff_romm_content_url(&client, 7, NULL, url, sizeof url));
}

static void test_a_file_that_is_not_downloadable_gets_no_url(void) {
    static const skiff_romm_name_status UNUSABLE[] = {SKIFF_ROMM_NAME_CONTROL_CHAR,
                                                      SKIFF_ROMM_NAME_TOO_LONG};
    for (size_t i = 0; i < sizeof UNUSABLE / sizeof UNUSABLE[0]; i++) {
        skiff_romm_file file = file_named("a?.iso");
        file.name_status = UNUSABLE[i];
        char url[SKIFF_ROMM_CONTENT_URL_MAX] = "untouched";
        TEST_ASSERT_EQUAL_INT(SKIFF_ERR_INVALID_ARG,
                              skiff_romm_content_url(&client, 7, &file, url, sizeof url));
        TEST_ASSERT_EQUAL_STRING("", url);
    }
}

static void test_the_longest_file_name_fits_the_content_url_buffer(void) {
    char long_base[SKIFF_CONFIG_URL_MAX];
    memset(long_base, 'h', sizeof long_base - 1);
    memcpy(long_base, "https://", 8);
    long_base[sizeof long_base - 1] = '\0';
    skiff_romm_client wide;
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, skiff_romm_client_init(&wide, &fake.base, long_base, NULL));
    skiff_romm_file file = file_named("");
    memset(file.file_name, '#', sizeof file.file_name - 1);
    file.file_name[sizeof file.file_name - 1] = '\0';
    char url[SKIFF_ROMM_CONTENT_URL_MAX];
    TEST_ASSERT_EQUAL_INT(SKIFF_OK,
                          skiff_romm_content_url(&wide, UINT64_MAX, &file, url, sizeof url));
}

/* ---- Covers ---- */

#define COVER_PREFIX SKIFF_ROMM_COVER_PREFIX
/* What RomM gives for an uploaded cover: a path on the server with a raw space in its "?ts="
 * (tests/integration/seed.py uploads synthetic artwork for the recorded payload ROM). */
#define COVER_PATH COVER_PREFIX "roms/1/2/cover/small.png?ts=2026-10-09 23:02:27"

/* A ROM's details JSON whose path_cover_small is cover_json, a JSON value or NULL for none. */
static void rom_with_cover(const char *cover_json, char *json, size_t json_size) {
    snprintf(
        json, json_size,
        ROM_HEAD "%s%s,\"files\":[{\"rom_id\":3,\"file_name\":\"a.iso\","
                 "\"file_size_bytes\":1}]}",
        cover_json == NULL ? "" : ",\"path_cover_small\":", cover_json == NULL ? "" : cover_json);
}

static void test_a_rom_names_its_small_cover(void) {
    serve_fixture("/api/roms/1", "romm/rom.http");
    serve_fixture("/api/roms/2", "romm/rom-extra.http");
    skiff_romm_rom rom;
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, skiff_romm_get_rom(&client, PAYLOAD_ROM_ID, &rom));
    TEST_PRINTF("ROM 1 cover '%s'", rom.cover_path);
    TEST_ASSERT_EQUAL_STRING_LEN(COVER_PREFIX "roms/1/1/cover/small.png?ts=", rom.cover_path,
                                 strlen(COVER_PREFIX "roms/1/1/cover/small.png?ts="));
    TEST_ASSERT_NOT_NULL(strchr(rom.cover_path, ' '));
    TEST_PRINTF("the extra ROM has none");
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, skiff_romm_get_rom(&client, EXTRA_ROM_ID, &rom));
    TEST_ASSERT_EQUAL_STRING("", rom.cover_path);
}

static void test_a_cover_path_skiff_would_not_request_is_no_cover(void) {
    char too_long[SKIFF_ROMM_COVER_PATH_MAX + 8] = "\"" COVER_PREFIX;
    const size_t start = strlen(too_long);
    memset(too_long + start, 'a', SKIFF_ROMM_COVER_PATH_MAX - start + 1);
    memcpy(too_long + SKIFF_ROMM_COVER_PATH_MAX + 1, "\"", 2);
    const char *const NONE[] = {
        NULL,
        "null",
        "\"\"",
        "42",
        "[\"" COVER_PREFIX "x.png\"]",
        "\"https://example.invalid/" COVER_PREFIX "x.png\"",
        "\"//example.invalid" COVER_PREFIX "x.png\"",
        "\"/api/roms/1/content/a.iso\"",
        "\"/assets/romm/resources\"",
        "\"" COVER_PREFIX "\"",
        "\"" COVER_PREFIX "../../api/roms\"",
        "\"" COVER_PREFIX "roms/..\"",
        "\"" COVER_PREFIX "roms/./1/small.png\"",
        "\"" COVER_PREFIX "a#/../../api/heartbeat\"",
        "\"" COVER_PREFIX "a#b/..\"",
        "\"" COVER_PREFIX "roms\\\\1\\\\small.png\"",
        "\"" COVER_PREFIX "small.png\\u0007\"",
        "\"" COVER_PREFIX "small.png?ts=\\u009b\"",
        too_long,
    };
    for (size_t i = 0; i < sizeof NONE / sizeof NONE[0]; i++) {
        char json[RAW_MAX];
        rom_with_cover(NONE[i], json, sizeof json);
        skiff_romm_rom rom;
        memset(&rom, 'x', sizeof rom);
        TEST_PRINTF("cover %s", NONE[i] == NULL ? "(absent)" : NONE[i]);
        TEST_ASSERT_EQUAL_INT(SKIFF_OK, skiff_romm_parse_rom(json, strlen(json), &rom));
        TEST_ASSERT_EQUAL_STRING("", rom.cover_path);
        TEST_ASSERT_EQUAL_size_t(1, rom.file_count);
    }
}

static void test_a_cover_path_keeps_dots_that_climb_nowhere(void) {
    const char *const KEPT[] = {
        COVER_PATH,
        COVER_PREFIX "roms/1/2/cover/small.png?ts=a/../b",
        COVER_PREFIX "roms/1/2/cover/...png",
        COVER_PREFIX "roms/1/2/cover/.hidden.png",
    };
    for (size_t i = 0; i < sizeof KEPT / sizeof KEPT[0]; i++) {
        char cover_json[SKIFF_ROMM_COVER_PATH_MAX + 2];
        snprintf(cover_json, sizeof cover_json, "\"%s\"", KEPT[i]);
        char json[RAW_MAX];
        rom_with_cover(cover_json, json, sizeof json);
        skiff_romm_rom rom;
        TEST_ASSERT_EQUAL_INT(SKIFF_OK, skiff_romm_parse_rom(json, strlen(json), &rom));
        TEST_PRINTF("kept '%s'", rom.cover_path);
        TEST_ASSERT_EQUAL_STRING(KEPT[i], rom.cover_path);
    }
}

typedef struct cover_url_case {
    const char *path;
    const char *expected_tail;
} cover_url_case;

static void test_cover_urls_encode_what_a_request_line_cannot_carry(void) {
    static const cover_url_case CASES[] = {
        {COVER_PATH, COVER_PREFIX "roms/1/2/cover/small.png?ts=2026-10-09%2023:02:27"},
        {COVER_PREFIX "a.png?ts=2026-10-09 23:02:27+00:00&v=1",
         COVER_PREFIX "a.png?ts=2026-10-09%2023:02:27%2B00:00&v=1"},
        {COVER_PREFIX "Caf\xC3\xA9 #1 100%.png", COVER_PREFIX "Caf%C3%A9%20%231%20100%25.png"},
        {COVER_PREFIX "AZaz09-._~/?=&:", COVER_PREFIX "AZaz09-._~/?=&:"},
    };
    for (size_t i = 0; i < sizeof CASES / sizeof CASES[0]; i++) {
        char url[SKIFF_ROMM_COVER_URL_MAX];
        TEST_ASSERT_EQUAL_INT(SKIFF_OK,
                              skiff_romm_cover_url(&client, CASES[i].path, url, sizeof url));
        TEST_PRINTF("'%s' -> %s", CASES[i].path, url);
        TEST_ASSERT_EQUAL_STRING_LEN(BASE_URL, url, strlen(BASE_URL));
        TEST_ASSERT_EQUAL_STRING(CASES[i].expected_tail, url + strlen(BASE_URL));
    }
}

static void test_a_cover_url_is_refused_when_it_would_lead_elsewhere_or_not_fit(void) {
    static const char *const REFUSED[] = {
        "",
        "/api/heartbeat",
        "//example.invalid" COVER_PREFIX "a.png",
        COVER_PREFIX "../api/heartbeat",
        COVER_PREFIX "a#/../../api/heartbeat",
        COVER_PREFIX "a\\b.png",
        COVER_PREFIX "a\x01.png",
    };
    char url[SKIFF_ROMM_COVER_URL_MAX];
    for (size_t i = 0; i < sizeof REFUSED / sizeof REFUSED[0]; i++) {
        snprintf(url, sizeof url, "untouched");
        TEST_ASSERT_EQUAL_INT(SKIFF_ERR_INVALID_ARG,
                              skiff_romm_cover_url(&client, REFUSED[i], url, sizeof url));
        TEST_ASSERT_EQUAL_STRING("", url);
    }
    const char *expected = BASE_URL COVER_PREFIX "a%20b.png";
    TEST_ASSERT_EQUAL_INT(
        SKIFF_ERR_BUFFER_TOO_SMALL,
        skiff_romm_cover_url(&client, COVER_PREFIX "a b.png", url, strlen(expected)));
    TEST_ASSERT_EQUAL_STRING("", url);
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_BUFFER_TOO_SMALL,
                          skiff_romm_cover_url(&client, COVER_PREFIX "a b.png", url, 4));
    TEST_ASSERT_EQUAL_STRING("", url);
    TEST_ASSERT_EQUAL_INT(
        SKIFF_OK, skiff_romm_cover_url(&client, COVER_PREFIX "a b.png", url, strlen(expected) + 1));
    TEST_ASSERT_EQUAL_STRING(expected, url);
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_INVALID_ARG,
                          skiff_romm_cover_url(NULL, COVER_PATH, url, sizeof url));
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_INVALID_ARG,
                          skiff_romm_cover_url(&client, NULL, url, sizeof url));
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_INVALID_ARG,
                          skiff_romm_cover_url(&client, COVER_PATH, NULL, sizeof url));
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_INVALID_ARG, skiff_romm_cover_url(&client, COVER_PATH, url, 0));
}

static void test_the_longest_cover_path_fits_the_cover_url_buffer(void) {
    char long_base[SKIFF_CONFIG_URL_MAX];
    memset(long_base, 'h', sizeof long_base - 1);
    memcpy(long_base, "https://", 8);
    long_base[sizeof long_base - 1] = '\0';
    skiff_romm_client wide;
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, skiff_romm_client_init(&wide, &fake.base, long_base, NULL));
    char path[SKIFF_ROMM_COVER_PATH_MAX];
    memset(path, ' ', sizeof path - 1);
    memcpy(path, COVER_PREFIX, strlen(COVER_PREFIX));
    path[sizeof path - 1] = '\0';
    char url[SKIFF_ROMM_COVER_URL_MAX];
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, skiff_romm_cover_url(&wide, path, url, sizeof url));
    TEST_PRINTF("longest cover URL: %zu of %d bytes", strlen(url) + 1, SKIFF_ROMM_COVER_URL_MAX);
}

static void test_a_cover_is_fetched_without_the_token(void) {
    static const char raw[] = "HTTP/1.1 200 OK\r\nContent-Type: image/png\r\n\r\n\x89PNG-bytes";
    TEST_ASSERT_NOT_NULL(fake_transport_add_raw(
        &fake, COVER_PREFIX "roms/1/2/cover/small.png?ts=2026-10-09%2023:02:27", raw,
        sizeof raw - 1));
    unsigned char *bytes = NULL;
    size_t size = 0;
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, skiff_romm_get_cover(&client, COVER_PATH, &bytes, &size));
    TEST_PRINTF("GET %s: %zu bytes, headers '%s'", fake.log[0].url, size, fake.log[0].headers);
    TEST_ASSERT_EQUAL_size_t(10, size);
    TEST_ASSERT_EQUAL_MEMORY("\x89PNG-bytes", bytes, size);
    TEST_ASSERT_NULL(strstr(fake.log[0].headers, "Authorization"));
    free(bytes);
}

static void test_a_cover_that_is_missing_empty_or_too_large_is_refused(void) {
    static const char missing[] = "HTTP/1.1 404 Not Found\r\nContent-Type: text/html\r\n\r\n<html>";
    static const char empty[] = "HTTP/1.1 200 OK\r\nContent-Length: 0\r\n\r\n";
    static const char large[] = "HTTP/1.1 200 OK\r\nContent-Length: 600000\r\n\r\nxxxx";
    const struct {
        const char *raw;
        size_t size;
        skiff_err expected;
    } CASES[] = {
        {missing, sizeof missing - 1, SKIFF_ERR_ROMM_NOT_FOUND},
        {empty, sizeof empty - 1, SKIFF_ERR_ROMM_COVER_DAMAGED},
        {large, sizeof large - 1, SKIFF_ERR_ROMM_BAD_RESPONSE},
    };
    for (size_t i = 0; i < sizeof CASES / sizeof CASES[0]; i++) {
        char path[64];
        snprintf(path, sizeof path, COVER_PREFIX "case%zu.png", i);
        TEST_ASSERT_NOT_NULL(fake_transport_add_raw(&fake, path, CASES[i].raw, CASES[i].size));
        unsigned char *bytes = (unsigned char *)"untouched";
        size_t size = 99;
        const skiff_err err = skiff_romm_get_cover(&client, path, &bytes, &size);
        TEST_PRINTF("case %zu -> %s", i, skiff_err_name(err));
        TEST_ASSERT_EQUAL_INT(CASES[i].expected, err);
        TEST_ASSERT_NULL(bytes);
        TEST_ASSERT_EQUAL_size_t(0, size);
    }
    unsigned char *bytes = NULL;
    size_t size = 0;
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_INVALID_ARG,
                          skiff_romm_get_cover(&client, "/api/heartbeat", &bytes, &size));
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_INVALID_ARG,
                          skiff_romm_get_cover(NULL, COVER_PATH, &bytes, &size));
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_INVALID_ARG,
                          skiff_romm_get_cover(&client, NULL, &bytes, &size));
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_INVALID_ARG,
                          skiff_romm_get_cover(&client, COVER_PATH, NULL, &size));
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_INVALID_ARG,
                          skiff_romm_get_cover(&client, COVER_PATH, &bytes, NULL));
}

/* ---- The client ---- */

static void test_the_client_checks_its_settings(void) {
    skiff_romm_client other;
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_CONFIG_INVALID_VALUE,
                          skiff_romm_client_init(&other, &fake.base, "romm.test", TOKEN));
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_INVALID_ARG,
                          skiff_romm_client_init(&other, &fake.base, BASE_URL, "rmm with space"));
    TEST_ASSERT_EQUAL_STRING("", other.authorization);
    char long_token[SKIFF_CONFIG_TOKEN_MAX + 1];
    memset(long_token, 't', sizeof long_token - 1);
    long_token[sizeof long_token - 1] = '\0';
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_INVALID_ARG,
                          skiff_romm_client_init(&other, &fake.base, BASE_URL, long_token));
    long_token[sizeof long_token - 2] = '\0';
    TEST_ASSERT_EQUAL_INT(SKIFF_OK,
                          skiff_romm_client_init(&other, &fake.base, BASE_URL, long_token));
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_INVALID_ARG,
                          skiff_romm_client_init(&other, NULL, BASE_URL, TOKEN));
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_INVALID_ARG,
                          skiff_romm_client_init(NULL, &fake.base, BASE_URL, TOKEN));
    TEST_PRINTF("trailing slashes are dropped, so paths never start with '//'");
    TEST_ASSERT_EQUAL_INT(SKIFF_OK,
                          skiff_romm_client_init(&other, &fake.base, BASE_URL "/romm//", NULL));
    TEST_ASSERT_EQUAL_STRING(BASE_URL "/romm", other.base_url);
    skiff_http_header header;
    TEST_ASSERT_EQUAL_size_t(0, skiff_romm_auth_header(&other, &header));
}

static void test_the_token_header_is_offered_and_wiped(void) {
    skiff_http_header header;
    TEST_ASSERT_EQUAL_size_t(1, skiff_romm_auth_header(&client, &header));
    TEST_ASSERT_EQUAL_STRING("Authorization", header.name);
    TEST_ASSERT_EQUAL_STRING("Bearer " TOKEN, header.value);
    TEST_ASSERT_TRUE(skiff_http_headers_valid(&header, 1));
    skiff_romm_client_clear(&client);
    for (size_t i = 0; i < sizeof client.authorization; i++) {
        TEST_ASSERT_EQUAL_CHAR('\0', client.authorization[i]);
    }
    TEST_ASSERT_EQUAL_size_t(0, skiff_romm_auth_header(&client, &header));
    skiff_romm_client_clear(NULL);
}

static void test_requests_refuse_bad_arguments(void) {
    skiff_romm_server server;
    skiff_romm_platform platform;
    skiff_romm_rom rom;
    skiff_romm_client empty;
    memset(&empty, 0, sizeof empty);
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_INVALID_ARG, skiff_romm_heartbeat(&empty, &server));
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_INVALID_ARG, skiff_romm_heartbeat(&client, NULL));
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_INVALID_ARG, skiff_romm_find_platform(&client, "", &platform));
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_INVALID_ARG, skiff_romm_find_platform(&client, "psp", NULL));
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_INVALID_ARG, skiff_romm_get_rom(NULL, 1, &rom));
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_INVALID_ARG,
                          skiff_romm_list_roms(&client, &ALL_ROMS, 0, 1, NULL));
    skiff_romm_rom_page page;
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_INVALID_ARG, skiff_romm_list_roms(&client, NULL, 0, 1, &page));
    TEST_ASSERT_EQUAL_size_t(0, fake.request_count);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_the_version_policy);
    RUN_TEST(test_heartbeat_reads_the_recorded_version_without_the_token);
    RUN_TEST(test_an_old_server_is_refused_and_still_named);
    RUN_TEST(test_a_heartbeat_with_an_escaped_control_is_a_bad_response);
    RUN_TEST(test_a_heartbeat_without_a_version_is_a_bad_response);
    RUN_TEST(test_the_psp_platform_is_found_with_the_token);
    RUN_TEST(test_a_missing_platform_is_not_found);
    RUN_TEST(test_a_rejected_token_is_a_romm_refusal);
    RUN_TEST(test_a_platform_list_that_is_not_an_array_is_a_bad_response);
    RUN_TEST(test_a_psp_platform_with_a_bad_field_is_a_bad_response);
    RUN_TEST(test_two_pages_of_one_rom_then_an_empty_page);
    RUN_TEST(test_another_page_than_the_one_asked_for_is_refused);
    RUN_TEST(test_a_page_longer_than_asked_is_refused);
    RUN_TEST(test_page_limits_are_checked);
    RUN_TEST(test_favourites_list_with_their_files_then_end);
    RUN_TEST(test_a_list_asks_only_for_what_its_query_wants);
    RUN_TEST(test_a_favourite_of_another_platform_is_refused);
    RUN_TEST(test_a_listed_file_is_kept_only_when_it_is_the_roms_only_one);
    RUN_TEST(test_every_field_is_checked_before_a_rom_is_shown);
    RUN_TEST(test_a_rom_with_a_control_character_is_listed_but_not_downloadable);
    RUN_TEST(test_a_title_with_a_control_character_marks_its_rom);
    RUN_TEST(test_a_file_with_a_control_character_is_listed_but_not_downloadable);
    RUN_TEST(test_a_clean_file_of_a_rom_with_an_unusable_name_gets_no_url);
    RUN_TEST(test_c1_control_characters_count_as_one_control_each);
    RUN_TEST(test_a_raw_delete_byte_in_a_name_is_still_refused);
    RUN_TEST(test_an_overlong_name_is_cut_at_a_character_with_a_marker);
    RUN_TEST(test_an_overlong_file_name_is_listed_but_not_downloadable);
    RUN_TEST(test_structural_problems_still_refuse_the_page);
    RUN_TEST(test_a_rom_of_another_platform_is_refused);
    RUN_TEST(test_a_page_beyond_its_total_is_refused);
    RUN_TEST(test_a_file_of_another_rom_is_refused);
    RUN_TEST(test_a_file_of_another_rom_past_the_kept_ones_is_refused_too);
    RUN_TEST(test_control_characters_in_a_string_are_refused);
    RUN_TEST(test_the_largest_exact_id_is_accepted);
    RUN_TEST(test_valid_json_followed_by_junk_is_refused);
    RUN_TEST(test_a_body_of_too_many_tiny_values_is_refused_before_parsing);
    RUN_TEST(test_a_crc_without_leading_zeros_keeps_its_value);
    RUN_TEST(test_a_proxy_login_page_is_a_bad_response);
    RUN_TEST(test_a_cut_response_is_a_bad_response);
    RUN_TEST(test_an_empty_body_is_a_bad_response);
    RUN_TEST(test_a_connection_lost_mid_body_keeps_its_error);
    RUN_TEST(test_a_server_error_is_a_romm_server_error);
    RUN_TEST(test_an_unreachable_server_keeps_the_network_error);
    RUN_TEST(test_a_body_past_the_cap_is_refused_not_cut);
    RUN_TEST(test_a_rom_comes_with_its_files);
    RUN_TEST(test_another_rom_under_the_id_is_refused);
    RUN_TEST(test_a_rom_with_many_files_counts_them_all);
    RUN_TEST(test_a_rom_without_files_or_with_a_broken_file_is_refused);
    RUN_TEST(test_download_urls_encode_every_reserved_byte);
    RUN_TEST(test_a_download_url_that_does_not_fit_is_refused);
    RUN_TEST(test_a_file_that_is_not_downloadable_gets_no_url);
    RUN_TEST(test_the_longest_file_name_fits_the_content_url_buffer);
    RUN_TEST(test_a_rom_names_its_small_cover);
    RUN_TEST(test_a_cover_path_skiff_would_not_request_is_no_cover);
    RUN_TEST(test_a_cover_path_keeps_dots_that_climb_nowhere);
    RUN_TEST(test_cover_urls_encode_what_a_request_line_cannot_carry);
    RUN_TEST(test_a_cover_url_is_refused_when_it_would_lead_elsewhere_or_not_fit);
    RUN_TEST(test_the_longest_cover_path_fits_the_cover_url_buffer);
    RUN_TEST(test_a_cover_is_fetched_without_the_token);
    RUN_TEST(test_a_cover_that_is_missing_empty_or_too_large_is_refused);
    RUN_TEST(test_the_client_checks_its_settings);
    RUN_TEST(test_the_token_header_is_offered_and_wiped);
    RUN_TEST(test_requests_refuse_bad_arguments);
    return UNITY_END();
}
