/*
 * RomM's save sync calls (skiff/romm_saves.h) against the fake transport replaying RomM 5.4.0's
 * answers (tests/fixtures/romm/saves-*.http, recorded by tests/integration/record-saves-fixtures.sh
 * with synthetic device ids): negotiate, upload (200 and 409), content, downloaded and complete,
 * with the requests checked byte for byte, every way an answer can be wrong refused, and the
 * Memory Stick failing under an upload or a download without leaving anything behind. The unit
 * files are the recorder's synthetic zips, read through the storage seam from the fixtures folder.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>

#include "skiff/romm_saves.h"

#include "fake_storage.h"
#include "fake_transport.h"
#include "storage_posix.h"
#include "temp_dir.h"
#include "unity.h"

#define BASE_URL "https://romm.test"
#define TOKEN "rmm_0123456789abcdef0123456789abcdef"
/* What the recorded answers hold (record-saves-fixtures.sh). */
#define DEVICE_A "00000000-0000-4000-8000-00000000000a"
#define DEVICE_B "00000000-0000-4000-8000-00000000000b"
#define ROM_ID 1
#define SAVE_1 1
#define SAVE_2 2
#define SESSION_A 1
#define UNIT_1 SKIFF_FIXTURES_DIR "/romm/save-unit-1.zip"
#define UNIT_2 SKIFF_FIXTURES_DIR "/romm/save-unit-2.zip"
#define UNIT_BYTES 1612U
#define HASH_1 "969f6b134c73db736eb427afde88aee4"
#define HASH_2 "3a54b3d66a1c01e3fdcfa5a2cfb9afb2"
#define FILE_NAME "SKIF00001.zip"
#define SLOT "autosave"
#define EMULATOR "psp_console"
#define KEEP_VERSIONS 10U
/* 2026-10-11 18:00:00 UTC, as the recorder sends it. */
#define UPDATED_AT_MS 1791741600000LL
#define UPDATED_AT "2026-10-11T18:00:00.000Z"
#define PATH_NEGOTIATE "/api/sync/negotiate"
#define UPLOAD_QUERY(device, hash, overwrite)                                                      \
    "/api/saves?rom_id=1&emulator=" EMULATOR "&slot=" SLOT "&device_id=" device                    \
    "&content_hash=" hash "&overwrite=" overwrite "&autocleanup=true&autocleanup_limit=10"
#define PATH_UPLOAD_A UPLOAD_QUERY(DEVICE_A, HASH_1, "false")
#define PATH_CONTENT "/api/saves/1/content?device_id=" DEVICE_B "&optimistic=false"
#define PATH_DOWNLOADED "/api/saves/1/downloaded"
#define PATH_COMPLETE "/api/sync/sessions/1/complete"
#define JSON_200 "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n\r\n"
#define RAW_MAX 8192
#define UNIT_MAX 4096
/* The multipart framing Skiff writes around the unit (src/romm/saves.c). */
#define BOUNDARY "skiff-" HASH_1
#define MULTIPART_HEAD                                                                             \
    "--" BOUNDARY "\r\nContent-Disposition: form-data; name=\"saveFile\"; filename=\"" FILE_NAME   \
    "\"\r\nContent-Type: application/zip\r\n\r\n"
#define MULTIPART_TAIL "\r\n--" BOUNDARY "--\r\n"

static fake_transport fake;
static skiff_romm_client client;
static char dir[TEMP_DIR_PATH_MAX];
static skiff_storage *posix;
static fake_storage storage;
static skiff_romm_sync_plan plan;
static skiff_romm_save save;

void setUp(void) {
    fake_transport_init(&fake);
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, skiff_romm_client_init(&client, &fake.base, BASE_URL, TOKEN));
    TEST_ASSERT_EQUAL_INT(0, temp_dir_create(dir, sizeof dir));
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, skiff_posix_storage_create(&posix));
    fake_storage_init(&storage, posix);
    memset(&plan, 0xAB, sizeof plan);
    memset(&save, 0xAB, sizeof save);
}

void tearDown(void) {
    skiff_transport_destroy(&fake.base);
    skiff_storage_destroy(posix);
    temp_dir_remove(dir);
}

/* Drops every route and the log, for a test that serves another answer. */
static void reset_fake(void) {
    skiff_transport_destroy(&fake.base);
    fake_transport_init(&fake);
}

static fake_route *serve(const char *path, const char *fixture) {
    fake_route *route = fake_transport_add_fixture(&fake, path, fixture);
    TEST_ASSERT_NOT_NULL_MESSAGE(route, fixture);
    return route;
}

static fake_route *serve_raw(const char *path, const char *raw) {
    fake_route *route = fake_transport_add_raw(&fake, path, raw, strlen(raw));
    TEST_ASSERT_NOT_NULL(route);
    return route;
}

/* Every request carried the token in its Authorization header and nowhere else. */
static void assert_token_only_in_headers(void) {
    for (size_t i = 0; i < fake.log_count; i++) {
        TEST_ASSERT_NOT_NULL_MESSAGE(strstr(fake.log[i].headers, "Authorization: Bearer " TOKEN),
                                     fake.log[i].url);
        TEST_ASSERT_NULL_MESSAGE(strstr(fake.log[i].url, TOKEN), "no token in a URL");
        TEST_ASSERT_NULL_MESSAGE(strstr(fake.log[i].body, TOKEN), "no token in a body");
    }
}

static size_t read_file(const char *path, unsigned char *out, size_t out_size) {
    FILE *file = fopen(path, "rb");
    TEST_ASSERT_NOT_NULL_MESSAGE(file, path);
    const size_t size = fread(out, 1, out_size, file);
    fclose(file);
    return size;
}

static int exists(const char *path) {
    FILE *file = fopen(path, "rb");
    if (file != NULL) {
        fclose(file);
    }
    return file != NULL;
}

/* ---- Negotiate ---- */

static const uint64_t ROMS[] = {ROM_ID};

static skiff_romm_save_state unit_state(void) {
    const skiff_romm_save_state state = {ROM_ID, FILE_NAME,     SLOT,      EMULATOR,
                                         HASH_1, UPDATED_AT_MS, UNIT_BYTES};
    return state;
}

static skiff_err negotiate(const skiff_romm_save_state *saves, size_t count, const char *device) {
    const skiff_romm_sync_query query = {device, ROMS, 1, saves, count, 0};
    const skiff_err err = skiff_romm_saves_negotiate(&client, &query, &plan);
    TEST_PRINTF("negotiate -> %s: session %llu, %zu operation(s)", skiff_err_name(err),
                (unsigned long long)plan.session_id, plan.count);
    for (size_t i = 0; i < plan.count; i++) {
        const skiff_romm_sync_operation *op = &plan.operations[i];
        TEST_PRINTF(
            "  action %d rom %llu save %s%llu '%s' slot '%s' emulator '%s' hash '%s' at '%s'",
            (int)op->action, (unsigned long long)op->rom_id, op->has_save_id ? "" : "none ",
            (unsigned long long)op->save_id, op->file_name, op->slot, op->emulator,
            op->server_content_hash, op->server_updated_at);
    }
    return err;
}

static void test_negotiate_sends_this_psps_save_and_reads_an_upload(void) {
    serve(PATH_NEGOTIATE, "romm/saves-negotiate-upload.http");
    const skiff_romm_save_state state = unit_state();
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, negotiate(&state, 1, DEVICE_A));
    TEST_PRINTF("sent: %s", fake.log[0].body);
    TEST_ASSERT_EQUAL_STRING(
        "{\"device_id\":\"" DEVICE_A
        "\",\"rom_ids\":[1],\"saves\":[{\"rom_id\":1,\"file_name\":\"" FILE_NAME
        "\",\"slot\":\"" SLOT "\",\"emulator\":\"" EMULATOR "\",\"content_hash\":\"" HASH_1
        "\",\"updated_at\":\"" UPDATED_AT "\",\"file_size_bytes\":1612}]}",
        fake.log[0].body);
    TEST_ASSERT_EQUAL_INT(SKIFF_HTTP_POST, fake.log[0].method);
    TEST_ASSERT_EQUAL_STRING("application/json", fake.log[0].content_type);
    TEST_ASSERT_NULL_MESSAGE(strstr(fake.log[0].body, "emulators"),
                             "no emulators filter: every emulator's save in a slot pairs");
    assert_token_only_in_headers();
    TEST_ASSERT_EQUAL_UINT64(SESSION_A, plan.session_id);
    TEST_ASSERT_EQUAL_size_t(1, plan.count);
    const skiff_romm_sync_operation *op = &plan.operations[0];
    TEST_ASSERT_EQUAL_INT(SKIFF_ROMM_SYNC_UPLOAD, op->action);
    TEST_ASSERT_EQUAL_UINT64(ROM_ID, op->rom_id);
    TEST_ASSERT_FALSE(op->has_save_id);
    TEST_ASSERT_EQUAL_STRING(FILE_NAME, op->file_name);
    TEST_ASSERT_TRUE(op->has_slot);
    TEST_ASSERT_EQUAL_STRING(SLOT, op->slot);
    TEST_ASSERT_EQUAL_STRING(EMULATOR, op->emulator);
    TEST_ASSERT_EQUAL_STRING("", op->server_content_hash);
    TEST_ASSERT_EQUAL_STRING("", op->server_updated_at);
    TEST_ASSERT_FALSE_MESSAGE(fake.log[0].long_transfer, "a negotiate keeps the screen's limit");
}

static void test_negotiate_asks_back_the_saves_this_psp_stopped_listing(void) {
    serve(PATH_NEGOTIATE, "romm/saves-negotiate-download.http");
    const skiff_romm_sync_query query = {DEVICE_B, ROMS, 1, NULL, 0, 1};
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, skiff_romm_saves_negotiate(&client, &query, &plan));
    TEST_PRINTF("sent: %s", fake.log[0].body);
    TEST_ASSERT_EQUAL_STRING("{\"device_id\":\"" DEVICE_B
                             "\",\"restore_unlisted\":true,\"rom_ids\":[1],\"saves\":[]}",
                             fake.log[0].body);
}

static void test_negotiate_offers_a_second_psp_the_download(void) {
    serve(PATH_NEGOTIATE, "romm/saves-negotiate-download.http");
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, negotiate(NULL, 0, DEVICE_B));
    TEST_ASSERT_EQUAL_STRING("{\"device_id\":\"" DEVICE_B "\",\"rom_ids\":[1],\"saves\":[]}",
                             fake.log[0].body);
    TEST_ASSERT_EQUAL_size_t(1, plan.count);
    const skiff_romm_sync_operation *op = &plan.operations[0];
    TEST_ASSERT_EQUAL_INT(SKIFF_ROMM_SYNC_DOWNLOAD, op->action);
    TEST_ASSERT_TRUE(op->has_save_id);
    TEST_ASSERT_EQUAL_UINT64(SAVE_1, op->save_id);
    TEST_ASSERT_EQUAL_STRING(HASH_1, op->server_content_hash);
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, strncmp(op->file_name, "SKIF00001 [", 11),
                                  "RomM's name carries its date tag");
    TEST_ASSERT_NOT_EQUAL_INT(0, (int)strlen(op->server_updated_at));
}

static void test_negotiate_finds_nothing_to_do_once_in_step(void) {
    serve(PATH_NEGOTIATE, "romm/saves-negotiate-no-op.http");
    const skiff_romm_save_state state = unit_state();
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, negotiate(&state, 1, DEVICE_B));
    TEST_ASSERT_EQUAL_size_t(1, plan.count);
    TEST_ASSERT_EQUAL_INT(SKIFF_ROMM_SYNC_NO_OP, plan.operations[0].action);
    TEST_ASSERT_EQUAL_UINT64(SAVE_1, plan.operations[0].save_id);
}

static void test_a_token_without_the_save_scopes_is_forbidden(void) {
    serve(PATH_NEGOTIATE, "romm/saves-negotiate-forbidden.http");
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_ROMM_FORBIDDEN, negotiate(NULL, 0, DEVICE_A));
    TEST_ASSERT_EQUAL_size_t(0, plan.count);
}

static void test_negotiate_returns_every_slot_and_action(void) {
    serve_raw(PATH_NEGOTIATE, JSON_200
              "{\"session_id\":9007199254740991,\"operations\":["
              "{\"action\":\"download\",\"rom_id\":1,\"save_id\":6,\"file_name\":\"a.srm\","
              "\"slot\":\"quick\",\"emulator\":null,\"reason\":\"r\","
              "\"server_updated_at\":null,\"server_content_hash\":null},"
              "{\"action\":\"conflict\",\"rom_id\":1,\"save_id\":7,\"file_name\":\"b\","
              "\"slot\":\"autosave\",\"emulator\":\"ppsspp\",\"reason\":\"r\"},"
              "{\"action\":\"delete\",\"rom_id\":1,\"save_id\":null,\"file_name\":\"c\","
              "\"slot\":null,\"reason\":\"r\"},"
              "{\"action\":\"no_op\",\"rom_id\":1,\"file_name\":\"d\",\"reason\":\"r\"}],"
              "\"total_upload\":0}");
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, negotiate(NULL, 0, DEVICE_A));
    TEST_PRINTF("the caller keeps its own slot's: Skiff filters, RomM does not");
    TEST_ASSERT_EQUAL_UINT64(9007199254740991ULL, plan.session_id);
    TEST_ASSERT_EQUAL_size_t(4, plan.count);
    TEST_ASSERT_EQUAL_STRING("quick", plan.operations[0].slot);
    TEST_ASSERT_EQUAL_STRING("", plan.operations[0].emulator);
    TEST_ASSERT_EQUAL_INT(SKIFF_ROMM_SYNC_CONFLICT, plan.operations[1].action);
    TEST_ASSERT_EQUAL_STRING("ppsspp", plan.operations[1].emulator);
    TEST_ASSERT_EQUAL_INT(SKIFF_ROMM_SYNC_DELETE, plan.operations[2].action);
    TEST_ASSERT_FALSE(plan.operations[2].has_slot);
    TEST_ASSERT_FALSE(plan.operations[2].has_save_id);
    TEST_ASSERT_EQUAL_INT(SKIFF_ROMM_SYNC_NO_OP, plan.operations[3].action);
}

static void test_negotiate_without_roms_asks_for_the_whole_library(void) {
    serve(PATH_NEGOTIATE, "romm/saves-negotiate-download.http");
    const skiff_romm_sync_query query = {DEVICE_B, NULL, 0, NULL, 0, 0};
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, skiff_romm_saves_negotiate(&client, &query, &plan));
    TEST_ASSERT_EQUAL_STRING("{\"device_id\":\"" DEVICE_B "\",\"saves\":[]}", fake.log[0].body);
    TEST_ASSERT_EQUAL_size_t(1, plan.count);
}

static void test_an_operation_for_a_rom_not_asked_about_is_refused(void) {
    serve_raw(PATH_NEGOTIATE,
              JSON_200 "{\"session_id\":1,\"operations\":[{\"action\":\"upload\",\"rom_id\":2,"
                       "\"file_name\":\"x.zip\",\"slot\":\"autosave\",\"reason\":\"r\"}]}");
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_ROMM_BAD_RESPONSE, negotiate(NULL, 0, DEVICE_A));
    TEST_ASSERT_EQUAL_size_t(0, plan.count);
    TEST_PRINTF("a ROM named only by a save counts as asked about");
    skiff_romm_save_state state = unit_state();
    state.rom_id = 2;
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, negotiate(&state, 1, DEVICE_A));
}

typedef struct bad_plan {
    const char *why;
    const char *body;
} bad_plan;

#define OPERATION(fields) "{\"session_id\":1,\"operations\":[{" fields "}]}"
#define UPLOAD_OP "\"action\":\"upload\",\"rom_id\":1,\"file_name\":\"x.zip\""

static void test_every_malformed_plan_is_refused(void) {
    static const bad_plan CASES[] = {
        {"not JSON", "<html>login</html>"},
        {"no session", "{\"operations\":[]}"},
        {"a session that is not a whole number", "{\"session_id\":1.5,\"operations\":[]}"},
        {"no operations", "{\"session_id\":1}"},
        {"operations that are not a list", "{\"session_id\":1,\"operations\":{}}"},
        {"an operation that is not an object", "{\"session_id\":1,\"operations\":[1]}"},
        {"an unknown action", OPERATION("\"action\":\"merge\",\"rom_id\":1,\"file_name\":\"x\"")},
        {"an action that is not a string",
         OPERATION("\"action\":1,\"rom_id\":1,\"file_name\":\"x\"")},
        {"no ROM", OPERATION("\"action\":\"upload\",\"file_name\":\"x\"")},
        {"a negative save id", OPERATION(UPLOAD_OP ",\"save_id\":-1")},
        {"a save id that is a string", OPERATION(UPLOAD_OP ",\"save_id\":\"1\"")},
        {"no file name", OPERATION("\"action\":\"upload\",\"rom_id\":1")},
        {"a control character in the file name",
         OPERATION("\"action\":\"upload\",\"rom_id\":1,\"file_name\":\"x\\u0007.zip\"")},
        {"a slot that is a number", OPERATION(UPLOAD_OP ",\"slot\":3")},
        {"an emulator that is not a string", OPERATION(UPLOAD_OP ",\"emulator\":[]")},
        {"an uppercase hash",
         OPERATION(UPLOAD_OP ",\"server_content_hash\":\"969F6B134C73DB736EB427AFDE88AEE4\"")},
        {"a short hash", OPERATION(UPLOAD_OP ",\"server_content_hash\":\"969f6b13\"")},
        {"a download without a save id",
         OPERATION("\"action\":\"download\",\"rom_id\":1,\"file_name\":\"x\"")},
        {"a conflict without a save id",
         OPERATION("\"action\":\"conflict\",\"rom_id\":1,\"save_id\":null,\"file_name\":\"x\"")},
        {"junk after the JSON", "{\"session_id\":1,\"operations\":[]} x"},
    };
    for (size_t i = 0; i < sizeof CASES / sizeof CASES[0]; i++) {
        const skiff_err err =
            skiff_romm_parse_sync_plan(CASES[i].body, strlen(CASES[i].body), &plan);
        TEST_PRINTF("%s -> %s", CASES[i].why, skiff_err_name(err));
        TEST_ASSERT_EQUAL_INT_MESSAGE(SKIFF_ERR_ROMM_BAD_RESPONSE, err, CASES[i].why);
        TEST_ASSERT_EQUAL_size_t(0, plan.count);
        TEST_ASSERT_EQUAL_UINT64(0, plan.session_id);
    }
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_INVALID_ARG, skiff_romm_parse_sync_plan("{}", 2, NULL));
}

/* A body of fields too long for one of them: name, then a string of length bytes. */
static void expect_long_field_refused(const char *name, size_t length) {
    static char body[RAW_MAX];
    int used = snprintf(body, sizeof body,
                        "{\"session_id\":1,\"operations\":[{" UPLOAD_OP ",\"%s\":\"", name);
    TEST_ASSERT_TRUE((size_t)used + length + 8 < sizeof body);
    memset(body + used, 'x', length);
    snprintf(body + used + length, sizeof body - (size_t)used - length, "\"}]}");
    TEST_PRINTF("%s of %zu bytes", name, length);
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_ROMM_BAD_RESPONSE,
                          skiff_romm_parse_sync_plan(body, strlen(body), &plan));
}

static void test_strings_too_long_for_their_fields_are_refused(void) {
    expect_long_field_refused("slot", SKIFF_ROMM_SLOT_MAX);
    expect_long_field_refused("emulator", SKIFF_ROMM_EMULATOR_MAX);
    expect_long_field_refused("server_updated_at", SKIFF_ROMM_TIMESTAMP_MAX);
}

static void test_more_operations_than_a_plan_holds_are_refused(void) {
    static char body[(SKIFF_ROMM_SYNC_OPERATIONS_MAX + 1) * 64 + 64];
    size_t used = (size_t)snprintf(body, sizeof body, "{\"session_id\":1,\"operations\":[");
    for (size_t i = 0; i <= SKIFF_ROMM_SYNC_OPERATIONS_MAX; i++) {
        used += (size_t)snprintf(body + used, sizeof body - used, "%s{" UPLOAD_OP "}",
                                 i == 0 ? "" : ",");
    }
    snprintf(body + used, sizeof body - used, "]}");
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_ROMM_BAD_RESPONSE,
                          skiff_romm_parse_sync_plan(body, strlen(body), &plan));
    TEST_PRINTF("exactly %d fit", SKIFF_ROMM_SYNC_OPERATIONS_MAX);
    char *last = strrchr(body, '{');
    snprintf(last - 1, sizeof body - (size_t)(last - 1 - body), "]}");
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, skiff_romm_parse_sync_plan(body, strlen(body), &plan));
    TEST_ASSERT_EQUAL_size_t(SKIFF_ROMM_SYNC_OPERATIONS_MAX, plan.count);
}

typedef struct bad_state {
    const char *why;
    skiff_romm_save_state state;
} bad_state;

static void test_negotiate_refuses_what_romm_would_and_sends_nothing(void) {
    static char long_slot[SKIFF_ROMM_SLOT_MAX + 1];
    memset(long_slot, 's', SKIFF_ROMM_SLOT_MAX);
    const bad_state CASES[] = {
        {"ROM 0", {0, FILE_NAME, SLOT, EMULATOR, HASH_1, UPDATED_AT_MS, UNIT_BYTES}},
        {"a path as the file name",
         {ROM_ID, "PSP/" FILE_NAME, SLOT, EMULATOR, HASH_1, UPDATED_AT_MS, UNIT_BYTES}},
        {"a quote in the file name",
         {ROM_ID, "a\"b.zip", SLOT, EMULATOR, HASH_1, UPDATED_AT_MS, UNIT_BYTES}},
        {"a non-ASCII file name",
         {ROM_ID, "Caf\xC3\xA9.zip", SLOT, EMULATOR, HASH_1, UPDATED_AT_MS, UNIT_BYTES}},
        {"no slot", {ROM_ID, FILE_NAME, NULL, EMULATOR, HASH_1, UPDATED_AT_MS, UNIT_BYTES}},
        {"a slot too long", {ROM_ID, FILE_NAME, long_slot, EMULATOR, HASH_1, UPDATED_AT_MS, 1}},
        {"a slot with a line break",
         {ROM_ID, FILE_NAME, "auto\nsave", EMULATOR, HASH_1, UPDATED_AT_MS, UNIT_BYTES}},
        {"an emulator with a slash",
         {ROM_ID, FILE_NAME, SLOT, "psp/console", HASH_1, UPDATED_AT_MS, UNIT_BYTES}},
        {"an emulator of 51 bytes",
         {ROM_ID, FILE_NAME, SLOT, "ppppppppppppppppppppppppppppppppppppppppppppppppppp", HASH_1,
          UPDATED_AT_MS, UNIT_BYTES}},
        {"an emulator with a blank at the end",
         {ROM_ID, FILE_NAME, SLOT, "psp ", HASH_1, UPDATED_AT_MS, UNIT_BYTES}},
        {"\"..\" as the emulator", {ROM_ID, FILE_NAME, SLOT, "..", HASH_1, UPDATED_AT_MS, 1}},
        {"an uppercase hash",
         {ROM_ID, FILE_NAME, SLOT, EMULATOR, "969F6B134C73DB736EB427AFDE88AEE4", UPDATED_AT_MS, 1}},
        {"a time before 1970", {ROM_ID, FILE_NAME, SLOT, EMULATOR, HASH_1, -1, UNIT_BYTES}},
        {"a time past year 9999",
         {ROM_ID, FILE_NAME, SLOT, EMULATOR, HASH_1, 253402300800000LL, UNIT_BYTES}},
        {"a size past 2^53",
         {ROM_ID, FILE_NAME, SLOT, EMULATOR, HASH_1, UPDATED_AT_MS, 9007199254740992ULL}},
    };
    for (size_t i = 0; i < sizeof CASES / sizeof CASES[0]; i++) {
        TEST_PRINTF("refused: %s", CASES[i].why);
        TEST_ASSERT_EQUAL_INT_MESSAGE(SKIFF_ERR_INVALID_ARG,
                                      negotiate(&CASES[i].state, 1, DEVICE_A), CASES[i].why);
    }
    const skiff_romm_save_state state = unit_state();
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_INVALID_ARG, negotiate(&state, 1, ""));
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_INVALID_ARG, negotiate(&state, 1, "a b"));
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_INVALID_ARG, negotiate(&state, 1, NULL));
    const skiff_romm_sync_query no_list = {DEVICE_A, NULL, 1, NULL, 0, 0};
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_INVALID_ARG,
                          skiff_romm_saves_negotiate(&client, &no_list, &plan));
    const skiff_romm_sync_query too_many = {DEVICE_A, ROMS, SKIFF_ROMM_SYNC_ROMS_MAX + 1,
                                            NULL,     0,    0};
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_INVALID_ARG,
                          skiff_romm_saves_negotiate(&client, &too_many, &plan));
    const uint64_t zero[] = {0};
    const skiff_romm_sync_query zero_rom = {DEVICE_A, zero, 1, NULL, 0, 0};
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_INVALID_ARG,
                          skiff_romm_saves_negotiate(&client, &zero_rom, &plan));
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_INVALID_ARG, skiff_romm_saves_negotiate(NULL, &no_list, &plan));
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_INVALID_ARG, skiff_romm_saves_negotiate(&client, NULL, &plan));
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_INVALID_ARG,
                          skiff_romm_saves_negotiate(&client, &zero_rom, NULL));
    TEST_ASSERT_EQUAL_size_t_MESSAGE(0, fake.request_count, "nothing was sent");
}

/* ---- Upload ---- */

static skiff_romm_upload unit_upload(const char *path) {
    const skiff_romm_upload spec = {ROM_ID, DEVICE_A,      EMULATOR,      SLOT, HASH_1,
                                    0,      KEEP_VERSIONS, &storage.base, path, FILE_NAME};
    return spec;
}

static skiff_err upload(const skiff_romm_upload *spec) {
    const skiff_err err = skiff_romm_saves_upload(&client, spec, &save);
    TEST_PRINTF("upload -> %s: save %llu '%s' slot '%s' emulator '%s' hash '%s' %llu bytes, from "
                "%s ('%s'); %d open(s), %d close(s)",
                skiff_err_name(err), (unsigned long long)save.id, save.file_name, save.slot,
                save.emulator, save.content_hash, (unsigned long long)save.file_size_bytes,
                save.origin_device_id, save.origin_device_name, storage.opens, storage.closes);
    return err;
}

/* The body Skiff must send for the unit at path: head, file, tail. */
static size_t expected_body(const char *path, unsigned char *out, size_t out_size) {
    const size_t head = sizeof MULTIPART_HEAD - 1;
    memcpy(out, MULTIPART_HEAD, head);
    const size_t file = read_file(path, out + head, out_size - head);
    memcpy(out + head + file, MULTIPART_TAIL, sizeof MULTIPART_TAIL - 1);
    return head + file + sizeof MULTIPART_TAIL - 1;
}

static void test_upload_streams_the_unit_as_multipart_and_reads_the_save(void) {
    fake_route *route = serve(PATH_UPLOAD_A, "romm/saves-upload.http");
    route->match_method = 1;
    route->method = SKIFF_HTTP_POST;
    const skiff_romm_upload spec = unit_upload(UNIT_1);
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, upload(&spec));

    static unsigned char body[UNIT_MAX];
    const size_t size = expected_body(UNIT_1, body, sizeof body);
    const fake_request *sent = &fake.log[0];
    TEST_PRINTF("sent %zu bytes (announced %zu), CRC-32 %08x", sent->body_received, sent->body_size,
                (unsigned)sent->body_crc32);
    TEST_ASSERT_TRUE(sent->body_streamed);
    TEST_ASSERT_EQUAL_size_t_MESSAGE(size, sent->body_size, "Content-Length is exact");
    TEST_ASSERT_EQUAL_size_t(size, sent->body_received);
    TEST_ASSERT_EQUAL_HEX32((uint32_t)crc32(0L, body, (uInt)size), sent->body_crc32);
    TEST_ASSERT_EQUAL_MEMORY(MULTIPART_HEAD, sent->body, sizeof MULTIPART_HEAD - 1);
    TEST_ASSERT_EQUAL_STRING("multipart/form-data; boundary=" BOUNDARY, sent->content_type);
    TEST_ASSERT_EQUAL_STRING(BASE_URL PATH_UPLOAD_A, sent->url);
    assert_token_only_in_headers();

    TEST_ASSERT_EQUAL_UINT64(SAVE_1, save.id);
    TEST_ASSERT_EQUAL_UINT64(ROM_ID, save.rom_id);
    TEST_ASSERT_EQUAL_INT(0, strncmp(save.file_name, "SKIF00001 [", 11));
    TEST_ASSERT_TRUE(save.has_slot);
    TEST_ASSERT_EQUAL_STRING(SLOT, save.slot);
    TEST_ASSERT_EQUAL_STRING(EMULATOR, save.emulator);
    TEST_ASSERT_EQUAL_STRING_MESSAGE(HASH_1, save.content_hash, "RomM hashed the same entries");
    TEST_ASSERT_EQUAL_UINT64(UNIT_BYTES, save.file_size_bytes);
    TEST_ASSERT_EQUAL_STRING(DEVICE_A, save.origin_device_id);
    TEST_ASSERT_EQUAL_STRING("Skiff fixture A", save.origin_device_name);
    TEST_ASSERT_EQUAL_INT(1, storage.opens);
    TEST_ASSERT_EQUAL_INT_MESSAGE(1, storage.closes, "the file is closed");
    TEST_ASSERT_TRUE_MESSAGE(sent->long_transfer,
                             "an upload is not held to the screen's limit on a request");
}

static void test_a_second_psps_upload_names_its_own_device(void) {
    serve(UPLOAD_QUERY(DEVICE_B, HASH_2, "false"), "romm/saves-upload-second-device.http");
    skiff_romm_upload spec = unit_upload(UNIT_2);
    spec.device_id = DEVICE_B;
    spec.content_hash = HASH_2;
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, upload(&spec));
    TEST_ASSERT_EQUAL_UINT64(SAVE_2, save.id);
    TEST_ASSERT_EQUAL_STRING(HASH_2, save.content_hash);
    TEST_ASSERT_EQUAL_STRING(DEVICE_B, save.origin_device_id);
    TEST_ASSERT_EQUAL_STRING("Skiff fixture B", save.origin_device_name);
}

static void test_romms_409_is_a_newer_save_since_this_psps_last_sync(void) {
    serve(PATH_UPLOAD_A, "romm/saves-upload-conflict.http");
    const skiff_romm_upload spec = unit_upload(UNIT_1);
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_ROMM_SAVE_CONFLICT, upload(&spec));
    TEST_ASSERT_EQUAL_UINT64(0, save.id);
    TEST_ASSERT_EQUAL_INT(1, storage.closes);
}

static void test_overwrite_and_odd_values_reach_the_query_encoded(void) {
    serve_raw(
        "/api/saves?rom_id=1&emulator=psp%20console&slot=slot%2F1%20%26%20more&device_id=" DEVICE_A
        "&content_hash=" HASH_1 "&overwrite=true&autocleanup=true"
        "&autocleanup_limit=100",
        JSON_200 "{\"id\":3,\"rom_id\":1,\"file_name\":\"x.zip\",\"slot\":\"slot/1 & more\","
                 "\"emulator\":\"psp console\",\"content_hash\":null,\"updated_at\":\"t\","
                 "\"file_size_bytes\":1612}");
    skiff_romm_upload spec = unit_upload(UNIT_1);
    spec.emulator = "psp console";
    spec.slot = "slot/1 & more";
    spec.overwrite = 1;
    spec.keep_versions = SKIFF_ROMM_SAVE_VERSIONS_MAX;
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, upload(&spec));
    TEST_ASSERT_EQUAL_STRING("", save.content_hash);
    TEST_ASSERT_EQUAL_STRING("", save.origin_device_id);
    TEST_ASSERT_EQUAL_STRING("", save.origin_device_name);
}

typedef struct status_case {
    const char *raw;
    skiff_err expected;
} status_case;

static void test_upload_statuses_map_to_errors(void) {
    static const status_case CASES[] = {
        {"HTTP/1.1 413 Request Entity Too Large\r\nContent-Length: 0\r\n\r\n",
         SKIFF_ERR_ROMM_SAVE_TOO_LARGE},
        {"HTTP/1.1 403 Forbidden\r\nContent-Length: 0\r\n\r\n", SKIFF_ERR_ROMM_FORBIDDEN},
        {"HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\n\r\n", SKIFF_ERR_ROMM_NOT_FOUND},
        {"HTTP/1.1 400 Bad Request\r\nContent-Length: 0\r\n\r\n", SKIFF_ERR_ROMM_BAD_RESPONSE},
        {"HTTP/1.1 500 Internal Server Error\r\nContent-Length: 0\r\n\r\n", SKIFF_ERR_ROMM_SERVER},
        {JSON_200 "<html>", SKIFF_ERR_ROMM_BAD_RESPONSE},
    };
    for (size_t i = 0; i < sizeof CASES / sizeof CASES[0]; i++) {
        reset_fake();
        serve_raw(PATH_UPLOAD_A, CASES[i].raw);
        const skiff_romm_upload spec = unit_upload(UNIT_1);
        TEST_ASSERT_EQUAL_INT(CASES[i].expected, upload(&spec));
        TEST_ASSERT_EQUAL_UINT64(0, save.id);
    }
    TEST_ASSERT_EQUAL_INT(storage.opens, storage.closes);
}

static void test_a_save_of_another_rom_or_slot_is_refused(void) {
    serve_raw(PATH_UPLOAD_A,
              JSON_200 "{\"id\":3,\"rom_id\":2,\"file_name\":\"x.zip\",\"slot\":\"" SLOT
                       "\",\"updated_at\":\"t\",\"file_size_bytes\":1}");
    const skiff_romm_upload spec = unit_upload(UNIT_1);
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_ROMM_BAD_RESPONSE, upload(&spec));
    reset_fake();
    serve_raw(PATH_UPLOAD_A, JSON_200 "{\"id\":3,\"rom_id\":1,\"file_name\":\"x.zip\",\"slot\":"
                                      "\"quick\",\"updated_at\":\"t\",\"file_size_bytes\":1}");
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_ROMM_BAD_RESPONSE, upload(&spec));
    reset_fake();
    serve_raw(PATH_UPLOAD_A,
              JSON_200 "{\"id\":3,\"rom_id\":1,\"file_name\":\"x.zip\",\"slot\":null,"
                       "\"updated_at\":\"t\",\"file_size_bytes\":1}");
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_ROMM_BAD_RESPONSE, upload(&spec));
}

static void test_an_upload_cut_part_way_leaves_nothing_behind(void) {
    fake_route *route = serve(PATH_UPLOAD_A, "romm/saves-upload.http");
    route->fail_after_upload_bytes = 600;
    route->fail_mid_upload = SKIFF_ERR_NET_CONNECTION_LOST;
    const skiff_romm_upload spec = unit_upload(UNIT_1);
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_NET_CONNECTION_LOST, upload(&spec));
    TEST_ASSERT_EQUAL_size_t(600, fake.log[0].body_received);
    TEST_ASSERT_EQUAL_UINT64(0, save.id);
    TEST_ASSERT_EQUAL_INT(1, storage.closes);
}

static void test_a_memory_stick_failing_mid_upload_stops_it(void) {
    serve(PATH_UPLOAD_A, "romm/saves-upload.http");
    storage.read_budget = 1000;
    storage.read_error = SKIFF_ERR_STORAGE_IO;
    const skiff_romm_upload spec = unit_upload(UNIT_1);
    TEST_ASSERT_EQUAL_INT_MESSAGE(SKIFF_ERR_STORAGE_IO, upload(&spec), "the storage's error");
    TEST_ASSERT_EQUAL_INT64_MESSAGE(0, fake.log[0].body_received > sizeof MULTIPART_HEAD - 1 + 1000,
                                    "nothing past what was read");
    TEST_ASSERT_EQUAL_UINT64(0, save.id);
    TEST_ASSERT_EQUAL_INT(1, storage.closes);
}

/* Rewrites the unit (copied to the temp dir) to size bytes when the upload opens it: the file
 * changed between its size and its bytes. */
typedef struct resize_hook {
    char path[TEMP_DIR_PATH_MAX];
    size_t size;
} resize_hook;

static void resize_on_open(void *ctx, const char *call, const char *path) {
    const resize_hook *hook = ctx;
    if (strcmp(call, "open") != 0 || strcmp(path, hook->path) != 0) {
        return;
    }
    static unsigned char bytes[UNIT_MAX];
    memset(bytes, 'z', sizeof bytes);
    FILE *file = fopen(path, "wb");
    fwrite(bytes, 1, hook->size, file);
    fclose(file);
}

static void copy_unit(const char *to) {
    static unsigned char bytes[UNIT_MAX];
    const size_t size = read_file(UNIT_1, bytes, sizeof bytes);
    FILE *file = fopen(to, "wb");
    TEST_ASSERT_NOT_NULL(file);
    TEST_ASSERT_EQUAL_size_t(size, fwrite(bytes, 1, size, file));
    fclose(file);
}

static void test_a_file_that_changes_while_it_is_sent_fails(void) {
    resize_hook hook;
    TEST_ASSERT_TRUE(temp_dir_path(dir, FILE_NAME, hook.path, sizeof hook.path));
    storage.on_call = resize_on_open;
    storage.on_call_ctx = &hook;
    const skiff_romm_upload spec = unit_upload(hook.path);
    const size_t sizes[] = {UNIT_BYTES - 1, UNIT_BYTES + 1, 0};
    for (size_t i = 0; i < sizeof sizes / sizeof sizes[0]; i++) {
        reset_fake();
        serve(PATH_UPLOAD_A, "romm/saves-upload.http");
        copy_unit(hook.path);
        hook.size = sizes[i];
        TEST_PRINTF("%u bytes when measured, %zu when read", UNIT_BYTES, hook.size);
        TEST_ASSERT_EQUAL_INT(SKIFF_ERR_STORAGE_IO, upload(&spec));
        TEST_ASSERT_TRUE_MESSAGE(fake.log[0].body_received < fake.log[0].body_size,
                                 "the body never reached its announced end");
    }
    TEST_ASSERT_EQUAL_INT(storage.opens, storage.closes);
}

/* A file of size bytes in the temp dir with the multipart delimiter at offset. */
static void write_with_delimiter(const char *path, size_t size, size_t offset) {
    static const char DELIMITER[] = "--" BOUNDARY;
    static unsigned char bytes[2 * SKIFF_ROMM_UPLOAD_BUFFER_BYTES];
    TEST_ASSERT_TRUE(size <= sizeof bytes && offset + sizeof DELIMITER - 1 <= size);
    memset(bytes, 'z', size);
    memcpy(bytes + offset, DELIMITER, sizeof DELIMITER - 1);
    FILE *file = fopen(path, "wb");
    TEST_ASSERT_NOT_NULL(file);
    TEST_ASSERT_EQUAL_size_t(size, fwrite(bytes, 1, size, file));
    fclose(file);
}

static void test_a_file_holding_the_boundary_is_never_sent_whole(void) {
    char path[TEMP_DIR_PATH_MAX];
    TEST_ASSERT_TRUE(temp_dir_path(dir, FILE_NAME, path, sizeof path));
    const skiff_romm_upload spec = unit_upload(path);
    const size_t size = SKIFF_ROMM_UPLOAD_BUFFER_BYTES + SKIFF_ROMM_UPLOAD_BUFFER_BYTES / 2;
    const size_t offsets[] = {0, 100, SKIFF_ROMM_UPLOAD_BUFFER_BYTES - 10,
                              SKIFF_ROMM_UPLOAD_BUFFER_BYTES,
                              size - 2 - SKIFF_ROMM_CONTENT_HASH_LENGTH - (sizeof "skiff-" - 1)};
    for (size_t i = 0; i < sizeof offsets / sizeof offsets[0]; i++) {
        reset_fake();
        serve(PATH_UPLOAD_A, "romm/saves-upload.http");
        write_with_delimiter(path, size, offsets[i]);
        TEST_PRINTF("the delimiter at byte %zu of %zu (reads of %zu)", offsets[i], size,
                    SKIFF_ROMM_UPLOAD_BUFFER_BYTES);
        TEST_ASSERT_EQUAL_INT(SKIFF_ERR_INVALID_ARG, upload(&spec));
        TEST_ASSERT_TRUE_MESSAGE(fake.log[0].body_received < fake.log[0].body_size,
                                 "the body never reached its announced end");
        TEST_ASSERT_TRUE_MESSAGE(fake.log[0].body_received < sizeof MULTIPART_HEAD - 1 +
                                                                 offsets[i] + sizeof "--" BOUNDARY -
                                                                 1,
                                 "the delimiter was never sent whole");
    }
    TEST_ASSERT_EQUAL_INT(storage.opens, storage.closes);
}

/* A file of size bytes of '-' (the delimiter's first byte) and near misses of the delimiter: its
 * dashes, then the boundary with its last character changed, every near_every bytes. */
static void write_near_misses(const char *path, size_t size, size_t near_every) {
    static const char DELIMITER[] = "--" BOUNDARY;
    static unsigned char bytes[2 * SKIFF_ROMM_UPLOAD_BUFFER_BYTES];
    TEST_ASSERT_TRUE(size <= sizeof bytes);
    memset(bytes, '-', size);
    for (size_t at = 0; at + sizeof DELIMITER - 1 <= size; at += near_every) {
        memcpy(bytes + at, DELIMITER, sizeof DELIMITER - 1);
        bytes[at + sizeof DELIMITER - 2] ^= 1U;
    }
    FILE *file = fopen(path, "wb");
    TEST_ASSERT_NOT_NULL(file);
    TEST_ASSERT_EQUAL_size_t(size, fwrite(bytes, 1, size, file));
    fclose(file);
}

static void test_dashes_and_near_misses_of_the_boundary_are_sent_whole(void) {
    char path[TEMP_DIR_PATH_MAX];
    TEST_ASSERT_TRUE(temp_dir_path(dir, FILE_NAME, path, sizeof path));
    const skiff_romm_upload spec = unit_upload(path);
    const size_t size = SKIFF_ROMM_UPLOAD_BUFFER_BYTES + SKIFF_ROMM_UPLOAD_BUFFER_BYTES / 2;
    const size_t spacings[] = {sizeof "--" BOUNDARY - 1, 61, SKIFF_ROMM_UPLOAD_BUFFER_BYTES - 7};
    for (size_t i = 0; i < sizeof spacings / sizeof spacings[0]; i++) {
        reset_fake();
        serve(PATH_UPLOAD_A, "romm/saves-upload.http");
        write_near_misses(path, size, spacings[i]);
        TEST_PRINTF("a near miss every %zu bytes of %zu, dashes between", spacings[i], size);
        TEST_ASSERT_EQUAL_INT(SKIFF_OK, upload(&spec));
        TEST_ASSERT_EQUAL_size_t(fake.log[0].body_size, fake.log[0].body_received);
    }
    TEST_ASSERT_EQUAL_INT(storage.opens, storage.closes);
}

static void test_a_missing_unit_sends_nothing(void) {
    char path[TEMP_DIR_PATH_MAX];
    TEST_ASSERT_TRUE(temp_dir_path(dir, "missing.zip", path, sizeof path));
    const skiff_romm_upload spec = unit_upload(path);
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_STORAGE_NOT_FOUND, upload(&spec));
    TEST_ASSERT_EQUAL_size_t(0, fake.request_count);
}

typedef struct bad_upload {
    const char *why;
    skiff_romm_upload upload;
} bad_upload;

static void test_upload_refuses_what_romm_would_and_sends_nothing(void) {
    bad_upload cases[] = {
        {"ROM 0", unit_upload(UNIT_1)},
        {"no device", unit_upload(UNIT_1)},
        {"a colon in the emulator", unit_upload(UNIT_1)},
        {"a short hash", unit_upload(UNIT_1)},
        {"keep no version", unit_upload(UNIT_1)},
        {"keep 101 versions", unit_upload(UNIT_1)},
        {"no storage", unit_upload(UNIT_1)},
        {"no path", unit_upload(UNIT_1)},
        {"a backslash in the name", unit_upload(UNIT_1)},
        {"a line break in the name", unit_upload(UNIT_1)},
        {"\".\" as the name", unit_upload(UNIT_1)},
        {"an empty slot", unit_upload(UNIT_1)},
    };
    cases[0].upload.rom_id = 0;
    cases[1].upload.device_id = NULL;
    cases[2].upload.emulator = "psp:console";
    cases[3].upload.content_hash = "969f6b134c73db736eb427afde88aee";
    cases[4].upload.keep_versions = 0;
    cases[5].upload.keep_versions = SKIFF_ROMM_SAVE_VERSIONS_MAX + 1;
    cases[6].upload.storage = NULL;
    cases[7].upload.path = NULL;
    cases[8].upload.file_name = "a\\b.zip";
    cases[9].upload.file_name = "a\r\nb.zip";
    cases[10].upload.file_name = ".";
    cases[11].upload.slot = "";
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        TEST_PRINTF("refused: %s", cases[i].why);
        TEST_ASSERT_EQUAL_INT_MESSAGE(SKIFF_ERR_INVALID_ARG, upload(&cases[i].upload),
                                      cases[i].why);
    }
    const skiff_romm_upload spec = unit_upload(UNIT_1);
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_INVALID_ARG, skiff_romm_saves_upload(NULL, &spec, &save));
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_INVALID_ARG, skiff_romm_saves_upload(&client, NULL, &save));
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_INVALID_ARG, skiff_romm_saves_upload(&client, &spec, NULL));
    TEST_ASSERT_EQUAL_size_t(0, fake.request_count);
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, storage.opens, "nothing was opened");
}

typedef struct bad_save {
    const char *why;
    const char *body;
} bad_save;

#define SAVE_FIELDS "\"id\":1,\"rom_id\":1,\"file_name\":\"x.zip\",\"slot\":\"autosave\","
#define SAVE_TIMES "\"updated_at\":\"t\",\"file_size_bytes\":1"

static void test_every_malformed_save_is_refused(void) {
    static const bad_save CASES[] = {
        {"not JSON", "Internal Server Error"},
        {"a list", "[]"},
        {"no id", "{\"rom_id\":1,\"file_name\":\"x\",\"updated_at\":\"t\",\"file_size_bytes\":1}"},
        {"no file name", "{\"id\":1,\"rom_id\":1," SAVE_TIMES "}"},
        {"no time", "{" SAVE_FIELDS "\"file_size_bytes\":1}"},
        {"a negative size", "{" SAVE_FIELDS "\"updated_at\":\"t\",\"file_size_bytes\":-1}"},
        {"a hash in capitals",
         "{" SAVE_FIELDS SAVE_TIMES ",\"content_hash\":\"969F6B134C73DB736EB427AFDE88AEE4\"}"},
        {"an origin that is a number", "{" SAVE_FIELDS SAVE_TIMES ",\"origin_device_id\":7}"},
        {"device syncs that are not a list", "{" SAVE_FIELDS SAVE_TIMES ",\"device_syncs\":{}}"},
        {"a device sync without its device",
         "{" SAVE_FIELDS SAVE_TIMES ",\"origin_device_id\":\"a\",\"device_syncs\":[{}]}"},
        {"a device name that is a number",
         "{" SAVE_FIELDS SAVE_TIMES ",\"origin_device_id\":\"a\",\"device_syncs\":[{\"device_id\":"
         "\"a\",\"device_name\":5}]}"},
    };
    for (size_t i = 0; i < sizeof CASES / sizeof CASES[0]; i++) {
        const skiff_err err = skiff_romm_parse_save(CASES[i].body, strlen(CASES[i].body), &save);
        TEST_PRINTF("%s -> %s", CASES[i].why, skiff_err_name(err));
        TEST_ASSERT_EQUAL_INT_MESSAGE(SKIFF_ERR_ROMM_BAD_RESPONSE, err, CASES[i].why);
        TEST_ASSERT_EQUAL_UINT64(0, save.id);
    }
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_INVALID_ARG, skiff_romm_parse_save("{}", 2, NULL));
}

static void test_the_origin_device_name_is_shown_safely(void) {
    static const char BODY[] =
        "{" SAVE_FIELDS SAVE_TIMES ",\"origin_device_id\":\"b\",\"device_syncs\":["
        "{\"device_id\":\"a\",\"device_name\":\"Other\"},"
        "{\"device_id\":\"b\",\"device_name\":\"Argosy\\u001b on Pixel \xC3\xA9\"}]}";
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, skiff_romm_parse_save(BODY, sizeof BODY - 1, &save));
    TEST_PRINTF("from '%s'", save.origin_device_name);
    TEST_ASSERT_EQUAL_STRING("Argosy? on Pixel \xC3\xA9", save.origin_device_name);
    static const char UNNAMED[] =
        "{" SAVE_FIELDS SAVE_TIMES ",\"origin_device_id\":\"b\",\"device_syncs\":["
        "{\"device_id\":\"b\",\"device_name\":null}]}";
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, skiff_romm_parse_save(UNNAMED, sizeof UNNAMED - 1, &save));
    TEST_ASSERT_EQUAL_STRING("b", save.origin_device_id);
    TEST_ASSERT_EQUAL_STRING("", save.origin_device_name);
}

/* ---- Download ---- */

static char target[TEMP_DIR_PATH_MAX];
static char part[TEMP_DIR_PATH_MAX + sizeof SKIFF_ROMM_DOWNLOAD_PART_SUFFIX];

static skiff_romm_download unit_download(uint64_t max_bytes) {
    TEST_ASSERT_TRUE(temp_dir_path(dir, FILE_NAME, target, sizeof target));
    snprintf(part, sizeof part, "%s" SKIFF_ROMM_DOWNLOAD_PART_SUFFIX, target);
    const skiff_romm_download spec = {SAVE_1, DEVICE_B, &storage.base, target, max_bytes};
    return spec;
}

static skiff_err download(const skiff_romm_download *spec, uint64_t *size) {
    const skiff_err err = skiff_romm_saves_download(&client, spec, size);
    TEST_PRINTF("download -> %s, %llu bytes; file %s, part %s", skiff_err_name(err),
                (unsigned long long)*size, exists(target) ? "present" : "absent",
                exists(part) ? "present" : "absent");
    return err;
}

static void test_download_writes_the_save_whole(void) {
    serve(PATH_CONTENT, "romm/saves-content.http");
    const skiff_romm_download spec = unit_download(UNIT_MAX);
    uint64_t size = 0;
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, download(&spec, &size));
    TEST_ASSERT_EQUAL_UINT64(UNIT_BYTES, size);
    static unsigned char got[UNIT_MAX];
    static unsigned char unit[UNIT_MAX];
    TEST_ASSERT_EQUAL_size_t(UNIT_BYTES, read_file(target, got, sizeof got));
    TEST_ASSERT_EQUAL_size_t(UNIT_BYTES, read_file(UNIT_1, unit, sizeof unit));
    TEST_ASSERT_EQUAL_MEMORY(unit, got, UNIT_BYTES);
    TEST_ASSERT_FALSE_MESSAGE(exists(part), "no part file left");
    TEST_ASSERT_EQUAL_STRING(BASE_URL PATH_CONTENT, fake.log[0].url);
    TEST_ASSERT_EQUAL_INT(SKIFF_HTTP_GET, fake.log[0].method);
    TEST_ASSERT_TRUE_MESSAGE(fake.log[0].long_transfer,
                             "a download is not held to the screen's limit on a request");
    assert_token_only_in_headers();
    TEST_PRINTF("a second download replaces the file");
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, download(&spec, &size));
    TEST_ASSERT_EQUAL_size_t(UNIT_BYTES, read_file(target, got, sizeof got));
}

static void expect_nothing_left(const skiff_romm_download *spec, skiff_err expected) {
    uint64_t size = 1;
    TEST_ASSERT_EQUAL_INT(expected, download(spec, &size));
    TEST_ASSERT_EQUAL_UINT64(0, size);
    TEST_ASSERT_FALSE_MESSAGE(exists(part), "the part file is removed");
    TEST_ASSERT_FALSE_MESSAGE(exists(target), "nothing in place of the file");
}

static void test_a_save_over_the_cap_is_refused(void) {
    serve(PATH_CONTENT, "romm/saves-content.http");
    TEST_PRINTF("announced: refused before a byte is written");
    const skiff_romm_download spec = unit_download(UNIT_BYTES - 1);
    expect_nothing_left(&spec, SKIFF_ERR_ROMM_BAD_RESPONSE);
    TEST_ASSERT_EQUAL_UINT64(0, storage.bytes_written);
    TEST_PRINTF("not announced: refused once past it");
    reset_fake();
    serve_raw(PATH_CONTENT, "HTTP/1.1 200 OK\r\n\r\n0123456789");
    const skiff_romm_download small = unit_download(5);
    expect_nothing_left(&small, SKIFF_ERR_ROMM_BAD_RESPONSE);
}

static void test_download_failures_leave_no_part_file(void) {
    const skiff_romm_download spec = unit_download(UNIT_MAX);
    TEST_PRINTF("RomM no longer has it");
    serve_raw(PATH_CONTENT, "HTTP/1.1 404 Not Found\r\nContent-Length: 9\r\n\r\nnot found");
    expect_nothing_left(&spec, SKIFF_ERR_ROMM_NOT_FOUND);
    TEST_PRINTF("a 2xx that is not the file");
    reset_fake();
    serve_raw(PATH_CONTENT, "HTTP/1.1 204 No Content\r\n\r\n");
    expect_nothing_left(&spec, SKIFF_ERR_ROMM_BAD_RESPONSE);
    TEST_PRINTF("the connection drops mid-body");
    reset_fake();
    fake_route *route = serve(PATH_CONTENT, "romm/saves-content.http");
    route->fail_after_bytes = 100;
    route->fail_mid_body = SKIFF_ERR_NET_CONNECTION_LOST;
    expect_nothing_left(&spec, SKIFF_ERR_NET_CONNECTION_LOST);
    TEST_PRINTF("the network is down");
    reset_fake();
    route = serve(PATH_CONTENT, "romm/saves-content.http");
    route->fail_before_response = SKIFF_ERR_NET_CONNECT;
    expect_nothing_left(&spec, SKIFF_ERR_NET_CONNECT);
}

static void test_memory_stick_failures_during_a_download_leave_no_part_file(void) {
    const skiff_romm_download spec = unit_download(UNIT_MAX);
    serve(PATH_CONTENT, "romm/saves-content.http");
    storage.fail_suffix = SKIFF_ROMM_DOWNLOAD_PART_SUFFIX;
    TEST_PRINTF("the Memory Stick fills up");
    storage.write_budget = 100;
    storage.write_error = SKIFF_ERR_STORAGE_NO_SPACE;
    expect_nothing_left(&spec, SKIFF_ERR_STORAGE_NO_SPACE);
    storage.write_error = SKIFF_OK;
    TEST_PRINTF("the sync fails");
    storage.sync_error = SKIFF_ERR_STORAGE_IO;
    expect_nothing_left(&spec, SKIFF_ERR_STORAGE_IO);
    storage.sync_error = SKIFF_OK;
    TEST_PRINTF("the rename fails");
    storage.rename_error = SKIFF_ERR_STORAGE_IO;
    expect_nothing_left(&spec, SKIFF_ERR_STORAGE_IO);
    storage.rename_error = SKIFF_OK;
    TEST_PRINTF("the handle is lost to a suspend mid-write");
    storage.stale_after_writes = storage.writes + 1;
    expect_nothing_left(&spec, SKIFF_ERR_STORAGE_IO);
}

static void test_a_file_reported_lost_while_removed_is_still_replaced(void) {
    const skiff_romm_download spec = unit_download(UNIT_MAX);
    serve(PATH_CONTENT, "romm/saves-content.http");
    copy_unit(target);
    TEST_PRINTF("the old file goes, but the device reports an error: the rename goes ahead");
    storage.fail_suffix = FILE_NAME;
    storage.remove_error = SKIFF_ERR_STORAGE_IO;
    uint64_t size = 0;
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, download(&spec, &size));
    TEST_ASSERT_EQUAL_UINT64(UNIT_BYTES, size);
    TEST_ASSERT_TRUE(exists(target));
    TEST_ASSERT_FALSE(exists(part));
}

static void test_a_failed_replacement_keeps_the_only_copy_left(void) {
    const skiff_romm_download spec = unit_download(UNIT_MAX);
    serve(PATH_CONTENT, "romm/saves-content.http");
    copy_unit(target);
    TEST_PRINTF("the old file is removed, then the rename fails: the whole .part stays");
    storage.fail_suffix = SKIFF_ROMM_DOWNLOAD_PART_SUFFIX;
    storage.rename_error = SKIFF_ERR_STORAGE_IO;
    uint64_t size = 1;
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_STORAGE_IO, download(&spec, &size));
    TEST_ASSERT_EQUAL_UINT64(0, size);
    TEST_ASSERT_FALSE(exists(target));
    static unsigned char got[UNIT_MAX];
    static unsigned char unit[UNIT_MAX];
    TEST_ASSERT_EQUAL_size_t(UNIT_BYTES, read_file(part, got, sizeof got));
    TEST_ASSERT_EQUAL_size_t(UNIT_BYTES, read_file(UNIT_1, unit, sizeof unit));
    TEST_ASSERT_EQUAL_MEMORY(unit, got, UNIT_BYTES);
}

static void test_download_refuses_bad_arguments(void) {
    skiff_romm_download spec = unit_download(UNIT_MAX);
    uint64_t size = 1;
    spec.save_id = 0;
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_INVALID_ARG, download(&spec, &size));
    spec = unit_download(UNIT_MAX);
    spec.device_id = "a\tb";
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_INVALID_ARG, download(&spec, &size));
    spec = unit_download(UNIT_MAX);
    spec.storage = NULL;
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_INVALID_ARG, download(&spec, &size));
    spec = unit_download(UNIT_MAX);
    spec.path = "";
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_INVALID_ARG, download(&spec, &size));
    static char long_path[SKIFF_STORAGE_PATH_MAX];
    memset(long_path, 'p', sizeof long_path - 1);
    spec = unit_download(UNIT_MAX);
    spec.path = long_path;
    TEST_ASSERT_EQUAL_INT_MESSAGE(SKIFF_ERR_INVALID_ARG, download(&spec, &size),
                                  "no room for the part suffix");
    spec = unit_download(UNIT_MAX);
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_INVALID_ARG, skiff_romm_saves_download(NULL, &spec, &size));
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_INVALID_ARG, skiff_romm_saves_download(&client, NULL, &size));
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_INVALID_ARG, skiff_romm_saves_download(&client, &spec, NULL));
    TEST_ASSERT_EQUAL_size_t(0, fake.request_count);
}

/* ---- Downloaded and complete ---- */

#define PATH_GET_2 "/api/saves/2?device_id=" DEVICE_A

static void test_a_save_names_the_device_that_uploaded_it(void) {
    serve(PATH_GET_2, "romm/saves-get.http");
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, skiff_romm_saves_get(&client, SAVE_2, DEVICE_A, &save));
    TEST_PRINTF("save %llu from '%s' (%s), hash %s", (unsigned long long)save.id,
                save.origin_device_name, save.origin_device_id, save.content_hash);
    TEST_ASSERT_EQUAL_STRING(BASE_URL PATH_GET_2, fake.log[0].url);
    TEST_ASSERT_EQUAL_INT(SKIFF_HTTP_GET, fake.log[0].method);
    assert_token_only_in_headers();
    TEST_ASSERT_EQUAL_UINT64(SAVE_2, save.id);
    TEST_ASSERT_EQUAL_STRING(HASH_2, save.content_hash);
    TEST_ASSERT_EQUAL_STRING(DEVICE_B, save.origin_device_id);
    TEST_ASSERT_EQUAL_STRING("Skiff fixture B", save.origin_device_name);
    TEST_PRINTF("another save under the id confirms nothing");
    reset_fake();
    serve(PATH_GET_2, "romm/saves-upload.http");
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_ROMM_BAD_RESPONSE,
                          skiff_romm_saves_get(&client, SAVE_2, DEVICE_A, &save));
    TEST_ASSERT_EQUAL_UINT64(0, save.id);
    TEST_PRINTF("a token without the save scopes is forbidden");
    reset_fake();
    serve_raw(PATH_GET_2, "HTTP/1.1 403 Forbidden\r\nContent-Type: application/json\r\n\r\n"
                          "{\"detail\":\"Forbidden\"}");
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_ROMM_FORBIDDEN,
                          skiff_romm_saves_get(&client, SAVE_2, DEVICE_A, &save));
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_INVALID_ARG, skiff_romm_saves_get(&client, 0, DEVICE_A, &save));
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_INVALID_ARG,
                          skiff_romm_saves_get(&client, SAVE_2, "bad device", &save));
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_INVALID_ARG,
                          skiff_romm_saves_get(NULL, SAVE_2, DEVICE_A, &save));
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_INVALID_ARG,
                          skiff_romm_saves_get(&client, SAVE_2, DEVICE_A, NULL));
}

static void test_downloaded_tells_romm_the_hash_this_psp_holds(void) {
    serve(PATH_DOWNLOADED, "romm/saves-downloaded.http");
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, skiff_romm_saves_downloaded(&client, SAVE_1, DEVICE_B, HASH_1));
    TEST_ASSERT_EQUAL_STRING("{\"device_id\":\"" DEVICE_B "\",\"content_hash\":\"" HASH_1 "\"}",
                             fake.log[0].body);
    TEST_ASSERT_EQUAL_STRING(BASE_URL PATH_DOWNLOADED, fake.log[0].url);
    TEST_ASSERT_EQUAL_INT(SKIFF_HTTP_POST, fake.log[0].method);
    assert_token_only_in_headers();
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_ROMM_NOT_FOUND,
                          skiff_romm_saves_downloaded(&client, SAVE_2, DEVICE_B, HASH_1));
    TEST_PRINTF("an answer that is not the save confirms nothing");
    serve_raw("/api/saves/3/downloaded", JSON_200 "<html>login</html>");
    serve_raw("/api/saves/4/downloaded", JSON_200 "{" SAVE_FIELDS SAVE_TIMES "}");
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_ROMM_BAD_RESPONSE,
                          skiff_romm_saves_downloaded(&client, 3, DEVICE_B, HASH_1));
    TEST_ASSERT_EQUAL_INT_MESSAGE(SKIFF_ERR_ROMM_BAD_RESPONSE,
                                  skiff_romm_saves_downloaded(&client, 4, DEVICE_B, HASH_1),
                                  "save 1 in answer to save 4");
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_INVALID_ARG,
                          skiff_romm_saves_downloaded(&client, 0, DEVICE_B, HASH_1));
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_INVALID_ARG,
                          skiff_romm_saves_downloaded(&client, SAVE_1, NULL, HASH_1));
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_INVALID_ARG,
                          skiff_romm_saves_downloaded(&client, SAVE_1, DEVICE_B, "x"));
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_INVALID_ARG,
                          skiff_romm_saves_downloaded(NULL, SAVE_1, DEVICE_B, HASH_1));
    TEST_ASSERT_EQUAL_size_t(4, fake.request_count);
}

static void test_complete_closes_the_session_with_its_counts(void) {
    serve(PATH_COMPLETE, "romm/saves-complete.http");
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, skiff_romm_saves_complete(&client, SESSION_A, 1, 0));
    TEST_ASSERT_EQUAL_STRING("{\"operations_completed\":1,\"operations_failed\":0}",
                             fake.log[0].body);
    TEST_ASSERT_EQUAL_STRING(BASE_URL PATH_COMPLETE, fake.log[0].url);
    assert_token_only_in_headers();
    reset_fake();
    serve_raw(PATH_COMPLETE, "HTTP/1.1 400 Bad Request\r\nContent-Type: application/json\r\n\r\n"
                             "{\"detail\":\"Session already completed\"}");
    TEST_ASSERT_EQUAL_INT_MESSAGE(SKIFF_ERR_ROMM_BAD_RESPONSE,
                                  skiff_romm_saves_complete(&client, SESSION_A, 1, 0),
                                  "a session closed already");
    TEST_PRINTF("an answer that does not name the session confirms nothing");
    reset_fake();
    serve_raw(PATH_COMPLETE, JSON_200 "<html>login</html>");
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_ROMM_BAD_RESPONSE,
                          skiff_romm_saves_complete(&client, SESSION_A, 1, 0));
    reset_fake();
    serve_raw(PATH_COMPLETE, JSON_200 "{\"session\":{\"id\":2}}");
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_ROMM_BAD_RESPONSE,
                          skiff_romm_saves_complete(&client, SESSION_A, 1, 0));
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_INVALID_ARG, skiff_romm_saves_complete(&client, 0, 1, 0));
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_INVALID_ARG, skiff_romm_saves_complete(NULL, 1, 1, 0));
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_negotiate_sends_this_psps_save_and_reads_an_upload);
    RUN_TEST(test_negotiate_offers_a_second_psp_the_download);
    RUN_TEST(test_negotiate_asks_back_the_saves_this_psp_stopped_listing);
    RUN_TEST(test_negotiate_finds_nothing_to_do_once_in_step);
    RUN_TEST(test_a_token_without_the_save_scopes_is_forbidden);
    RUN_TEST(test_negotiate_returns_every_slot_and_action);
    RUN_TEST(test_negotiate_without_roms_asks_for_the_whole_library);
    RUN_TEST(test_an_operation_for_a_rom_not_asked_about_is_refused);
    RUN_TEST(test_every_malformed_plan_is_refused);
    RUN_TEST(test_strings_too_long_for_their_fields_are_refused);
    RUN_TEST(test_more_operations_than_a_plan_holds_are_refused);
    RUN_TEST(test_negotiate_refuses_what_romm_would_and_sends_nothing);
    RUN_TEST(test_upload_streams_the_unit_as_multipart_and_reads_the_save);
    RUN_TEST(test_a_second_psps_upload_names_its_own_device);
    RUN_TEST(test_romms_409_is_a_newer_save_since_this_psps_last_sync);
    RUN_TEST(test_overwrite_and_odd_values_reach_the_query_encoded);
    RUN_TEST(test_upload_statuses_map_to_errors);
    RUN_TEST(test_a_save_of_another_rom_or_slot_is_refused);
    RUN_TEST(test_an_upload_cut_part_way_leaves_nothing_behind);
    RUN_TEST(test_a_memory_stick_failing_mid_upload_stops_it);
    RUN_TEST(test_a_file_that_changes_while_it_is_sent_fails);
    RUN_TEST(test_a_file_holding_the_boundary_is_never_sent_whole);
    RUN_TEST(test_dashes_and_near_misses_of_the_boundary_are_sent_whole);
    RUN_TEST(test_a_missing_unit_sends_nothing);
    RUN_TEST(test_upload_refuses_what_romm_would_and_sends_nothing);
    RUN_TEST(test_every_malformed_save_is_refused);
    RUN_TEST(test_the_origin_device_name_is_shown_safely);
    RUN_TEST(test_download_writes_the_save_whole);
    RUN_TEST(test_a_save_over_the_cap_is_refused);
    RUN_TEST(test_download_failures_leave_no_part_file);
    RUN_TEST(test_memory_stick_failures_during_a_download_leave_no_part_file);
    RUN_TEST(test_a_file_reported_lost_while_removed_is_still_replaced);
    RUN_TEST(test_a_failed_replacement_keeps_the_only_copy_left);
    RUN_TEST(test_download_refuses_bad_arguments);
    RUN_TEST(test_a_save_names_the_device_that_uploaded_it);
    RUN_TEST(test_downloaded_tells_romm_the_hash_this_psp_holds);
    RUN_TEST(test_complete_closes_the_session_with_its_counts);
    return UNITY_END();
}
